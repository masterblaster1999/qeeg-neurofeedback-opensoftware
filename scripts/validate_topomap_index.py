#!/usr/bin/env python3
"""Validate qeeg_topomap_cli --json-index output (topomap_index.json).

This validator is designed for CI and downstream tooling checks. It performs:

  - JSON schema validation (if jsonschema is installed)
  - Basic semantic checks (always)
  - Optional file existence checks (--check-files)

Exit code:
  0 = valid
  1 = invalid
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _read_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"Expected object at top-level JSON: {path}")
    return data


def _resolve_index_file(index_path: Path, file_field: str) -> Path:
    f = str(file_field or "").strip()
    if not f:
        return Path("")
    # JSON index is POSIX-slash oriented; normalize for the host.
    f_os = f.replace("/", os.sep).replace("\\", os.sep)
    p = Path(f_os)
    if p.is_absolute():
        return p
    base = index_path.parent
    return (base / p).resolve()


def _schema_path(schemas_dir: Optional[Path]) -> Path:
    if schemas_dir is None:
        # repo_root/scripts/validate_... -> repo_root/schemas
        here = Path(__file__).resolve()
        return (here.parent.parent / "schemas" / "qeeg_topomap_index.schema.json").resolve()
    return (schemas_dir / "qeeg_topomap_index.schema.json").resolve()


def _basic_checks(idx: Dict[str, Any]) -> List[str]:
    errors: List[str] = []

    sv = idx.get("schema_version")
    if sv != 1:
        errors.append(f"schema_version must be 1, got: {sv!r}")

    if not isinstance(idx.get("generated_utc"), str):
        errors.append("generated_utc must be a string")

    if idx.get("tool") != "qeeg_topomap_cli":
        # keep as a warning-level semantic check (some forks may rename)
        errors.append(f"tool should be 'qeeg_topomap_cli', got: {idx.get('tool')!r}")

    for k in ("input_path", "outdir", "run_meta_json"):
        if not isinstance(idx.get(k), str) or not idx.get(k):
            errors.append(f"{k} must be a non-empty string")

    # report_html may be string or null
    rh = idx.get("report_html")
    if rh is not None and not isinstance(rh, str):
        errors.append("report_html must be a string or null")

    # montage block
    montage = idx.get("montage")
    if not isinstance(montage, dict):
        errors.append("montage must be an object")
    else:
        if not isinstance(montage.get("spec"), str) or not montage.get("spec"):
            errors.append("montage.spec must be a non-empty string")
        n_ch = montage.get("n_channels")
        if not isinstance(n_ch, int) or n_ch <= 0:
            errors.append("montage.n_channels must be a positive integer")
        chans = montage.get("channels")
        if not isinstance(chans, list):
            errors.append("montage.channels must be an array")
        else:
            if isinstance(n_ch, int) and len(chans) != n_ch:
                # not fatal, but strongly indicative of mismatch
                errors.append(f"montage.n_channels ({n_ch}) != len(montage.channels) ({len(chans)})")

    # maps block
    maps = idx.get("maps")
    if not isinstance(maps, list) or not maps:
        errors.append("maps must be a non-empty array")
    else:
        for i, m in enumerate(maps):
            if not isinstance(m, dict):
                errors.append(f"maps[{i}] must be an object")
                continue
            if not isinstance(m.get("metric"), str) or not m.get("metric"):
                errors.append(f"maps[{i}].metric must be a non-empty string")
            f = m.get("file")
            if not isinstance(f, str) or not f:
                errors.append(f"maps[{i}].file must be a non-empty string")
            nc = m.get("n_channels")
            if not isinstance(nc, int) or nc < 0:
                errors.append(f"maps[{i}].n_channels must be an integer >= 0")
            chs = m.get("channels")
            if not isinstance(chs, list):
                errors.append(f"maps[{i}].channels must be an array")
            else:
                if isinstance(nc, int) and len(chs) != nc:
                    errors.append(f"maps[{i}].n_channels ({nc}) != len(maps[{i}].channels) ({len(chs)})")

            # discourage backslashes in portable path fields
            for pk in ("file",):
                pv = m.get(pk)
                if isinstance(pv, str) and "\\" in pv:
                    errors.append(f"maps[{i}].{pk} contains backslashes; expected POSIX-style '/'")

    return errors


def _schema_validate(idx: Dict[str, Any], schema: Dict[str, Any]) -> List[str]:
    try:
        import jsonschema  # type: ignore
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return []  # schema validation is optional

    v = Draft202012Validator(schema)
    errors = sorted(v.iter_errors(idx), key=lambda e: e.path)
    out: List[str] = []
    for e in errors:
        loc = "/".join([str(p) for p in e.path]) if e.path else "<root>"
        out.append(f"schema: {loc}: {e.message}")
    return out


def _file_checks(idx: Dict[str, Any], index_path: Path) -> List[str]:
    errors: List[str] = []

    def check_path(field_name: str, rel_or_abs: str) -> None:
        if not isinstance(rel_or_abs, str) or not rel_or_abs:
            errors.append(f"{field_name}: expected non-empty string path")
            return
        p = _resolve_index_file(index_path, rel_or_abs)
        if not p.exists():
            errors.append(f"{field_name}: file not found: {p}")
            return
        if not p.is_file():
            errors.append(f"{field_name}: not a file: {p}")

    check_path("run_meta_json", idx.get("run_meta_json"))

    rh = idx.get("report_html")
    if isinstance(rh, str) and rh:
        check_path("report_html", rh)

    maps = idx.get("maps")
    if isinstance(maps, list):
        for i, m in enumerate(maps):
            if not isinstance(m, dict):
                continue
            f = m.get("file")
            if isinstance(f, str) and f:
                check_path(f"maps[{i}].file", f)

    return errors


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("index", nargs="+", help="Path(s) to topomap_index.json")
    ap.add_argument("--schemas-dir", default=None, help="Directory containing JSON schemas (default: repo/schemas)")
    ap.add_argument("--check-files", action="store_true", help="Verify referenced files exist (relative to the index file)")
    ap.add_argument("--verbose", action="store_true", help="Print extra diagnostics")
    args = ap.parse_args(argv)

    schemas_dir = Path(args.schemas_dir).resolve() if args.schemas_dir else None
    schema_path = _schema_path(schemas_dir)
    schema: Optional[Dict[str, Any]] = None
    if schema_path.exists():
        schema = _read_json(schema_path)
    elif args.verbose:
        print(f"Note: schema not found at {schema_path}; skipping JSON Schema validation", file=sys.stderr)

    any_errors: List[Tuple[str, str]] = []

    for idx_file in args.index:
        p = Path(idx_file).resolve()
        try:
            idx = _read_json(p)
        except Exception as e:
            any_errors.append((str(p), f"read: {e}"))
            continue

        # Schema validation (optional)
        if schema is not None:
            for msg in _schema_validate(idx, schema):
                any_errors.append((str(p), msg))

        # Always do basic checks
        for msg in _basic_checks(idx):
            any_errors.append((str(p), msg))

        if args.check_files:
            for msg in _file_checks(idx, p):
                any_errors.append((str(p), msg))

    if any_errors:
        for path, msg in any_errors:
            print(f"{path}: {msg}", file=sys.stderr)
        return 1

    if args.verbose:
        print("OK", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
