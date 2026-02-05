#!/usr/bin/env python3
"""Validate scripts/render_nf_sessions_dashboard.py --json-index outputs.

The neurofeedback sessions dashboard generator (scripts/render_nf_sessions_dashboard.py)
can optionally emit a machine-readable JSON index via --json-index. This is
intended for downstream tooling (GUIs, offline bundles, automation) that wants
to discover and summarize multiple neurofeedback sessions without parsing the
HTML dashboard.

This validator is intended for:
  - CI: catching accidental breaking changes to the JSON index structure.
  - Developers: quickly sanity-checking index files while iterating.

It prefers full JSON Schema validation using the python-jsonschema package
(Draft 2020-12) when available.

Regardless of whether jsonschema is available, it also runs additional
portability/invariant checks (stdlib) that enforce:
  - POSIX-style paths use forward slashes
  - sessions_summary counts are internally consistent
  - session indices are contiguous starting at 1
  - per-session stats objects contain required keys and plausible types

If python-jsonschema is not installed (or is too old to expose a Draft 2020-12
validator), it additionally falls back to minimal structural checks for required
keys and basic types.

Optionally, you can add --check-files to verify that dashboard_html and
per-session referenced files exist on disk (resolved relative to the index
file's directory).

Usage:
  python3 scripts/validate_nf_sessions_dashboard_index.py path/to/index.json
  python3 scripts/validate_nf_sessions_dashboard_index.py index1.json index2.json --verbose
  python3 scripts/validate_nf_sessions_dashboard_index.py index.json --check-files
"""

from __future__ import annotations

import argparse
import json
import math
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
            #   validate_nf_sessions_dashboard_index.cpython-311.pyc
            if fn.startswith("validate_nf_sessions_dashboard_index.") and fn.endswith(".pyc"):
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


SCHEMA_FILENAME = "qeeg_nf_sessions_dashboard_index.schema.json"


def _default_schemas_dir() -> str:
    # Repo layout: <root>/scripts/validate_nf_sessions_dashboard_index.py
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


def _is_number(x: Any) -> bool:
    # bool is a subclass of int
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _is_number_or_none(x: Any) -> bool:
    if x is None:
        return True
    if not _is_number(x):
        return False
    # JSON does not allow NaN/Infinity; guard anyway.
    try:
        return math.isfinite(float(x))
    except Exception:
        return False


def _require_posix_rel_path(v: Any, *, field: str) -> str:
    if not isinstance(v, str) or not v:
        raise RuntimeError(f"{field} must be a non-empty string")
    if "\\" in v:
        raise RuntimeError(f"{field} must use forward slashes (POSIX-style)")
    return v


def _validate_stats(st: Any, *, where: str) -> None:
    if not isinstance(st, dict):
        raise RuntimeError(f"{where}.stats must be an object")

    required_keys = [
        "n_frames",
        "duration_sec",
        "dt_median_sec",
        "reward_frac",
        "artifact_frac",
        "artifact_ready_frac",
        "metric_mean",
        "metric_min",
        "metric_max",
        "metric_last",
        "threshold_mean",
        "threshold_min",
        "threshold_max",
        "threshold_last",
        "reward_rate_mean",
        "reward_rate_last",
        "bad_channels_mean",
        "phase_counts",
        "derived_durations",
    ]

    for k in required_keys:
        if k not in st:
            raise RuntimeError(f"{where}.stats missing required key: {k}")

    if not _is_int(st.get("n_frames")) or int(st["n_frames"]) < 0:
        raise RuntimeError(f"{where}.stats.n_frames must be a non-negative integer")

    # Numeric-or-null fields.
    for k in [
        "duration_sec",
        "dt_median_sec",
        "reward_frac",
        "artifact_frac",
        "artifact_ready_frac",
        "metric_mean",
        "metric_min",
        "metric_max",
        "metric_last",
        "threshold_mean",
        "threshold_min",
        "threshold_max",
        "threshold_last",
        "reward_rate_mean",
        "reward_rate_last",
        "bad_channels_mean",
    ]:
        if not _is_number_or_none(st.get(k)):
            raise RuntimeError(f"{where}.stats.{k} must be a number or null")

    pc = st.get("phase_counts")
    if not isinstance(pc, dict):
        raise RuntimeError(f"{where}.stats.phase_counts must be an object")
    for kk, vv in pc.items():
        if not isinstance(kk, str):
            raise RuntimeError(f"{where}.stats.phase_counts keys must be strings")
        if not _is_int(vv) or int(vv) < 0:
            raise RuntimeError(f"{where}.stats.phase_counts[{kk!r}] must be a non-negative integer")

    dd = st.get("derived_durations")
    if not isinstance(dd, dict):
        raise RuntimeError(f"{where}.stats.derived_durations must be an object")
    for kk, vv in dd.items():
        if not isinstance(kk, str):
            raise RuntimeError(f"{where}.stats.derived_durations keys must be strings")
        if not _is_number_or_none(vv):
            raise RuntimeError(f"{where}.stats.derived_durations[{kk!r}] must be a number or null")
        if vv is not None and float(vv) < 0:
            raise RuntimeError(f"{where}.stats.derived_durations[{kk!r}] must be >= 0")


