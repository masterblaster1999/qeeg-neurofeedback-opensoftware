#!/usr/bin/env python3
"""Self-test for qeeg_loreta_metrics_cli JSON index + report rendering.

This test is intended for CI (CTest). It generates a small synthetic ROI table,
runs qeeg_loreta_metrics_cli with --json-index, validates the resulting index,
and renders an embedded HTML report.

Exit code:
  0 = success
  1 = failure
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence


def _run(cmd: List[str], *, cwd: Optional[Path] = None) -> None:
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
    # Three ROIs (Brodmann-style labels) and a mix of raw-like and z-like metrics.
    header = "roi,z_theta,z_alpha,csd_beta\n"
    rows = [
        "BA24,2.3,-0.4,0.012\n",
        "BA9,-1.8,0.2,0.008\n",
        "BA17,0.1,-2.6,0.020\n",
    ]
    path.write_text(header + "".join(rows), encoding="utf-8")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--loreta-cli",
        default="qeeg_loreta_metrics_cli",
        help="Path to qeeg_loreta_metrics_cli executable",
    )
    ap.add_argument("--keep", action="store_true", help="Keep the temp directory (prints its path)")
    args = ap.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    scripts = repo_root / "scripts"
    schemas = repo_root / "schemas"

    with tempfile.TemporaryDirectory(prefix="qeeg_loreta_metrics_index_") as td:
        tdir = Path(td)
        csv_path = tdir / "sample_loreta_metrics.csv"
        outdir = tdir / "out_loreta"
        outdir.mkdir(parents=True, exist_ok=True)

        _write_sample_csv(csv_path)

        # Run the CLI.
        cmd = [
            str(args.loreta_cli),
            "--input",
            str(csv_path),
            "--outdir",
            str(outdir),
            "--atlas",
            "brodmann",
            "--html-report",
            "--json-index",
            "--protocol-json",
            "--protocol-only-z",
            "--protocol-top",
            "5",
            "--protocol-threshold",
            "1.0",
        ]
        _run(cmd)

        index_path = outdir / "loreta_metrics_index.json"
        meta_path = outdir / "loreta_metrics_run_meta.json"
        protocol_path = outdir / "loreta_protocol.json"

        if not index_path.exists():
            raise RuntimeError(f"Missing index: {index_path}")
        if not meta_path.exists():
            raise RuntimeError(f"Missing run meta: {meta_path}")
        if not protocol_path.exists():
            raise RuntimeError(f"Missing protocol: {protocol_path}")

        # Validate index against schema + check referenced files.
        validate_py = scripts / "validate_loreta_metrics_index.py"
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

        # Validate protocol JSON (schema + semantic checks).
        validate_proto_py = scripts / "validate_loreta_protocol.py"
        _run(
            [
                sys.executable,
                "-B",
                str(validate_proto_py),
                str(protocol_path),
                "--schemas-dir",
                str(schemas),
                "--check-files",
                "--verbose",
            ]
        )

        # Render embedded report (separate output name to avoid clobbering the CLI's html report).
        report_py = scripts / "render_loreta_metrics_report.py"
        report_out = outdir / "loreta_metrics_report_embedded.html"
        _run([sys.executable, "-B", str(report_py), "--input", str(outdir), "--out", str(report_out)])

        if not report_out.exists():
            raise RuntimeError(f"Missing report: {report_out}")

        if args.keep:
            print(f"Kept temp dir: {tdir}")
            input("Press Enter to exit...")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
