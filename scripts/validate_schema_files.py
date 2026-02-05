#!/usr/bin/env python3
"""Validate the JSON Schema files shipped in schemas/.

This repository publishes a set of Draft 2020-12 JSON Schemas under
schemas/*.schema.json.

This validator is intended for:
  - CI: ensure schema files remain valid JSON Schema and keep stable identifiers.
  - Developers: quick sanity checks after editing schema files.

It performs three layers of validation:
  1) Parse each schema as JSON.
  2) Lightweight stdlib checks for repo conventions:
       - $schema is Draft 2020-12
       - $id is a non-empty string and ends with the schema filename
       - type exists and is a string
       - optional properties.$schema.const matches top-level $id
       - $id values are unique across schemas/
  3) $ref integrity checks (stdlib):
       - local refs ("#/<json-pointer>") resolve to an existing location
       - cross-document refs resolve to a known schema (by $id or filename)

When python-jsonschema is available, it also runs Draft2020-12
Draft202012Validator.check_schema() on each schema.

Usage:
  python3 scripts/validate_schema_files.py
  python3 scripts/validate_schema_files.py --schemas-dir ./schemas --verbose
  python3 scripts/validate_schema_files.py --require-jsonschema
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple
from urllib.parse import unquote

# Avoid writing .pyc / __pycache__ during CI/CTest runs.
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
sys.dont_write_bytecode = True

_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"


def _load_json(path: Path) -> Any:
    with open(str(path), "r", encoding="utf-8") as f:
        return json.load(f)


def _try_get_draft202012_validator() -> Optional[Any]:
    """Return jsonschema.Draft202012Validator if available, else None."""

    try:
        import jsonschema  # type: ignore
    except Exception:
        return None

    v = getattr(jsonschema, "Draft202012Validator", None)
    if v is None:
        return None

    if not hasattr(v, "check_schema"):
        return None

    return v


def _json_pointer_unescape(token: str) -> str:
    # RFC 6901 JSON Pointer escaping.
    return token.replace("~1", "/").replace("~0", "~")


def _resolve_json_pointer(doc: Any, pointer: str, *, context: str) -> Any:
    """Resolve a JSON Pointer within *doc*.

    pointer is the fragment part without the leading '#'. Examples:
      - "" (empty) references the root document.
      - "/$defs/column" references doc["$defs"]["column"]

    Raises AssertionError on invalid pointers.
    """

    if pointer == "":
        return doc

    if not pointer.startswith("/"):
        raise AssertionError(f"Unsupported JSON Pointer (expected leading '/'): {pointer!r} ({context})")

    cur: Any = doc
    for raw in pointer.lstrip("/").split("/"):
        tok = _json_pointer_unescape(raw)

        if isinstance(cur, dict):
            if tok not in cur:
                raise AssertionError(f"JSON Pointer token not found: {tok!r} in {pointer!r} ({context})")
            cur = cur[tok]
        elif isinstance(cur, list):
            try:
                idx = int(tok)
            except Exception:
                raise AssertionError(f"Expected array index token, got {tok!r} in {pointer!r} ({context})")
            if idx < 0 or idx >= len(cur):
                raise AssertionError(f"Array index out of range: {idx} in {pointer!r} ({context})")
            cur = cur[idx]
        else:
            raise AssertionError(
                f"JSON Pointer traversed into non-container at token {tok!r} in {pointer!r} ({context})"
            )

    return cur


def _format_json_path(path: List[Any]) -> str:
    if not path:
        return "$"
    out = "$"
    for p in path:
        if isinstance(p, int):
            out += f"[{p}]"
        else:
            out += f".{p}"
    return out


class _SchemaStore:
    def __init__(self) -> None:
        self.by_id: Dict[str, Dict[str, Any]] = {}
        self.by_name: Dict[str, Dict[str, Any]] = {}

    def add(self, schema_path: Path, schema: Dict[str, Any]) -> None:
        sid = schema.get("$id")
        if isinstance(sid, str) and sid:
            self.by_id[sid] = schema
        self.by_name[schema_path.name] = schema

    def resolve_base(self, base: str) -> Optional[Dict[str, Any]]:
        """Resolve a $ref base (without fragment) to a schema document."""

        b = base.strip()
        if not b:
            return None

        # Exact match (common when base is a canonical $id).
        if b in self.by_id:
            return self.by_id[b]

        # Common relative form.
        if b.startswith("./") and b[2:] in self.by_id:
            return self.by_id[b[2:]]

        # Filename match.
        name = os.path.basename(b)
        if name in self.by_name:
            return self.by_name[name]

        return None


def _basic_schema_checks(schema: Dict[str, Any], *, path: Path) -> List[str]:
    errs: List[str] = []

    if schema.get("$schema") != _DRAFT_2020_12:
        errs.append(f"$schema must be '{_DRAFT_2020_12}'")

    sid = schema.get("$id")
    if not isinstance(sid, str) or not sid:
        errs.append("$id must be a non-empty string")
    else:
        if not sid.endswith(path.name):
            errs.append(f"$id should end with '{path.name}'")

    stype = schema.get("type")
    if stype is None:
        errs.append("missing required key: type")
    elif not isinstance(stype, str):
        errs.append("type must be a string")

    props = schema.get("properties")
    if isinstance(props, dict) and "$schema" in props:
        sch_prop = props.get("$schema")
        if isinstance(sch_prop, dict):
            const = sch_prop.get("const")
            if isinstance(const, str) and isinstance(sid, str) and const != sid:
                errs.append("properties.$schema.const should match top-level $id")

    return errs


def _walk_refs(node: Any, path: List[Any], out: List[Tuple[List[Any], Any]]) -> None:
    if isinstance(node, dict):
        if "$ref" in node:
            out.append((path + ["$ref"], node.get("$ref")))
        for k, v in node.items():
            if k == "$ref":
                continue
            _walk_refs(v, path + [k], out)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            _walk_refs(v, path + [i], out)


def _defrag_ref(ref: str) -> Tuple[str, str]:
    base, sep, frag = ref.partition("#")
    if sep == "":
        return ref, ""
    return base, frag


def _check_ref(
    ref_value: Any,
    *,
    root_schema: Dict[str, Any],
    store: _SchemaStore,
    context: str,
) -> Optional[str]:
    if not isinstance(ref_value, str) or not ref_value:
        return "invalid $ref (expected non-empty string)"

    if ref_value.startswith("#"):
        frag = unquote(ref_value[1:])
        try:
            _resolve_json_pointer(root_schema, frag, context=context)
        except AssertionError as e:
            return str(e)
        return None

    base, frag = _defrag_ref(ref_value)
    base = base.strip()
    frag = unquote(frag)

    doc = store.resolve_base(base)
    if doc is None:
        return f"unresolved $ref base: {base!r}"

    # Empty fragment references the document root.
    if frag == "":
        return None

    if not frag.startswith("/"):
        return f"unsupported $ref fragment (expected JSON Pointer): {frag!r}"

    try:
        _resolve_json_pointer(doc, frag, context=context)
    except AssertionError as e:
        return str(e)

    return None


def _validate_one(
    schema_path: Path,
    *,
    schema: Dict[str, Any],
    store: _SchemaStore,
    draft_validator: Optional[Any],
    use_jsonschema: bool,
    check_refs: bool,
) -> List[str]:
    errs: List[str] = []

    errs.extend(_basic_schema_checks(schema, path=schema_path))

    if use_jsonschema and draft_validator is not None:
        try:
            draft_validator.check_schema(schema)
        except Exception as e:
            errs.append(f"jsonschema check_schema failed: {e}")

    if check_refs:
        refs: List[Tuple[List[Any], Any]] = []
        _walk_refs(schema, [], refs)
        for ref_path, ref_value in refs:
            ctx = f"{schema_path.name}:{_format_json_path(ref_path)}"
            msg = _check_ref(ref_value, root_schema=schema, store=store, context=ctx)
            if msg:
                errs.append(f"$ref error at {_format_json_path(ref_path)}: {msg}")

    return errs


def _discover_schema_files(schemas_dir: Path) -> List[Path]:
    return sorted(schemas_dir.glob("*.schema.json"))


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Validate JSON Schema files in schemas/.")
    ap.add_argument(
        "--schemas-dir",
        default=None,
        help="Directory containing *.schema.json files (default: <repo>/schemas).",
    )
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="Print each validated schema path.",
    )
    ap.add_argument(
        "--no-jsonschema",
        action="store_true",
        help="Force stdlib-only validation (skip python-jsonschema even if installed).",
    )
    ap.add_argument(
        "--require-jsonschema",
        action="store_true",
        help="Fail if python-jsonschema Draft202012Validator is not available.",
    )
    ap.add_argument(
        "--no-ref-check",
        action="store_true",
        help="Skip stdlib $ref integrity checks.",
    )

    args = ap.parse_args(list(argv) if argv is not None else None)

    here = Path(__file__).resolve()
    default_schemas = here.parent.parent / "schemas"
    schemas_dir = Path(args.schemas_dir) if args.schemas_dir else default_schemas
    schemas_dir = schemas_dir.resolve()

    if not schemas_dir.is_dir():
        print(f"ERROR: schemas directory not found: {schemas_dir}", file=sys.stderr)
        return 2

    schema_files = _discover_schema_files(schemas_dir)
    if not schema_files:
        print(f"ERROR: no *.schema.json files found in: {schemas_dir}", file=sys.stderr)
        return 2

    draft_validator = _try_get_draft202012_validator()
    if args.require_jsonschema and draft_validator is None:
        print("ERROR: python-jsonschema Draft202012Validator is required but not available", file=sys.stderr)
        return 2

    use_jsonschema = (not args.no_jsonschema) and (draft_validator is not None)
    check_refs = not bool(args.no_ref_check)

    # Load all schemas once.
    store = _SchemaStore()
    schemas: Dict[Path, Dict[str, Any]] = {}

    ok = True
    for p in schema_files:
        try:
            data = _load_json(p)
        except Exception as e:
            ok = False
            print(f"ERROR: {p.name}: invalid JSON: {e}", file=sys.stderr)
            continue

        if not isinstance(data, dict):
            ok = False
            print(f"ERROR: {p.name}: schema root must be a JSON object", file=sys.stderr)
            continue

        schemas[p] = data
        store.add(p, data)

    # Ensure $id uniqueness (load-only step above already stored by_id).
    ids_seen: Dict[str, Path] = {}
    for p, schema in schemas.items():
        sid = schema.get("$id")
        if isinstance(sid, str) and sid:
            prev = ids_seen.get(sid)
            if prev is not None and prev != p:
                ok = False
                print(f"ERROR: duplicate $id: {sid} (files: {prev.name} and {p.name})", file=sys.stderr)
            else:
                ids_seen[sid] = p

    # Validate each schema.
    for p in schema_files:
        if p not in schemas:
            continue
        if args.verbose:
            print(f"[schema] {p}")

        errs = _validate_one(
            p,
            schema=schemas[p],
            store=store,
            draft_validator=draft_validator,
            use_jsonschema=use_jsonschema,
            check_refs=check_refs,
        )
        if errs:
            ok = False
            for e in errs:
                print(f"ERROR: {p.name}: {e}", file=sys.stderr)

    mode = "jsonschema Draft 2020-12" if use_jsonschema else "stdlib-only"
    if check_refs:
        mode += "+$ref"

    if ok:
        print(f"OK: validated {len(schema_files)} schema files ({mode})")
    else:
        print(f"FAILED: schema validation errors detected ({mode})", file=sys.stderr)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