def _validate_minimal(instance: Any, schema_id: str, *, verbose: bool) -> None:
    """Portability/invariant checks (stdlib only).

    This always runs, even when full JSON Schema validation is available, to
    enforce repo-specific invariants that are intentionally stricter than the
    published schema (e.g., POSIX-style relative paths).
    """

    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    # Optional schema hint.
    if "$schema" in instance:
        if not isinstance(instance["$schema"], str):
            raise RuntimeError("$schema must be a string")
        if instance["$schema"] != schema_id:
            raise RuntimeError(f"$schema mismatch: expected {schema_id!r}, got {instance['$schema']!r}")

    # Required keys.
    for k in [
        "schema_version",
        "generated_utc",
        "roots",
        "dashboard_html",
        "sessions_summary",
        "sessions",
    ]:
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
        if any("\\" in r for r in roots_rel):
            raise RuntimeError("roots_rel must use forward slashes (POSIX-style)")

    _require_posix_rel_path(instance.get("dashboard_html"), field="dashboard_html")

    # Optional dashboard metadata.
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

    ss = instance.get("sessions_summary")
    if not isinstance(ss, dict):
        raise RuntimeError("sessions_summary must be an object")
    for k in ["total", "ok", "error"]:
        if k not in ss:
            raise RuntimeError(f"sessions_summary missing required key: {k}")
        if not _is_int(ss[k]) or int(ss[k]) < 0:
            raise RuntimeError(f"sessions_summary.{k} must be a non-negative integer")

    sessions = instance.get("sessions")
    if not isinstance(sessions, list):
        raise RuntimeError("sessions must be an array")

    total = int(ss["total"])
    ok_n = int(ss["ok"])
    err_n = int(ss["error"])
    if total != len(sessions):
        raise RuntimeError(f"sessions_summary.total ({total}) must match len(sessions) ({len(sessions)})")
    if ok_n + err_n != total:
        raise RuntimeError("sessions_summary counts must sum to total")

    seen_idx: set[int] = set()

    for i, it in enumerate(sessions):
        where = f"sessions[{i}]"
        if not isinstance(it, dict):
            raise RuntimeError(f"{where} must be an object")

        for k in [
            "index",
            "session_dir",
            "timestamp_utc",
            "protocol",
            "metric_spec",
            "nf_feedback_csv",
            "note",
            "ok",
            "stats",
        ]:
            if k not in it:
                raise RuntimeError(f"{where} missing required key: {k}")

        if not _is_int(it.get("index")) or int(it["index"]) < 1:
            raise RuntimeError(f"{where}.index must be an integer >= 1")
        idx = int(it["index"])
        if idx in seen_idx:
            raise RuntimeError(f"Duplicate session index: {idx}")
        seen_idx.add(idx)

        _require_posix_rel_path(it.get("session_dir"), field=f"{where}.session_dir")
        _require_posix_rel_path(it.get("nf_feedback_csv"), field=f"{where}.nf_feedback_csv")

        for k in ["timestamp_utc", "protocol", "metric_spec", "note"]:
            v = it.get(k)
            if not isinstance(v, str):
                raise RuntimeError(f"{where}.{k} must be a string")

        if not isinstance(it.get("ok"), bool):
            raise RuntimeError(f"{where}.ok must be a boolean")

        # Optional relative file paths.
        for k in ["nf_summary_json", "nf_run_meta_json", "derived_events", "report_html"]:
            if k in it:
                v = it.get(k)
                if v is None:
                    continue
                _require_posix_rel_path(v, field=f"{where}.{k}")

        _validate_stats(it.get("stats"), where=where)

    # Best-effort: ensure indices are 1..N (renderer uses this convention).
    if seen_idx and sorted(seen_idx) != list(range(1, len(seen_idx) + 1)):
        raise RuntimeError("Session indices must be contiguous starting at 1")

    # Best-effort: ensure summary counts align with per-session ok flags.
    ok_flags = sum(1 for it in sessions if isinstance(it, dict) and bool(it.get("ok")))
    if ok_flags != ok_n:
        raise RuntimeError(f"sessions_summary.ok ({ok_n}) does not match number of sessions with ok=true ({ok_flags})")

    if verbose:
        print("OK: portability/invariant checks")


