#!/usr/bin/env python3
"""Validate scripts/render_reports_dashboard.py --json-index outputs.

This repo's report dashboard generator (scripts/render_reports_dashboard.py) can
optionally emit a machine-readable JSON index via --json-index. This is intended
for downstream tooling (GUIs, offline bundles, automation) that wants to discover
available HTML reports without parsing the dashboard HTML.

This script is intended for:
  - CI: catching accidental breaking changes to the JSON index structure.
  - Developers: quickly sanity-checking index files while iterating.

It prefers full JSON Schema validation using the python-jsonschema package
(Draft 2020-12) when available.

If python-jsonschema is not installed (or is too old to expose a Draft 2020-12
validator), it falls back to a minimal structural validation that checks:
  - required keys exist
  - basic types match
  - status fields are valid (ok|skipped|error)
  - optional summary counts are internally consistent

Optionally, you can add --check-files to verify that dashboard_html and referenced
report_html files exist on disk (relative to the index file), and that each
report's outdir exists.

Usage:
  python3 scripts/validate_reports_dashboard_index.py path/to/index.json
  python3 scripts/validate_reports_dashboard_index.py index1.json index2.json --verbose
  python3 scripts/validate_reports_dashboard_index.py index.json --check-files
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Optional, Sequence


# Best-effort: avoid polluting the source tree with __pycache__ when this script
# is invoked from CI/CTest.
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
            # Typical filename:
            #   validate_reports_dashboard_index.cpython-311.pyc
            if fn.startswith("validate_reports_dashboard_index.") and fn.endswith(".pyc"):
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


def _default_schemas_dir() -> str:
    # Repo layout: <root>/scripts/validate_reports_dashboard_index.py
    #             <root>/schemas/*.json
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", "schemas"))


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _load_schema(schemas_dir: str) -> Dict[str, Any]:
    schema_path = os.path.join(schemas_dir, SCHEMA_FILENAME)
    with open(schema_path, "r", encoding="utf-8") as f:
        schema = json.load(f)
    if not isinstance(schema, dict):
        raise RuntimeError(f"Schema must be a JSON object: {schema_path}")
    return schema


def _try_get_draft202012_validator():
    try:
        from jsonschema import Draft202012Validator  # type: ignore

        return Draft202012Validator
    except Exception:
        return None


def _format_path(p: Sequence[Any]) -> str:
    # jsonschema error.path is a deque-like of keys/indices.
    if not p:
        return "$"
    out = "$"
    for tok in p:
        if isinstance(tok, int):
            out += f"[{tok}]"
        else:
            out += f".{tok}"
    return out


def _validate_with_jsonschema(instance: Any, schema: Dict[str, Any], *, verbose: bool) -> None:
    Draft202012Validator = _try_get_draft202012_validator()
    if Draft202012Validator is None:
        raise RuntimeError("python-jsonschema Draft 2020-12 validator not available")

    # Validate the schema itself first (helps catch typos early).
    Draft202012Validator.check_schema(schema)

    v = Draft202012Validator(schema)
    errs = sorted(v.iter_errors(instance), key=lambda e: list(e.path))
    if errs:
        lines: List[str] = []
        for e in errs[:50]:
            lines.append(f"{_format_path(list(e.path))}: {e.message}")
        more = "" if len(errs) <= 50 else f"\n... ({len(errs) - 50} more errors)"
        raise RuntimeError("JSON Schema validation failed:\n" + "\n".join(lines) + more)

    if verbose:
        print("OK: full JSON Schema validation")


def _is_int(x: Any) -> bool:
    # bool is a subclass of int in Python, so exclude it.
    return isinstance(x, int) and not isinstance(x, bool)


def _validate_minimal(instance: Any, schema_id: str, *, verbose: bool) -> None:
    """Dependency-free validation (stdlib only)."""

    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    # Optional schema hint.
    if "$schema" in instance:
        if not isinstance(instance["$schema"], str):
            raise RuntimeError("$schema must be a string")
        if instance["$schema"] != schema_id:
            raise RuntimeError(f"$schema mismatch: expected {schema_id!r}, got {instance['$schema']!r}")

    # Required keys
    for k in ["schema_version", "generated_utc", "roots", "dashboard_html", "reports"]:
        if k not in instance:
            raise RuntimeError(f"Missing required key: {k}")

    if not _is_int(instance.get("schema_version")) or int(instance["schema_version"]) != 1:
        raise RuntimeError("schema_version must be integer 1")

    if not isinstance(instance.get("generated_utc"), str) or not instance["generated_utc"]:
        raise RuntimeError("generated_utc must be a non-empty string")

    roots = instance.get("roots")
    if not isinstance(roots, list) or not all(isinstance(r, str) for r in roots):
        raise RuntimeError("roots must be an array of strings")

    if "roots_rel" in instance:
        roots_rel = instance["roots_rel"]
        if not isinstance(roots_rel, list) or not all(isinstance(r, str) for r in roots_rel):
            raise RuntimeError("roots_rel must be an array of strings")

    dash = instance.get("dashboard_html")
    if not isinstance(dash, str) or not dash:
        raise RuntimeError("dashboard_html must be a non-empty string")
    if "\\" in dash:
        raise RuntimeError("dashboard_html must use forward slashes (POSIX-style)")

    # Optional dashboard metadata
    if "dashboard_exists" in instance and not isinstance(instance["dashboard_exists"], bool):
        raise RuntimeError("dashboard_exists must be a boolean")
    if "dashboard_mtime_utc" in instance:
        v = instance["dashboard_mtime_utc"]
        if not isinstance(v, str) or not v:
            raise RuntimeError("dashboard_mtime_utc must be a non-empty string")
    if "dashboard_size_bytes" in instance:
        v = instance["dashboard_size_bytes"]
        if not _is_int(v) or int(v) < 0:
            raise RuntimeError("dashboard_size_bytes must be a non-negative integer")

    reports = instance.get("reports")
    if not isinstance(reports, list):
        raise RuntimeError("reports must be an array")

    allowed_status = {"ok", "skipped", "error"}
    for i, it in enumerate(reports):
        if not isinstance(it, dict):
            raise RuntimeError(f"reports[{i}] must be an object")

        for k in ["kind", "outdir", "report_html", "status"]:
            if k not in it:
                raise RuntimeError(f"reports[{i}] missing required key: {k}")

        if not isinstance(it.get("kind"), str) or not it["kind"]:
            raise RuntimeError(f"reports[{i}].kind must be a non-empty string")

        for path_key in ["outdir", "report_html"]:
            v = it.get(path_key)
            if not isinstance(v, str) or not v:
                raise RuntimeError(f"reports[{i}].{path_key} must be a non-empty string")
            if "\\" in v:
                raise RuntimeError(f"reports[{i}].{path_key} must use forward slashes (POSIX-style)")

        st = it.get("status")
        if not isinstance(st, str) or st not in allowed_status:
            raise RuntimeError(f"reports[{i}].status must be one of {sorted(allowed_status)}")

        if "message" in it and not isinstance(it["message"], str):
            raise RuntimeError(f"reports[{i}].message must be a string")

        # Optional per-report metadata
        if "report_exists" in it and not isinstance(it["report_exists"], bool):
            raise RuntimeError(f"reports[{i}].report_exists must be a boolean")
        if "report_mtime_utc" in it:
            v = it["report_mtime_utc"]
            if not isinstance(v, str) or not v:
                raise RuntimeError(f"reports[{i}].report_mtime_utc must be a non-empty string")
        if "report_size_bytes" in it:
            v = it["report_size_bytes"]
            if not _is_int(v) or int(v) < 0:
                raise RuntimeError(f"reports[{i}].report_size_bytes must be a non-negative integer")

    if "reports_summary" in instance:
        rs = instance["reports_summary"]
        if not isinstance(rs, dict):
            raise RuntimeError("reports_summary must be an object")
        for k in ["total", "ok", "skipped", "error"]:
            if k not in rs:
                raise RuntimeError(f"reports_summary missing required key: {k}")
            if not _is_int(rs[k]) or int(rs[k]) < 0:
                raise RuntimeError(f"reports_summary.{k} must be a non-negative integer")

        total = int(rs["total"])
        ok = int(rs["ok"])
        skipped = int(rs["skipped"])
        err = int(rs["error"])
        if total != len(reports):
            raise RuntimeError(f"reports_summary.total ({total}) must match len(reports) ({len(reports)})")
        if ok + skipped + err != total:
            raise RuntimeError("reports_summary counts must sum to total")

    if verbose:
        print("OK: minimal structural validation")


def _resolve_rel_posix(base_dir: str, posix_path: str) -> str:
    """Resolve a POSIX-style path relative to base_dir into an OS path."""

    def _looks_like_windows_abs(x: str) -> bool:
        # Treat Windows drive-letter paths as absolute even when validating on
        # a POSIX host. This makes --check-files behave sensibly when the index
        # was generated on Windows.
        #
        # Accept:
        #   C:\path\to\file
        #   C:/path/to/file
        if len(x) < 3:
            return False
        if x[1] != ":":
            return False
        if not ("A" <= x[0].upper() <= "Z"):
            return False
        return x[2] in ("/", "\\", os.sep)

    # The JSON index intentionally uses forward slashes for portability.
    p = str(posix_path).replace("/", os.sep)
    if os.path.isabs(p) or _looks_like_windows_abs(p):
        return os.path.normpath(p)
    return os.path.normpath(os.path.join(base_dir, p))


def _validate_invariants(instance: Any, *, verbose: bool) -> None:
    """Validate non-schema invariants that downstream tools rely on."""

    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    dash = instance.get("dashboard_html")
    if isinstance(dash, str) and "\\" in dash:
        raise RuntimeError("dashboard_html must use forward slashes (POSIX-style)")

    roots_rel = instance.get("roots_rel")
    if isinstance(roots_rel, list):
        for i, r in enumerate(roots_rel):
            if isinstance(r, str) and "\\" in r:
                raise RuntimeError(f"roots_rel[{i}] must use forward slashes (POSIX-style)")

    reports = instance.get("reports")
    if not isinstance(reports, list):
        raise RuntimeError("reports must be an array")

    allowed_status = {"ok", "skipped", "error"}
    ok_n = 0
    skipped_n = 0
    error_n = 0

    for i, it in enumerate(reports):
        if not isinstance(it, dict):
            raise RuntimeError(f"reports[{i}] must be an object")

        for path_key in ("outdir", "report_html"):
            v = it.get(path_key)
            if isinstance(v, str) and "\\" in v:
                raise RuntimeError(f"reports[{i}].{path_key} must use forward slashes (POSIX-style)")

        st = it.get("status")
        if isinstance(st, str):
            if st not in allowed_status:
                raise RuntimeError(f"reports[{i}].status must be one of {sorted(allowed_status)}")
            if st == "ok":
                ok_n += 1
            elif st == "skipped":
                skipped_n += 1
            elif st == "error":
                error_n += 1

    if "reports_summary" in instance:
        rs = instance.get("reports_summary")
        if not isinstance(rs, dict):
            raise RuntimeError("reports_summary must be an object")

        for k in ("total", "ok", "skipped", "error"):
            if k not in rs:
                raise RuntimeError(f"reports_summary missing required key: {k}")
            if not _is_int(rs[k]) or int(rs[k]) < 0:
                raise RuntimeError(f"reports_summary.{k} must be a non-negative integer")

        total = int(rs["total"])
        ok = int(rs["ok"])
        skipped = int(rs["skipped"])
        err = int(rs["error"])

        if total != len(reports):
            raise RuntimeError(f"reports_summary.total ({total}) must match len(reports) ({len(reports)})")
        if ok + skipped + err != total:
            raise RuntimeError("reports_summary counts must sum to total")

        # Stronger invariant: summary counts must reflect actual report statuses.
        if ok != ok_n or skipped != skipped_n or err != error_n:
            raise RuntimeError(
                "reports_summary counts do not match reports statuses: "
                f"expected ok={ok_n}, skipped={skipped_n}, error={error_n}; "
                f"got ok={ok}, skipped={skipped}, error={err}"
            )

    if verbose:
        print("OK: invariants")


def _check_files(instance: Any, *, index_path: str, verbose: bool) -> None:
    """Validate that referenced files exist on disk (best-effort)."""

    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    base_dir = os.path.dirname(os.path.abspath(index_path)) or "."

    # Dashboard HTML
    dash_rel = str(instance.get("dashboard_html") or "")
    dash_abs = _resolve_rel_posix(base_dir, dash_rel)
    dash_exists = os.path.isfile(dash_abs)

    if not dash_exists:
        raise RuntimeError(f"dashboard_html does not exist: {dash_rel!r} (resolved: {dash_abs})")

    if "dashboard_exists" in instance:
        declared = bool(instance.get("dashboard_exists"))
        if declared and not dash_exists:
            raise RuntimeError("dashboard_exists=true but dashboard_html is missing")
        if (not declared) and dash_exists and verbose:
            print("Note: dashboard_exists=false but dashboard_html exists (index may be stale)")

    # Per-report files
    reports = instance.get("reports")
    if not isinstance(reports, list):
        raise RuntimeError("reports must be an array")

    for i, it in enumerate(reports):
        if not isinstance(it, dict):
            raise RuntimeError(f"reports[{i}] must be an object")

        outdir_rel = str(it.get("outdir") or "")
        outdir_abs = _resolve_rel_posix(base_dir, outdir_rel)
        if not os.path.isdir(outdir_abs):
            raise RuntimeError(f"reports[{i}].outdir does not exist or is not a directory: {outdir_rel!r} (resolved: {outdir_abs})")

        report_rel = str(it.get("report_html") or "")
        report_abs = _resolve_rel_posix(base_dir, report_rel)
        exists = os.path.isfile(report_abs)

        st = str(it.get("status") or "")
        if st in ("ok", "skipped"):
            if not exists:
                raise RuntimeError(
                    f"reports[{i}] status={st!r} but report_html is missing: {report_rel!r} (resolved: {report_abs})"
                )

        if "report_exists" in it:
            declared = bool(it.get("report_exists"))
            if declared and not exists:
                raise RuntimeError(
                    f"reports[{i}].report_exists=true but report_html is missing: {report_rel!r} (resolved: {report_abs})"
                )
            if (not declared) and exists and verbose:
                print(
                    f"Note: reports[{i}].report_exists=false but report_html exists (index may be stale): {report_rel!r}"
                )


def validate_index(path: str, *, schemas_dir: str, verbose: bool, check_files: bool) -> None:
    schema = _load_schema(schemas_dir)
    schema_id = str(schema.get("$id") or "")
    if not schema_id:
        raise RuntimeError("Schema missing $id")

    instance = _load_json(path)

    validated = False
    Draft202012Validator = _try_get_draft202012_validator()
    if Draft202012Validator is not None:
        try:
            _validate_with_jsonschema(instance, schema, verbose=verbose)
            validated = True
        except Exception as e:
            # Fall back to minimal validation if jsonschema exists but fails due
            # to missing Draft2020-12 support / referencing issues. If the error
            # looks like an instance failure, re-raise.
            msg = str(e)
            if "JSON Schema validation failed" in msg:
                raise
            if verbose:
                print(f"Note: full schema validation unavailable ({msg}); falling back to minimal checks")

    if not validated:
        _validate_minimal(instance, schema_id, verbose=verbose)

    # Even when JSON Schema validation succeeds, enforce invariants that are
    # intentionally kept out of the schema (e.g., POSIX-style path separators
    # and internal summary consistency).
    _validate_invariants(instance, verbose=verbose)

    if check_files:
        _check_files(instance, index_path=path, verbose=verbose)
        if verbose:
            print("OK: file existence checks")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Validate qeeg reports dashboard JSON index files.")
    ap.add_argument(
        "indexes",
        nargs="+",
        help="One or more qeeg_reports_dashboard_index.json files to validate.",
    )
    ap.add_argument(
        "--schemas-dir",
        default=_default_schemas_dir(),
        help="Path to the repo's schemas/ directory (default: ../schemas relative to this script).",
    )
    ap.add_argument(
        "--check-files",
        action="store_true",
        help=(
            "Verify that dashboard_html/report_html paths exist on disk (resolved relative to the index file), "
            "and that each report outdir exists."
        ),
    )
    ap.add_argument("--verbose", action="store_true", help="Print extra details.")

    args = ap.parse_args(list(argv) if argv is not None else None)

    schemas_dir = os.path.abspath(str(args.schemas_dir))

    ok = True
    for p in args.indexes:
        try:
            validate_index(
                os.path.abspath(str(p)),
                schemas_dir=schemas_dir,
                verbose=bool(args.verbose),
                check_files=bool(args.check_files),
            )
            if args.verbose:
                print(f"Validated: {os.path.abspath(str(p))}")
        except Exception as e:
            ok = False
            print(f"ERROR: {p}: {e}", file=sys.stderr)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
