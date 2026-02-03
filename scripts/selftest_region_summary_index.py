#!/usr/bin/env python3
"""Self-test for qeeg_region_summary_cli JSON index output.

This test is intended for CI (CTest). It generates a small synthetic per-channel
CSV, runs qeeg_region_summary_cli with --json-index, validates the resulting
index, and checks that the expected artifacts are written.

Exit code:
  0 = success
  1 = failure
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List


def _run(cmd: List[str], *, cwd: Path | None = None) -> None:
    p = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if p.returncode != 0:
        raise RuntimeError(
            f"Command failed ({p.returncode}): {' '.join(cmd)}\n--- output ---\n{p.stdout}\n--- end ---\n"
        )


def _write_sample_csv(path: Path) -> None:
    # 10-20 style channels (19). Values are arbitrary but non-constant.
    chans = [
        "Fp1",
        "Fp2",
        "F7",
        "F3",
        "Fz",
        "F4",
        "F8",
        "T3",
        "C3",
        "Cz",
        "C4",
        "T4",
        "T5",
        "P3",
        "Pz",
        "P4",
        "T6",
        "O1",
        "O2",
    ]

    header = "channel,alpha,theta,beta\n"
    rows = []
    for i, ch in enumerate(chans):
        # Make values vary smoothly with i (and include both signs)
        alpha = (i - 9) / 9.0
        theta = (9 - i) / 11.0
        beta = ((i % 7) - 3) / 3.0
        rows.append(f"{ch},{alpha:.6f},{theta:.6f},{beta:.6f}\n")

    path.write_text(header + "".join(rows), encoding="utf-8")


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--region-cli", default="qeeg_region_summary_cli", help="Path to qeeg_region_summary_cli executable")
    ap.add_argument("--keep", action="store_true", help="Keep the temp directory (prints its path)")
    args = ap.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    scripts = repo_root / "scripts"
    schemas = repo_root / "schemas"

    with tempfile.TemporaryDirectory(prefix="qeeg_region_summary_index_") as td:
        tdir = Path(td)
        csv_path = tdir / "sample_channels.csv"
        outdir = tdir / "out_regions"
        outdir.mkdir(parents=True, exist_ok=True)

        _write_sample_csv(csv_path)

        cmd = [
            str(args.region_cli),
            "--input",
            str(csv_path),
            "--outdir",
            str(outdir),
            "--html-report",
            "--json-index",
        ]
        _run(cmd)

        index_path = outdir / "region_summary_index.json"
        meta_path = outdir / "region_summary_run_meta.json"
        wide_path = outdir / "region_summary.csv"
        long_path = outdir / "region_summary_long.csv"
        html_path = outdir / "region_report.html"

        for p in (index_path, meta_path, wide_path, long_path, html_path):
            if not p.exists():
                raise RuntimeError(f"Missing output: {p}")

        # Basic sanity: index contains expected keys.
        idx = json.loads(index_path.read_text(encoding="utf-8"))
        for k in ("schema_version", "tool", "metrics", "groups", "csv_wide", "csv_long"):
            if k not in idx:
                raise RuntimeError(f"Index missing key {k!r}: {index_path}")

        validate_py = scripts / "validate_region_summary_index.py"
        _run(
            [
                sys.executable,
                "-B",
                str(validate_py),
                str(index_path),
                "--schemas-dir",
                str(schemas),
                "--check-files",
                "--verbose",
            ]
        )

        if args.keep:
            print(f"Kept temp dir: {tdir}")
            input("Press Enter to exit...")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
