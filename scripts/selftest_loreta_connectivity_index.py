#!/usr/bin/env python3
"""Self-test for qeeg_loreta_connectivity_cli JSON outputs.

This script generates a tiny synthetic ROI-to-ROI connectivity edge-list,
runs qeeg_loreta_connectivity_cli to produce matrix outputs + JSON index/protocol,
and validates them against the repo schemas.

Used by CTest.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List


def _run(cmd: List[str], cwd: Path | None = None) -> None:
    r = subprocess.run(cmd, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout)
        raise SystemExit(r.returncode)


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--loreta-conn-cli", required=True, help="Path to qeeg_loreta_connectivity_cli")
    args = ap.parse_args(argv)

    repo = Path(__file__).resolve().parents[1]
    schemas = repo / "schemas"

    with tempfile.TemporaryDirectory(prefix="qeeg_loreta_conn_selftest_") as td:
        tdir = Path(td)
        inp = tdir / "lps_edges.csv"
        outdir = tdir / "out_loreta_conn"

        # Synthetic: 4 ROIs, 2 bands.
        rows = [
            ("ROI_A", "ROI_B", "band", "metric", "value"),
            ("ACC", "PCC", "theta", "lps_z", 2.5),
            ("ACC", "PCC", "alpha", "lps_z", -2.1),
            ("ACC", "mPFC", "theta", "lps_z", 1.2),
            ("PCC", "mPFC", "theta", "lps_z", -1.8),
            ("PCC", "TPJ", "alpha", "lps_z", 0.9),
        ]
        with open(inp, "w", encoding="utf-8") as f:
            for r in rows:
                f.write(",".join(str(x) for x in r) + "\n")

        cmd = [
            args.loreta_conn_cli,
            "--input",
            str(inp),
            "--outdir",
            str(outdir),
            "--json-index",
            "--protocol-json",
            "--atlas",
            "unit_test",
        ]
        _run(cmd)

        idx = outdir / "loreta_connectivity_index.json"
        proto = outdir / "loreta_connectivity_protocol.json"
        assert idx.exists(), f"Missing {idx}"
        assert proto.exists(), f"Missing {proto}"

        # Basic sanity: index references at least one matrix.
        obj = json.loads(idx.read_text(encoding="utf-8"))
        measures = obj.get("measures")
        assert isinstance(measures, list) and measures, "index.measures must be non-empty"
        matrices = measures[0].get("matrices") if isinstance(measures[0], dict) else None
        assert isinstance(matrices, list) and matrices, "index.measures[0].matrices must be non-empty"

        # Validate against schemas (if jsonschema available).
        _run([sys.executable, "-B", str(repo / "scripts" / "validate_loreta_connectivity_index.py"), str(idx), "--schemas-dir", str(schemas), "--check-files"])
        _run([sys.executable, "-B", str(repo / "scripts" / "validate_loreta_connectivity_protocol.py"), str(proto), "--schemas-dir", str(schemas), "--check-files"])

        # Render HTML connectivity report to smoke-test compatibility.
        report_html = outdir / "connectivity_report.html"
        _run([
            sys.executable,
            "-B",
            str(repo / "scripts" / "render_connectivity_report.py"),
            "--input",
            str(outdir),
            "--out",
            str(report_html),
        ])
        assert report_html.exists(), "connectivity_report.html should be generated"

    print("OK: qeeg_loreta_connectivity_cli selftest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
