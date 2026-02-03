#!/usr/bin/env python3
"""Smoke test for scripts/render_nf_sessions_dashboard.py.

This selftest:
  1) Generates a few small synthetic neurofeedback session folders using
     scripts/rt_dashboard_simulate_output.py
  2) Renders an aggregated sessions dashboard (HTML) and a machine-readable
     JSON index via --json-index
  3) Performs light sanity checks on the outputs (no diff-marker artifacts, valid JSON, expected keys).

Stdlib only. Intended for CI/CTest.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List


# Avoid writing .pyc / __pycache__ during CI.
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
sys.dont_write_bytecode = True


def _run(cmd: List[str], *, cwd: Path) -> None:
    p = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if p.returncode != 0:
        raise RuntimeError(f"Command failed ({p.returncode}): {' '.join(cmd)}\n--- output ---\n{p.stdout}")


def main() -> int:
    scripts_dir = Path(__file__).resolve().parent
    repo_root = scripts_dir.parent

    sim = scripts_dir / "rt_dashboard_simulate_output.py"
    render = scripts_dir / "render_nf_sessions_dashboard.py"

    if not sim.is_file():
        raise RuntimeError(f"Missing simulator: {sim}")
    if not render.is_file():
        raise RuntimeError(f"Missing renderer: {render}")

    with tempfile.TemporaryDirectory(prefix="qeeg_nf_sessions_dashboard_selftest_") as td:
        root = Path(td)

        # Create a small set of synthetic sessions.
        sess_dirs: List[Path] = []
        for i in range(3):
            d = root / f"out_nf_{i+1}"
            sess_dirs.append(d)
            _run(
                [
                    sys.executable,
                    "-B",
                    str(sim),
                    "--outdir",
                    str(d),
                    "--seconds",
                    "1.5",
                    "--update",
                    "0.25",
                    "--seed",
                    str(12345 + i),
                    "--overwrite",
                    "--with-bandpower",
                    "--with-artifact",
                ],
                cwd=repo_root,
            )

        out_html = root / "nf_sessions_dashboard.html"
        out_json = root / "nf_sessions_dashboard_index.json"

        _run(
            [
                sys.executable,
                "-B",
                str(render),
                str(root),
                "--out",
                str(out_html),
                "--json-index",
                str(out_json),
                "--no-generate-reports",
            ],
            cwd=repo_root,
        )

        if not out_html.is_file():
            raise RuntimeError("Expected dashboard HTML was not created")
        if not out_json.is_file():
            raise RuntimeError("Expected JSON index was not created")

        # Validate the JSON index against the published schema.
        validate = scripts_dir / "validate_nf_sessions_dashboard_index.py"
        if not validate.is_file():
            raise RuntimeError(f"Missing validator: {validate}")
        _run(
            [
                sys.executable,
                "-B",
                str(validate),
                str(out_json),
                "--schemas-dir",
                str(repo_root / "schemas"),
                "--check-files",
            ],
            cwd=repo_root,
        )

        # The original bug that prompted this selftest was diff-marker artifacts
        # ('+<tag>') leaking into the HTML templates. Ensure the output doesn't
        # contain those sequences.
        html = out_html.read_text(encoding="utf-8", errors="replace")
        if "\n+<" in html or "\n+/*" in html:
            raise RuntimeError("Dashboard HTML appears to contain diff-marker artifacts (leading '+')")

        data = json.loads(out_json.read_text(encoding="utf-8"))
        if int(data.get("schema_version", 0)) != 1:
            raise RuntimeError("Unexpected or missing schema_version in JSON index")
        if not isinstance(data.get("sessions"), list) or len(data["sessions"]) < 1:
            raise RuntimeError("Expected at least one session in JSON index")

        # Check that key paths are strings (renderer should output POSIX-style relative paths).
        if not isinstance(data.get("dashboard_html"), str):
            raise RuntimeError("dashboard_html should be a string")

        s0 = data["sessions"][0]
        if not isinstance(s0.get("session_dir"), str):
            raise RuntimeError("session_dir should be a string")
        if not isinstance(s0.get("nf_feedback_csv"), str):
            raise RuntimeError("nf_feedback_csv should be a string")

    print("OK: nf sessions dashboard selftest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
