#!/usr/bin/env python3
"""biotrace_run_nf.py

One-step helper for Mind Media BioTrace+ / NeXus session containers (ZIP-like .bcd/.mbd/.m2k/.zip).

Background
----------
BioTrace+ can export recordings to open formats like EDF/BDF or ASCII. Those exported
files are supported directly by this repository.

However, some exported/backup `.bcd`/`.mbd`/`.m2k` files are *ZIP containers* that already
include an embedded EDF/BDF/ASCII export. For those cases, this script can:

  1) Extract the best embedded export to a temp folder (or user-specified folder)
  2) Launch `qeeg_nf_cli` with `--input <extracted>` and `--outdir <outdir>`
  3) Pass through the remaining CLI args you provide (metric, baseline, realtime, etc.)

This does **not** implement the proprietary BioTrace container format; it only works
for the ZIP-like containers (best effort).

Usage
-----
List embedded items:

  python3 scripts/biotrace_run_nf.py --container session.m2k --list

Run neurofeedback from a container:

  python3 scripts/biotrace_run_nf.py --container session.m2k --outdir out_nf \
    --metric alpha/beta:Pz --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
    --realtime --export-bandpowers --flush-csv

If the container has multiple embedded recordings, you can pick one explicitly:

  # Use the 2nd candidate shown by --list
  python3 scripts/biotrace_run_nf.py --container session.m2k --outdir out_nf --select 2 \
    --metric alpha:Pz --window 2.0

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
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import List, Optional


# Best-effort: avoid polluting the source tree with __pycache__ when this script
# is invoked from CI/CTest.
sys.dont_write_bytecode = True


# JSON Schema identifier for --list-json output (matches schemas/biotrace_run_nf_list.schema.json).
SCHEMA_LIST_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_run_nf_list.schema.json"
)


def _cleanup_stale_pycache() -> None:
    """Remove bytecode cache files for this script (best effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            # Typical filename:
            #   biotrace_run_nf.cpython-311.pyc
            if fn.startswith("biotrace_run_nf.") and fn.endswith(".pyc"):
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
    """Try to locate a built qeeg_nf_cli binary (best effort).

    Priority:
      1) QEEG_NF_CLI env var (if set to an existing file)
      2) Common build folders in this repo (including CMake preset build dirs)
      3) PATH lookup (shutil.which)

    Note: CMakePresets.json in this repo uses build/* subdirectories (e.g. build/release),
    so we check those explicitly in addition to legacy build/ locations.
    """

    # Allow callers / CI wrappers to override the lookup in a stable way.
    env_path = os.environ.get("QEEG_NF_CLI", "").strip()
    if env_path:
        p = Path(env_path)
        if p.exists() and p.is_file():
            return str(p)

    root = _repo_root()

    # Executable name varies by platform.
    exe = "qeeg_nf_cli.exe" if os.name == "nt" else "qeeg_nf_cli"

    # Common build locations in this repo (ordered by usefulness / typical presets).
    build_root = root / "build"
    build_dirs = [
        build_root,  # legacy / manual builds
        build_root / "release",
        build_root / "lto-release",
        build_root / "debug",
        build_root / "asan",
        build_root / "shared-release",
        build_root / "shared-debug",
        build_root / "package-release",
    ]

    # Multi-config generators (MSVC) may place binaries under <dir>/<Config>/.
    configs = ["Release", "Debug", "RelWithDebInfo", "MinSizeRel"]

    candidates: List[Path] = []
    for d in build_dirs:
        candidates.append(d / exe)
        for cfg in configs:
            candidates.append(d / cfg / exe)

    # Legacy capitalized dirs sometimes used by IDE builds.
    for cfg in configs:
        candidates.append(build_root / cfg / exe)

    # Windows convenience: if we didn't find an .exe, allow a .cmd/.bat wrapper with the same base name.
    if os.name == "nt":
        for d in build_dirs + [build_root]:
            for ext in (".cmd", ".bat"):
                candidates.append(d / ("qeeg_nf_cli" + ext))
                for cfg in configs:
                    candidates.append(d / cfg / ("qeeg_nf_cli" + ext))

    for p in candidates:
        try:
            if p.exists() and p.is_file():
                return str(p)
        except Exception:
            # Defensive: ignore permission/path errors and keep searching.
            pass

    # Last resort: scan build/ for the binary name (can help when the build dir is custom).
    try:
        if build_root.exists():
            # Prefer deterministic ordering (string sort).
            matches = sorted([p for p in build_root.rglob(exe) if p.is_file()], key=lambda x: str(x))
            if matches:
                return str(matches[0])
    except Exception:
        pass

    # Fall back to PATH.
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
        description="Extract a ZIP-like BioTrace+/NeXus container (.bcd/.mbd/.m2k/.zip) and run qeeg_nf_cli in one step.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        allow_abbrev=False,
        epilog=(
            "Any additional arguments not recognized by this wrapper are forwarded to qeeg_nf_cli. "
            "You may optionally insert a `--` separator before the forwarded arguments."
        ),
    )
    ap.add_argument(
        "--container",
        required=True,
        help="Path to a ZIP-like BioTrace+/NeXus session container (.bcd/.mbd/.m2k/.zip) or a directory",
    )
    ap.add_argument(
        "--outdir",
        default="",
        help="Output directory for qeeg_nf_cli (required unless --list/--list-json)",
    )
    ap.add_argument(
        "--extract-dir",
        default="",
        help="Directory to place extracted export (default: a temporary folder that is deleted after qeeg_nf_cli exits)",
    )
    ap.add_argument(
        "--keep-extracted",
        action="store_true",
        help="Do not delete temporary extracted export folder",
    )
    ap.add_argument(
        "--prefer",
        default="",
        help=(
            "Comma-separated extension preference order used to pick the embedded recording "
            "(e.g. .edf,.bdf,.csv). Default: extractor defaults."
        ),
    )
    ap.add_argument(
        "--select",
        default="",
        help=(
            "Select which embedded recording to run on when the container has multiple candidates. "
            "Accepts 1-based index from --list, exact name, substring, or glob pattern (case-insensitive)."
        ),
    )
    ap.add_argument(
        "--nf-cli",
        default="",
        help="Path to qeeg_nf_cli (default: auto-detect in ./build/* preset dirs, env QEEG_NF_CLI, then PATH)",
    )

    list_group = ap.add_mutually_exclusive_group()
    list_group.add_argument("--list", action="store_true", help="List container contents and exit")
    list_group.add_argument("--list-json", action="store_true", help="List container contents as JSON and exit")

    # parse_known_args() lets callers pass qeeg_nf_cli flags directly without requiring a '--' separator.
    ns, unknown = ap.parse_known_args(argv)
    setattr(ns, "nf_args", list(unknown))
    return ns


