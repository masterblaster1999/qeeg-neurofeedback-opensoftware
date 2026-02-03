#!/usr/bin/env python3
"""Self-test for qeeg_topomap_cli JSON index + report rendering.

This test is intended for CI (CTest). It generates a small synthetic channel
table, runs qeeg_topomap_cli with --json-index, validates the resulting index,
and renders an embedded HTML report.

Exit code:
  0 = success
  1 = failure
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List


def _run(cmd: List[str], *, cwd: Path | None = None) -> None:
    p = subprocess.run(cmd, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"Command failed ({p.returncode}): {' '.join(cmd)}\n--- output ---\n{p.stdout}\n--- end ---\n")


def _write_sample_csv(path: Path) -> None:
    # 10-20 style channels (19). Values are arbitrary but non-constant.
    chans = [
        "Fp1","Fp2","F7","F3","Fz","F4","F8",
        "T3","C3","Cz","C4","T4",
        "T5","P3","Pz","P4","T6",
        "O1","O2",
    ]
    # Three metrics, to ensure multiple maps.
    header = "channel,alpha,theta,beta\n"
    rows = []
    for i, ch in enumerate(chans):
        # Make values vary smoothly with i (and include both signs)
        alpha = (i - 9) / 9.0
        theta = (9 - i) / 11.0
        beta = (i % 7 - 3) / 3.0
        rows.append(f"{ch},{alpha:.6f},{theta:.6f},{beta:.6f}\n")
    path.write_text(header + "".join(rows), encoding="utf-8")


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--topomap-cli", default="qeeg_topomap_cli", help="Path to qeeg_topomap_cli executable")
    ap.add_argument("--keep", action="store_true", help="Keep the temp directory (prints its path)")
    args = ap.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    scripts = repo_root / "scripts"
    schemas = repo_root / "schemas"

    with tempfile.TemporaryDirectory(prefix="qeeg_topomap_index_") as td:
        tdir = Path(td)
        csv_path = tdir / "sample_channels.csv"
        outdir = tdir / "out_topomap"
        outdir.mkdir(parents=True, exist_ok=True)

        _write_sample_csv(csv_path)

        # Run topomap CLI with JSON index enabled.
        cmd = [
            str(args.topomap_cli),
            "--input", str(csv_path),
            "--outdir", str(outdir),
            "--montage", "builtin:standard_1020_19",
            "--annotate",
            "--html-report",
            "--json-index",
            "--robust-range", "0.05", "0.95",
        ]
        _run(cmd)

        index_path = outdir / "topomap_index.json"
        meta_path = outdir / "topomap_run_meta.json"

        if not index_path.exists():
            raise RuntimeError(f"Missing index: {index_path}")
        if not meta_path.exists():
            raise RuntimeError(f"Missing run meta: {meta_path}")
        bmps = sorted(outdir.glob("topomap_*.bmp"))
        if not bmps:
            raise RuntimeError(f"No topomap_*.bmp files were produced under: {outdir}")

        # Validate index against schema + check referenced files.
        validate_py = scripts / "validate_topomap_index.py"
        _run([sys.executable, "-B", str(validate_py), str(index_path), "--schemas-dir", str(schemas), "--check-files", "--verbose"])

        # Render embedded report (separate output name to avoid clobbering the CLI's html report).
        report_py = scripts / "render_topomap_report.py"
        report_out = outdir / "topomap_report_embedded.html"
        _run([sys.executable, "-B", str(report_py), "--input", str(outdir), "--out", str(report_out)])

        if not report_out.exists():
            raise RuntimeError(f"Missing report: {report_out}")

        if args.keep:
            print(f"Kept temp dir: {tdir}")
            # do not delete
            input("Press Enter to exit...")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
