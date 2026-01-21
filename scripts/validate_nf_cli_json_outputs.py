#!/usr/bin/env python3
"""Validate qeeg_nf_cli JSON outputs against the repo's JSON Schemas.

This script is intended for:
  - CI: catching accidental breaking changes to machine-readable outputs.
  - Developers: quickly sanity-checking JSON outputs while iterating.

It validates three categories of outputs:
  1) JSON printed to STDOUT by qeeg_nf_cli listing/introspection flags
     (e.g. --version-json, --list-*-json, --print-config-json).
  2) JSON files written under --outdir by a short demo run
     (nf_run_meta.json, nf_summary.json, and nf_derived_events.json), or an
     existing outdir passed via --validate-outdir.
  3) When derived events are exported, it also performs lightweight checks on
     nf_derived_events.tsv and nf_derived_events.csv:
       - required columns exist
       - onset/duration fields are finite numbers
       - basic consistency checks between the TSV/CSV and the JSON sidecar

It prefers full JSON Schema validation using the python-jsonschema package
(Draft 2020-12 validator) when available.

If python-jsonschema is not installed (or is too old to expose a Draft 2020-12
validator), it falls back to a minimal check:
  - JSON parses
  - best-effort structural checks based on common keywords used in this repo's schemas
    (objects with required keys, arrays with typed items, basic numeric bounds)

In minimal mode, it supports:
  - local $ref references (e.g. "#/$defs/...")
  - cross-document $ref references when the referenced schema is available in the
    repo's schemas/ directory (by canonical $id or by filename).

When full validation is available, the script also builds an in-memory
"referencing" registry of all repo schemas (keyed by their $id) and passes it
into Draft202012Validator. This keeps validation deterministic and avoids any
network retrieval for $ref resolution.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Sequence, Tuple
from urllib.parse import unquote


# Best-effort: avoid polluting the source tree with __pycache__ when this script
# is run from CTest/CI. (CTests run from the build tree but may execute this
# source-tree script directly.)
sys.dont_write_bytecode = True


def _cleanup_stale_pycache() -> None:
    """Remove stale bytecode caches for this script.

    Earlier patch zips accidentally included a scripts/__pycache__/*.pyc file.
    Keeping it around is typically harmless, but it can be confusing, and it
    makes diffs / packaging noisier.

    This cleanup is best-effort and only deletes cache files for this script
    (not other Python scripts).
    """

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            # Typical filename:
            #   validate_nf_cli_json_outputs.cpython-311.pyc
            if fn.startswith("validate_nf_cli_json_outputs.") and fn.endswith(".pyc"):
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


def _json_pointer_unescape(token: str) -> str:
    # RFC 6901 JSON Pointer escaping.
    return token.replace("~1", "/").replace("~0", "~")


def _resolve_json_pointer(doc: Any, pointer: str, *, context: str) -> Any:
    """Resolve a JSON Pointer within *doc*.

    pointer is the fragment part *without* the leading '#'. Examples:
      - "" (empty) references the root document.
      - "/$defs/column" references doc["$defs"]["column"]

    Note: When JSON Pointer is used as a URI fragment identifier, the pointer
    may be percent-encoded. Callers should percent-decode before calling this
    function when appropriate.
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


def _resolve_local_ref(root_schema: Dict[str, Any], ref: str, *, context: str) -> Optional[Any]:
    """Resolve a local JSON Schema $ref.

    Only supports in-document refs ("#..."), which is what most of this repo's
    schemas use.
    """
    if not ref.startswith("#"):
        return None
    frag = ref[1:]  # may be empty (root) or a JSON Pointer
    return _resolve_json_pointer(root_schema, frag, context=context)


def _defrag_ref(ref: str) -> Tuple[str, str]:
    """Split a $ref into (base, fragment) without the leading '#'."""
    base, sep, frag = ref.partition("#")
    if sep == "":
        return ref, ""
    return base, frag


def _resolve_ref(
    root_schema: Dict[str, Any],
    ref: str,
    *,
    schema_registry: Optional[Dict[str, Dict[str, Any]]] = None,
    context: str,
) -> Optional[Tuple[Any, Dict[str, Any]]]:
    """Resolve a $ref to a subschema.

    Supports:
      - local refs ("#..."), resolved within *root_schema*
      - cross-document refs ("<uri-or-filename>#..."), resolved using *schema_registry*

    Returns (resolved_subschema, resolved_root_schema) on success.
    """
    if ref.startswith("#"):
        resolved = _resolve_local_ref(root_schema, ref, context=context)
        if resolved is None:
            return None
        return resolved, root_schema

    base, frag = _defrag_ref(ref)
    base = base.strip()
    frag = unquote(frag)  # fragments in URIs may be percent-encoded

    if schema_registry is None:
        return None

    # Exact match (common when base is a canonical $id).
    doc = schema_registry.get(base)

    # Common relative forms.
    if doc is None and base.startswith("./"):
        doc = schema_registry.get(base[2:])

    # Last-path-segment fallback (works for both file paths and URLs).
    if doc is None:
        doc = schema_registry.get(os.path.basename(base))

    if doc is None:
        return None

    if not isinstance(doc, dict):
        return None

    resolved = _resolve_json_pointer(doc, frag, context=context)
    return resolved, doc


def _run(cmd: Sequence[str]) -> str:
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        raise RuntimeError(
            "Command failed (exit code %d): %s\n\nSTDOUT:\n%s\n\nSTDERR:\n%s"
            % (res.returncode, " ".join(cmd), res.stdout, res.stderr)
        )
    return res.stdout


def _json_from_stdout(stdout: str) -> Any:
    s = stdout.strip()
    try:
        return json.loads(s)
    except json.JSONDecodeError:
        # Best-effort salvage in case a tool ever prepends non-JSON text.
        for start in ("{", "["):
            idx = s.find(start)
            if idx >= 0:
                try:
                    return json.loads(s[idx:])
                except Exception:
                    pass
        raise


def _load_json_file(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _load_all_schemas(schemas_dir: str) -> Tuple[Dict[str, Dict[str, Any]], Dict[str, Dict[str, Any]]]:
    """Load all *.schema.json files under *schemas_dir*.

    Returns:
      - schemas_by_id: mapping from canonical $id to schema document
      - registry: mapping that can be used to resolve cross-document $ref targets
        (includes both canonical $id and filename aliases).

    The script treats missing/duplicate $id values as errors because:
      - the project emits $schema hints pointing at schema $id values
      - reference resolution relies on stable identifiers
    """

    schemas_by_id: Dict[str, Dict[str, Any]] = {}
    registry: Dict[str, Dict[str, Any]] = {}

    if not os.path.isdir(schemas_dir):
        raise AssertionError(f"schemas-dir is not a directory: {schemas_dir}")

    for fn in sorted(os.listdir(schemas_dir)):
        if not fn.endswith(".schema.json"):
            continue
        path = os.path.join(schemas_dir, fn)
        schema = _load_json_file(path)
        if not isinstance(schema, dict):
            raise AssertionError(f"Schema is not an object: {path}")

        schema_id = schema.get("$id")
        if not isinstance(schema_id, str) or not schema_id.strip():
            raise AssertionError(f"Schema file is missing required $id: {path}")

        if schema_id in schemas_by_id:
            raise AssertionError(f"Duplicate schema $id {schema_id!r} found in: {path}")

        schemas_by_id[schema_id] = schema

        # Canonical identifier.
        registry[schema_id] = schema

        # Common alias: filename.
        registry[fn] = schema

        # Common alias: relative path under schemas/ (useful if a $ref uses a
        # relative file path like "schemas/foo.schema.json").
        registry[f"schemas/{fn}"] = schema

    return schemas_by_id, registry


def _check_type(value: Any, expected: Any) -> bool:
    if isinstance(expected, list):
        return any(_check_type(value, t) for t in expected)

    t = expected
    if t == "string":
        return isinstance(value, str)
    if t == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "boolean":
        return isinstance(value, bool)
    if t == "object":
        return isinstance(value, dict)
    if t == "array":
        return isinstance(value, list)
    if t == "null":
        return value is None
    # Unknown type: don't fail hard.
    return True


def _minimal_validate(
    schema: Any,
    instance: Any,
    path: str = "$",
    *,
    root_schema: Optional[Dict[str, Any]] = None,
    schema_registry: Optional[Dict[str, Dict[str, Any]]] = None,
    _ref_stack: Optional[set] = None,
) -> None:
    """Best-effort structural validation when python-jsonschema isn't available.

    This is intentionally *not* a full JSON Schema implementation. It aims to catch
    common breakages in CI on platforms where the optional dependency isn't installed.

    Supported (best-effort) keywords:
      - $ref (local refs and cross-document refs), $defs
      - type, required, properties
      - additionalProperties (bool or schema)
      - items / prefixItems for arrays
      - const / enum
      - minimum / maximum (numbers)
      - allOf / anyOf / oneOf (shallow)

    Unsupported keywords are ignored rather than treated as errors.
    """

    # Establish the root schema for local $ref resolution.
    if root_schema is None and isinstance(schema, dict):
        root_schema = schema

    if _ref_stack is None:
        _ref_stack = set()

    # JSON Schema allows boolean schemas.
    if schema is True:
        return
    if schema is False:
        raise AssertionError(f"Schema is 'false' at {path} (instance is always invalid)")

    if not isinstance(schema, dict):
        # Unknown schema representation; don't fail hard.
        return

    # Local + cross-document $ref support (best-effort).
    if "$ref" in schema and isinstance(schema["$ref"], str):
        ref = schema["$ref"]

        # Prevent infinite recursion on cyclic references.
        # Note: refs are commonly reused across multiple properties, so this must
        # behave like a *stack* (only block refs already in the current chain).
        if ref in _ref_stack:
            return

        if root_schema is not None:
            _ref_stack.add(ref)
            try:
                resolved = _resolve_ref(
                    root_schema,
                    ref,
                    schema_registry=schema_registry,
                    context=f"{path} ($ref)",
                )
                if resolved is not None:
                    resolved_schema, resolved_root = resolved
                    _minimal_validate(
                        resolved_schema,
                        instance,
                        path=path,
                        root_schema=resolved_root,
                        schema_registry=schema_registry,
                        _ref_stack=_ref_stack,
                    )
                else:
                    raise AssertionError(
                        f"Unable to resolve $ref {ref!r} at {path}. "
                        "Install the 'jsonschema' package for full Draft 2020-12 validation."
                    )
            finally:
                _ref_stack.remove(ref)

        # Draft 2019-09+ treats $ref as an applicator; sibling keywords may still
        # apply. For minimal validation, validate the resolved ref *and* continue
        # with sibling keywords (excluding $ref itself).
        schema = {k: v for (k, v) in schema.items() if k != "$ref"}
        if not schema:
            return

    # Shallow combinators (rare in this repo, but inexpensive to support).
    if "allOf" in schema and isinstance(schema["allOf"], list):
        for i, sub in enumerate(schema["allOf"]):
            _minimal_validate(
                sub,
                instance,
                path=f"{path}.allOf[{i}]",
                root_schema=root_schema,
                schema_registry=schema_registry,
                _ref_stack=_ref_stack,
            )

    if "anyOf" in schema and isinstance(schema["anyOf"], list):
        ok = False
        last_err: Optional[BaseException] = None
        for i, sub in enumerate(schema["anyOf"]):
            try:
                _minimal_validate(
                    sub,
                    instance,
                    path=f"{path}.anyOf[{i}]",
                    root_schema=root_schema,
                    schema_registry=schema_registry,
                    _ref_stack=_ref_stack,
                )
                ok = True
                break
            except BaseException as e:
                last_err = e
        if not ok:
            raise AssertionError(f"anyOf failed at {path}: {last_err}")
        # Don't return early: sibling keywords may still apply.

    if "oneOf" in schema and isinstance(schema["oneOf"], list):
        n_ok = 0
        last_err: Optional[BaseException] = None
        for i, sub in enumerate(schema["oneOf"]):
            try:
                _minimal_validate(
                    sub,
                    instance,
                    path=f"{path}.oneOf[{i}]",
                    root_schema=root_schema,
                    schema_registry=schema_registry,
                    _ref_stack=_ref_stack,
                )
                n_ok += 1
            except BaseException as e:
                last_err = e
        if n_ok != 1:
            raise AssertionError(f"oneOf failed at {path}: matched {n_ok} subschemas (last error: {last_err})")
        # Don't return early: sibling keywords may still apply.

    if "const" in schema:
        if instance != schema["const"]:
            raise AssertionError(f"const mismatch at {path}: expected {schema['const']!r}, got {instance!r}")

    if "enum" in schema and isinstance(schema["enum"], list):
        if instance not in schema["enum"]:
            raise AssertionError(
                f"enum mismatch at {path}: expected one of {schema['enum']!r}, got {instance!r}"
            )

    expected_type = schema.get("type")
    if expected_type is not None and not _check_type(instance, expected_type):
        raise AssertionError(f"Type mismatch at {path}: expected {expected_type}, got {type(instance).__name__}")

    # Numeric bounds.
    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema:
            try:
                if instance < float(schema["minimum"]):
                    raise AssertionError(f"minimum violation at {path}: {instance} < {schema['minimum']}")
            except Exception:
                pass
        if "maximum" in schema:
            try:
                if instance > float(schema["maximum"]):
                    raise AssertionError(f"maximum violation at {path}: {instance} > {schema['maximum']}")
            except Exception:
                pass

    # Object recursion.
    if isinstance(instance, dict):
        required = schema.get("required", [])
        if isinstance(required, list):
            for k in required:
                if k not in instance:
                    raise AssertionError(f"Missing required key at {path}: {k}")

        props = schema.get("properties", {})
        if not isinstance(props, dict):
            props = {}

        additional = schema.get("additionalProperties", True)
        # Validate declared properties.
        for k, subschema in props.items():
            if k in instance:
                _minimal_validate(
                    subschema,
                    instance.get(k),
                    path=f"{path}.{k}",
                    root_schema=root_schema,
                    schema_registry=schema_registry,
                    _ref_stack=_ref_stack,
                )

        unknown_keys = [k for k in instance.keys() if k not in props]
        if additional is False:
            if unknown_keys:
                raise AssertionError(f"Unexpected keys at {path}: {unknown_keys}")
        elif isinstance(additional, dict) or additional is True:
            if isinstance(additional, dict):
                for k in unknown_keys:
                    _minimal_validate(
                        additional,
                        instance.get(k),
                        path=f"{path}.{k}",
                        root_schema=root_schema,
                        schema_registry=schema_registry,
                        _ref_stack=_ref_stack,
                    )

    # Array recursion.
    if isinstance(instance, list):
        if "minItems" in schema:
            try:
                if len(instance) < int(schema["minItems"]):
                    raise AssertionError(f"minItems violation at {path}: {len(instance)} < {schema['minItems']}")
            except Exception:
                pass
        if "maxItems" in schema:
            try:
                if len(instance) > int(schema["maxItems"]):
                    raise AssertionError(f"maxItems violation at {path}: {len(instance)} > {schema['maxItems']}")
            except Exception:
                pass

        # Draft 2020-12 tuple validation uses prefixItems + items.
        prefix = schema.get("prefixItems")
        if isinstance(prefix, list):
            for i, subschema in enumerate(prefix):
                if i >= len(instance):
                    break
                _minimal_validate(
                    subschema,
                    instance[i],
                    path=f"{path}[{i}]",
                    root_schema=root_schema,
                    schema_registry=schema_registry,
                    _ref_stack=_ref_stack,
                )

        items_schema = schema.get("items")
        if isinstance(items_schema, dict):
            start = len(prefix) if isinstance(prefix, list) else 0
            for i in range(start, len(instance)):
                _minimal_validate(
                    items_schema,
                    instance[i],
                    path=f"{path}[{i}]",
                    root_schema=root_schema,
                    schema_registry=schema_registry,
                    _ref_stack=_ref_stack,
                )


def _check_schema_hint(schema: Dict[str, Any], instance: Any, schema_name: str) -> None:
    """Best-effort verification of a '$schema' hint in the JSON instance.

    Many editors (e.g. VS Code) can associate a JSON document with a schema when the
    document contains a top-level '$schema' property. When full JSON Schema validation
    is unavailable (minimal mode), we still want to catch accidental mismatches.

    This check is only applied to object-shaped outputs.
    """
    schema_id = schema.get("$id")
    if not schema_id:
        raise AssertionError(f"Schema is missing required $id: {schema_name}")

    if not isinstance(instance, dict):
        return

    if "$schema" not in instance:
        # Older outputs may not include a hint. Don't fail hard.
        return

    if instance["$schema"] != schema_id:
        raise AssertionError(
            f"$schema mismatch for {schema_name}: got {instance['$schema']!r}, expected {schema_id!r}"
        )


def _try_build_referencing_registry(schemas_by_id: Dict[str, Dict[str, Any]]) -> Optional[Any]:
    """Build an in-memory referencing.Registry (if available).

    This allows Draft202012Validator to resolve $ref targets without network access,
    as long as the referenced schemas are present in schemas_by_id.
    """
    try:
        from referencing import Registry, Resource  # type: ignore
    except Exception:
        return None

    resources: List[Tuple[str, Any]] = []
    for uri, schema in schemas_by_id.items():
        try:
            res = Resource.from_contents(schema)
        except Exception:
            # Fallback: force Draft2020-12 if $schema is missing/unrecognized.
            try:
                from referencing.jsonschema import DRAFT202012  # type: ignore

                res = DRAFT202012.create_resource(schema)
            except Exception:
                return None
        resources.append((uri, res))

    try:
        return Registry().with_resources(resources)
    except Exception:
        # Older referencing versions may not have with_resources.
        reg = Registry()
        for uri, res in resources:
            reg = reg.with_resource(uri=uri, resource=res)
        return reg


def _try_full_validate(schema: Dict[str, Any], instance: Any, *, registry: Optional[Any] = None) -> Optional[str]:
    """Return a string describing the validator used, or None if unavailable."""
    try:
        import jsonschema  # type: ignore

        # Draft202012Validator exists on jsonschema>=4.
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return None

    kwargs: Dict[str, Any] = {}
    if registry is not None:
        kwargs["registry"] = registry

    try:
        Draft202012Validator(schema, **kwargs).validate(instance)
        return "jsonschema.Draft202012Validator" + ("+registry" if registry is not None else "")
    except TypeError:
        # Some jsonschema versions expose Draft202012Validator but may not
        # accept the newer "registry=" constructor argument. Fall back to
        # default resolution behavior.
        if "registry" in kwargs:
            Draft202012Validator(schema).validate(instance)
            return "jsonschema.Draft202012Validator"
        raise


def _validate_instance(
    schema_path: str,
    schema_name: str,
    instance: Any,
    *,
    schema_registry: Optional[Dict[str, Dict[str, Any]]] = None,
    full_registry: Optional[Any] = None,
) -> str:
    schema = _load_json_file(schema_path)
    _check_schema_hint(schema, instance, schema_name)

    used = _try_full_validate(schema, instance, registry=full_registry)
    if used is None:
        _minimal_validate(schema, instance, schema_registry=schema_registry)
        used = "minimal"
    return used


def _read_delimited_table(path: str, delimiter: str) -> Tuple[List[str], List[Dict[str, str]]]:
    with open(path, "r", encoding="utf-8", newline="") as f:
        dr = csv.DictReader(f, delimiter=delimiter)
        if dr.fieldnames is None:
            raise AssertionError(f"Expected header row in: {path}")
        fieldnames = [h.strip() for h in dr.fieldnames]
        rows: List[Dict[str, str]] = []
        for r in dr:
            # Normalize None values to empty strings.
            rows.append({(k or "").strip(): (v or "").strip() for k, v in r.items()})
        return fieldnames, rows


def _require_columns(path: str, fieldnames: Sequence[str], required: Sequence[str]) -> None:
    missing = [c for c in required if c not in fieldnames]
    if missing:
        raise AssertionError(f"Missing required columns in {path}: {missing}. Have: {list(fieldnames)}")


def _parse_float_cell(path: str, row_idx: int, col: str, value: str) -> float:
    try:
        x = float(value.strip())
    except Exception:
        raise AssertionError(f"Failed to parse float in {path} row {row_idx + 1} col '{col}': {value!r}")
    if not math.isfinite(x):
        raise AssertionError(f"Non-finite number in {path} row {row_idx + 1} col '{col}': {value!r}")
    return x


def _validate_events_tsv(path: str) -> List[Tuple[float, float, str]]:
    fieldnames, rows = _read_delimited_table(path, "\t")
    _require_columns(path, fieldnames, ["onset", "duration", "trial_type"])

    out: List[Tuple[float, float, str]] = []
    for i, r in enumerate(rows):
        onset = _parse_float_cell(path, i, "onset", r.get("onset", ""))
        duration = _parse_float_cell(path, i, "duration", r.get("duration", ""))
        trial_type = (r.get("trial_type") or "").strip()

        if onset < 0.0:
            raise AssertionError(f"Negative onset in {path} row {i + 1}: {onset}")
        if duration < 0.0:
            raise AssertionError(f"Negative duration in {path} row {i + 1}: {duration}")
        if trial_type == "":
            raise AssertionError(f"Empty trial_type in {path} row {i + 1}")

        out.append((onset, duration, trial_type))
    return out


def _validate_events_csv(path: str) -> List[Tuple[float, float, str]]:
    fieldnames, rows = _read_delimited_table(path, ",")
    _require_columns(path, fieldnames, ["onset_sec", "duration_sec", "text"])

    out: List[Tuple[float, float, str]] = []
    for i, r in enumerate(rows):
        onset = _parse_float_cell(path, i, "onset_sec", r.get("onset_sec", ""))
        duration = _parse_float_cell(path, i, "duration_sec", r.get("duration_sec", ""))
        text = (r.get("text") or "").strip()

        if onset < 0.0:
            raise AssertionError(f"Negative onset_sec in {path} row {i + 1}: {onset}")
        if duration < 0.0:
            raise AssertionError(f"Negative duration_sec in {path} row {i + 1}: {duration}")
        if text == "":
            raise AssertionError(f"Empty text in {path} row {i + 1}")

        out.append((onset, duration, text))
    return out


def _validate_trial_type_levels(sidecar_json: Any, trial_types: Sequence[str], sidecar_path: str) -> None:
    if not isinstance(sidecar_json, dict):
        return
    tt = sidecar_json.get("trial_type")
    if not isinstance(tt, dict):
        return
    levels = tt.get("Levels")
    if not isinstance(levels, dict):
        return

    missing = sorted({t for t in trial_types if t not in levels})
    if missing:
        raise AssertionError(
            "trial_type values present in nf_derived_events.tsv but missing from nf_derived_events.json Levels: "
            f"{missing}. Sidecar: {sidecar_path}"
        )


def _validate_derived_events_consistency(
    events_tsv: Sequence[Tuple[float, float, str]],
    events_csv: Sequence[Tuple[float, float, str]],
    tol: float = 1e-6,
) -> None:
    if len(events_tsv) != len(events_csv):
        raise AssertionError(
            f"nf_derived_events row count mismatch: TSV has {len(events_tsv)}, CSV has {len(events_csv)}"
        )

    for i, (a, b) in enumerate(zip(events_tsv, events_csv)):
        onset_a, dur_a, tt_a = a
        onset_b, dur_b, tt_b = b
        if abs(onset_a - onset_b) > tol:
            raise AssertionError(f"Row {i + 1} onset mismatch TSV vs CSV: {onset_a} vs {onset_b}")
        if abs(dur_a - dur_b) > tol:
            raise AssertionError(f"Row {i + 1} duration mismatch TSV vs CSV: {dur_a} vs {dur_b}")
        if tt_a != tt_b:
            raise AssertionError(f"Row {i + 1} label mismatch TSV vs CSV: {tt_a!r} vs {tt_b!r}")


def _try_full_check_schema(schema: Dict[str, Any]) -> Optional[str]:
    """Validate the *schema itself* using python-jsonschema, if available."""
    try:
        # Draft202012Validator exists on jsonschema>=4.
        from jsonschema import Draft202012Validator  # type: ignore
    except Exception:
        return None

    Draft202012Validator.check_schema(schema)
    return "jsonschema.Draft202012Validator.check_schema"


def _validate_stdout_json_outputs(
    nf_cli: str,
    schemas_dir: str,
    *,
    fs: int,
    seconds: int,
    verbose: bool,
    schema_registry: Optional[Dict[str, Dict[str, Any]]],
    full_registry: Optional[Any],
) -> bool:
    """Validate the machine-readable STDOUT JSON flags."""

    stdout_cases: List[Tuple[str, List[str]]] = [
        ("qeeg_nf_cli_version.schema.json", [nf_cli, "--version-json"]),
        ("qeeg_nf_cli_list_protocols.schema.json", [nf_cli, "--list-protocols-json"]),
        ("qeeg_nf_cli_list_bands.schema.json", [nf_cli, "--list-bands-json"]),
        ("qeeg_nf_cli_list_metrics.schema.json", [nf_cli, "--list-metrics-json"]),
        (
            "qeeg_nf_cli_list_channels.schema.json",
            [
                nf_cli,
                "--demo",
                "--fs",
                str(fs),
                "--seconds",
                str(seconds),
                "--list-channels-json",
            ],
        ),
        (
            "qeeg_nf_cli_print_config.schema.json",
            [
                nf_cli,
                "--demo",
                "--fs",
                str(fs),
                "--seconds",
                str(seconds),
                "--print-config-json",
            ],
        ),
    ]

    any_failed = False
    for schema_name, cmd in stdout_cases:
        schema_path = os.path.join(schemas_dir, schema_name)
        try:
            if not os.path.isfile(schema_path):
                raise AssertionError(f"schema file not found: {schema_path}")

            out = _run(cmd)
            instance = _json_from_stdout(out)
            used = _validate_instance(
                schema_path,
                schema_name,
                instance,
                schema_registry=schema_registry,
                full_registry=full_registry,
            )

            if verbose:
                print(f"OK: {schema_name} ({used})")
        except BaseException as e:
            print(f"ERROR: {schema_name}: {e}", file=sys.stderr)
            any_failed = True

    return any_failed


def _validate_outdir_reports(
    outdir: str,
    schemas_dir: str,
    *,
    require_derived_events: bool,
    verbose: bool,
    schema_registry: Optional[Dict[str, Dict[str, Any]]],
    full_registry: Optional[Any],
) -> bool:
    """Validate on-disk JSON reports written under --outdir."""

    if not os.path.isdir(outdir):
        print(f"ERROR: outdir is not a directory: {outdir}", file=sys.stderr)
        return True

    any_failed = False

    def validate_json(schema_name: str, filename: str, *, required: bool) -> Optional[Any]:
        nonlocal any_failed

        schema_path = os.path.join(schemas_dir, schema_name)
        json_path = os.path.join(outdir, filename)
        try:
            if not os.path.isfile(schema_path):
                raise AssertionError(f"schema file not found: {schema_path}")

            if not os.path.isfile(json_path):
                if required:
                    raise RuntimeError(f"Expected qeeg_nf_cli to write: {json_path}")
                return None

            instance = _load_json_file(json_path)
            used = _validate_instance(
                schema_path,
                schema_name,
                instance,
                schema_registry=schema_registry,
                full_registry=full_registry,
            )

            if verbose:
                print(f"OK: {schema_name} ({used})")
            return instance
        except BaseException as e:
            print(f"ERROR: {schema_name} ({json_path}): {e}", file=sys.stderr)
            any_failed = True
            return None

    # Required reports.
    validate_json("qeeg_nf_cli_nf_run_meta.schema.json", "nf_run_meta.json", required=True)
    validate_json("qeeg_nf_cli_nf_summary.schema.json", "nf_summary.json", required=True)

    # Derived events reports are optional unless explicitly required.
    p_tsv = os.path.join(outdir, "nf_derived_events.tsv")
    p_csv = os.path.join(outdir, "nf_derived_events.csv")
    p_json = os.path.join(outdir, "nf_derived_events.json")

    have_any = any(os.path.isfile(p) for p in (p_tsv, p_csv, p_json))
    if require_derived_events or have_any:
        if not os.path.isfile(p_tsv):
            print(f"ERROR: Expected derived events TSV: {p_tsv}", file=sys.stderr)
            any_failed = True
        if not os.path.isfile(p_csv):
            print(f"ERROR: Expected derived events CSV: {p_csv}", file=sys.stderr)
            any_failed = True
        if not os.path.isfile(p_json):
            print(f"ERROR: Expected derived events JSON sidecar: {p_json}", file=sys.stderr)
            any_failed = True

        sidecar_instance = validate_json(
            "qeeg_nf_cli_nf_derived_events.schema.json",
            "nf_derived_events.json",
            required=True,
        )

        if os.path.isfile(p_tsv) and os.path.isfile(p_csv) and os.path.isfile(p_json):
            try:
                events_tsv = _validate_events_tsv(p_tsv)
                events_csv = _validate_events_csv(p_csv)
                _validate_derived_events_consistency(events_tsv, events_csv)

                if sidecar_instance is None:
                    sidecar_instance = _load_json_file(p_json)
                _validate_trial_type_levels(sidecar_instance, [t[2] for t in events_tsv], p_json)

                if verbose:
                    print("OK: nf_derived_events.tsv/.csv (sanity + consistency)")
            except BaseException as e:
                print(f"ERROR: nf_derived_events.tsv/.csv: {e}", file=sys.stderr)
                any_failed = True

    return any_failed


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nf-cli", help="Path to the qeeg_nf_cli executable")
    ap.add_argument("--schemas-dir", required=True, help="Path to the repo's schemas/ directory")
    ap.add_argument("--fs", type=int, default=250, help="Demo sampling rate (Hz)")
    ap.add_argument("--seconds", type=int, default=1, help="Demo duration (seconds)")
    ap.add_argument(
        "--validate-outdir",
        default=None,
        help=(
            "If set, validate on-disk JSON outputs under an existing --outdir. "
            "This validation runs in addition to the demo run unless --skip-demo is used."
        ),
    )
    ap.add_argument(
        "--skip-stdout",
        action="store_true",
        help="Skip validating qeeg_nf_cli STDOUT JSON outputs",
    )
    ap.add_argument(
        "--skip-demo",
        action="store_true",
        help="Skip running a short --demo session into a temporary outdir",
    )
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="Print per-output validation status (useful for CI debugging)",
    )
    args = ap.parse_args(list(argv))

    if args.skip_stdout and args.skip_demo and args.validate_outdir is None:
        ap.error("Nothing to validate (use --validate-outdir or omit --skip-stdout/--skip-demo)")

    needs_cli = (not args.skip_stdout) or (not args.skip_demo)
    if needs_cli and not args.nf_cli:
        ap.error("--nf-cli is required unless --skip-stdout and --skip-demo are both set")

    nf_cli = os.path.abspath(args.nf_cli) if args.nf_cli else ""
    schemas_dir = os.path.abspath(args.schemas_dir)

    # Preload schemas to (a) enforce $id presence/uniqueness and (b) enable
    # deterministic cross-document reference resolution.
    schemas_by_id, schema_registry = _load_all_schemas(schemas_dir)
    full_registry = _try_build_referencing_registry(schemas_by_id)

    any_failed = False

    # Best-effort: validate the schemas themselves when python-jsonschema is available.
    schema_check_used: Optional[str] = None
    for schema_id, schema in schemas_by_id.items():
        try:
            used = _try_full_check_schema(schema)
        except BaseException as e:
            print(f"ERROR: schema self-check failed for {schema_id}: {e}", file=sys.stderr)
            any_failed = True
            used = None
        if used is None:
            schema_check_used = None
            break
        schema_check_used = used
    if args.verbose and schema_check_used is not None:
        print(f"OK: schemas self-check ({schema_check_used})")

    # 1) Validate STDOUT JSON outputs.
    if not args.skip_stdout:
        any_failed = any_failed or _validate_stdout_json_outputs(
            nf_cli,
            schemas_dir,
            fs=args.fs,
            seconds=args.seconds,
            verbose=args.verbose,
            schema_registry=schema_registry,
            full_registry=full_registry,
        )

    # 2) Validate an existing outdir, if requested.
    if args.validate_outdir is not None:
        any_failed = any_failed or _validate_outdir_reports(
            os.path.abspath(args.validate_outdir),
            schemas_dir,
            require_derived_events=False,
            verbose=args.verbose,
            schema_registry=schema_registry,
            full_registry=full_registry,
        )

    # 3) Validate on-disk JSON reports written under a temporary outdir by a demo run.
    if not args.skip_demo:
        with tempfile.TemporaryDirectory(prefix="qeeg_nf_cli_demo_") as td:
            outdir = os.path.join(td, "out_nf")
            try:
                _run(
                    [
                        nf_cli,
                        "--demo",
                        "--fs",
                        str(args.fs),
                        "--seconds",
                        str(args.seconds),
                        "--outdir",
                        outdir,
                        "--export-derived-events",
                    ]
                )
            except BaseException as e:
                print(f"ERROR: failed to run qeeg_nf_cli demo session: {e}", file=sys.stderr)
                any_failed = True
            else:
                any_failed = any_failed or _validate_outdir_reports(
                    outdir,
                    schemas_dir,
                    require_derived_events=True,
                    verbose=args.verbose,
                    schema_registry=schema_registry,
                    full_registry=full_registry,
                )

    return 1 if any_failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
