#!/usr/bin/env python3
"""Validate qeeg_loreta_connectivity_index.json (qeeg_loreta_connectivity_cli --json-index).

Checks performed:
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
    p = schemas_dir / "qeeg_loreta_connectivity_index.schema.json"
    return p if p.exists() else None


def _schema_validate(obj: Dict[str, Any], schema: Dict[str, Any]) -> List[str]:
    try:
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return []

    v = Draft202012Validator(schema)
    errors = sorted(v.iter_errors(obj), key=lambda e: e.path)
    out: List[str] = []
    for e in errors:
        path = ".".join(str(p) for p in e.path) if e.path else "<root>"
        out.append(f"schema: {path}: {e.message}")
    return out


def _resolve_rel(index_path: Path, rel: str) -> Path:
    rel = (rel or "").replace("\\", "/")
    return (index_path.parent / rel).resolve()


def _semantic_errors(obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []

    measures = obj.get("measures")
    if not isinstance(measures, list) or not measures:
        return ["measures must be a non-empty array"]

    for mi, m in enumerate(measures):
        if not isinstance(m, dict):
            errs.append(f"measures[{mi}] must be an object")
            continue
        measure = m.get("measure")
        if not isinstance(measure, str) or not measure:
            errs.append(f"measures[{mi}].measure must be a non-empty string")

        bands = m.get("bands")
        matrices = m.get("matrices")
        if not isinstance(bands, list) or not bands:
            errs.append(f"measures[{mi}].bands must be a non-empty array")
        if not isinstance(matrices, list) or not matrices:
            errs.append(f"measures[{mi}].matrices must be a non-empty array")
        if isinstance(bands, list) and isinstance(matrices, list) and len(bands) != len(matrices):
            errs.append(
                f"measures[{mi}] bands/matrices length mismatch: {len(bands)} vs {len(matrices)}"
            )

        # basic check for each matrix record
        if isinstance(matrices, list):
            for ji, rec in enumerate(matrices):
                if not isinstance(rec, dict):
                    errs.append(f"measures[{mi}].matrices[{ji}] must be an object")
                    continue
                if not isinstance(rec.get("matrix_csv"), str) or not rec.get("matrix_csv"):
                    errs.append(f"measures[{mi}].matrices[{ji}].matrix_csv must be a non-empty string")

    return errs


def _check_files_errors(index_path: Path, obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []

    def _get_str(k: str) -> Optional[str]:
        v = obj.get(k)
        return v if isinstance(v, str) and v else None

    required = ["run_meta_json"]
    for k in required:
        rel = _get_str(k)
        if rel is None:
            errs.append(f"missing or invalid '{k}' path")
            continue
        p = _resolve_rel(index_path, rel)
        if not p.exists():
            errs.append(f"missing file: {k} -> {p}")

    proto = obj.get("protocol_json")
    if isinstance(proto, str) and proto:
        p = _resolve_rel(index_path, proto)
        if not p.exists():
            errs.append(f"missing file: protocol_json -> {p}")

    measures = obj.get("measures")
    if isinstance(measures, list):
        for mi, m in enumerate(measures):
            if not isinstance(m, dict):
                continue
            matrices = m.get("matrices")
            if not isinstance(matrices, list):
                continue
            for ji, rec in enumerate(matrices):
                if not isinstance(rec, dict):
                    continue
                rel = rec.get("matrix_csv")
                if isinstance(rel, str) and rel:
                    p = _resolve_rel(index_path, rel)
                    if not p.exists():
                        errs.append(f"missing matrix file: measures[{mi}].matrices[{ji}] -> {p}")

    return errs


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("index_json", nargs="+", help="Path(s) to loreta_connectivity_index.json")
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
        print("OK: loreta connectivity index is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
