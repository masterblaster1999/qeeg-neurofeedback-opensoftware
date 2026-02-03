#!/usr/bin/env python3
"""Validate LORETA protocol candidate JSON (loreta_protocol.json).

Produced by:
  qeeg_loreta_metrics_cli --protocol-json

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
import math
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _read_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _schema_path(schemas_dir: Optional[Path]) -> Optional[Path]:
    if schemas_dir is None:
        return None
    p = schemas_dir / "qeeg_loreta_protocol.schema.json"
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


def _resolve_rel(base: Path, rel: str) -> Path:
    rel = (rel or "").replace("\\", "/")
    return (base.parent / rel).resolve()


def _semantic_errors(obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []
    targets = obj.get("targets")
    if not isinstance(targets, list):
        return ["targets must be an array"]
    last_abs: Optional[float] = None
    for i, t in enumerate(targets):
        if not isinstance(t, dict):
            errs.append(f"targets[{i}] must be an object")
            continue
        rank = t.get("rank")
        if rank != i + 1:
            errs.append(f"targets[{i}].rank must equal {i+1} (got {rank})")
        v = t.get("value")
        av = t.get("abs_value")
        if isinstance(v, (int, float)) and isinstance(av, (int, float)):
            if not math.isfinite(v) or not math.isfinite(av):
                errs.append(f"targets[{i}] contains non-finite numbers")
            else:
                if abs(abs(float(v)) - float(av)) > 1e-9:
                    errs.append(f"targets[{i}] abs_value != abs(value)")
                if last_abs is not None and float(av) > last_abs + 1e-12:
                    errs.append(f"targets not sorted by abs_value (descending) at i={i}")
                last_abs = float(av)
        else:
            errs.append(f"targets[{i}] value/abs_value must be numbers")

        value_kind = t.get("value_kind")
        sugg = t.get("suggested_direction")
        if value_kind == "raw" and sugg is not None:
            errs.append(f"targets[{i}]: suggested_direction must be null for raw metrics")

    return errs


def _check_files_errors(path: Path, obj: Dict[str, Any]) -> List[str]:
    errs: List[str] = []
    idx = obj.get("metrics_index_json")
    if isinstance(idx, str) and idx:
        p = _resolve_rel(path, idx)
        if not p.exists():
            errs.append(f"missing metrics_index_json: {p}")
    return errs


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("protocol_json", nargs="+", help="Path(s) to loreta_protocol.json")
    ap.add_argument("--schemas-dir", default=None, help="Directory containing JSON schemas (default: repo/schemas)")
    ap.add_argument("--check-files", action="store_true", help="Verify referenced files exist")
    ap.add_argument("--verbose", action="store_true")
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

    for f in args.protocol_json:
        p = Path(f).resolve()
        if not p.exists():
            any_errors.append((str(p), "missing"))
            continue
        try:
            obj0 = _read_json(p)
        except Exception as e:
            any_errors.append((str(p), f"read: {e}"))
            continue
        if not isinstance(obj0, dict):
            any_errors.append((str(p), "protocol JSON must be an object"))
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
        print("OK: loreta protocol JSON is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