def main(argv: Optional[List[str]] = None) -> int:
    ns = parse_args(argv)
    container = Path(ns.container)
    if not container.exists():
        print(f"Error: container does not exist: {container}", file=sys.stderr)
        return 2

    bt = _import_biotrace_extract()

    # Optional preference override.
    prefer_exts = getattr(bt, "DEFAULT_PREFER", None)
    if ns.prefer:
        parse_fn = getattr(bt, "_parse_prefer_list", None)
        if callable(parse_fn):
            prefer_exts = parse_fn(ns.prefer)
        else:
            prefer_exts = [p.strip().lower() for p in ns.prefer.split(",") if p.strip()]

    # Validate container type early for clearer errors.
    #
    # For --list-json, we still emit a machine-readable JSON object on error
    # (matching the extractor script's behavior), so GUIs and wrappers can
    # handle failures without scraping stderr.
    if not container.is_dir() and not zipfile.is_zipfile(str(container)):
        msg = (
            'Error: container is not a ZIP-like session container.\n'
            'Some BioTrace+/NeXus exports are proprietary (e.g., BCD/MBD/M2K session backups) and cannot be opened here.\n'
            'Please export an open format from BioTrace+ (EDF/BDF/ASCII) and run qeeg_nf_cli on that file.\n'
            f'Container: {container}'
        )
        if getattr(ns, "list_json", False):
            json.dump(
                {
                    "$schema": SCHEMA_LIST_ID,
                    "input": str(container),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "candidates": [],
                    "error": msg,
                },
                sys.stdout,
                indent=2,
                sort_keys=True,
            )
            sys.stdout.write("\n")
        else:
            print(msg, file=sys.stderr)
        return 2


    if getattr(ns, "list_json", False):
        # Machine-friendly listing (stable JSON).
        try:
            if hasattr(bt, "list_contents_jsonable") and callable(getattr(bt, "list_contents_jsonable")):
                cand = bt.list_contents_jsonable(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents_jsonable(container)
            else:
                raw = bt.list_contents(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents(container)
                conv = getattr(bt, "candidates_to_jsonable", None)
                cand = conv(raw) if callable(conv) else raw

            obj = {
                "$schema": SCHEMA_LIST_ID,
                "input": str(container),
                "prefer_exts": list(prefer_exts) if prefer_exts else [],
                "candidates": cand,
            }
            json.dump(obj, sys.stdout, indent=2, sort_keys=True)
            sys.stdout.write("\n")
            return 0
        except Exception as e:
            json.dump(
                {
                    "$schema": SCHEMA_LIST_ID,
                    "input": str(container),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "candidates": [],
                    "error": str(e),
                },
                sys.stdout,
                indent=2,
                sort_keys=True,
            )
            sys.stdout.write("\n")
            return 2

    if ns.list:
        # Print a friendly listing and exit.
        items = bt.list_contents(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents(container)
        if not items:
            print("(no listable contents; file may not be ZIP-like)")
            return 0
        for i, it in enumerate(items):
            name = it.get("name", "")
            ext = str(it.get("ext", "") or "")
            disp_name = name
            if ext and (not name.lower().endswith(ext.lower())):
                disp_name = f"{name} [as {ext}]"
            size = it.get("size", None)
            mtime = it.get("mtime", None)
            extra = []
            if size is not None:
                extra.append(f"{size} bytes")
            if mtime:
                extra.append(str(mtime))
            extra_s = ("; " + ", ".join(extra)) if extra else ""
            print(f"{i+1:02d}. {disp_name}{extra_s}")
        return 0

    if not ns.outdir:
        print("Error: --outdir is required unless --list/--list-json is used", file=sys.stderr)
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
        if ns.select:
            # Selection overrides the "best export" heuristic.
            if prefer_exts:
                extracted = bt.extract_selected_export(container, extract_dir, ns.select, prefer_exts=prefer_exts)
            else:
                extracted = bt.extract_selected_export(container, extract_dir, ns.select)
        else:
            if prefer_exts:
                extracted = bt.extract_best_export(container, extract_dir, prefer_exts=prefer_exts)
            else:
                extracted = bt.extract_best_export(container, extract_dir)
    except Exception as e:
        mode = "select/extract" if ns.select else "extract"
        print(f"Error: failed to {mode} embedded export from {container}: {e}", file=sys.stderr)
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
