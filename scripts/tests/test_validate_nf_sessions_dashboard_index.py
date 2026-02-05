#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path


# Ensure scripts/ is on sys.path so we can import validate_nf_sessions_dashboard_index.py
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

import validate_nf_sessions_dashboard_index as vnsdi  # noqa: E402


@contextlib.contextmanager
def _capture_stdio() -> tuple[io.StringIO, io.StringIO]:
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        yield out, err


def _write_json(path: Path, obj: object) -> None:
    path.write_text(json.dumps(obj, indent=2, sort_keys=True), encoding="utf-8")


def _minimal_stats() -> dict:
    # Matches required stats keys in the schema and in the validator's minimal checks.
    return {
        "n_frames": 0,
        "duration_sec": None,
        "dt_median_sec": None,
        "reward_frac": None,
        "artifact_frac": None,
        "artifact_ready_frac": None,
        "metric_mean": None,
        "metric_min": None,
        "metric_max": None,
        "metric_last": None,
        "threshold_mean": None,
        "threshold_min": None,
        "threshold_max": None,
        "threshold_last": None,
        "reward_rate_mean": None,
        "reward_rate_last": None,
        "bad_channels_mean": None,
        "phase_counts": {},
        "derived_durations": {},
    }


def _session(*, index: int, ok: bool, session_dir: str, nf_feedback_csv: str) -> dict:
    return {
        "index": index,
        "session_dir": session_dir,
        "timestamp_utc": "",
        "protocol": "",
        "metric_spec": "",
        "nf_feedback_csv": nf_feedback_csv,
        "note": "",
        "ok": ok,
        "stats": _minimal_stats(),
    }


def _index(*, dashboard_html: str, sessions: list[dict], ok_count: int, error_count: int) -> dict:
    return {
        "schema_version": 1,
        "generated_utc": "2026-02-05T00:00:00Z",
        "roots": ["/tmp"],
        "dashboard_html": dashboard_html,
        "sessions_summary": {
            "total": len(sessions),
            "ok": ok_count,
            "error": error_count,
        },
        "sessions": sessions,
    }


class ValidateNfSessionsDashboardIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
        sys.dont_write_bytecode = True

    def test_valid_index_with_check_files(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_index_test_") as td:
            root = Path(td)
            (root / "dashboard.html").write_text("<html></html>", encoding="utf-8")

            (root / "sess1").mkdir(parents=True)
            (root / "sess1" / "nf_feedback.csv").write_text("t,x\n", encoding="utf-8")

            idx = _index(
                dashboard_html="dashboard.html",
                sessions=[
                    _session(index=1, ok=True, session_dir="sess1", nf_feedback_csv="sess1/nf_feedback.csv")
                ],
                ok_count=1,
                error_count=0,
            )
            index_path = root / "nf_sessions_dashboard_index.json"
            _write_json(index_path, idx)

            with _capture_stdio():
                code = int(
                    vnsdi.main(
                        [
                            str(index_path),
                            "--schemas-dir",
                            str(schemas_dir),
                            "--check-files",
                        ]
                    )
                )

            self.assertEqual(code, 0)

    def test_backslash_paths_fail(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dash\\board.html",
                sessions=[
                    _session(index=1, ok=True, session_dir="sess1", nf_feedback_csv="sess1/nf_feedback.csv")
                ],
                ok_count=1,
                error_count=0,
            )
            index_path = root / "nf_sessions_dashboard_index.json"
            _write_json(index_path, idx)

            with _capture_stdio():
                code = int(vnsdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)

    def test_non_contiguous_indices_fail(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dashboard.html",
                sessions=[
                    _session(index=1, ok=True, session_dir="sess1", nf_feedback_csv="sess1/nf_feedback.csv"),
                    _session(index=3, ok=True, session_dir="sess3", nf_feedback_csv="sess3/nf_feedback.csv"),
                ],
                ok_count=2,
                error_count=0,
            )
            index_path = root / "nf_sessions_dashboard_index.json"
            _write_json(index_path, idx)

            with _capture_stdio():
                code = int(vnsdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)

    def test_summary_ok_mismatch_fails(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dashboard.html",
                sessions=[
                    _session(index=1, ok=True, session_dir="sess1", nf_feedback_csv="sess1/nf_feedback.csv"),
                    _session(index=2, ok=False, session_dir="sess2", nf_feedback_csv="sess2/nf_feedback.csv"),
                ],
                ok_count=2,
                error_count=0,
            )
            index_path = root / "nf_sessions_dashboard_index.json"
            _write_json(index_path, idx)

            with _capture_stdio():
                code = int(vnsdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)


    def test_invariants_enforced_even_when_schema_validation_passes(self) -> None:
        """Simulate a successful jsonschema validation and ensure invariants still run."""

        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        class _FakeDraft202012Validator:
            def __init__(self, schema):
                self._schema = schema

            @staticmethod
            def check_schema(schema):
                return None

            def iter_errors(self, instance):
                return iter(())

        with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dash\\board.html",
                sessions=[
                    _session(index=1, ok=True, session_dir="sess1", nf_feedback_csv="sess1/nf_feedback.csv")
                ],
                ok_count=1,
                error_count=0,
            )
            index_path = root / "nf_sessions_dashboard_index.json"
            _write_json(index_path, idx)

            with patch.object(vnsdi, "_try_get_draft202012_validator", return_value=_FakeDraft202012Validator):
                with _capture_stdio():
                    code = int(vnsdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