def _resolve_rel_posix(base_dir: str, posix_path: str) -> str:
    """Resolve a POSIX-style path relative to base_dir into an OS path."""

    p = str(posix_path).replace("/", os.sep)
    if os.path.isabs(p):
        return os.path.normpath(p)
    return os.path.normpath(os.path.join(base_dir, p))


def _check_files(instance: Any, *, index_path: str, verbose: bool) -> None:
    """Validate that referenced files exist on disk (best-effort)."""

    if not isinstance(instance, dict):
        raise RuntimeError("Index must be a JSON object")

    base_dir = os.path.dirname(os.path.abspath(index_path)) or "."

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

    sessions = instance.get("sessions")
    if not isinstance(sessions, list):
        raise RuntimeError("sessions must be an array")

    for i, it in enumerate(sessions):
        if not isinstance(it, dict):
            raise RuntimeError(f"sessions[{i}] must be an object")

        sess_dir_rel = str(it.get("session_dir") or "")
        sess_dir_abs = _resolve_rel_posix(base_dir, sess_dir_rel)
        if not os.path.isdir(sess_dir_abs):
            raise RuntimeError(
                f"sessions[{i}].session_dir does not exist or is not a directory: {sess_dir_rel!r} (resolved: {sess_dir_abs})"
            )

        csv_rel = str(it.get("nf_feedback_csv") or "")
        csv_abs = _resolve_rel_posix(base_dir, csv_rel)
        if not os.path.isfile(csv_abs):
            raise RuntimeError(
                f"sessions[{i}].nf_feedback_csv does not exist: {csv_rel!r} (resolved: {csv_abs})"
            )

        # Optional files.
        for k in ["nf_summary_json", "nf_run_meta_json", "derived_events", "report_html"]:
            v = it.get(k)
            if v is None:
                continue
            if not isinstance(v, str) or not v:
                continue
            p_abs = _resolve_rel_posix(base_dir, v)
            if not os.path.isfile(p_abs):
                raise RuntimeError(f"sessions[{i}].{k} missing: {v!r} (resolved: {p_abs})")


def validate_index(path: str, *, schemas_dir: str, verbose: bool, check_files: bool) -> None:
    schema = _load_schema(schemas_dir)
    schema_id = str(schema.get("$id") or "")
    if not schema_id:
        raise RuntimeError("Schema missing $id")

    instance = _load_json(path)

    Draft202012Validator = _try_get_draft202012_validator()
    if Draft202012Validator is not None:
        try:
            _validate_with_jsonschema(instance, schema, verbose=verbose)
        except Exception as e:
            # Fall back to stdlib checks if jsonschema exists but cannot perform
            # Draft 2020-12 validation for environmental reasons. If the error
            # looks like an instance failure, re-raise.
            msg = str(e)
            if "JSON Schema validation failed" in msg:
                raise
            if verbose:
                print(f"Note: full schema validation unavailable ({msg}); continuing with stdlib checks")
    elif verbose:
        print("Note: python-jsonschema Draft 2020-12 validator not available; continuing with stdlib checks")

    # Always enforce repo-specific portability/invariant checks, even when full
    # JSON Schema validation succeeds.
    _validate_minimal(instance, schema_id, verbose=verbose)

    if check_files:
        _check_files(instance, index_path=path, verbose=verbose)
        if verbose:
            print("OK: file existence checks")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Validate qeeg nf sessions dashboard JSON index files.")
    ap.add_argument(
        "indexes",
        nargs="+",
        help="One or more nf_sessions_dashboard_index.json files to validate.",
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
            "Verify that dashboard_html and per-session referenced files exist on disk "
            "(resolved relative to the index file)."
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
