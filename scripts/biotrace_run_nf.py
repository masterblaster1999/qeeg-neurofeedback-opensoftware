#!/usr/bin/env python3
"""biotrace_run_nf.py

One-step helper for Mind Media BioTrace+ / NeXus session containers (.bcd/.mbd).

Background
----------
BioTrace+ can export recordings to open formats like EDF/BDF or ASCII. Those exported
files are supported directly by this repository.

However, some exported/backup `.bcd`/`.mbd` files are *ZIP containers* that already
include an embedded EDF/BDF/ASCII export. For those cases, this script can:

  1) Extract the best embedded export to a temp folder (or user-specified folder)
  2) Launch `qeeg_nf_cli` with `--input <extracted>` and `--outdir <outdir>`
  3) Pass through the remaining CLI args you provide (metric, baseline, realtime, etc.)

This does **not** implement the proprietary BioTrace container format; it only works
for the ZIP-like containers (best effort).

Usage
-----
List embedded items:

  python3 scripts/biotrace_run_nf.py --container session.bcd --list

Run neurofeedback from a container (note the `--`):

  python3 scripts/biotrace_run_nf.py --container session.bcd --outdir out_nf -- \
    --metric alpha/beta:Pz --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
    --realtime --export-bandpowers --flush-csv

Tips
----
- You can view the outputs live with:
    python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --open
- For tablets on the same LAN:
    python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --host 0.0.0.0 --allow-remote
    (open the printed Kiosk (LAN) URL)

"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _scripts_dir() -> Path:
    return _repo_root() / "scripts"


def _import_biotrace_extract():
    # Import scripts/biotrace_extract_container.py as a normal module
    scripts = _scripts_dir()
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    import biotrace_extract_container  # type: ignore

    return biotrace_extract_container


def _default_nf_cli_path() -> Optional[str]:
    root = _repo_root()
    # Common build locations in this repo.
    candidates = [
        root / "build" / "qeeg_nf_cli",
        root / "build" / "Release" / "qeeg_nf_cli",
        root / "build" / "Debug" / "qeeg_nf_cli",
    ]
    if os.name == "nt":
        candidates = [Path(str(p) + ".exe") for p in candidates] + candidates
    for p in candidates:
        if p.exists() and p.is_file():
            return str(p)
    # Fall back to PATH
    return shutil.which("qeeg_nf_cli")


def _strip_overrides(args: List[str]) -> List[str]:
    """Remove any user-provided --input/--outdir from passthrough args.

    This script owns those flags, so they cannot accidentally point somewhere else.
    """
    out: List[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--":
            i += 1
            continue
        if a.startswith("--input=") or a.startswith("--outdir="):
            i += 1
            continue
        if a in ("--input", "--outdir"):
            # Skip flag + value if present.
            i += 2 if (i + 1) < len(args) else 1
            continue
        out.append(a)
        i += 1
    return out


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Extract a ZIP-like BioTrace+ .bcd/.mbd container and run qeeg_nf_cli in one step.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--container", required=True, help="Path to BioTrace+ session container (.bcd/.mbd)")
    ap.add_argument("--outdir", default="", help="Output directory for qeeg_nf_cli (required unless --list)")
    ap.add_argument(
        "--extract-dir",
        default="",
        help="Directory to place extracted export (default: a temporary folder that is deleted after qeeg_nf_cli exits)",
    )
    ap.add_argument("--keep-extracted", action="store_true", help="Do not delete temporary extracted export folder")
    ap.add_argument("--nf-cli", default="", help="Path to qeeg_nf_cli (default: try ./build/qeeg_nf_cli, then PATH)")
    ap.add_argument("--list", action="store_true", help="List container contents and exit")
    ap.add_argument(
        "nf_args",
        nargs=argparse.REMAINDER,
        help="Arguments forwarded to qeeg_nf_cli (prefix with --; use a -- separator before them)",
    )
    return ap.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    ns = parse_args(argv)
    container = Path(ns.container)
    if not container.exists():
        print(f"Error: container does not exist: {container}", file=sys.stderr)
        return 2

    bt = _import_biotrace_extract()

    if ns.list:
        # Print a friendly listing and exit.
        items = bt.list_contents(container)
        if not items:
            print("(no listable contents; file may not be ZIP-like)")
            return 0
        for i, it in enumerate(items):
            name = it.get("name", "")
            size = it.get("size", None)
            mtime = it.get("mtime", None)
            extra = []
            if size is not None:
                extra.append(f"{size} bytes")
            if mtime:
                extra.append(str(mtime))
            extra_s = ("; " + ", ".join(extra)) if extra else ""
            print(f"{i+1:02d}. {name}{extra_s}")
        return 0

    if not ns.outdir:
        print("Error: --outdir is required unless --list is used", file=sys.stderr)
        return 2

    outdir = Path(ns.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    # Determine where to extract.
    tmp_obj = None
    if ns.extract_dir:
        extract_dir = Path(ns.extract_dir)
        extract_dir.mkdir(parents=True, exist_ok=True)
    else:
        tmp_obj = tempfile.TemporaryDirectory(prefix="qeeg_biotrace_extract_")
        extract_dir = Path(tmp_obj.name)

    try:
        extracted = bt.extract_best_export(container, extract_dir)
    except Exception as e:
        print(f"Error: failed to extract embedded export from {container}: {e}", file=sys.stderr)
        if tmp_obj is not None:
            tmp_obj.cleanup()
        return 3

    nf_cli = ns.nf_cli.strip() or _default_nf_cli_path()
    if not nf_cli:
        print(
            "Error: could not find qeeg_nf_cli. Build it first (see README), or pass --nf-cli /path/to/qeeg_nf_cli.",
            file=sys.stderr,
        )
        if tmp_obj is not None:
            tmp_obj.cleanup()
        return 4

    passthrough = _strip_overrides(list(ns.nf_args))
    cmd = [nf_cli, "--input", str(extracted), "--outdir", str(outdir)] + passthrough

    print("Running:")
    print("  " + " ".join([str(c) for c in cmd]))
    sys.stdout.flush()

    try:
        proc = subprocess.run(cmd)
        rc = int(proc.returncode)
    except FileNotFoundError:
        print(f"Error: qeeg_nf_cli not found/executable: {nf_cli}", file=sys.stderr)
        rc = 5

    # Cleanup temp extraction.
    if tmp_obj is not None and (not ns.keep_extracted):
        tmp_obj.cleanup()
    elif tmp_obj is not None and ns.keep_extracted:
        print(f"Kept extracted files in: {extract_dir}")

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
