#!/usr/bin/env python3
"""Validate qeeg_loreta_metrics_index.json (qeeg_loreta_metrics_cli --json-index).

This validator is designed for CI and downstream tooling checks. It performs:

  - JSON Schema validation (if jsonschema is installed and schema is available)
  - Basic semantic checks (always)
  - Optional file existence checks (--check-files)

Exit code:
  0 = valid
  1 = invalid
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _read_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _schema_path(schemas_dir: Optional[Path]) -> Optional[Path]:
    if schemas_dir is None:
        return None
    p = schemas_dir / "qeeg_loreta_metrics_index.schema.json"
    return p if p.exists() else None


def _schema_validate(obj: Dict[str, Any], schema: Dict[str, Any]) -> List[str]:
    try:
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return []  # jsonschema is optional

    v = Draft202012Validator(schema)
    errors = sorted(v.iter_errors(obj), key=lambda e: e.path)
    out: List[str] = []
    for e in errors:
        path = ".".join(str(p) for p in e.path) if e.path else "<root>"
        out.append(f"schema: {path}: {e.message}")
    return out


def _resolve_rel(index_path: Path, rel: str) -> Path:
    # Treat schema-emitted paths as relative to the index file directory.
    rel = (rel or "").replace("\\", "/")
    return (index_path.parent / rel).resolve()


def _semantic_errors(obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []

    metrics = obj.get("metrics")
    rois = obj.get("rois")
    if not isinstance(metrics, list) or not metrics:
        errs.append("metrics must be a non-empty array")
        return errs
    n_metrics = len(metrics)

    if not isinstance(rois, list) or not rois:
        errs.append("rois must be a non-empty array")
        return errs

    seen: set[str] = set()
    for i, r in enumerate(rois):
        if not isinstance(r, dict):
            errs.append(f"rois[{i}] must be an object")
            continue
        roi = r.get("roi")
        vals = r.get("values")
        if not isinstance(roi, str) or not roi:
            errs.append(f"rois[{i}].roi must be a non-empty string")
            continue
        if roi in seen:
            errs.append(f"duplicate roi: {roi}")
        seen.add(roi)
        if not isinstance(vals, list):
            errs.append(f"rois[{i}].values must be an array")
            continue
        if len(vals) != n_metrics:
            errs.append(
                f"shape mismatch for roi '{roi}': values has {len(vals)} entries but metrics has {n_metrics}"
            )

    return errs


def _check_files_errors(index_path: Path, obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []

    def _get_str(k: str) -> Optional[str]:
        v = obj.get(k)
        return v if isinstance(v, str) and v else None

    required = ["run_meta_json", "csv_wide", "csv_long"]
    for k in required:
        rel = _get_str(k)
        if rel is None:
            errs.append(f"missing or invalid '{k}' path")
            continue
        p = _resolve_rel(index_path, rel)
        if not p.exists():
            errs.append(f"missing file: {k} -> {p}")

    for k in ["report_html", "protocol_json"]:
        rel = _get_str(k)
        if rel:
            p = _resolve_rel(index_path, rel)
            if not p.exists():
                errs.append(f"missing file: {k} -> {p}")

    return errs


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("index_json", nargs="+", help="Path(s) to loreta_metrics_index.json")
    ap.add_argument(
        "--schemas-dir",
        default=None,
        help="Directory containing JSON schemas (default: <repo>/schemas)",
    )
    ap.add_argument("--check-files", action="store_true", help="Verify referenced output files exist")
    ap.add_argument("--verbose", action="store_true", help="Print extra diagnostics")
    args = ap.parse_args(argv)

    schemas_dir = Path(args.schemas_dir).resolve() if args.schemas_dir else None
    schema_path = _schema_path(schemas_dir)

    schema: Optional[Dict[str, Any]] = None
    if schema_path is not None:
        try:
            schema = _read_json(schema_path)
        except Exception as e:
            if args.verbose:
                print(f"Note: failed to read schema at {schema_path}: {e}", file=sys.stderr)
            schema = None
    elif args.verbose and schemas_dir is not None:
        print(f"Note: schema not found at {schemas_dir}", file=sys.stderr)

    any_errors: List[Tuple[str, str]] = []

    for idx_file in args.index_json:
        p = Path(idx_file).resolve()
        if not p.exists():
            any_errors.append((str(p), "missing"))
            continue
        try:
            obj0 = _read_json(p)
        except Exception as e:
            any_errors.append((str(p), f"read: {e}"))
            continue
        if not isinstance(obj0, dict):
            any_errors.append((str(p), "index JSON must be an object"))
            continue

        errs: List[str] = []
        if schema is not None:
            errs.extend(_schema_validate(obj0, schema))
        errs.extend(_semantic_errors(obj0))
        if args.check_files:
            errs.extend(_check_files_errors(p, obj0))

        if errs:
            for e in errs:
                any_errors.append((str(p), e))
        elif args.verbose:
            print(f"OK: {p}")

    if any_errors:
        for path, msg in any_errors:
            print(f"ERROR {path}: {msg}", file=sys.stderr)
        return 1

    if args.verbose:
        print("OK: loreta metrics index is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
