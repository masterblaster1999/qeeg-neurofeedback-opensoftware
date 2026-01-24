#!/usr/bin/env python3
"""biotrace_extract_container.py

Best-effort extractor for Mind Media BioTrace+ / NeXus session containers.

BioTrace+ can export sessions in formats like `.bcd` and `.mbd`. These are
*container* formats used by BioTrace+ and are not guaranteed to be directly
readable by third-party tools.

In practice, some `.bcd`/`.mbd` files are ZIP containers that include one or
more open-format recordings (e.g., `.edf`, `.bdf`, ASCII/CSV exports). When this
is the case, this script can extract those recordings so they can be used as
input to the tools in this repository.

If your container is not a ZIP (or it does not contain any recognized open
formats), you will need to export your session from BioTrace+ to an open format
(EDF/BDF/ASCII) first.

No third-party dependencies.

Examples:

  # List candidate recordings inside a container
  python3 scripts/biotrace_extract_container.py --input session.bcd --list

  # Extract the "best" recording by preference order (EDF > BDF > BrainVision > CSV)
  python3 scripts/biotrace_extract_container.py --input session.bcd --outdir extracted --print

  # Extract all recognized recordings
  python3 scripts/biotrace_extract_container.py --input session.bcd --outdir extracted --all
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


DEFAULT_PREFER = [
    ".edf",
    ".edf+",
    ".rec",  # some exporters use .rec for EDF data
    ".bdf",
    ".bdf+",
    ".vhdr",  # BrainVision header
    ".csv",
    ".tsv",
    ".txt",
    ".asc",
]


def _norm_ext(x: str) -> str:
    x = (x or "").strip().lower()
    if not x:
        return ""
    if not x.startswith("."):
        x = "." + x
    return x


def _is_recognized_recording(name: str, prefer_exts: Sequence[str]) -> bool:
    low = (name or "").lower()
    for e in prefer_exts:
        if low.endswith(e):
            return True
    return False


def _safe_join(base: Path, rel: str) -> Path:
    # Prevent Zip Slip: ensure the resolved path stays within base.
    rel_path = Path(rel)
    # ZipInfo filenames always use '/' separators.
    # Convert to a Path, then normalize.
    out = (base / rel_path).resolve()
    base_res = base.resolve()
    try:
        out.relative_to(base_res)
    except Exception:
        raise ValueError(f"Refusing to extract unsafe path: {rel}")
    return out


@dataclass(frozen=True)
class Candidate:
    name: str
    ext: str
    size: int


def _collect_zip_candidates(zf: zipfile.ZipFile, prefer_exts: Sequence[str]) -> List[Candidate]:
    out: List[Candidate] = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = info.filename
        if not name or name.endswith("/"):
            continue
        if not _is_recognized_recording(name, prefer_exts):
            continue
        low = name.lower()
        ext = ""
        for e in prefer_exts:
            if low.endswith(e):
                ext = e
                break
        out.append(Candidate(name=name, ext=ext, size=int(getattr(info, "file_size", 0) or 0)))
    return out


def _collect_dir_candidates(root: Path, prefer_exts: Sequence[str]) -> List[Candidate]:
    out: List[Candidate] = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        rel = str(p.relative_to(root))
        if not _is_recognized_recording(rel, prefer_exts):
            continue
        ext = _norm_ext(p.suffix)
        out.append(Candidate(name=rel.replace(os.sep, "/"), ext=ext, size=int(p.stat().st_size)))
    return out


def _rank_candidates(cands: Iterable[Candidate], prefer_exts: Sequence[str]) -> List[Candidate]:
    pref_rank = {e: i for i, e in enumerate(prefer_exts)}

    def key(c: Candidate) -> Tuple[int, int, str]:
        # Prefer earlier extensions. For same extension, prefer larger files (likely the main recording).
        return (pref_rank.get(c.ext, 10_000), -int(c.size), c.name.lower())

    return sorted(list(cands), key=key)


def _ensure_outdir(outdir: Path) -> None:
    outdir.mkdir(parents=True, exist_ok=True)


def _extract_zip_member(zf: zipfile.ZipFile, member: str, outdir: Path) -> Path:
    target = _safe_join(outdir, member)
    target.parent.mkdir(parents=True, exist_ok=True)
    with zf.open(member, "r") as src, target.open("wb") as dst:
        shutil.copyfileobj(src, dst, length=1024 * 1024)
    return target


def _copy_from_dir(root: Path, member: str, outdir: Path) -> Path:
    src = (root / Path(member)).resolve()
    # Safety: require src is under root.
    root_res = root.resolve()
    try:
        src.relative_to(root_res)
    except Exception:
        raise ValueError(f"Refusing to copy unsafe path: {member}")

    dst = _safe_join(outdir, member)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    return dst


def _parse_prefer_list(s: str) -> List[str]:
    if not s:
        return list(DEFAULT_PREFER)
    parts = [p.strip() for p in s.split(",")]
    out: List[str] = []
    for p in parts:
        e = _norm_ext(p)
        if not e:
            continue
        out.append(e)
    return out or list(DEFAULT_PREFER)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Extract open-format recordings from BioTrace+/NeXus .bcd/.mbd containers")
    ap.add_argument("--input", required=True, help="Path to .bcd/.mbd container, .zip, or a directory")
    ap.add_argument("--outdir", default="extracted", help="Output directory for extracted files (default: extracted)")
    ap.add_argument(
        "--prefer",
        default=",".join(DEFAULT_PREFER),
        help="Comma-separated extension preference order (default: EDF,BDF,BrainVision,CSV)",
    )
    ap.add_argument("--list", action="store_true", help="List candidate recordings and exit")
    ap.add_argument("--all", action="store_true", help="Extract all candidate recordings (default: extract best only)")
    ap.add_argument("--print", dest="print_paths", action="store_true", help="Print extracted path(s) to stdout")
    args = ap.parse_args(list(argv) if argv is not None else None)

    in_path = Path(args.input)
    outdir = Path(args.outdir)
    prefer_exts = _parse_prefer_list(args.prefer)

    if not in_path.exists():
        raise SystemExit(f"Input not found: {in_path}")

    # Directory mode.
    if in_path.is_dir():
        cands = _rank_candidates(_collect_dir_candidates(in_path, prefer_exts), prefer_exts)
        if args.list:
            if not cands:
                print("No recognized recordings found.")
                return 1
            for c in cands:
                print(f"{c.size:>10}  {c.name}")
            return 0

        if not cands:
            print("No recognized recordings found.")
            return 1

        _ensure_outdir(outdir)
        chosen = cands if args.all else [cands[0]]
        extracted: List[Path] = []
        for c in chosen:
            extracted.append(_copy_from_dir(in_path, c.name, outdir))

        if args.print_paths:
            for p in extracted:
                print(str(p))
        else:
            print(f"Extracted {len(extracted)} file(s) to: {outdir}")
            for p in extracted:
                print(f"  - {p}")
        return 0

    # Zip mode.
    if not zipfile.is_zipfile(str(in_path)):
        # Not a zip container. Provide a helpful message and exit.
        print(
            "Input is not a ZIP container (or is an unsupported container type).\n"
            "BioTrace+/NeXus .bcd/.mbd files are proprietary; if this extractor cannot open it,\n"
            "please export your session from BioTrace+ to an open format (EDF/BDF/ASCII) first.\n"
            f"Input: {in_path}"
        )
        return 2

    with zipfile.ZipFile(str(in_path), "r") as zf:
        cands = _rank_candidates(_collect_zip_candidates(zf, prefer_exts), prefer_exts)
        if args.list:
            if not cands:
                print("No recognized recordings found inside the container.")
                return 1
            for c in cands:
                print(f"{c.size:>10}  {c.name}")
            return 0

        if not cands:
            print("No recognized recordings found inside the container.")
            return 1

        _ensure_outdir(outdir)
        chosen = cands if args.all else [cands[0]]
        extracted: List[Path] = []
        for c in chosen:
            extracted.append(_extract_zip_member(zf, c.name, outdir))

    if args.print_paths:
        for p in extracted:
            print(str(p))
    else:
        print(f"Extracted {len(extracted)} file(s) to: {outdir}")
        for p in extracted:
            print(f"  - {p}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
