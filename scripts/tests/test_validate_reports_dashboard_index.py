#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


# Ensure scripts/ is on sys.path so we can import validate_reports_dashboard_index.py
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

import validate_reports_dashboard_index as vrdi  # noqa: E402


@contextlib.contextmanager
def _capture_stdio() -> tuple[io.StringIO, io.StringIO]:
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        yield out, err


def _write_json(path: Path, obj: object) -> None:
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _report(*, kind: str, outdir: str, report_html: str, status: str) -> dict:
    return {
        "kind": kind,
        "outdir": outdir,
        "report_html": report_html,
        "status": status,
        "message": "",
        "report_exists": True,
    }


def _index(*, dashboard_html: str, reports: list[dict], ok: int, skipped: int, error: int) -> dict:
    return {
        "schema_version": 1,
        "generated_utc": "2026-02-05T00:00:00Z",
        "roots": ["/tmp"],
        "dashboard_html": dashboard_html,
        "reports_summary": {
            "total": len(reports),
            "ok": ok,
            "skipped": skipped,
            "error": error,
        },
        "reports": reports,
    }


class ValidateReportsDashboardIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
        sys.dont_write_bytecode = True

    def test_valid_index_with_check_files(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_reports_index_test_") as td:
            root = Path(td)
            (root / "dashboard.html").write_text("<html></html>", encoding="utf-8")

            (root / "run1").mkdir(parents=True)
            (root / "run1" / "report.html").write_text("<html>r1</html>", encoding="utf-8")

            idx = _index(
                dashboard_html="dashboard.html",
                reports=[
                    _report(kind="quality", outdir="run1", report_html="run1/report.html", status="ok"),
                ],
                ok=1,
                skipped=0,
                error=0,
            )

            index_path = root / "qeeg_reports_dashboard_index.json"
            _write_json(index_path, idx)

            with _capture_stdio():
                code = int(
                    vrdi.main(
                        [
                            str(index_path),
                            "--schemas-dir",
                            str(schemas_dir),
                            "--check-files",
                        ]
                    )
                )

            self.assertEqual(code, 0)

    def test_backslash_paths_fail_even_when_schema_validation_succeeds(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_reports_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dash\\board.html",
                reports=[
                    _report(kind="quality", outdir="run1", report_html="run1/report.html", status="ok"),
                ],
                ok=1,
                skipped=0,
                error=0,
            )
            index_path = root / "qeeg_reports_dashboard_index.json"
            _write_json(index_path, idx)

            # Force the code path where JSON Schema validation is considered
            # available/successful, then ensure invariants are still enforced.
            with patch.object(vrdi, "_try_get_draft202012_validator", return_value=object()), patch.object(
                vrdi, "_validate_with_jsonschema", return_value=None
            ):
                with _capture_stdio():
                    code = int(vrdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)

    def test_reports_summary_status_mismatch_fails(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        schemas_dir = repo_root / "schemas"

        with tempfile.TemporaryDirectory(prefix="qeeg_reports_index_test_") as td:
            root = Path(td)

            idx = _index(
                dashboard_html="dashboard.html",
                reports=[
                    _report(kind="quality", outdir="run1", report_html="run1/report.html", status="ok"),
                    _report(kind="topomap", outdir="run2", report_html="run2/report.html", status="error"),
                ],
                ok=2,
                skipped=0,
                error=0,
            )
            index_path = root / "qeeg_reports_dashboard_index.json"
            _write_json(index_path, idx)

            # Ensure mismatch is caught even when schema validation passes.
            with patch.object(vrdi, "_try_get_draft202012_validator", return_value=object()), patch.object(
                vrdi, "_validate_with_jsonschema", return_value=None
            ):
                with _capture_stdio():
                    code = int(vrdi.main([str(index_path), "--schemas-dir", str(schemas_dir)]))

            self.assertNotEqual(code, 0)

    def test_resolve_rel_posix_drive_letter_absolute(self) -> None:
        # On POSIX hosts, Windows drive-letter absolute paths are not treated as
        # absolute by os.path.isabs(). Ensure the helper treats them as absolute
        # so --check-files errors are understandable.
        base_dir = "/base"
        p = vrdi._resolve_rel_posix(base_dir, "C:/tmp/foo.txt")
        self.assertEqual(os.path.normpath("C:/tmp/foo.txt"), p)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
