#!/usr/bin/env python3
"""Validate qeeg_region_summary_cli --json-index output (region_summary_index.json).

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
import math
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
        return (here.parent.parent / "schemas" / "qeeg_region_summary_index.schema.json").resolve()
    return (schemas_dir / "qeeg_region_summary_index.schema.json").resolve()


_ALLOWED_GROUP_TYPES = {"all", "lobe", "hemisphere", "lobe_hemi"}


def _basic_checks(idx: Dict[str, Any]) -> List[str]:
    errors: List[str] = []

    sv = idx.get("schema_version")
    if sv != 1:
        errors.append(f"schema_version must be 1, got: {sv!r}")

    if not isinstance(idx.get("generated_utc"), str):
        errors.append("generated_utc must be a string")

    if idx.get("tool") != "qeeg_region_summary_cli":
        errors.append(f"tool should be 'qeeg_region_summary_cli', got: {idx.get('tool')!r}")

    for k in ("input_path", "outdir", "run_meta_json", "csv_wide", "csv_long"):
        if not isinstance(idx.get(k), str) or not idx.get(k):
            errors.append(f"{k} must be a non-empty string")

    rh = idx.get("report_html")
    if rh is not None and not isinstance(rh, str):
        errors.append("report_html must be a string or null")

    metrics = idx.get("metrics")
    if not isinstance(metrics, list) or not metrics:
        errors.append("metrics must be a non-empty array")
        metrics = []
    else:
        for i, m in enumerate(metrics):
            if not isinstance(m, str) or not m.strip():
                errors.append(f"metrics[{i}] must be a non-empty string")

    mlen = len(metrics) if isinstance(metrics, list) else 0

    # discourage backslashes in portable path fields
    for pk in ("run_meta_json", "csv_wide", "csv_long"):
        pv = idx.get(pk)
        if isinstance(pv, str) and "\\" in pv:
            errors.append(f"{pk} contains backslashes; expected POSIX-style '/'")
    if isinstance(rh, str) and "\\" in rh:
        errors.append("report_html contains backslashes; expected POSIX-style '/'")

    groups = idx.get("groups")
    if not isinstance(groups, list) or not groups:
        errors.append("groups must be a non-empty array")
        return errors

    for gi, g in enumerate(groups):
        if not isinstance(g, dict):
            errors.append(f"groups[{gi}] must be an object")
            continue

        gt = g.get("group_type")
        if not isinstance(gt, str) or not gt.strip():
            errors.append(f"groups[{gi}].group_type must be a non-empty string")
        else:
            if gt not in _ALLOWED_GROUP_TYPES:
                errors.append(f"groups[{gi}].group_type must be one of {sorted(_ALLOWED_GROUP_TYPES)}, got: {gt!r}")

        gr = g.get("group")
        if not isinstance(gr, str) or not gr.strip():
            errors.append(f"groups[{gi}].group must be a non-empty string")

        nc = g.get("n_channels")
        if not isinstance(nc, int) or nc < 0:
            errors.append(f"groups[{gi}].n_channels must be an integer >= 0")

        vals = g.get("values")
        if not isinstance(vals, list):
            errors.append(f"groups[{gi}].values must be an array")
            vals = []
        elif mlen and len(vals) != mlen:
            errors.append(f"groups[{gi}].values length ({len(vals)}) != len(metrics) ({mlen})")

        for vi, v in enumerate(vals):
            if v is None:
                continue
            if not isinstance(v, (int, float)):
                errors.append(f"groups[{gi}].values[{vi}] must be number or null")
                continue
            if not math.isfinite(float(v)):
                errors.append(f"groups[{gi}].values[{vi}] must be finite")

        n_valid = g.get("n_valid")
        if not isinstance(n_valid, list):
            errors.append(f"groups[{gi}].n_valid must be an array")
            n_valid = []
        elif mlen and len(n_valid) != mlen:
            errors.append(f"groups[{gi}].n_valid length ({len(n_valid)}) != len(metrics) ({mlen})")

        for ni, n in enumerate(n_valid):
            if not isinstance(n, int) or n < 0:
                errors.append(f"groups[{gi}].n_valid[{ni}] must be an integer >= 0")

    return errors


def _schema_validate(idx: Dict[str, Any], schema: Dict[str, Any]) -> List[str]:
    try:
        import jsonschema  # type: ignore
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return []

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
    check_path("csv_wide", idx.get("csv_wide"))
    check_path("csv_long", idx.get("csv_long"))

    rh = idx.get("report_html")
    if isinstance(rh, str) and rh:
        check_path("report_html", rh)

    return errors


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("index", nargs="+", help="Path(s) to region_summary_index.json")
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

        if schema is not None:
            for msg in _schema_validate(idx, schema):
                any_errors.append((str(p), msg))

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
