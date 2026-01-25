#!/usr/bin/env python3
"""biotrace_extract_container.py

Best-effort extractor for Mind Media BioTrace+ / NeXus session containers.

BioTrace+ can export sessions in formats like `.bcd`, `.mbd`, and sometimes `.m2k`. These are
*container* formats used by BioTrace+ and are not guaranteed to be directly
readable by third-party tools.

In practice, some `.bcd`/`.mbd`/`.m2k` files are ZIP containers that include one or
more open-format recordings (e.g., `.edf`, `.bdf`, BrainVision `.vhdr`/`.eeg`/`.vmrk`,
ASCII/CSV exports). When this is the case, this script can extract those recordings
so they can be used as input to the tools in this repository.

Note: BrainVision exports consist of multiple files. When a `.vhdr` is selected,
this script also extracts the referenced data/marker files (e.g., `.eeg`, `.vmrk`) so
the result remains readable.

If your container is not a ZIP (or it does not contain any recognized open
formats), you will need to export your session from BioTrace+ to an open format
(EDF/BDF/ASCII) first.

No third-party dependencies.

Examples:

  # List candidate recordings inside a container
  python3 scripts/biotrace_extract_container.py --input session.m2k --list

  # Alias for --input (for convenience):
  python3 scripts/biotrace_extract_container.py --container session.m2k --list

  # Extract the "best" recording by preference order (EDF > BDF > BrainVision > CSV)
  python3 scripts/biotrace_extract_container.py --input session.m2k --outdir extracted --print

  # If the container has multiple candidates, pick a specific one:
  #   - by 1-based index shown in --list
  #   - by exact member name (case-insensitive)
  #   - by substring (case-insensitive; must be unambiguous)
  #   - by glob pattern (case-insensitive), e.g. "*.vhdr" or "exports/*_raw.csv"
  python3 scripts/biotrace_extract_container.py --input session.m2k --outdir extracted --select 2 --print
  python3 scripts/biotrace_extract_container.py --input session.m2k --outdir extracted --select "*.vhdr" --print

  # Extract all recognized recordings
  python3 scripts/biotrace_extract_container.py --input session.m2k --outdir extracted --all
"""

from __future__ import annotations

import argparse
import datetime as _dt
import fnmatch
import os
import shutil
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


