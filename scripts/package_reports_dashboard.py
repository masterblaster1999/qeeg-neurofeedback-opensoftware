#!/usr/bin/env python3
"""Create a portable ZIP bundle for a QEEG reports dashboard.

This repository's report dashboard generator can optionally emit a machine-readable
JSON index:

  python3 scripts/render_reports_dashboard.py <roots...> \
    --out qeeg_reports_dashboard.html \
    --json-index qeeg_reports_dashboard_index.json

The index describes the dashboard HTML and the per-run report HTML files.
This script uses that index to create a single ZIP bundle containing:

  - the JSON index file itself
  - the dashboard HTML
  - any report HTML files referenced by the index (best-effort)
  - (optional) any *local* assets referenced by those HTML files
    (e.g., trace_plot SVGs when the report includes a link to open the raw SVG)
  - (optional) a SHA-256 manifest for all files embedded in the ZIP
  - (optional) the JSON Schema used to validate the index (handy for offline use)

The ZIP preserves the directory layout *relative to the index file* so that:

  unzip bundle.zip -d some_folder
  open some_folder/qeeg_reports_dashboard.html

works offline with correct relative links.

Security note:
  - Files discovered via the JSON index and via HTML asset discovery are only
    bundled if they resolve *under the index file's directory tree*.
    This prevents accidentally capturing unrelated files and avoids creating
    archives with directory traversal entries ("Zip Slip"-class issues).
  - Optional "extras" (manifest and schema) are written under fixed, safe paths.

Usage:
  python3 scripts/package_reports_dashboard.py --index qeeg_reports_dashboard_index.json
  python3 scripts/package_reports_dashboard.py --index qeeg_reports_dashboard_index.json --out bundle.zip
  python3 scripts/package_reports_dashboard.py --index qeeg_reports_dashboard_index.json --no-assets
  python3 scripts/package_reports_dashboard.py --index qeeg_reports_dashboard_index.json --no-manifest
  python3 scripts/package_reports_dashboard.py --index qeeg_reports_dashboard_index.json --no-schema
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import html
import json
import os
import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


# Best-effort: avoid polluting the source tree with __pycache__ when invoked
# from CI/CTest.
sys.dont_write_bytecode = True


def _cleanup_stale_pycache() -> None:
    """Remove bytecode cache files for this script (best-effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            if fn.startswith("package_reports_dashboard.") and fn.endswith(".pyc"):
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


SCHEMA_FILENAME = "qeeg_reports_dashboard_index.schema.json"
DEFAULT_MANIFEST_NAME = "qeeg_reports_bundle_manifest.json"


@dataclass(frozen=True)
class _IndexPaths:
    base_dir: Path
    index_path: Path
    dashboard_html: Path
    report_htmls: List[Path]
    outdirs: List[Path]


def _utc_now_iso() -> str:
    return _dt.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"


def _mtime_iso_utc(p: Path) -> str:
    try:
        ts = float(p.stat().st_mtime)
        return _dt.datetime.utcfromtimestamp(ts).replace(microsecond=0).isoformat() + "Z"
    except Exception:
        return ""


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _load_json(path: Path) -> object:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _resolve_rel_posix(base_dir: Path, posix_path: str) -> Path:
    """Resolve a POSIX-style relative path (from the JSON index) to a Path."""

    s = str(posix_path or "")
    if not s:
        return base_dir

    # Absolute path? Accept it, but we still enforce containment below.
    if os.path.isabs(s) or re.match(r"^[A-Za-z]:[\\/]", s):
        return Path(s).resolve()

    pp = PurePosixPath(s)
    return (base_dir.joinpath(*pp.parts)).resolve()


def _require_under_base(p: Path, base_dir: Path, *, label: str) -> None:
    """Raise if p does not live under base_dir."""

    try:
        p.relative_to(base_dir)
    except Exception:
        raise RuntimeError(
            f"Refusing to bundle {label} outside the index directory tree.\n"
            f"  index base: {str(base_dir)}\n"
            f"  resolved  : {str(p)}\n"
            "Tip: write the JSON index in a common parent directory of all report outputs "
            "(so paths do not contain '../')."
        )


def _zip_relpath(p: Path, base_dir: Path) -> str:
    """Return the zip arcname for p, relative to base_dir, using forward slashes."""

    rel = p.relative_to(base_dir)
    return rel.as_posix()


def _parse_local_asset_paths(html_text: str) -> Set[str]:
    """Extract candidate local asset paths from an HTML document.

    We look for href/src/data attributes and CSS url(...) references.
    Only *syntactic* extraction is performed here; existence/normalization is
    handled by the caller.
    """

    # Very small and permissive extractor (reports are self-generated).
    attr_re = re.compile(r"(?:href|src|data)\s*=\s*([\"'])(.*?)\1", re.IGNORECASE)
    css_url_re = re.compile(r"url\(\s*([\"']?)([^\"')]+)\1\s*\)", re.IGNORECASE)

    out: Set[str] = set()
    for _q, val in attr_re.findall(html_text):
        out.add(val)
    for _q, val in css_url_re.findall(html_text):
        out.add(val)

    # Unescape HTML entities (&amp; etc.) and strip.
    cleaned: Set[str] = set()
    for s in out:
        s2 = html.unescape(str(s or "")).strip()
        if s2:
            cleaned.add(s2)
    return cleaned


def _is_local_ref(ref: str) -> bool:
    s = str(ref or "").strip()
    if not s:
        return False
    # In-page anchors, JS pseudo-links, mailto, etc.
    if s.startswith("#"):
        return False
    low = s.lower()
    if low.startswith(("data:", "mailto:", "javascript:", "file:")):
        return False
    if low.startswith(("http://", "https://", "//")):
        return False
    return True


def _strip_query_fragment(ref: str) -> str:
    # Remove ?query and #fragment parts.
    s = str(ref)
    s = s.split("#", 1)[0]
    s = s.split("?", 1)[0]
    return s


def _collect_paths_from_index(index_path: Path, instance: object) -> _IndexPaths:
    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    base_dir = index_path.resolve().parent

    dash_rel = str(instance.get("dashboard_html") or "")
    dashboard_html = _resolve_rel_posix(base_dir, dash_rel)
    _require_under_base(dashboard_html, base_dir, label="dashboard_html")

    reports = instance.get("reports")
    if not isinstance(reports, list):
        raise RuntimeError("reports must be an array")

    report_htmls: List[Path] = []
    outdirs: List[Path] = []

    for i, it in enumerate(reports):
        if not isinstance(it, dict):
            raise RuntimeError(f"reports[{i}] must be an object")

        outdir_rel = str(it.get("outdir") or "")
        outdir = _resolve_rel_posix(base_dir, outdir_rel)
        _require_under_base(outdir, base_dir, label=f"reports[{i}].outdir")
        outdirs.append(outdir)

        st = str(it.get("status") or "")
        rep_rel = str(it.get("report_html") or "")
        rep = _resolve_rel_posix(base_dir, rep_rel)
        _require_under_base(rep, base_dir, label=f"reports[{i}].report_html")

        # Only include report_html files that exist, or those expected to exist.
        # validate_reports_dashboard_index.py enforces that status ok/skipped
        # implies the report exists.
        if st in ("ok", "skipped"):
            report_htmls.append(rep)
        else:
            # status=error: include only if file exists (best-effort)
            if rep.is_file():
                report_htmls.append(rep)

    # De-duplicate while preserving sort order.
    report_htmls = sorted(set(report_htmls), key=lambda p: str(p))
    outdirs = sorted(set(outdirs), key=lambda p: str(p))

    return _IndexPaths(
        base_dir=base_dir,
        index_path=index_path.resolve(),
        dashboard_html=dashboard_html,
        report_htmls=report_htmls,
        outdirs=outdirs,
    )


def _discover_assets(html_files: Sequence[Path], base_dir: Path) -> Set[Path]:
    assets: Set[Path] = set()
    for p in html_files:
        try:
            txt = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue

        for ref in _parse_local_asset_paths(txt):
            if not _is_local_ref(ref):
                continue
            ref2 = _strip_query_fragment(ref)
            if not ref2:
                continue

            # Resolve relative to the HTML file's folder.
            try:
                cand = (p.parent / Path(ref2)).resolve()
            except Exception:
                continue

            # Only include if file exists and lives under base.
            if cand.is_file():
                try:
                    _require_under_base(cand, base_dir, label=f"asset referenced by {p.name}")
                except Exception:
                    # Ignore out-of-tree assets; this mirrors the bundler's
                    # primary safety policy.
                    continue
                assets.add(cand)
    return assets


def _write_dir_entry(zf: zipfile.ZipFile, arc_dir: str) -> None:
    """Ensure an explicit directory entry exists in the ZIP."""

    d = arc_dir.replace("\\", "/")
    if not d.endswith("/"):
        d += "/"
    # Avoid duplicates.
    try:
        zf.getinfo(d)
        return
    except KeyError:
        pass
    zf.writestr(d, b"")


def _default_schema_path() -> Optional[Path]:
    """Best-effort locate the repo schema file relative to this script."""
    try:
        here = Path(__file__).resolve().parent
        p = (here.parent / "schemas" / SCHEMA_FILENAME).resolve()
        if p.is_file():
            return p
    except Exception:
        pass
    return None


def _safe_arcname(name: str) -> str:
    """Normalize and sanity-check an arcname (Zip Slip hardening)."""
    n = str(name).replace("\\", "/").lstrip("/")
    if not n:
        raise RuntimeError("Invalid empty ZIP entry name")
    parts = [p for p in PurePosixPath(n).parts if p not in ("", ".")]
    # Reject names that collapse to an empty path after normalization
    # (e.g. "."), since ZipFile requires a non-empty entry name.
    if not parts:
        raise RuntimeError(f"Invalid ZIP entry name (empty after normalization): {name!r}")
    if any(p == ".." for p in parts):
        raise RuntimeError(f"Refusing unsafe ZIP entry name containing '..': {name}")
    # Windows drive letters.
    if re.match(r"^[A-Za-z]:", parts[0] if parts else ""):
        raise RuntimeError(f"Refusing unsafe ZIP entry name with drive prefix: {name}")
    return "/".join(parts)


def create_bundle(
    index_path: Path,
    *,
    out_zip: Path,
    include_assets: bool,
    include_manifest: bool,
    include_schema: bool,
    manifest_name: str = DEFAULT_MANIFEST_NAME,
    verbose: bool,
) -> Tuple[int, List[str]]:
    """Create a ZIP bundle.

    Returns: (num_files_written, log_lines)
    """

    log: List[str] = []

    instance = _load_json(index_path)
    paths = _collect_paths_from_index(index_path, instance)

    # Primary set: index + dashboard + reports.
    must_files: List[Path] = [paths.index_path, paths.dashboard_html] + list(paths.report_htmls)

    # Ensure they exist (except error cases).
    for p in must_files:
        if not p.exists():
            raise RuntimeError(f"Missing required file to bundle: {p}")
        if not p.is_file():
            raise RuntimeError(f"Expected a file, got: {p}")

    asset_files: Set[Path] = set()
    if include_assets:
        asset_files = _discover_assets(must_files, paths.base_dir)

    # Outdir directory entries (even if empty).
    outdir_dirs = list(paths.outdirs)

    # Collect all unique files, rooted under base_dir.
    # Represent as explicit (src_path, arcname) to allow optional extras.
    entries: List[Tuple[Path, str]] = []

    def _add_under_base(p: Path) -> None:
        if not p.exists() or not p.is_file():
            return
        _require_under_base(p, paths.base_dir, label=str(p))
        arc = _safe_arcname(_zip_relpath(p, paths.base_dir))
        entries.append((p, arc))

    for p in sorted(set(must_files) | set(asset_files), key=lambda q: str(q)):
        _add_under_base(p)

    # Optional: include schema (not required to live under base_dir).
    if include_schema:
        schema_path = _default_schema_path()
        if schema_path is None:
            log.append("WARN: --include-schema requested but schema file could not be found; skipping.")
        else:
            arc = _safe_arcname(f"schemas/{SCHEMA_FILENAME}")
            entries.append((schema_path, arc))

    # De-duplicate by arcname (keep the first occurrence).
    uniq: Dict[str, Path] = {}
    for p, arc in entries:
        if arc not in uniq:
            uniq[arc] = p
    entries = [(p, arc) for arc, p in sorted(uniq.items(), key=lambda kv: kv[0])]

    # Manifest is written as an in-memory file, not sourced from disk.
    manifest_arc = _safe_arcname(str(manifest_name or DEFAULT_MANIFEST_NAME))
    if include_manifest and any(arc == manifest_arc for _p, arc in entries):
        raise RuntimeError(f"Manifest name collides with an existing bundled file: {manifest_arc}")

    # Build manifest (hashes from disk to match zip payload).
    manifest_obj: Dict[str, object] = {}
    if include_manifest:
        files_meta: List[Dict[str, object]] = []
        total_bytes = 0

        for p, arc in entries:
            try:
                size = int(p.stat().st_size)
            except Exception:
                size = 0
            total_bytes += size
            files_meta.append(
                {
                    "path": arc,
                    "size_bytes": size,
                    "mtime_utc": _mtime_iso_utc(p),
                    "sha256": _sha256_file(p),
                }
            )

        manifest_obj = {
            "manifest_version": 1,
            "generated_utc": _utc_now_iso(),
            "index_path": _safe_arcname(paths.index_path.name),
            "dashboard_html": _safe_arcname(_zip_relpath(paths.dashboard_html, paths.base_dir)),
            "file_count": len(entries),
            "total_bytes": total_bytes,
            "files": files_meta,
        }

    # Write zip.
    out_zip.parent.mkdir(parents=True, exist_ok=True)
    files_written = 0

    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        # Write explicit directory entries for each outdir (even if empty).
        for d in sorted(set(outdir_dirs), key=lambda q: str(q)):
            try:
                _require_under_base(d, paths.base_dir, label=f"outdir {d}")
            except Exception:
                continue
            rel = _zip_relpath(d, paths.base_dir)
            # If the outdir is the bundler's base directory, a directory entry
            # is unnecessary and (if included) can collapse to an empty arcname
            # after normalization. Skip it.
            if rel in ("", "."):
                continue

            arc_dir = _safe_arcname(rel)
            _write_dir_entry(zf, arc_dir)
            if verbose:
                log.append(f"dir  {arc_dir}/")

        for p, arc in entries:
            zf.write(str(p), arcname=arc)
            files_written += 1
            if verbose:
                log.append(f"file {arc}")

        if include_manifest:
            payload = json.dumps(manifest_obj, indent=2, sort_keys=True).encode("utf-8") + b"\n"
            zf.writestr(manifest_arc, payload)
            files_written += 1
            if verbose:
                log.append(f"file {manifest_arc}")

    if include_assets:
        log.append(f"Included {len(asset_files)} local asset(s) referenced by HTML.")
    if include_manifest:
        log.append(f"Wrote manifest: {manifest_arc}")
    if include_schema:
        # Only log if it actually made it in (avoid false positive if skipped).
        if any(arc == f"schemas/{SCHEMA_FILENAME}" for _p, arc in entries):
            log.append(f"Included schema: schemas/{SCHEMA_FILENAME}")

    return files_written, log


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Create a portable ZIP bundle for a QEEG reports dashboard.")
    ap.add_argument(
        "--index",
        required=True,
        help="Path to qeeg_reports_dashboard_index.json (from render_reports_dashboard.py --json-index).",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="Output ZIP path (default: <index-dir>/qeeg_reports_bundle.zip)",
    )
    ap.add_argument(
        "--no-assets",
        action="store_true",
        help="Do not attempt to include local assets referenced by HTML files (bundle only HTML + index).",
    )
    ap.add_argument(
        "--no-manifest",
        action="store_true",
        help=f"Do not include {DEFAULT_MANIFEST_NAME} (SHA-256 checksums for all bundled files).",
    )
    ap.add_argument(
        "--no-schema",
        action="store_true",
        help=f"Do not include schemas/{SCHEMA_FILENAME} in the bundle.",
    )
    ap.add_argument(
        "--manifest-name",
        default=DEFAULT_MANIFEST_NAME,
        help=f"Manifest filename inside the ZIP (default: {DEFAULT_MANIFEST_NAME}).",
    )
    ap.add_argument("--verbose", action="store_true", help="Print every file added to the ZIP.")

    args = ap.parse_args(list(argv) if argv is not None else None)

    index_path = Path(str(args.index)).resolve()
    if not index_path.is_file():
        print(f"ERROR: index does not exist: {str(index_path)}", file=sys.stderr)
        return 1

    out_zip = Path(str(args.out)).resolve() if args.out else (index_path.parent / "qeeg_reports_bundle.zip").resolve()

    try:
        _n, log = create_bundle(
            index_path,
            out_zip=out_zip,
            include_assets=not bool(args.no_assets),
            include_manifest=not bool(args.no_manifest),
            include_schema=not bool(args.no_schema),
            manifest_name=str(args.manifest_name or DEFAULT_MANIFEST_NAME),
            verbose=bool(args.verbose),
        )
        for ln in log:
            print(ln)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    print(f"Wrote: {str(out_zip)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
