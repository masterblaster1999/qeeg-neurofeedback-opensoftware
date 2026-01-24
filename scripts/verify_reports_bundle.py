#!/usr/bin/env python3
"""Verify a portable QEEG reports dashboard bundle ZIP.

`scripts/package_reports_dashboard.py` can create a portable ZIP containing:

  - the dashboard HTML
  - the JSON index
  - per-run report HTML files
  - (optional) local assets referenced by HTML
  - (optional) a SHA-256 manifest: qeeg_reports_bundle_manifest.json

This script verifies (stdlib only):

  1) The ZIP does not contain unsafe entry names (no absolute paths, no "..").
  2) If a manifest is present (or requested), the SHA-256 hashes match.
  3) If an index is present (or requested), files referenced by the index exist
     inside the ZIP (dashboard_html + report_html paths).

Optionally it can extract the bundle safely (Zip Slip hardened).

Usage:
  python3 scripts/verify_reports_bundle.py --bundle qeeg_reports_bundle.zip
  python3 scripts/verify_reports_bundle.py --bundle qeeg_reports_bundle.zip --no-hashes
  python3 scripts/verify_reports_bundle.py --bundle qeeg_reports_bundle.zip --extract ./out_folder
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


# Best-effort: avoid polluting the source tree with __pycache__ when invoked
# from CI/CTest.
sys.dont_write_bytecode = True


DEFAULT_MANIFEST_NAME = "qeeg_reports_bundle_manifest.json"


def _cleanup_stale_pycache() -> None:
    """Remove bytecode cache files for this script (best-effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            if fn.startswith("verify_reports_bundle.") and fn.endswith(".pyc"):
                try:
                    os.remove(os.path.join(pycache, fn))
                    removed_any = True
                except Exception:
                    pass

        if removed_any:
            try:
                if not os.listdir(pycache):
                    os.rmdir(pycache)
            except Exception:
                pass
    except Exception:
        pass


_cleanup_stale_pycache()


def _safe_zip_name(name: str) -> str:
    """Normalize and sanity-check a zip entry name (Zip Slip hardening)."""
    n = str(name).replace("\\", "/")
    # zipfile can contain directory entries; keep trailing slash if present.
    is_dir = n.endswith("/")
    n = n.lstrip("/")
    if not n:
        raise RuntimeError("Invalid empty ZIP entry name")
    parts = [p for p in PurePosixPath(n).parts if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise RuntimeError(f"Unsafe ZIP entry name containing '..': {name}")
    # Windows drive letters.
    if parts and re.match(r"^[A-Za-z]:", parts[0]):
        raise RuntimeError(f"Unsafe ZIP entry name with drive prefix: {name}")
    out = "/".join(parts)
    if is_dir and not out.endswith("/"):
        out += "/"
    return out


def _sha256_bytes_iter(chunks: Iterable[bytes]) -> str:
    h = hashlib.sha256()
    for c in chunks:
        h.update(c)
    return h.hexdigest()


def _sha256_zip_member(zf: zipfile.ZipFile, name: str) -> str:
    with zf.open(name, "r") as f:
        return _sha256_bytes_iter(iter(lambda: f.read(1024 * 1024), b""))


def _read_json_member(zf: zipfile.ZipFile, name: str) -> Any:
    with zf.open(name, "r") as f:
        return json.loads(f.read().decode("utf-8"))


def _normalize_join(base_dir: str, rel_posix: str) -> str:
    """Join base_dir and rel_posix using POSIX semantics, returning a safe path."""
    b = PurePosixPath(str(base_dir or ""))
    r = PurePosixPath(str(rel_posix or ""))
    joined = (b / r).as_posix()
    return _safe_zip_name(joined)


def _find_default_index_name(names: List[str]) -> Optional[str]:
    # Common convention in this repo, but allow custom names.
    for cand in names:
        if cand.endswith("qeeg_reports_dashboard_index.json"):
            return cand
    # If there's exactly one .json at the top level, treat it as the index.
    top_json = [n for n in names if "/" not in n and n.lower().endswith(".json")]
    if len(top_json) == 1:
        return top_json[0]
    return None


def verify_bundle(
    bundle_zip: Path,
    *,
    manifest_name: str = DEFAULT_MANIFEST_NAME,
    index_name: Optional[str] = None,
    check_hashes: bool = True,
    check_index: bool = True,
    verbose: bool = False,
) -> Tuple[bool, List[str]]:
    """Verify the bundle ZIP.

    Returns: (ok, log_lines)
    """
    log: List[str] = []
    if not bundle_zip.is_file():
        return False, [f"ERROR: bundle does not exist: {str(bundle_zip)}"]

    with zipfile.ZipFile(bundle_zip, "r") as zf:
        raw_names = zf.namelist()
        # Normalize + validate entry names.
        names: List[str] = []
        for n in raw_names:
            try:
                nn = _safe_zip_name(n)
            except Exception as e:
                return False, [f"ERROR: {e}"]
            names.append(nn)

        name_set = set(names)

        # Locate manifest (optional).
        manifest_arc = _safe_zip_name(manifest_name or DEFAULT_MANIFEST_NAME)
        has_manifest = manifest_arc in name_set

        manifest_obj: Optional[Dict[str, Any]] = None
        if has_manifest:
            try:
                obj = _read_json_member(zf, manifest_arc)
            except Exception as e:
                return False, [f"ERROR: Failed to read manifest {manifest_arc}: {e}"]
            if not isinstance(obj, dict):
                return False, [f"ERROR: Manifest {manifest_arc} must be a JSON object"]
            manifest_obj = obj

        # Determine index name.
        idx_name = index_name
        if idx_name:
            idx_name = _safe_zip_name(idx_name)
        elif manifest_obj and isinstance(manifest_obj.get("index_path"), str):
            idx_name = _safe_zip_name(str(manifest_obj.get("index_path")))
        else:
            idx_name = _find_default_index_name(names)

        if check_hashes:
            if not has_manifest:
                return False, [f"ERROR: Manifest not found in ZIP: {manifest_arc} (use --no-hashes to skip)"]
            assert manifest_obj is not None

            files = manifest_obj.get("files")
            if not isinstance(files, list):
                return False, [f"ERROR: Manifest {manifest_arc} missing 'files' array"]

            bad = 0
            checked = 0
            for i, it in enumerate(files):
                if not isinstance(it, dict):
                    return False, [f"ERROR: Manifest files[{i}] must be an object"]
                path = it.get("path")
                sha = it.get("sha256")
                size = it.get("size_bytes")
                if not isinstance(path, str) or not isinstance(sha, str):
                    return False, [f"ERROR: Manifest files[{i}] missing 'path'/'sha256' strings"]
                arc = _safe_zip_name(path)

                if arc not in name_set:
                    log.append(f"ERROR: Missing file listed in manifest: {arc}")
                    bad += 1
                    continue

                # Compare size to ZIP header if available.
                try:
                    zi = zf.getinfo(arc)
                    if isinstance(size, int) and zi.file_size != int(size):
                        log.append(f"ERROR: Size mismatch for {arc}: manifest={size} zip={zi.file_size}")
                        bad += 1
                except Exception:
                    pass

                try:
                    digest = _sha256_zip_member(zf, arc)
                except Exception as e:
                    log.append(f"ERROR: Failed to hash {arc}: {e}")
                    bad += 1
                    continue

                if digest.lower() != str(sha).lower():
                    log.append(f"ERROR: SHA256 mismatch for {arc}")
                    bad += 1
                checked += 1
                if verbose and checked % 25 == 0:
                    log.append(f"... hashed {checked}/{len(files)} files")

            if bad:
                return False, log + [f"ERROR: Manifest verification failed ({bad} file(s) mismatched)"]
            log.append(f"OK: Manifest verified ({checked} file(s))")

        if check_index:
            if not idx_name or idx_name not in name_set:
                return False, [f"ERROR: Could not locate index JSON in ZIP (use --index-name to specify)"]
            try:
                idx_obj = _read_json_member(zf, idx_name)
            except Exception as e:
                return False, [f"ERROR: Failed to read index {idx_name}: {e}"]
            if not isinstance(idx_obj, dict):
                return False, [f"ERROR: Index {idx_name} must be a JSON object"]

            base_dir = PurePosixPath(idx_name).parent.as_posix()
            if base_dir == ".":
                base_dir = ""

            dash_rel = str(idx_obj.get("dashboard_html") or "")
            dash_arc = _normalize_join(base_dir, dash_rel)
            if dash_arc not in name_set:
                return False, [f"ERROR: dashboard_html referenced by index is missing in ZIP: {dash_arc}"]
            log.append(f"OK: dashboard_html present: {dash_arc}")

            reports = idx_obj.get("reports")
            if not isinstance(reports, list):
                return False, [f"ERROR: Index {idx_name} missing 'reports' array"]

            missing = 0
            checked = 0
            for i, r in enumerate(reports):
                if not isinstance(r, dict):
                    return False, [f"ERROR: Index reports[{i}] must be an object"]
                st = str(r.get("status") or "")
                rep_rel = str(r.get("report_html") or "")
                if not rep_rel:
                    continue
                rep_arc = _normalize_join(base_dir, rep_rel)

                # Match the bundler's policy: ok/skipped should exist.
                if st in ("ok", "skipped"):
                    checked += 1
                    if rep_arc not in name_set:
                        log.append(f"ERROR: Missing report_html for reports[{i}] status={st}: {rep_arc}")
                        missing += 1
                else:
                    # status=error: best-effort (ignore)
                    pass

            if missing:
                return False, log + [f"ERROR: Index verification failed ({missing}/{checked} expected report(s) missing)"]
            log.append(f"OK: index references resolved ({checked} report(s) checked)")

    return True, log


def safe_extract_bundle(bundle_zip: Path, out_dir: Path, *, verbose: bool = False) -> None:
    """Safely extract the bundle ZIP into out_dir (Zip Slip hardened)."""

    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(bundle_zip, "r") as zf:
        for raw in zf.namelist():
            name = _safe_zip_name(raw)
            if name.endswith("/"):
                (out_dir / name.rstrip("/")).mkdir(parents=True, exist_ok=True)
                continue

            dest = (out_dir / name).resolve()
            # Enforce containment.
            try:
                dest.relative_to(out_dir)
            except Exception:
                raise RuntimeError(f"Unsafe extraction path escapes destination: {name}")

            dest.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(name, "r") as src, open(dest, "wb") as dst:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    dst.write(chunk)
            if verbose:
                print(f"extracted {name}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Verify a portable QEEG reports dashboard bundle ZIP.")
    ap.add_argument("--bundle", required=True, help="Path to the bundle ZIP (qeeg_reports_bundle.zip).")
    ap.add_argument(
        "--manifest-name",
        default=DEFAULT_MANIFEST_NAME,
        help=f"Manifest filename inside the ZIP (default: {DEFAULT_MANIFEST_NAME}).",
    )
    ap.add_argument(
        "--index-name",
        default=None,
        help="Index JSON filename inside the ZIP (optional; auto-detected or read from manifest).",
    )
    ap.add_argument("--no-hashes", action="store_true", help="Skip SHA-256 manifest verification.")
    ap.add_argument("--no-index", action="store_true", help="Skip index cross-check verification.")
    ap.add_argument("--extract", default=None, help="If provided, safely extract the ZIP into this folder.")
    ap.add_argument("--verbose", action="store_true", help="Print verbose progress details.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    bundle_zip = Path(str(args.bundle)).resolve()

    ok, log = verify_bundle(
        bundle_zip,
        manifest_name=str(args.manifest_name or DEFAULT_MANIFEST_NAME),
        index_name=str(args.index_name) if args.index_name else None,
        check_hashes=not bool(args.no_hashes),
        check_index=not bool(args.no_index),
        verbose=bool(args.verbose),
    )
    for ln in log:
        if ln.startswith("ERROR"):
            print(ln, file=sys.stderr)
        else:
            print(ln)

    if not ok:
        return 1

    if args.extract:
        try:
            safe_extract_bundle(bundle_zip, Path(str(args.extract)), verbose=bool(args.verbose))
        except Exception as e:
            print(f"ERROR: extraction failed: {e}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