DEFAULT_PREFER = [
    ".edf",
    ".edf+",
    ".rec",  # some exporters use .rec for EDF data
    ".bdf",
    ".bdf+",
    ".vhdr",  # BrainVision header
    ".csv",
    ".tsv",
    ".m2k",  # NeXus SD-card/exports may use .m2k (treat as ASCII/CSV)
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



def _looks_like_brainvision_vhdr_bytes(data: bytes) -> bool:
    """Return True if `data` looks like a BrainVision header (.vhdr) file.

    This uses best-effort decoding (including UTF-16 with or without a BOM),
    because some exporters save the header/marker files as "Unicode text" on
    Windows, or store them with non-.vhdr suffixes (e.g. .txt/.ini).
    """
    if not data:
        return False
    head = data[:8192]
    txt = _decode_text_best_effort(head)
    # Some decoding paths can leave embedded NULs; strip them for robust matching.
    low = txt.replace("\x00", "").lower()

    if "brain vision data exchange header file" in low:
        return True

    return ("[common infos]" in low) and ("datafile=" in low) and ("markerfile=" in low)

def _peek_zip_member_prefix(zf: zipfile.ZipFile, member: str, n: int = 4096) -> bytes:
    try:
        with zf.open(member, "r") as f:
            return f.read(n)
    except Exception:
        return b""


def _peek_file_prefix(p: Path, n: int = 4096) -> bytes:
    try:
        with p.open("rb") as f:
            return f.read(n)
    except Exception:
        return b""


def _effective_candidate_ext(name: str, ext: str, prefix: bytes) -> str:
    """Map some exported "text" containers to a more useful effective type.

    Real-world BioTrace+/NeXus containers sometimes store BrainVision headers with non-.vhdr
    extensions (e.g. .txt). We detect that and treat it as a .vhdr candidate.
    """
    ext = _norm_ext(ext)
    if ext == ".vhdr":
        return ext
    if ext in (".txt", ".asc", ".m2k", ".hdr", ".ini", ""):
        if _looks_like_brainvision_vhdr_bytes(prefix):
            return ".vhdr"
    return ext

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
    """A candidate open-format recording inside a container."""

    name: str
    ext: str
    size: int
    mtime: Optional[_dt.datetime] = None


def _zipinfo_mtime(info: zipfile.ZipInfo) -> Optional[_dt.datetime]:
    # ZipInfo.date_time is (Y, M, D, H, M, S) in local time.
    dt = getattr(info, "date_time", None)
    if not dt:
        return None
    try:
        return _dt.datetime(*dt)
    except Exception:
        return None


def _collect_zip_candidates(zf: zipfile.ZipFile, prefer_exts: Sequence[str]) -> List[Candidate]:
    out: List[Candidate] = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = info.filename
        if not name or name.endswith("/"):
            continue

        low = name.lower()
        recognized = _is_recognized_recording(name, prefer_exts)

        # Determine the most specific matching extension from the preference list.
        ext = ""
        if recognized:
            for e in prefer_exts:
                if low.endswith(e):
                    ext = e
                    break

            # Sniff for BrainVision headers that were exported with a non-.vhdr suffix.
            if ext in (".txt", ".asc", ".m2k", ".ini", ".hdr", ""):
                prefix = _peek_zip_member_prefix(zf, name)
                ext = _effective_candidate_ext(name, ext, prefix)
        else:
            # Special-case: some BrainVision exports store the header as .ini/.hdr.
            # Only treat these as candidates if the contents look like a BrainVision header.
            if low.endswith(".ini") or low.endswith(".hdr"):
                prefix = _peek_zip_member_prefix(zf, name)
                if _looks_like_brainvision_vhdr_bytes(prefix):
                    ext = ".vhdr"
                else:
                    continue
            else:
                continue

        out.append(
            Candidate(
                name=name,
                ext=ext,
                size=int(getattr(info, "file_size", 0) or 0),
                mtime=_zipinfo_mtime(info),
            )
        )
    return out



def _collect_dir_candidates(root: Path, prefer_exts: Sequence[str]) -> List[Candidate]:
    out: List[Candidate] = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        rel = str(p.relative_to(root))
        low = rel.lower()

        recognized = _is_recognized_recording(rel, prefer_exts)

        ext = ""
        if recognized:
            for e in prefer_exts:
                if low.endswith(e):
                    ext = e
                    break

            if ext in (".txt", ".asc", ".m2k", ".ini", ".hdr", ""):
                ext = _effective_candidate_ext(rel, ext, _peek_file_prefix(p))
        else:
            # Special-case: some BrainVision exports store the header as .ini/.hdr.
            if low.endswith(".ini") or low.endswith(".hdr"):
                if _looks_like_brainvision_vhdr_bytes(_peek_file_prefix(p)):
                    ext = ".vhdr"
                else:
                    continue
            else:
                continue

        try:
            mt = _dt.datetime.fromtimestamp(p.stat().st_mtime)
        except Exception:
            mt = None
        out.append(Candidate(name=rel.replace(os.sep, "/"), ext=ext, size=int(p.stat().st_size), mtime=mt))
    return out



def _rank_candidates(cands: Iterable[Candidate], prefer_exts: Sequence[str]) -> List[Candidate]:
    pref_rank = {e: i for i, e in enumerate(prefer_exts)}

    def key(c: Candidate) -> Tuple[int, int, str]:
        # Prefer earlier extensions. For same extension, prefer larger files (likely the main recording).
        return (pref_rank.get(c.ext, 10_000), -int(c.size), c.name.lower())

    return sorted(list(cands), key=key)


def _format_candidate_list(cands: Sequence[Candidate]) -> str:
    lines: List[str] = []
    for i, c in enumerate(cands):
        lines.append(f"{i+1:02d}. {int(c.size):>10}  {c.name}")
    return "\n".join(lines)


def _is_glob_pattern(s: str) -> bool:
    """Return True if `s` looks like a glob pattern.

    We treat '*', '?', and '[' as glob metacharacters (same semantics as
    :func:`fnmatch.fnmatch`).
    """
    s = s or ""
    return any(ch in s for ch in ("*", "?", "["))


def select_candidate(cands: Sequence[Candidate], select: str) -> Candidate:
    # Select a candidate from a ranked list.
    #
    # Supported selectors:
    #   - 1-based integer index (as shown in --list)
    #   - exact member name (case-insensitive)
    #   - substring (case-insensitive; must be unambiguous)
    #   - glob pattern (case-insensitive)
    #   - extension selectors: ".vhdr" or "*.vhdr" match the effective Candidate.ext
    if not cands:
        raise RuntimeError("No candidates available")

    s = (select or "").strip()
    if not s:
        return cands[0]

    # 1) Index.
    try:
        idx = int(s, 10)
    except Exception:
        idx = None
    if idx is not None:
        if idx < 1 or idx > len(cands):
            raise RuntimeError(
                f"--select index out of range: {idx} (valid: 1..{len(cands)})\n" + _format_candidate_list(cands)
            )
        return cands[idx - 1]

    needle = s.casefold()

    def _no_seps(x: str) -> bool:
        return ("/" not in x) and ("\\" not in x)

    # 2) Extension-only selector (e.g. ".vhdr").
    if needle.startswith(".") and _no_seps(needle):
        want = _norm_ext(needle)
        ms = [c for c in cands if (c.ext or "").casefold() == want.casefold()]
        if len(ms) == 1:
            return ms[0]
        if len(ms) > 1:
            raise RuntimeError(f"--select extension is ambiguous: {select}\n" + _format_candidate_list(ms))

    # 3) Glob pattern.
    if _is_glob_pattern(s):
        # Special case: patterns like "*.vhdr" act as extension filters.
        if needle.startswith("*.") and _no_seps(needle):
            want = _norm_ext(needle[1:])
            ms = [c for c in cands if (c.ext or "").casefold() == want.casefold()]
            if len(ms) == 1:
                return ms[0]
            if not ms:
                raise RuntimeError(f"--select pattern matched no candidates: {select}\n" + _format_candidate_list(cands))
            raise RuntimeError(
                f"--select pattern is ambiguous ({len(ms)} matches): {select}\n" + _format_candidate_list(ms)
            )

        # Name glob.
        ms = [c for c in cands if fnmatch.fnmatch(c.name.casefold(), needle)]
        if len(ms) == 1:
            return ms[0]
        if not ms:
            raise RuntimeError(f"--select pattern matched no candidates: {select}\n" + _format_candidate_list(cands))
        raise RuntimeError(f"--select pattern is ambiguous ({len(ms)} matches): {select}\n" + _format_candidate_list(ms))

    # 4) Exact name.
    exact = [c for c in cands if c.name.casefold() == needle]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise RuntimeError(f"--select name is ambiguous: {select}\n" + _format_candidate_list(exact))

    # 5) Substring (must be unambiguous).
    subs = [c for c in cands if needle in c.name.casefold()]
    if len(subs) == 1:
        return subs[0]
    if not subs:
        raise RuntimeError(f"--select did not match any candidates: {select}\n" + _format_candidate_list(cands))
    raise RuntimeError(f"--select is ambiguous ({len(subs)} matches): {select}\n" + _format_candidate_list(subs))



def _sniff_utf16_no_bom(data: bytes, max_bytes: int = 4096) -> Optional[str]:
    """Heuristically detect UTF-16LE/BE for text without a BOM.

    Many ASCII-heavy UTF-16 files have NUL bytes in either the odd (LE) or even (BE)
    positions. We use a conservative ratio-based heuristic.

    Returns an encoding name ("utf-16-le" or "utf-16-be") or None.
    """
    if not data:
        return None
    head = data[:max_bytes]
    # Keep the minimum sample fairly small so we can still detect tiny UTF-16
    # header/marker files (or test snippets) that omit a BOM.
    if len(head) < 16:
        return None

    even_zeros = 0
    odd_zeros = 0
    even_n = 0
    odd_n = 0

    for i, b in enumerate(head):
        if (i & 1) == 0:
            even_n += 1
            if b == 0:
                even_zeros += 1
        else:
            odd_n += 1
            if b == 0:
                odd_zeros += 1

    total_zero_ratio = (even_zeros + odd_zeros) / max(1, len(head))
    even_zero_ratio = even_zeros / max(1, even_n)
    odd_zero_ratio = odd_zeros / max(1, odd_n)

    # Require many NULs overall AND a strong asymmetry between even/odd positions.
    if total_zero_ratio >= 0.20:
        if odd_zero_ratio >= 0.40 and even_zero_ratio <= 0.10:
            return "utf-16-le"
        if even_zero_ratio >= 0.40 and odd_zero_ratio <= 0.10:
            return "utf-16-be"

    return None


def _decode_text_best_effort(data: bytes) -> str:
    """Decode text from a container member using best-effort heuristics.

    BrainVision header/marker files are typically UTF-8/ANSI text, but in practice
    you may encounter:
      - UTF-8 with BOM
      - UTF-16 (LE/BE) with BOM
      - UTF-16 (LE/BE) without a BOM ("Unicode text" exports on some systems)
      - Windows-1252 / Latin-1

    This function tries to decode robustly and never raises; it returns a string
    with replacement characters for undecodable sequences.
    """
    if not data:
        return ""

    # BOM sniffing.
    if data.startswith(b"\xef\xbb\xbf"):
        try:
            return data.decode("utf-8-sig", errors="replace")
        except Exception:
            pass

    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        # Decode with BOM-aware utf-16.
        try:
            return data.decode("utf-16", errors="replace")
        except Exception:
            pass

    # Heuristic UTF-16 without BOM.
    enc16 = _sniff_utf16_no_bom(data)
    if enc16:
        try:
            return data.decode(enc16, errors="replace")
        except Exception:
            pass

    for enc in ("utf-8", "cp1252", "latin-1"):
        try:
            return data.decode(enc, errors="replace")
        except Exception:
            continue
    return data.decode("latin-1", errors="replace")

def _normalize_relpath(ref: str) -> Optional[str]:
    """Normalize a path reference from an INI-style file.

    Returns a forward-slash path (may include ".." segments), or None if the
    reference is empty or clearly absolute/unsafe.

    Notes
    -----
    - BrainVision .vhdr/.vmrk references are *usually* relative paths. Some
      exports use Windows-style backslashes, so we normalize '\' -> '/'.
    - We do **not** reject ".." here; instead, we collapse and validate ".."
      safely when joining against a base member path.
    """
    if ref is None:
        return None

    s = (ref or "").strip().strip('"').strip("'")
    if not s:
        return None

    # Normalize Windows-style separators.
    s = s.replace("\\", "/")

    # Trim leading ./ sequences.
    while s.startswith("./"):
        s = s[2:]

    # Reject absolute paths or UNC paths.
    if s.startswith("/") or s.startswith("//"):
        return None

    # Reject Windows drive-letter paths like "C:/..." or "D:...".
    if len(s) >= 2 and s[1] == ":" and s[0].isalpha():
        return None

    return Path(s).as_posix()


def _posix_join_and_normalize(base_posix: str, rel_posix: str) -> Optional[str]:
    """Safely join base+rel (POSIX) and collapse '.'/'..' without escaping root.

    Returns a normalized POSIX path relative to the container root, or None if
    the join would escape above the root.
    """
    if not rel_posix:
        return None

    base = PurePosixPath(base_posix or ".")
    rel = PurePosixPath(rel_posix)

    # Join first, then normalize with a manual stack so we can detect escaping.
    joined = base / rel
    parts: List[str] = []
    for part in joined.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                return None
            parts.pop()
            continue
        parts.append(part)

    return "/".join(parts)



def _parse_ini_kv(text: str) -> Dict[str, str]:
    """Parse a simple INI-style key=value file (case-insensitive keys)."""
    out: Dict[str, str] = {}
    for raw in (text or "").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(";"):
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        key = (k or "").strip().lower()
        val = (v or "").strip()
        if not key:
            continue
        out[key] = val
    return out


def _brainvision_related_members(vhdr_member: str, vhdr_text: str) -> List[str]:
    """Return referenced companion members for a BrainVision .vhdr member.

    BrainVision datasets are typically a triplet:
      - header:  *.vhdr
      - markers: *.vmrk
      - data:    *.eeg (or sometimes *.dat)

    The .vhdr file contains relative references like:
      DataFile=recording.eeg
      MarkerFile=recording.vmrk

    Some exporters use backslashes and/or ".." path segments. We normalize and
    safely resolve those references relative to the .vhdr member directory.
    """
    refs = _parse_ini_kv(vhdr_text)
    base_posix = Path(vhdr_member).parent.as_posix()

    out: List[str] = []
    for k in ("datafile", "markerfile"):
        v = refs.get(k, "")
        rel = _normalize_relpath(v)
        if not rel:
            continue
        mem = _posix_join_and_normalize(base_posix, rel)
        if mem and mem != vhdr_member:
            out.append(mem)
    return out



def _brainvision_related_member_map(vhdr_member: str, vhdr_text: str) -> Dict[str, str]:
    """Return referenced companion members for a BrainVision .vhdr member as a mapping.

    Keys are lower-case INI keys ('datafile', 'markerfile'), values are member
    paths (POSIX) relative to the container root, resolved relative to the
    .vhdr member's directory.

    This function also tolerates Windows-style backslashes and ".." path
    segments (as long as they do not escape the container root).
    """
    refs = _parse_ini_kv(vhdr_text)
    base_posix = Path(vhdr_member).parent.as_posix()

    out: Dict[str, str] = {}
    for k in ("datafile", "markerfile"):
        v = refs.get(k, "")
        rel = _normalize_relpath(v)
        if not rel:
            continue
        mem = _posix_join_and_normalize(base_posix, rel)
        if mem and mem != vhdr_member:
            out[k] = mem
    return out



def _posix_relpath(from_dir: Path, to_path: Path) -> str:
    """Return a forward-slash relative path from from_dir to to_path."""
    try:
        rel = os.path.relpath(str(to_path), str(from_dir))
    except Exception:
        rel = str(to_path)
    # Normalize platform separators to POSIX (BrainVision expects '/' or bare filenames).
    rel = rel.replace(os.sep, "/")
    return rel


def _detect_file_encoding_for_rewrite(data: bytes) -> Tuple[str, bytes]:
    """Detect encoding + BOM prefix to preserve when rewriting INI-style files.

    We preserve BOMs when present, and also detect UTF-16 without BOM (common for
    "Unicode text" exports).
    """
    if data.startswith(b"\xff\xfe"):
        return "utf-16-le", b"\xff\xfe"
    if data.startswith(b"\xfe\xff"):
        return "utf-16-be", b"\xfe\xff"
    if data.startswith(b"\xef\xbb\xbf"):
        return "utf-8", b"\xef\xbb\xbf"

    enc16 = _sniff_utf16_no_bom(data)
    if enc16:
        return enc16, b""

    return "utf-8", b""

def _rewrite_ini_kv_inplace(path: Path, replacements: Dict[str, str]) -> bool:
    """Rewrite key=value entries in an INI-style file (case-insensitive keys).

    Only updates keys present in `replacements` (keys compared as lower-case).
    Returns True if the file content changed.
    """
    try:
        data = path.read_bytes()
    except Exception:
        return False

    enc, bom = _detect_file_encoding_for_rewrite(data)
    text = _decode_text_best_effort(data)
    if not text:
        return False

    lines = text.splitlines(True)
    out_lines: List[str] = []
    changed = False

    for line in lines:
        base = line.rstrip("\r\n")
        suffix = line[len(base):]  # preserve original newline(s) if present

        stripped = base.lstrip()
        if not stripped or stripped.startswith(";"):
            out_lines.append(line)
            continue

        eq = base.find("=")
        if eq < 0:
            out_lines.append(line)
            continue

        key = (base[:eq] or "").strip().lower()
        if not key or key not in replacements:
            out_lines.append(line)
            continue

        # Preserve whitespace after '='.
        after = base[eq + 1 :]
        ws_len = 0
        while ws_len < len(after) and after[ws_len].isspace():
            ws_len += 1
        ws = after[:ws_len]

        new_val = replacements[key]
        new_line = base[: eq + 1] + ws + new_val + suffix
        if new_line != line:
            changed = True
        out_lines.append(new_line)

    if not changed:
        return False

    new_text = "".join(out_lines)
    try:
        raw = new_text.encode(enc)
    except Exception:
        raw = new_text.encode("utf-8")
        bom = b""

    try:
        path.write_bytes(bom + raw)
    except Exception:
        return False
    return True


def _patch_brainvision_refs(main_vhdr: Path, data_path: Optional[Path], marker_path: Optional[Path]) -> None:
    """Best-effort patch BrainVision references after extraction/copy.

    This helps keep extracted datasets readable on case-sensitive file systems
    when the container stores files with different casing than the references
    inside .vhdr/.vmrk.
    """
    # Patch the header.
    repl: Dict[str, str] = {}
    if data_path is not None and data_path.exists():
        repl["datafile"] = _posix_relpath(main_vhdr.parent, data_path)
    if marker_path is not None and marker_path.exists():
        repl["markerfile"] = _posix_relpath(main_vhdr.parent, marker_path)
    if repl:
        _rewrite_ini_kv_inplace(main_vhdr, repl)

    # Patch the marker file (vmrk) to reference the extracted data file, if present.
    if marker_path is not None and marker_path.exists() and data_path is not None and data_path.exists():
        repl2 = {"datafile": _posix_relpath(marker_path.parent, data_path)}
        _rewrite_ini_kv_inplace(marker_path, repl2)


def _resolve_case_insensitive_path(root: Path, rel_posix: str) -> Optional[Path]:
    """Resolve a relative path under root by matching path components case-insensitively.

    This is primarily used as a fallback when a BrainVision .vhdr references
    companion files with different casing than what's on disk.
    """
    try:
        cur = root.resolve()
        for part in Path(rel_posix).parts:
            if part in ("", "."):
                continue
            if part == "..":
                return None
            try:
                entries = list(cur.iterdir())
            except Exception:
                return None
            match = None
            part_cf = part.casefold()
            for e in entries:
                if e.name.casefold() == part_cf:
                    match = e
                    break
            if match is None:
                return None
            cur = match
        return cur
    except Exception:
        return None

def _build_zip_name_map(zf: zipfile.ZipFile) -> Dict[str, str]:
    """Case-insensitive mapping of ZIP members -> canonical names."""
    out: Dict[str, str] = {}
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = info.filename
        if not name:
            continue
        out[name.lower()] = name
    return out


def _unique_path_with_suffix(path: Path, suffix: str) -> Path:
    suffix = _norm_ext(suffix)
    if _norm_ext(path.suffix) == suffix:
        return path

    base = path.with_suffix("")
    cand = Path(str(base) + suffix)
    if not cand.exists():
        return cand

    # Avoid clobbering.
    for i in range(1, 1000):
        cand = Path(str(base) + f"_{i}" + suffix)
        if not cand.exists():
            return cand
    return Path(str(base) + f"_{os.getpid()}" + suffix)


def _maybe_rename_to_vhdr(main: Path) -> Path:
    """If `main` is a BrainVision header with a non-.vhdr suffix, rename it to .vhdr."""
    if _norm_ext(main.suffix) == ".vhdr":
        return main

    prefix = _peek_file_prefix(main)
    if not _looks_like_brainvision_vhdr_bytes(prefix):
        return main

    target = _unique_path_with_suffix(main, ".vhdr")
    try:
        main.rename(target)
        return target
    except Exception:
        return main


def _extract_candidate_from_zip(zf: zipfile.ZipFile, name_map: Dict[str, str], member: str, outdir: Path) -> List[Path]:
    """Extract one candidate recording from a ZIP container.

    For BrainVision header candidates, also extract the referenced data/marker files.
    Returns extracted paths (main file first).
    """
    extracted: List[Path] = []

    main = _extract_zip_member(zf, member, outdir)
    extracted.append(main)

    # Treat BrainVision headers even if they were exported without a .vhdr suffix.
    ext = _norm_ext(Path(member).suffix)
    vhdr_bytes = b""
    is_bv = (ext == ".vhdr")
    if not is_bv:
        vhdr_bytes = _peek_zip_member_prefix(zf, member)
        is_bv = _looks_like_brainvision_vhdr_bytes(vhdr_bytes)

    if not is_bv:
        return extracted

    # If the member wasn't named *.vhdr, rename the extracted header to *.vhdr so qeeg can auto-detect it.
    main = _maybe_rename_to_vhdr(main)
    extracted[0] = main

    # Parse BrainVision references.
    if not vhdr_bytes:
        try:
            vhdr_bytes = main.read_bytes()
        except Exception:
            vhdr_bytes = b""

    vhdr_text = _decode_text_best_effort(vhdr_bytes)
    ref_map = _brainvision_related_member_map(member, vhdr_text)

    data_path: Optional[Path] = None
    marker_path: Optional[Path] = None

    for key, rel_member in ref_map.items():
        actual = name_map.get(rel_member.lower())
        if not actual:
            print(f"Warning: BrainVision referenced file not found in container: {rel_member}", file=sys.stderr)
            continue
        if actual.lower() == member.lower():
            continue
        try:
            p = _extract_zip_member(zf, actual, outdir)
            extracted.append(p)
            if key == "datafile":
                data_path = p
            elif key == "markerfile":
                marker_path = p
        except ValueError as e:
            print(f"Warning: {e}", file=sys.stderr)

    _patch_brainvision_refs(main, data_path, marker_path)
    return extracted


def _extract_candidate_from_dir(root: Path, member: str, outdir: Path) -> List[Path]:
    """Copy one candidate recording from a directory container.

    For BrainVision header candidates, also copy the referenced data/marker files.
    Returns extracted paths (main file first).
    """
    extracted: List[Path] = []

    main = _copy_from_dir(root, member, outdir)
    extracted.append(main)

    ext = _norm_ext(Path(member).suffix)
    is_bv = (ext == ".vhdr")
    vhdr_bytes = b""
    if not is_bv:
        vhdr_bytes = _peek_file_prefix(main)
        is_bv = _looks_like_brainvision_vhdr_bytes(vhdr_bytes)

    if not is_bv:
        return extracted

    main = _maybe_rename_to_vhdr(main)
    extracted[0] = main

    if not vhdr_bytes:
        try:
            vhdr_bytes = main.read_bytes()
        except Exception:
            vhdr_bytes = b""

    vhdr_text = _decode_text_best_effort(vhdr_bytes)
    ref_map = _brainvision_related_member_map(member, vhdr_text)

    data_path: Optional[Path] = None
    marker_path: Optional[Path] = None

    for key, rel_member in ref_map.items():
        if rel_member.lower() == member.lower():
            continue

        src_rel = rel_member
        src = (root / Path(src_rel)).resolve()
        if not src.exists():
            resolved = _resolve_case_insensitive_path(root, src_rel)
            if resolved is not None and resolved.exists():
                try:
                    src_rel = resolved.relative_to(root.resolve()).as_posix()
                    src = resolved
                except Exception:
                    pass

        if not src.exists():
            print(
                f"Warning: BrainVision referenced file not found next to header: {root / Path(rel_member)}",
                file=sys.stderr,
            )
            continue

        try:
            p = _copy_from_dir(root, src_rel, outdir)
            extracted.append(p)
            if key == "datafile":
                data_path = p
            elif key == "markerfile":
                marker_path = p
        except ValueError as e:
            print(f"Warning: {e}", file=sys.stderr)

    _patch_brainvision_refs(main, data_path, marker_path)
    return extracted



def list_contents(input_path: Path, prefer_exts: Sequence[str] = DEFAULT_PREFER) -> List[Dict[str, Any]]:
    """List *candidate recordings* in a BioTrace+/NeXus container.

    Returns a list of dicts with keys:
      - name (str): member path (ZIP) or relative path (dir)
      - size (int)
      - ext (str)
      - mtime (datetime|None)
    """

    in_path = Path(input_path)
    if not in_path.exists():
        raise FileNotFoundError(str(in_path))

    if in_path.is_dir():
        cands = _rank_candidates(_collect_dir_candidates(in_path, prefer_exts), prefer_exts)
    else:
        if not zipfile.is_zipfile(str(in_path)):
            return []
        with zipfile.ZipFile(str(in_path), "r") as zf:
            cands = _rank_candidates(_collect_zip_candidates(zf, prefer_exts), prefer_exts)

    out: List[Dict[str, Any]] = []
    for c in cands:
        out.append({"name": c.name, "size": int(c.size), "ext": c.ext, "mtime": c.mtime})
    return out


def extract_best_export(
    input_path: Path,
    outdir: Path,
    prefer_exts: Sequence[str] = DEFAULT_PREFER,
) -> Path:
    """Extract the best candidate recording from a container.

    - If input_path is a directory: copy the best candidate file.
    - If input_path is a zip-like file (.bcd/.mbd/.zip): extract the best candidate member.

    Returns the extracted file path.
    """

    extracted = extract_exports(input_path, outdir, prefer_exts=prefer_exts, extract_all=False)
    if not extracted:
        raise RuntimeError("No recognized recordings found.")
    return extracted[0]


def extract_exports(
    input_path: Path,
    outdir: Path,
    prefer_exts: Sequence[str] = DEFAULT_PREFER,
    extract_all: bool = False,
) -> List[Path]:
    """Extract one (best) or all candidate recordings from a container.

    Notes:
      - Directory mode copies files.
      - ZIP mode extracts members.
      - If the selected recording is a BrainVision `.vhdr`, this also extracts/copies
        the referenced companion files (e.g., `.eeg`, `.vmrk`) so the dataset is usable.
    """

    in_path = Path(input_path)
    if not in_path.exists():
        raise FileNotFoundError(str(in_path))

    _ensure_outdir(Path(outdir))

    extracted: List[Path] = []

    # Directory mode.
    if in_path.is_dir():
        cands = _rank_candidates(_collect_dir_candidates(in_path, prefer_exts), prefer_exts)
        if not cands:
            return []
        chosen = cands if extract_all else [cands[0]]
        for c in chosen:
            try:
                extracted.extend(_extract_candidate_from_dir(in_path, c.name, Path(outdir)))
            except ValueError as e:
                # Unsafe path; skip.
                print(f"Warning: {e}", file=sys.stderr)
        return extracted

    # Zip mode.
    if not zipfile.is_zipfile(str(in_path)):
        return []

    with zipfile.ZipFile(str(in_path), "r") as zf:
        name_map = _build_zip_name_map(zf)
        cands = _rank_candidates(_collect_zip_candidates(zf, prefer_exts), prefer_exts)
        if not cands:
            return []
        chosen = cands if extract_all else [cands[0]]
        for c in chosen:
            try:
                extracted.extend(_extract_candidate_from_zip(zf, name_map, c.name, Path(outdir)))
            except ValueError as e:
                print(f"Warning: {e}", file=sys.stderr)

    return extracted


def extract_selected_exports(
    input_path: Path,
    outdir: Path,
    select: str,
    prefer_exts: Sequence[str] = DEFAULT_PREFER,
) -> List[Path]:
    """Extract a *specific* candidate recording from a container.

    The selection is applied to the ranked candidate list produced by list_contents(),
    using the semantics described in select_candidate().
    """
    in_path = Path(input_path)
    if not in_path.exists():
        raise FileNotFoundError(str(in_path))

    _ensure_outdir(Path(outdir))

    # Directory mode.
    if in_path.is_dir():
        cands = _rank_candidates(_collect_dir_candidates(in_path, prefer_exts), prefer_exts)
        if not cands:
            return []
        chosen = select_candidate(cands, select)
        extracted: List[Path] = []
        try:
            extracted.extend(_extract_candidate_from_dir(in_path, chosen.name, Path(outdir)))
        except ValueError as e:
            print(f"Warning: {e}", file=sys.stderr)
        return extracted

    # Zip mode.
    if not zipfile.is_zipfile(str(in_path)):
        return []

    extracted: List[Path] = []
    with zipfile.ZipFile(str(in_path), "r") as zf:
        name_map = _build_zip_name_map(zf)
        cands = _rank_candidates(_collect_zip_candidates(zf, prefer_exts), prefer_exts)
        if not cands:
            return []
        chosen = select_candidate(cands, select)
        try:
            extracted.extend(_extract_candidate_from_zip(zf, name_map, chosen.name, Path(outdir)))
        except ValueError as e:
            print(f"Warning: {e}", file=sys.stderr)
    return extracted


def extract_selected_export(
    input_path: Path,
    outdir: Path,
    select: str,
    prefer_exts: Sequence[str] = DEFAULT_PREFER,
) -> Path:
    """Extract the selected candidate recording and return the main file path."""
    extracted = extract_selected_exports(input_path, outdir, select, prefer_exts=prefer_exts)
    if not extracted:
        raise RuntimeError("No recognized recordings found.")
    return extracted[0]

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
    ap = argparse.ArgumentParser(
        description="Extract open-format recordings from BioTrace+/NeXus session containers (ZIP-like .bcd/.mbd/.m2k/.zip)"
    )
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--input", help="Path to a session container (.bcd/.mbd/.m2k/.zip) or a directory")
    g.add_argument("--container", help="Alias for --input")
    ap.add_argument("--outdir", default="extracted", help="Output directory for extracted files (default: extracted)")
    ap.add_argument(
        "--prefer",
        default=",".join(DEFAULT_PREFER),
        help="Comma-separated extension preference order (default: EDF,BDF,BrainVision,CSV)",
    )
    ap.add_argument("--list", action="store_true", help="List candidate recordings and exit")
    ap.add_argument("--all", action="store_true", help="Extract all candidate recordings (default: extract best only)")
    ap.add_argument(
        "--select",
        default="",
        help=(
            "Select which candidate recording to extract (mutually exclusive with --all). "
            "Accepts 1-based index from --list, exact member name, substring, or glob pattern (case-insensitive)."
        ),
    )
    ap.add_argument("--print", dest="print_paths", action="store_true", help="Print extracted path(s) to stdout")
    args = ap.parse_args(list(argv) if argv is not None else None)

    in_path = Path(args.input if getattr(args, "input", None) else args.container)
    outdir = Path(args.outdir)
    prefer_exts = _parse_prefer_list(args.prefer)

    if not in_path.exists():
        raise SystemExit(f"Input not found: {in_path}")

    # List mode: show candidate recordings only.
    if args.list:
        items = list_contents(in_path, prefer_exts=prefer_exts)
        if not items:
            if in_path.is_dir():
                print("No recognized recordings found.")
                return 1
            if not zipfile.is_zipfile(str(in_path)):
                print(
                    "Input is not a ZIP container (or is an unsupported container type).\n"
                    "BioTrace+/NeXus session containers can be proprietary; if this extractor cannot open it,\n"
                    "please export your session from BioTrace+ to an open format (EDF/BDF/ASCII) first.\n"
                    f"Input: {in_path}"
                )
                return 2
            print("No recognized recordings found inside the container.")
            return 1

        for i, it in enumerate(items):
            print(f"{i+1:02d}. {int(it.get('size', 0)):>10}  {it.get('name', '')}")
        return 0

    # Extract mode.
    if (not in_path.is_dir()) and (not zipfile.is_zipfile(str(in_path))):
        print(
            "Input is not a ZIP container (or is an unsupported container type).\n"
            "BioTrace+/NeXus session containers can be proprietary; if this extractor cannot open it,\n"
            "please export your session from BioTrace+ to an open format (EDF/BDF/ASCII) first.\n"
            f"Input: {in_path}"
        )
        return 2

    if args.all and args.select:
        print("Error: --all and --select are mutually exclusive", file=sys.stderr)
        return 2

    if args.select:
        try:
            extracted = extract_selected_exports(in_path, outdir, select=args.select, prefer_exts=prefer_exts)
        except Exception as e:
            print(f"Error: failed to select/extract from container: {e}", file=sys.stderr)
            return 3
    else:
        extracted = extract_exports(in_path, outdir, prefer_exts=prefer_exts, extract_all=bool(args.all))

    if not extracted:
        # Distinguish "no candidates" from "unsafe candidates only".
        candidates = list_contents(in_path, prefer_exts=prefer_exts)
        if candidates:
            print(
                "No safe recordings could be extracted (all candidates had unsafe paths).\n"
                "Refused to write outside --outdir.\n"
                f"Input: {in_path}"
            )
            return 3
        if in_path.is_dir():
            print("No recognized recordings found.")
        else:
            print("No recognized recordings found inside the container.")
        return 1

    if args.print_paths:
        for p in extracted:
            print(str(p))
        return 0

    print(f"Extracted {len(extracted)} file(s) to: {outdir}")
    for p in extracted:
        print(f"  - {p}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
