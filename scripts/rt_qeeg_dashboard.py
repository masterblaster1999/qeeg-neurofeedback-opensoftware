#!/usr/bin/env python3
"""rt_qeeg_dashboard.py

Real-time qEEG visualization dashboard (local-only, no third-party deps).

This script is intended to be used alongside qeeg_nf_cli in paced mode:

  ./build/qeeg_nf_cli ... --realtime --export-bandpowers --flush-csv
  python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --open

The dashboard watches CSV outputs (as they are appended) and streams them to a
browser using Server-Sent Events (SSE).

Security note:
- By default this binds to 127.0.0.1 only.
- If you bind to a non-loopback host (e.g., 0.0.0.0), you must pass --allow-remote.
  Only do this on trusted networks.
- A token is required for API requests to reduce the chance of other pages on
  your machine (or LAN) connecting to the stream.

Tested with Python 3.8+.
"""

from __future__ import annotations

import argparse
import csv
import json
import hashlib
import math
import os
import socket
import secrets
import sys
import threading
import time
import traceback
import webbrowser
from collections import deque
from dataclasses import dataclass
from email.utils import formatdate
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Deque, Dict, Iterable, List, Optional, Tuple
from urllib.parse import parse_qs, urlparse


# ---------------------------------------------------------------------------
# Channel name normalization (matches the C++ logic in src/utils.cpp).
# ---------------------------------------------------------------------------


def normalize_channel_name(name: str) -> str:
    """Normalize channel names for lookups.

    This is intentionally conservative and mirrors qeeg::normalize_channel_name.
    """
    s = (name or "").strip()
    if not s:
        return ""

    # Strip common prefixes.
    s_low = s.lower()
    for prefix in ("eeg ", "eeg_", "eeg-"):
        if s_low.startswith(prefix):
            s = s[len(prefix) :]
            s_low = s_low[len(prefix) :]
            break

    # Strip common suffixes.
    if s_low.endswith("-ref"):
        s = s[: -4]
        s_low = s_low[: -4]

    # Remove separators.
    s = s.replace(" ", "").replace("_", "")

    # Lower -> title case for the standard electrode labels.
    # Special case: midline 'z' is uppercase (Fz, Cz, ...)
    if s:
        # Keep existing capitalization pattern in the middle if provided (e.g., "CPz").
        # But for normalized keys we use lowercase + canonicalization.
        pass

    # Canonicalize historic aliases.
    key = s.lower()
    aliases = {
        "t3": "t7",
        "t4": "t8",
        "t5": "p7",
        "t6": "p8",
    }
    key = aliases.get(key, key)

    return key


# ---------------------------------------------------------------------------
# Built-in (approximate) 2D positions for common 10-10 / 10-20 electrodes.
# These positions are the same as the built-in montage in src/montage.cpp.
# Coordinates are on a unit circle-ish head model, x left->right, y back->front.
# ---------------------------------------------------------------------------


# NOTE: Keys are already normalized (lowercase, no separators).
MONTAGE_10_10_61: Dict[str, Tuple[float, float]] = {
    # Midline
    "fpz": (0.0, 0.95),
    "afz": (0.0, 0.88),
    "fz": (0.0, 0.75),
    "fcz": (0.0, 0.55),
    "cz": (0.0, 0.0),
    "cpz": (0.0, -0.55),
    "pz": (0.0, -0.75),
    "poz": (0.0, -0.88),
    "oz": (0.0, -0.95),

    # Left hemisphere (odd)
    "fp1": (-0.5, 0.92),
    "af7": (-0.8, 0.75),
    "af3": (-0.35, 0.82),
    "f7": (-0.9, 0.55),
    "f5": (-0.7, 0.62),
    "f3": (-0.5, 0.65),
    "f1": (-0.25, 0.70),
    "ft7": (-0.95, 0.25),
    "fc5": (-0.7, 0.35),
    "fc3": (-0.5, 0.40),
    "fc1": (-0.25, 0.45),
    "t7": (-1.0, 0.0),
    "c5": (-0.75, 0.0),
    "c3": (-0.55, 0.0),
    "c1": (-0.25, 0.0),
    "tp7": (-0.95, -0.25),
    "cp5": (-0.7, -0.35),
    "cp3": (-0.5, -0.40),
    "cp1": (-0.25, -0.45),
    "p7": (-0.9, -0.55),
    "p5": (-0.7, -0.62),
    "p3": (-0.5, -0.65),
    "p1": (-0.25, -0.70),
    "po7": (-0.8, -0.75),
    "po3": (-0.35, -0.82),
    "o1": (-0.5, -0.92),

    # Right hemisphere (even)
    "fp2": (0.5, 0.92),
    "af8": (0.8, 0.75),
    "af4": (0.35, 0.82),
    "f8": (0.9, 0.55),
    "f6": (0.7, 0.62),
    "f4": (0.5, 0.65),
    "f2": (0.25, 0.70),
    "ft8": (0.95, 0.25),
    "fc6": (0.7, 0.35),
    "fc4": (0.5, 0.40),
    "fc2": (0.25, 0.45),
    "t8": (1.0, 0.0),
    "c6": (0.75, 0.0),
    "c4": (0.55, 0.0),
    "c2": (0.25, 0.0),
    "tp8": (0.95, -0.25),
    "cp6": (0.7, -0.35),
    "cp4": (0.5, -0.40),
    "cp2": (0.25, -0.45),
    "p8": (0.9, -0.55),
    "p6": (0.7, -0.62),
    "p4": (0.5, -0.65),
    "p2": (0.25, -0.70),
    "po8": (0.8, -0.75),
    "po4": (0.35, -0.82),
    "o2": (0.5, -0.92),
}




# ---------------------------------------------------------------------------
# Optional custom montage loading
# ---------------------------------------------------------------------------


def _load_montage_csv(path: Path) -> Dict[str, Tuple[float, float]]:
    """Load a montage CSV (name,x,y) into a normalized-name -> (x,y) dict.

    The format matches qeeg::Montage::load_csv:
      - CSV rows: name,x,y
      - lines starting with # are ignored
    """
    out: Dict[str, Tuple[float, float]] = {}
    try:
        with path.open("r", encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.reader(f):
                if not row:
                    continue
                if row[0].strip().startswith("#"):
                    continue
                if len(row) < 3:
                    continue
                name = (row[0] or "").strip()
                if not name:
                    continue
                try:
                    x = float((row[1] or "").strip())
                    y = float((row[2] or "").strip())
                except Exception:
                    continue
                key = normalize_channel_name(name)
                if key:
                    out[key] = (x, y)
    except FileNotFoundError:
        return {}
    except Exception:
        return {}
    return out

# ---------------------------------------------------------------------------
# CSV tailing helpers
# ---------------------------------------------------------------------------


@dataclass
class BandpowerHeader:
    time_idx: int
    bands: List[str]
    channels: List[str]
    # Column indices in band-major order: values[b_i * n_ch + c_i] = row[col_idx]
    col_indices: List[int]


def _safe_float(s: str) -> Optional[float]:
    try:
        v = float(s)
        if v != v:  # NaN
            return None
        return v
    except Exception:
        return None


def _read_csv_header_line(path: Path) -> Optional[List[str]]:
    try:
        with path.open("r", encoding="utf-8", errors="replace", newline="") as f:
            line = f.readline()
            if not line:
                return None
            return next(csv.reader([line]))
    except FileNotFoundError:
        return None


def parse_bandpower_timeseries_header(header: List[str]) -> Optional[BandpowerHeader]:
    if not header or len(header) < 3:
        return None

    # Find time column index.
    time_candidates = {
        "t_end_sec",
        "t_end_s",
        "t_sec",
        "time_sec",
        "time_s",
        "time",
        "t",
    }
    time_idx = 0
    for i, h in enumerate(header):
        if (h or "").strip().lower() in time_candidates:
            time_idx = i
            break

    # Remaining columns are expected to be <band>_<channel>
    bands: List[str] = []
    channels: List[str] = []

    # Collect first pass mapping
    parsed: List[Tuple[str, str, int]] = []
    for j, name in enumerate(header):
        if j == time_idx:
            continue
        n = (name or "").strip()
        if not n:
            continue
        # Ignore z-score columns if present.
        if n.lower().endswith("_z"):
            continue
        if "_" not in n:
            continue
        band, ch = n.split("_", 1)
        band = band.strip()
        ch = ch.strip()
        if not band or not ch:
            continue
        parsed.append((band, ch, j))

    if not parsed:
        return None

    # Establish band order and channel order from first encountered band.
    for band, ch, _ in parsed:
        if band not in bands:
            bands.append(band)

    first_band = bands[0]
    for band, ch, _ in parsed:
        if band == first_band and ch not in channels:
            channels.append(ch)

    if not channels or len(bands) < 1:
        return None

    # Build band-major indices, skipping any missing columns.
    col_indices: List[int] = []
    index_map: Dict[Tuple[str, str], int] = {(b, c): idx for (b, c, idx) in parsed}
    for b in bands:
        for c in channels:
            col_indices.append(index_map.get((b, c), -1))

    return BandpowerHeader(time_idx=time_idx, bands=bands, channels=channels, col_indices=col_indices)


class CsvTailer:
    """Incrementally tails a CSV file that is appended over time."""

    def __init__(self, path: Path, *, max_initial_rows: int = 1200):
        self.path = path
        self.max_initial_rows = max_initial_rows
        self._fp = None  # type: Optional[object]
        self._buf = b""
        self._header: Optional[List[str]] = None

    def header(self) -> Optional[List[str]]:
        return self._header

    def close(self) -> None:
        try:
            if self._fp:
                self._fp.close()
        finally:
            self._fp = None

    def _open_wait(self, timeout: float = 0.0) -> None:
        t0 = time.time()
        while True:
            if self.path.exists() and self.path.stat().st_size > 0:
                self._fp = self.path.open("rb")
                self._buf = b""
                # Read header line.
                while True:
                    line = self._fp.readline()
                    if not line:
                        # No data yet.
                        time.sleep(0.05)
                        continue
                    if b"\n" not in line and b"\r" not in line:
                        # Incomplete line; keep waiting.
                        time.sleep(0.05)
                        continue
                    hdr = next(csv.reader([line.decode("utf-8", errors="replace")]))
                    self._header = hdr
                    return
            if timeout > 0 and (time.time() - t0) >= timeout:
                raise TimeoutError(f"Timed out waiting for file: {self.path}")
            time.sleep(0.1)

    def _ensure_open(self) -> None:
        if self._fp is not None:
            # Detect truncation and reopen.
            try:
                cur = self._fp.tell()
                size = self.path.stat().st_size
                if size < cur:
                    self.close()
            except Exception:
                self.close()

        if self._fp is None:
            self._open_wait(timeout=0.0)

    def read_initial_rows(self) -> List[List[str]]:
        """Return up to max_initial_rows from the existing file (tail)."""
        self._ensure_open()
        assert self._fp is not None

        # Read the remainder of the file and keep only the last N rows.
        from collections import deque

        dq: Deque[List[str]] = deque(maxlen=self.max_initial_rows)
        # Use readline() from the current position (just after header).
        while True:
            line = self._fp.readline()
            if not line:
                break
            row = next(csv.reader([line.decode("utf-8", errors="replace")]))
            if row:
                dq.append(row)

        return list(dq)

    def iter_new_rows(self, *, stop_event: Optional[threading.Event] = None) -> Iterable[List[str]]:
        """Yield new rows as they appear.

        If stop_event is provided, the generator exits promptly when it is set.
        """
        self._ensure_open()
        assert self._fp is not None

        # File pointer is expected to be at EOF after read_initial_rows.
        while True:
            if stop_event is not None and stop_event.is_set():
                return
            try:
                chunk = self._fp.read(4096)
            except Exception:
                # Reopen on transient errors.
                self.close()
                self._ensure_open()
                assert self._fp is not None
                chunk = b""

            if not chunk:
                # No new data.
                if stop_event is not None and stop_event.is_set():
                    return
                time.sleep(0.05)
                continue

            self._buf += chunk
            while True:
                # Support \r\n and \n.
                nl = self._buf.find(b"\n")
                if nl < 0:
                    break
                line = self._buf[: nl + 1]
                self._buf = self._buf[nl + 1 :]
                # Strip CR/LF.
                s = line.decode("utf-8", errors="replace").strip("\r\n")
                if not s:
                    continue
                row = next(csv.reader([s]))
                if row:
                    yield row


class StreamBuffer:
    """Thread-safe ring buffer for SSE frames.

    Each appended frame gets a monotonically increasing sequence number.
    Consumers can snapshot and/or wait for new frames.
    """

    def __init__(self, *, maxlen: int = 20000):
        self._items: Deque[Tuple[int, Any]] = deque(maxlen=maxlen)
        self._seq: int = 0
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)

    def append(self, obj: Any) -> int:
        with self._cv:
            self._seq += 1
            self._items.append((self._seq, obj))
            self._cv.notify_all()
            return self._seq

    def latest_seq(self) -> int:
        with self._lock:
            return self._seq

    def oldest_seq(self) -> int:
        with self._lock:
            if not self._items:
                return 0
            return int(self._items[0][0])

    def size(self) -> int:
        with self._lock:
            return len(self._items)

    def snapshot(self) -> List[Tuple[int, Any]]:
        with self._lock:
            return list(self._items)

    def get_since(self, last_seq: int, *, limit: int = 2000) -> Tuple[int, List[Any], bool]:
        """Return frames with seq > last_seq.

        Returns: (new_last_seq, frames, reset)
        - reset=True if last_seq is older than the oldest frame we still have.
        """
        with self._lock:
            if not self._items:
                return last_seq, [], False
            oldest = self._items[0][0]
            newest = self._items[-1][0]
            reset = (last_seq != 0) and (last_seq < oldest - 1)
            frames: List[Any] = []
            new_last = last_seq
            for seq, obj in self._items:
                if seq > last_seq:
                    frames.append(obj)
                    new_last = seq
                    if len(frames) >= limit:
                        break
            # If we hit the limit but there are even newer frames, bump new_last so the consumer
            # doesn't spin scanning the same prefix over and over.
            if frames and len(frames) >= limit:
                new_last = min(new_last, newest)
            return new_last, frames, reset

    def wait_for_new(self, last_seq: int, timeout: float) -> int:
        """Block until seq advances beyond last_seq or timeout. Returns current seq."""
        with self._cv:
            if self._seq > last_seq:
                return self._seq
            self._cv.wait(timeout=timeout)
            return self._seq


# ------------------------ run meta (nf_run_meta.json) ------------------------
# We keep only a small summary in the frequently-updated /api/meta payload to
# avoid resending a large, mostly-static JSON blob every meta interval.
# The full nf_run_meta.json is served on-demand via /api/run_meta.


_RUN_META_SUMMARY_LOCK = threading.Lock()
_RUN_META_SUMMARY_KEY: Optional[Tuple[float, int]] = None  # (mtime_utc, size_bytes)
_RUN_META_SUMMARY: Optional[Dict[str, object]] = None
_RUN_META_SUMMARY_ERROR: Optional[str] = None


def _truncate_list(v: object, *, max_items: int = 12) -> object:
    if not isinstance(v, list):
        return v
    if len(v) <= max_items:
        return v
    # Keep the list readable in the UI (and small on the wire).
    return list(v[:max_items]) + [f"... ({len(v) - max_items} more)"]


def _summarize_run_meta_obj(obj: object) -> Optional[Dict[str, object]]:
    """Extract a compact, UI-friendly subset from nf_run_meta.json."""
    if not isinstance(obj, dict):
        return None

    keys = [
        "Tool",
        "Version",
        "GitDescribe",
        "TimestampLocal",
        "OutputDir",
        "demo",
        "input_path",
        "protocol",
        "fs_hz",
        "metric_spec",
        "band_spec",
        "reward_direction",
        "threshold_init",
        "baseline_seconds",
        "baseline_quantile_used",
        "target_reward_rate",
        "adapt_mode",
        "adapt_eta",
        "window_seconds",
        "update_seconds",
        "metric_smooth_seconds",
        "artifact_gate",
        "qc_bad_channel_count",
        "qc_bad_channels",
        "biotrace_ui",
        "export_derived_events",
        "derived_events_written",
    ]

    out: Dict[str, object] = {}
    for k in keys:
        if k in obj:
            out[k] = _truncate_list(obj.get(k), max_items=12)
    return out or None


def _run_meta_summary_info(outdir: Path) -> Dict[str, object]:
    """Return stat + cached summary for nf_run_meta.json (if present)."""
    p = outdir / "nf_run_meta.json"
    st = _stat_dict(p)
    info: Dict[str, object] = {
        "path": str(p),
        "stat": st,
        "etag": None,
        "summary": None,
        "parse_error": None,
    }
    if not st.get("exists"):
        return info

    try:
        etag = _weak_etag_from_stat(int(st.get("size_bytes", 0) or 0), float(st.get("mtime_utc", 0.0) or 0.0))
    except Exception:
        etag = None
    info["etag"] = etag

    key = (float(st.get("mtime_utc", 0.0) or 0.0), int(st.get("size_bytes", 0) or 0))

    global _RUN_META_SUMMARY_KEY, _RUN_META_SUMMARY, _RUN_META_SUMMARY_ERROR
    with _RUN_META_SUMMARY_LOCK:
        if _RUN_META_SUMMARY_KEY == key:
            info["summary"] = _RUN_META_SUMMARY
            info["parse_error"] = _RUN_META_SUMMARY_ERROR
            return info

    # Parse outside the lock (file IO + JSON decode).
    summary: Optional[Dict[str, object]] = None
    perr: Optional[str] = None
    try:
        obj = json.loads(p.read_text(encoding="utf-8"))
        summary = _summarize_run_meta_obj(obj)
    except Exception as e:
        perr = str(e)

    with _RUN_META_SUMMARY_LOCK:
        _RUN_META_SUMMARY_KEY = key
        _RUN_META_SUMMARY = summary
        _RUN_META_SUMMARY_ERROR = perr

    info["summary"] = summary
    info["parse_error"] = perr
    return info



def compute_meta(outdir: Path) -> Dict[str, object]:
    """Compute lightweight metadata + file stats for the dashboard."""

    run_meta_path = outdir / "nf_run_meta.json"

    meta: Dict[str, object] = {
        "schema_version": 2,
        "outdir": str(outdir),
        "server_time_utc": float(time.time()),
        "files": {
            "nf_feedback": str(outdir / "nf_feedback.csv"),
            "bandpower_timeseries": str(outdir / "bandpower_timeseries.csv"),
            "artifact_gate_timeseries": str(outdir / "artifact_gate_timeseries.csv"),
            "nf_run_meta": str(run_meta_path),
        },
        "files_stat": {
            "nf_feedback": _stat_dict(outdir / "nf_feedback.csv"),
            "bandpower_timeseries": _stat_dict(outdir / "bandpower_timeseries.csv"),
            "artifact_gate_timeseries": _stat_dict(outdir / "artifact_gate_timeseries.csv"),
            "nf_run_meta": _stat_dict(run_meta_path),
        },
        # Compact run meta summary (full JSON via /api/run_meta).
        "run_meta": _run_meta_summary_info(outdir),
    }

    # Bandpower meta.
    bp_path = outdir / "bandpower_timeseries.csv"
    bp_hdr = _read_csv_header_line(bp_path)
    if bp_hdr:
        bp_info = parse_bandpower_timeseries_header(bp_hdr)
        if bp_info:
            # Optional montage override: if <outdir>/montage.csv exists (name,x,y), use it.
            custom_montage_path = outdir / "montage.csv"
            custom_pos = _load_montage_csv(custom_montage_path) if custom_montage_path.exists() else {}

            positions: List[Optional[Tuple[float, float]]] = []
            pos_src: List[str] = []
            for ch in bp_info.channels:
                key = normalize_channel_name(ch)
                if key and key in custom_pos:
                    positions.append(custom_pos[key])
                    pos_src.append("custom")
                else:
                    p = MONTAGE_10_10_61.get(key)
                    if p is not None:
                        positions.append(p)
                        pos_src.append("builtin")
                    else:
                        positions.append(None)
                        pos_src.append("fallback")

            # If any positions are missing, place them on a simple ring layout as a fallback.
            missing = [i for i, p in enumerate(positions) if p is None]
            if missing:
                n = len(missing)
                r = 0.85
                for j, i in enumerate(missing):
                    ang = math.pi / 2.0 - 2.0 * math.pi * (j / float(n))
                    positions[i] = (r * math.cos(ang), r * math.sin(ang))
                    pos_src[i] = "fallback"

            fallback_count = sum(1 for s in pos_src if s == "fallback")
            meta["bandpower"] = {
                "bands": bp_info.bands,
                "channels": bp_info.channels,
                "positions": positions,
                "positions_source": pos_src,
                "fallback_positions_count": fallback_count,
                "montage_csv": str(custom_montage_path) if custom_pos else None,
                "time_col": bp_hdr[bp_info.time_idx] if bp_info.time_idx < len(bp_hdr) else "time",
            }
        else:
            meta["bandpower"] = None
    else:
        meta["bandpower"] = None

    return meta


class LiveHub:
    """Background tailers + caches for dashboard SSE streams."""

    def __init__(self, outdir: Path, *, history_rows: int = 1200, meta_interval_sec: float = 1.0):
        self.outdir = outdir
        self.history_rows = history_rows
        self.meta_interval_sec = meta_interval_sec

        self.nf = StreamBuffer(maxlen=max(20000, history_rows * 4))
        self.bandpower = StreamBuffer(maxlen=max(20000, history_rows * 4))
        self.artifact = StreamBuffer(maxlen=max(40000, history_rows * 6))
        self.meta = StreamBuffer(maxlen=2000)

        self._stop = threading.Event()
        self._threads: List[threading.Thread] = []
        self._last_meta: Optional[Dict[str, object]] = None
        self._last_meta_json: Optional[str] = None

    def start(self) -> None:
        self._threads = [
            threading.Thread(target=self._run_nf, name="qeeg_rt_nf", daemon=True),
            threading.Thread(target=self._run_bandpower, name="qeeg_rt_bp", daemon=True),
            threading.Thread(target=self._run_artifact, name="qeeg_rt_art", daemon=True),
            threading.Thread(target=self._run_meta, name="qeeg_rt_meta", daemon=True),
        ]
        for t in self._threads:
            t.start()

    def stop(self) -> None:
        self._stop.set()

    def latest_meta(self) -> Dict[str, object]:
        if self._last_meta is not None:
            return self._last_meta
        m = compute_meta(self.outdir)
        self._last_meta = m
        try:
            self._last_meta_json = json.dumps(m, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        except Exception:
            self._last_meta_json = None
        return m

    # ------------------------ worker loops ------------------------

    def _run_meta(self) -> None:
        while not self._stop.is_set():
            try:
                m = compute_meta(self.outdir)
                s = json.dumps(m, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
                if s != self._last_meta_json:
                    self._last_meta = m
                    self._last_meta_json = s
                    self.meta.append(m)
            except Exception:
                # Don't crash the server for meta failures.
                pass
            self._stop.wait(self.meta_interval_sec)

    def _run_nf(self) -> None:
        path = self.outdir / "nf_feedback.csv"
        while not self._stop.is_set():
            tailer = CsvTailer(path, max_initial_rows=self.history_rows)
            try:
                init_rows = tailer.read_initial_rows()
                header = tailer.header() or []
                hmap = {(header[i] or "").strip().lower(): i for i in range(len(header))}
                idx_t = hmap.get("t_end_sec", 0)
                idx_metric = hmap.get("metric", 1)
                idx_thr = hmap.get("threshold", 2)
                idx_reward = hmap.get("reward", 3)
                idx_rr = hmap.get("reward_rate", 4)
                idx_art_ready = hmap.get("artifact_ready", -1)
                idx_art = hmap.get("artifact", -1)
                idx_bad_ch = hmap.get("bad_channels", -1)
                idx_phase = hmap.get("phase", -1)

                def emit_row(row: List[str]) -> Optional[Dict[str, object]]:
                    if not row:
                        return None
                    t = _safe_float(row[idx_t]) if idx_t < len(row) else None
                    metric = _safe_float(row[idx_metric]) if idx_metric < len(row) else None
                    thr = _safe_float(row[idx_thr]) if idx_thr < len(row) else None
                    reward = int(_safe_float(row[idx_reward]) or 0) if idx_reward < len(row) else 0
                    rr = _safe_float(row[idx_rr]) if idx_rr < len(row) else None
                    obj: Dict[str, object] = {
                        "t": t,
                        "metric": metric,
                        "threshold": thr,
                        "reward": reward,
                        "reward_rate": rr,
                    }
                    if 0 <= idx_art_ready < len(row):
                        obj["artifact_ready"] = int(_safe_float(row[idx_art_ready]) or 0)
                    if 0 <= idx_art < len(row):
                        obj["artifact"] = int(_safe_float(row[idx_art]) or 0)
                    if 0 <= idx_bad_ch < len(row):
                        obj["bad_channels"] = int(_safe_float(row[idx_bad_ch]) or 0)
                    if 0 <= idx_phase < len(row):
                        obj["phase"] = row[idx_phase]
                    return obj

                for r in init_rows:
                    obj = emit_row(r)
                    if obj is not None:
                        self.nf.append(obj)

                for r in tailer.iter_new_rows(stop_event=self._stop):
                    if self._stop.is_set():
                        break
                    obj = emit_row(r)
                    if obj is not None:
                        self.nf.append(obj)
            except Exception:
                # If the file disappears or is malformed, keep retrying.
                self._stop.wait(0.2)
            finally:
                tailer.close()

    def _run_artifact(self) -> None:
        path = self.outdir / "artifact_gate_timeseries.csv"
        while not self._stop.is_set():
            tailer = CsvTailer(path, max_initial_rows=self.history_rows)
            try:
                init_rows = tailer.read_initial_rows()
                header = tailer.header() or []
                hmap = {(header[i] or "").strip().lower(): i for i in range(len(header))}
                idx_t = hmap.get("t_end_sec", 0)
                idx_ready = hmap.get("ready", hmap.get("baseline_ready", 1))
                idx_bad = hmap.get("bad", 2)
                idx_bad_ch = hmap.get("bad_channels", hmap.get("bad_channel_count", 3))

                def emit_row(row: List[str]) -> Optional[Dict[str, object]]:
                    if not row:
                        return None
                    t = _safe_float(row[idx_t]) if idx_t < len(row) else None
                    ready = int(_safe_float(row[idx_ready]) or 0) if idx_ready < len(row) else 0
                    bad = int(_safe_float(row[idx_bad]) or 0) if idx_bad < len(row) else 0
                    bad_ch = int(_safe_float(row[idx_bad_ch]) or 0) if idx_bad_ch < len(row) else 0
                    return {"t": t, "ready": ready, "bad": bad, "bad_channels": bad_ch}

                for r in init_rows:
                    obj = emit_row(r)
                    if obj is not None:
                        self.artifact.append(obj)

                for r in tailer.iter_new_rows(stop_event=self._stop):
                    if self._stop.is_set():
                        break
                    obj = emit_row(r)
                    if obj is not None:
                        self.artifact.append(obj)
            except Exception:
                self._stop.wait(0.2)
            finally:
                tailer.close()

    def _run_bandpower(self) -> None:
        path = self.outdir / "bandpower_timeseries.csv"
        while not self._stop.is_set():
            tailer = CsvTailer(path, max_initial_rows=self.history_rows)
            try:
                init_rows = tailer.read_initial_rows()
                header = tailer.header() or []
                bp = parse_bandpower_timeseries_header(header) if header else None
                if not bp:
                    # Not ready yet.
                    self._stop.wait(0.25)
                    continue

                def emit_row(row: List[str]) -> Optional[Dict[str, object]]:
                    if not row:
                        return None
                    t = _safe_float(row[bp.time_idx]) if bp.time_idx < len(row) else None
                    vals: List[Optional[float]] = []
                    for idx in bp.col_indices:
                        if idx < 0 or idx >= len(row):
                            vals.append(None)
                        else:
                            vals.append(_safe_float(row[idx]))
                    return {"t": t, "values": vals}

                for r in init_rows:
                    obj = emit_row(r)
                    if obj is not None:
                        self.bandpower.append(obj)

                for r in tailer.iter_new_rows(stop_event=self._stop):
                    if self._stop.is_set():
                        break
                    obj = emit_row(r)
                    if obj is not None:
                        self.bandpower.append(obj)
            except Exception:
                self._stop.wait(0.2)
            finally:
                tailer.close()


# ---------------------------------------------------------------------------
# Dashboard HTTP server
# ---------------------------------------------------------------------------


@dataclass
class ServerConfig:
    outdir: Path
    host: str
    port: int
    token: str
    max_hz: float = 15.0
    history_rows: int = 1200
    meta_interval_sec: float = 1.0
    frontend_dir: Optional[Path] = None


def _is_loopback_host(host: str) -> bool:
    h = (host or "").strip().lower()
    return h in ("127.0.0.1", "localhost", "::1")


def _is_wildcard_host(host: str) -> bool:
    h = (host or "").strip().lower()
    return h in ("0.0.0.0", "0", "::", "")


def _guess_lan_ip() -> Optional[str]:
    """Best-effort guess of the LAN IP address for URL printing.

    This uses a UDP connect() trick (no packets are sent) to ask the OS which
    source address it would use for an external route.
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            if ip and not ip.startswith("127."):
                return ip
        finally:
            s.close()
    except Exception:
        pass
    return None


def _build_urls(bind_host: str, port: int, token: str) -> Dict[str, str]:
    """Build local + (optional) LAN URLs for the dashboard and kiosk pages."""
    h = (bind_host or "").strip()

    # For wildcard binds (0.0.0.0/::), prefer a usable loopback URL for local opens.
    local_host = "127.0.0.1" if _is_wildcard_host(h) else (h if h else "127.0.0.1")

    out: Dict[str, str] = {}
    out["dashboard_url"] = f"http://{local_host}:{port}/?token={token}"
    out["kiosk_url"] = f"http://{local_host}:{port}/kiosk?token={token}"

    lan_host: Optional[str] = None
    if _is_wildcard_host(h):
        lan_host = _guess_lan_ip()
    elif not _is_loopback_host(h):
        # If the user bound to a concrete non-loopback host, that's the one to advertise.
        lan_host = h

    if lan_host and lan_host != local_host:
        out["dashboard_url_lan"] = f"http://{lan_host}:{port}/?token={token}"
        out["kiosk_url_lan"] = f"http://{lan_host}:{port}/kiosk?token={token}"

    return out


def _http_date(ts: float) -> str:
    """Format a UNIX timestamp as an HTTP-date."""
    try:
        return formatdate(timeval=float(ts), usegmt=True)
    except Exception:
        return formatdate(timeval=time.time(), usegmt=True)


def _weak_etag_from_stat(size_bytes: int, mtime_utc: float) -> str:
    """Generate a weak ETag from file size + mtime."""
    try:
        return f'W/"{int(size_bytes)}-{int(mtime_utc)}"'
    except Exception:
        return 'W/"0-0"'


def _strong_etag_from_bytes(data: bytes) -> str:
    """Generate a strong ETag from content bytes (sha1)."""
    h = hashlib.sha1(data).hexdigest()
    return f'"sha1-{h}"'


def _if_none_match_matches(header_value: str, etag: str) -> bool:
    """Return True if If-None-Match header matches the given ETag."""
    if not header_value:
        return False
    hv = header_value.strip()
    if hv == "*":
        return True
    parts = [p.strip() for p in hv.split(",") if p.strip()]
    return etag in parts


def _json_bytes(obj: object) -> bytes:
    return (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def _default_frontend_dir() -> Path:
    return Path(__file__).resolve().parent / "rt_dashboard_frontend"


_FRONTEND_PATH_MAP: Dict[str, str] = {
    "/": "index.html",
    "/index.html": "index.html",
    "/kiosk": "kiosk.html",
    "/kiosk.html": "kiosk.html",
    "/app.js": "app.js",
    "/app_legacy.js": "app_legacy.js",

    "/kiosk.js": "kiosk.js",
    "/style.css": "style.css",
}


_MIME_MAP: Dict[str, str] = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
}


def _guess_mime(path: Path) -> str:
    return _MIME_MAP.get(path.suffix.lower(), "application/octet-stream")


def _stat_dict(p: Path) -> Dict[str, object]:
    """Best-effort stat info for UI/status pages."""
    try:
        st = p.stat()
        return {"exists": True, "size_bytes": int(st.st_size), "mtime_utc": float(st.st_mtime)}
    except FileNotFoundError:
        return {"exists": False}
    except Exception:
        return {"exists": False}


def _safe_read_file(path: Path, *, max_bytes: int = 2_000_000) -> Optional[bytes]:
    try:
        st = path.stat()
        if st.st_size > max_bytes:
            return None
        return path.read_bytes()
    except FileNotFoundError:
        return None
    except Exception:
        return None


DASH_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>qEEG Real-time Dashboard</title>
  <style>
    :root{
      --bg:#0b0f14; --panel:#121a22; --panel2:#0f151c; --fg:#e8eef6; --muted:#9fb0c3;
      --border:#203040; --accent:#64d2ff; --warn:#ffcf5c; --bad:#ff6b6b;
    }
    html,body{height:100%;}
    body{margin:0; font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Cantarell,Noto Sans,sans-serif; background:var(--bg); color:var(--fg);}
    a{color:var(--accent);}
    header{padding:14px 16px; border-bottom:1px solid var(--border); background:linear-gradient(180deg, #0d141d, #0b0f14);}
    header h1{margin:0; font-size:16px; letter-spacing:0.4px;}
    header .sub{margin-top:6px; font-size:12px; color:var(--muted)}
    main{padding:12px 12px 24px;}
    .grid{display:grid; grid-template-columns: 1.2fr 1fr; gap:12px; align-items:start;}
    @media (max-width: 980px){ .grid{grid-template-columns:1fr;} }
    .panel{background:var(--panel); border:1px solid var(--border); border-radius:10px; overflow:hidden;}
    .panel h2{margin:0; padding:10px 12px; font-size:13px; background:var(--panel2); border-bottom:1px solid var(--border);}
    .panel .content{padding:12px;}
    .row{display:flex; gap:10px; flex-wrap:wrap; align-items:center;}
    .badge{display:inline-block; padding:3px 8px; border-radius:999px; border:1px solid var(--border); background:#0c1118; font-size:12px; color:var(--muted)}
    .badge.good{color:#b9fbc0; border-color:#2e5b33}
    .badge.warn{color:var(--warn); border-color:#5b4a2e}
    .badge.bad{color:var(--bad); border-color:#5b2e2e}
    .kv{display:grid; grid-template-columns:auto auto; gap:4px 10px; font-size:12px; color:var(--muted)}
    .kv b{color:var(--fg); font-weight:600}
    canvas{width:100%; height:260px; background:#071018; border:1px solid var(--border); border-radius:10px;}
    #bpChart{height:240px;}
    .ctrl{font-size:12px; color:var(--muted)}
    select,button{font:inherit; font-size:12px; padding:6px 8px; background:#0c1118; color:var(--fg); border:1px solid var(--border); border-radius:8px;}
    button{cursor:pointer}
    button:disabled{opacity:0.5; cursor:not-allowed}
    .small{font-size:12px; color:var(--muted)}
    .topoWrap{position:relative; width:100%; max-width:520px; margin:0 auto;}
    .topoWrap canvas{height:320px;}
    .legend{display:flex; justify-content:space-between; font-size:11px; color:var(--muted); margin-top:8px;}
    .warnbox{padding:10px 12px; background:#1b1520; border:1px solid #3b2a45; border-radius:10px; color:#e5d4ff;}
    .warnbox code{color:#fff}
    .split{display:grid; grid-template-columns:1fr; gap:12px;}
  </style>
</head>
<body>
<header>
  <h1>qEEG Real-time Dashboard</h1>
  <div class="sub">Token-protected dashboard for <code>qeeg_nf_cli</code> outputs (SSE streaming).</div>
</header>
<main>
  <div id="status" class="warnbox" style="display:none"></div>
  <div class="grid">
    <section class="panel">
      <h2>Neurofeedback</h2>
      <div class="content">
        <div class="row" style="justify-content:space-between; margin-bottom:8px;">
          <div class="row" style="gap:8px">
            <span id="nfConn" class="badge">nf: …</span>
            <span id="bpConn" class="badge">bandpower: …</span>
            <span id="artConn" class="badge">artifact: …</span>
            <span id="metaConn" class="badge">meta: …</span>
          </div>
          <div class="row">
            <span class="ctrl">Window</span>
            <select id="winSel">
              <option value="30">30s</option>
              <option value="60" selected>60s</option>
              <option value="120">120s</option>
              <option value="300">300s</option>
            </select>
            <button id="pauseBtn" type="button">Pause</button>
          </div>
        </div>
        <div class="kv" id="nfStats"></div>
        <div style="height:10px"></div>
        <canvas id="nfChart" width="900" height="260"></canvas>
        <div class="small" style="margin-top:8px">Plot: metric (line) + threshold (dashed). Reward frames shaded. Artifacts (if present) shown in red.</div>
      </div>
    </section>

    <section class="panel">
      <h2>Bandpower</h2>
      <div class="content">
        <div class="split">
          <div>
            <div class="row" style="justify-content:space-between; margin-bottom:10px;">
              <div class="row">
                <span class="ctrl">Band</span>
                <select id="bandSel"></select>
                <span class="ctrl">Channel</span>
                <select id="chSel"></select>
              </div>
              <div class="row">
                <span class="ctrl">Transform</span>
                <select id="xformSel">
                  <option value="linear" selected>linear</option>
                  <option value="log10">log10</option>
                  <option value="db">dB (10·log10)</option>
                </select>
                <span class="ctrl">Scale</span>
                <select id="scaleSel">
                  <option value="auto" selected>auto</option>
                  <option value="fixed">fixed (rolling)</option>
                </select>
                <span class="ctrl">Labels</span>
                <select id="lblSel">
                  <option value="on" selected>on</option>
                  <option value="off">off</option>
                </select>
              </div>
            </div>
            <div class="topoWrap">
              <canvas id="topoCanvas" width="900" height="320" aria-label="scalp topography"></canvas>
            </div>
            <div class="legend"><span id="vmin">min: —</span><span id="vmax">max: —</span></div>
            <div class="small" style="margin-top:8px">Topography uses a simple inverse-distance interpolation of electrode values (approx 10-10 positions) from <code>bandpower_timeseries.csv</code>. If channels are not standard electrode labels, a fallback ring layout is used; for accurate placement, use <code>qeeg_nf_cli --channel-map</code> and/or drop a <code>montage.csv</code> (name,x,y) into the output directory. Tip: click an electrode to select that channel.</div>
          </div>

          <div>
            <canvas id="bpChart" width="900" height="240" aria-label="bandpower time series"></canvas>
            <div class="small" style="margin-top:8px">Time-series plot of selected band/channel (from <code>bandpower_timeseries.csv</code>).</div>
          </div>
        </div>
      </div>
    </section>
  </div>
</main>
<script>
(() => {
  const TOKEN = (window.location.search.match(/(?:\?|&)token=([^&]+)/)||[])[1] || "";
  const qs = (id) => document.getElementById(id);

  const statusBox = qs('status');
  function showStatus(msg){ statusBox.style.display='block'; statusBox.innerHTML = msg; }
  function hideStatus(){ statusBox.style.display='none'; statusBox.innerHTML = ''; }

  function fmtAgeSec(age){
    if(age===null || age===undefined || !isFinite(age)) return '—';
    if(age < 1.0) return '<1s';
    if(age < 60) return `${Math.round(age)}s`;
    const m = Math.floor(age/60); const s = Math.round(age - 60*m);
    return `${m}m ${s}s`;
  }

  function updateFileStatus(meta){
    const fs = (meta && meta.files_stat) ? meta.files_stat : null;
    if(!fs){ return; }
    const now = Date.now() / 1000;
    const rows = [];
    let anyMissing = false;
    let anyStale = false;
    const add = (label, st) => {
      const ok = !!(st && st.exists);
      if(!ok) anyMissing = true;
      const age = (ok && st.mtime_utc) ? (now - st.mtime_utc) : null;
      if(ok && age !== null && age > 5) anyStale = true;
      const cls = ok ? (age !== null && age > 5 ? 'warn' : 'good') : 'bad';
      rows.push(`<div class="row" style="justify-content:space-between; margin:3px 0">
        <span><b>${label}</b> <span class="badge ${cls}" style="margin-left:8px">${ok?'ok':'missing'}</span></span>
        <span class="small">age: ${fmtAgeSec(age)} &nbsp; size: ${ok ? (st.size_bytes||0) : '—'}</span>
      </div>`);
    };
    add('nf_feedback.csv', fs.nf_feedback);
    add('bandpower_timeseries.csv', fs.bandpower_timeseries);
    add('artifact_gate_timeseries.csv', fs.artifact_gate_timeseries);

    if(anyMissing){
      showStatus(`<div style="margin-bottom:6px"><b>Waiting on outputs…</b> (start qeeg_nf_cli, and consider <code>--flush-csv</code> for live updates)</div>${rows.join('')}`);
    } else if(anyStale){
      showStatus(`<div style="margin-bottom:6px"><b>Outputs look stale</b> (no new writes in &gt;5s). Is the session paused or finished?</div>${rows.join('')}`);
    } else {
      hideStatus();
    }
  }
  function setBadge(el, txt, cls){ el.textContent = txt; el.className = 'badge' + (cls?(' '+cls):''); }

  const nfConn = qs('nfConn');
  const bpConn = qs('bpConn');
  const artConn = qs('artConn');
  const metaConn = qs('metaConn');

  const winSel = qs('winSel');
  const pauseBtn = qs('pauseBtn');

  const nfStats = qs('nfStats');
  const canvas = qs('nfChart');
  const ctx = canvas.getContext('2d');

  const bandSel = qs('bandSel');
  const chSel = qs('chSel');
  const xformSel = qs('xformSel');
  const scaleSel = qs('scaleSel');
  const lblSel = qs('lblSel');
  const vminEl = qs('vmin');
  const vmaxEl = qs('vmax');

  const topoCanvas = qs('topoCanvas');
  const topoCtx = topoCanvas.getContext('2d');
  const bpCanvas = qs('bpChart');
  const bpCtx = bpCanvas.getContext('2d');

  const state = {
    paused: false,
    winSec: 60,
    nf: {frames: [], lastT: -Infinity},
    bp: {meta: null, frames: [], lastT: -Infinity, rollingMin: null, rollingMax: null, tmpCanvas: null, tmpW: 0, tmpH: 0, electrodesPx: []},
    art: {frames: [], latest: null, lastT: -Infinity},
  };

  pauseBtn.onclick = () => {
    state.paused = !state.paused;
    pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
  };
  winSel.onchange = () => { state.winSec = parseFloat(winSel.value||'60') || 60; };

  function fmt(x){
    if(x===null || x===undefined || !isFinite(x)) return '—';
    if(Math.abs(x) >= 100) return x.toFixed(1);
    if(Math.abs(x) >= 10) return x.toFixed(2);
    return x.toFixed(3);
  }

  function xformValue(v){
    if(v===null || v===undefined || !isFinite(v)) return null;
    const mode = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    if(mode === 'linear') return v;
    const eps = 1e-12;
    if(v <= eps) return null;
    if(mode === 'log10') return Math.log10(v);
    if(mode === 'db') return 10.0 * Math.log10(v);
    return v;
  }

  function updateStats(){
    const f = state.nf.frames.length ? state.nf.frames[state.nf.frames.length-1] : null;
    const a = state.art.latest;
    const parts = [];
    const add = (k,v) => parts.push(`<span>${k}</span><b>${v}</b>`);
    if(f){
      add('t (s)', fmt(f.t));
      add('metric', fmt(f.metric));
      add('threshold', fmt(f.threshold));
      add('reward', (f.reward? '1':'0'));
      add('reward_rate', fmt(f.reward_rate));
      if(f.artifact_ready!==null && f.artifact_ready!==undefined) add('artifact_ready', String(f.artifact_ready));
      if(f.phase) add('phase', String(f.phase));
      if(f.bad_channels!==null && f.bad_channels!==undefined) add('bad_ch', String(f.bad_channels));
    } else {
      add('status', 'waiting for nf_feedback.csv');
    }
    if(a){
      add('artifact', a.bad? '1':'0');
      add('artifact_bad_ch', String(a.bad_channels||0));
      if(a.ready!==null && a.ready!==undefined) add('artifact_ready', String(a.ready));
    }
    nfStats.innerHTML = parts.join('');
  }

  function resizeCanvas(){
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = Math.max(300, Math.floor(rect.width * dpr));
    const h = Math.max(180, Math.floor(rect.height * dpr));
    if(canvas.width !== w || canvas.height !== h){
      canvas.width = w; canvas.height = h;
    }
  }

  function resizeCanvasTo(el){
    const dpr = window.devicePixelRatio || 1;
    const rect = el.getBoundingClientRect();
    const w = Math.max(320, Math.floor(rect.width * dpr));
    const h = Math.max(200, Math.floor(rect.height * dpr));
    if(el.width !== w || el.height !== h){
      el.width = w; el.height = h;
      return true;
    }
    return false;
  }

  function drawChart(){
    resizeCanvas();
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0,0,w,h);

    const frames = state.nf.frames;
    if(!frames.length){
      ctx.fillStyle = '#9fb0c3';
      ctx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      ctx.fillText('Waiting for nf_feedback.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    const tNow = frames[frames.length-1].t;
    const tMin = Math.max(frames[0].t, tNow - state.winSec);

    const vis = [];
    for(let i=0;i<frames.length;i++){
      const f = frames[i];
      if(f.t >= tMin) vis.push(f);
    }
    if(vis.length<2) return;

    let yMin = Infinity, yMax = -Infinity;
    for(const f of vis){
      if(isFinite(f.metric)){ yMin = Math.min(yMin, f.metric); yMax = Math.max(yMax, f.metric); }
      if(isFinite(f.threshold)){ yMin = Math.min(yMin, f.threshold); yMax = Math.max(yMax, f.threshold); }
    }
    if(!(yMax>yMin)) { yMax = yMin + 1; }
    const pad = 0.05*(yMax-yMin);
    yMin -= pad; yMax += pad;

    const x = (t) => (t - tMin) / (tNow - tMin) * (w-20) + 10;
    const y = (v) => (h-20) - (v - yMin)/(yMax-yMin) * (h-30);

    // Grid
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1;
    for(let i=0;i<=4;i++){
      const yy = 10 + i*(h-30)/4;
      ctx.beginPath(); ctx.moveTo(10,yy); ctx.lineTo(w-10,yy); ctx.stroke();
    }

    // Reward shading
    for(let i=0;i<vis.length-1;i++){
      if(vis[i].reward){
        const x0 = x(vis[i].t);
        const x1 = x(vis[i+1].t);
        ctx.fillStyle = 'rgba(100, 210, 255, 0.08)';
        ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
      }
    }

    // Artifact shading (prefer artifact_gate_timeseries if present; fall back to nf frames)
    const art = state.art.frames;
    if(art && art.length >= 2){
      // Find art frames overlapping the visible window.
      let startIdx = 0;
      while(startIdx+1 < art.length && art[startIdx+1].t < tMin) startIdx++;
      for(let i=startIdx; i<art.length-1; i++){
        const a0 = art[i], a1 = art[i+1];
        if(a0.t > tNow) break;
        if(a0.bad){
          const x0 = x(Math.max(a0.t, tMin));
          const x1 = x(Math.min(a1.t, tNow));
          if(x1 > x0){
            ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
            ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
          }
        }
      }
      // Extend last segment to now.
      const last = art[art.length-1];
      if(last && last.t <= tNow && last.bad){
        const x0 = x(Math.max(last.t, tMin));
        const x1 = x(tNow);
        if(x1 > x0){
          ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
          ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
        }
      }
    } else {
      for(let i=0;i<vis.length-1;i++){
        if(vis[i].artifact){
          const x0 = x(vis[i].t);
          const x1 = x(vis[i+1].t);
          ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
          ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
        }
      }
    }

    // Metric line
    ctx.strokeStyle = '#e8eef6';
    ctx.lineWidth = 2;
    ctx.beginPath();
    for(let i=0;i<vis.length;i++){
      const f = vis[i];
      const xx = x(f.t);
      const yy = y(f.metric);
      if(i===0) ctx.moveTo(xx,yy); else ctx.lineTo(xx,yy);
    }
    ctx.stroke();

    // Threshold line (dashed)
    ctx.strokeStyle = '#64d2ff';
    ctx.setLineDash([6,4]);
    ctx.lineWidth = 2;
    ctx.beginPath();
    for(let i=0;i<vis.length;i++){
      const f = vis[i];
      const xx = x(f.t);
      const yy = y(f.threshold);
      if(i===0) ctx.moveTo(xx,yy); else ctx.lineTo(xx,yy);
    }
    ctx.stroke();
    ctx.setLineDash([]);

    // Axis labels
    ctx.fillStyle = '#9fb0c3';
    ctx.font = `${Math.round(11*(window.devicePixelRatio||1))}px system-ui`;
    ctx.fillText(`t: ${tMin.toFixed(1)}–${tNow.toFixed(1)}s`, 12, h-6);
    ctx.fillText(`y: ${fmt(yMin)}–${fmt(yMax)}`, 12, 22);
  }

  function valueToColor(v, vmin, vmax){
    if(v===null || v===undefined || !isFinite(v)) return '#2a3440';
    if(!(vmax>vmin)) return '#2a3440';
    let t = (v - vmin) / (vmax - vmin);
    if(t<0) t=0; if(t>1) t=1;
    const hue = (1 - t) * 240; // blue->red
    return `hsl(${hue}, 90%, 55%)`;
  }

  function hslToRgb(h, s, l){
    // h in [0,360)
    h = ((h%360)+360)%360;
    s = Math.max(0, Math.min(1, s));
    l = Math.max(0, Math.min(1, l));
    const c = (1 - Math.abs(2*l - 1)) * s;
    const hp = h / 60;
    const x = c * (1 - Math.abs((hp % 2) - 1));
    let r=0,g=0,b=0;
    if(0<=hp && hp<1){ r=c; g=x; b=0; }
    else if(1<=hp && hp<2){ r=x; g=c; b=0; }
    else if(2<=hp && hp<3){ r=0; g=c; b=x; }
    else if(3<=hp && hp<4){ r=0; g=x; b=c; }
    else if(4<=hp && hp<5){ r=x; g=0; b=c; }
    else if(5<=hp && hp<6){ r=c; g=0; b=x; }
    const m = l - c/2;
    r = Math.round(255*(r+m)); g=Math.round(255*(g+m)); b=Math.round(255*(b+m));
    return [r,g,b];
  }

  function valueToRgb(v, vmin, vmax){
    if(v===null || v===undefined || !isFinite(v) || !(vmax>vmin)) return [42,52,64];
    let t = (v - vmin) / (vmax - vmin);
    if(t<0) t=0; if(t>1) t=1;
    const hue = (1 - t) * 240;
    return hslToRgb(hue, 0.90, 0.55);
  }

  function drawTopo(){
    const meta = state.bp.meta;
    if(!meta){
      resizeCanvasTo(topoCanvas);
      topoCtx.clearRect(0,0,topoCanvas.width, topoCanvas.height);
      topoCtx.fillStyle = '#9fb0c3';
      topoCtx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      topoCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      vminEl.textContent = 'min: —';
      vmaxEl.textContent = 'max: —';
      return;
    }

    const band = bandSel.value || (meta.bands[0] || '');
    const bIdx = Math.max(0, meta.bands.indexOf(band));
    const nCh = meta.channels.length;
    const latest = state.bp.frames.length ? state.bp.frames[state.bp.frames.length-1] : null;

    let vals = null;
    if(latest && Array.isArray(latest.values) && latest.values.length >= (meta.bands.length*nCh)){
      vals = [];
      for(let c=0;c<nCh;c++) vals.push(xformValue(latest.values[bIdx*nCh + c]));
    }

    // Compute vmin/vmax.
    let vmin = Infinity, vmax = -Infinity;
    if(vals){
      for(const v of vals){ if(v!==null && v!==undefined && isFinite(v)){ vmin=Math.min(vmin,v); vmax=Math.max(vmax,v);} }
    }
    if(!(vmax>vmin)) { vmin = 0; vmax = 1; }

    // Rolling fixed scale.
    if(scaleSel.value === 'fixed'){
      if(state.bp.rollingMin===null || state.bp.rollingMax===null){
        state.bp.rollingMin = vmin; state.bp.rollingMax = vmax;
      } else {
        state.bp.rollingMin = 0.98*state.bp.rollingMin + 0.02*vmin;
        state.bp.rollingMax = 0.98*state.bp.rollingMax + 0.02*vmax;
      }
      vmin = state.bp.rollingMin;
      vmax = state.bp.rollingMax;
    }

    vminEl.textContent = `min: ${isFinite(vmin)?vmin.toFixed(3):'—'}`;
    vmaxEl.textContent = `max: ${isFinite(vmax)?vmax.toFixed(3):'—'}`;

    resizeCanvasTo(topoCanvas);
    const w = topoCanvas.width, h = topoCanvas.height;
    topoCtx.clearRect(0,0,w,h);

    // Geometry for head model.
    const dpr = window.devicePixelRatio || 1;
    const pad = 16 * dpr;
    const cx = w/2, cy = h/2 + 6*dpr;
    const R = Math.min(w, h) * 0.42;

    // Background.
    topoCtx.fillStyle = '#071018';
    topoCtx.fillRect(0,0,w,h);

    // Compute interpolated grid on a coarse lattice and draw via ImageData.
    const grid = 120;
    const imgW = grid, imgH = grid;
    const img = topoCtx.createImageData(imgW, imgH);
    const eps = 1e-6;
    const p = 2.0;

    // Pre-pack electrode coords.
    const pts = [];
    if(vals){
      for(let i=0;i<nCh;i++){
        const pos = meta.positions[i];
        const v = vals[i];
        if(!pos) continue;
        if(v===null || v===undefined || !isFinite(v)) continue;
        pts.push({x:pos[0], y:pos[1], v});
      }
    }

    for(let gy=0; gy<imgH; gy++){
      for(let gx=0; gx<imgW; gx++){
        const xh = (gx/(imgW-1))*2 - 1;
        const yh = (gy/(imgH-1))*2 - 1;
        const rr = xh*xh + yh*yh;
        const idx = (gy*imgW + gx)*4;
        if(rr > 1.0){
          img.data[idx+3] = 0;
          continue;
        }
        if(!pts.length){
          img.data[idx+0] = 42; img.data[idx+1] = 52; img.data[idx+2] = 64; img.data[idx+3] = 255;
          continue;
        }
        let sw = 0, sv = 0;
        for(const pt of pts){
          const dx = xh - pt.x;
          const dy = yh - pt.y;
          const d2 = dx*dx + dy*dy;
          const wgt = 1.0 / (Math.pow(d2 + eps, p/2));
          sw += wgt;
          sv += wgt * pt.v;
        }
        const vv = (sw>0) ? (sv/sw) : null;
        const [r,g,b] = valueToRgb(vv, vmin, vmax);
        img.data[idx+0]=r; img.data[idx+1]=g; img.data[idx+2]=b; img.data[idx+3]=255;
      }
    }

    // Draw interpolated field clipped to head circle.
    topoCtx.save();
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.clip();
    topoCtx.imageSmoothingEnabled = true;
    // Draw scaled image centered on the head.
    const x0 = cx - R, y0 = cy - R;
    // putImageData draws at 1:1; we copy to an internal tmp canvas and scale to the head.
    if(!state.bp.tmpCanvas){ state.bp.tmpCanvas = document.createElement('canvas'); }
    const tmp = state.bp.tmpCanvas;
    if(state.bp.tmpW !== imgW || state.bp.tmpH !== imgH){ tmp.width = imgW; tmp.height = imgH; state.bp.tmpW = imgW; state.bp.tmpH = imgH; }
    tmp.getContext('2d').putImageData(img, 0, 0);
    topoCtx.drawImage(tmp, x0, y0, 2*R, 2*R);
    topoCtx.restore();

    // Head outline + features.
    topoCtx.strokeStyle = 'rgba(255,255,255,0.25)';
    topoCtx.lineWidth = 2*dpr;
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.stroke();
    // Nose
    topoCtx.beginPath();
    topoCtx.moveTo(cx - 0.08*R, cy - 1.02*R);
    topoCtx.lineTo(cx, cy - 1.12*R);
    topoCtx.lineTo(cx + 0.08*R, cy - 1.02*R);
    topoCtx.stroke();
    // Ears
    topoCtx.beginPath();
    topoCtx.moveTo(cx - 1.02*R, cy - 0.18*R);
    topoCtx.quadraticCurveTo(cx - 1.12*R, cy, cx - 1.02*R, cy + 0.18*R);
    topoCtx.stroke();
    topoCtx.beginPath();
    topoCtx.moveTo(cx + 1.02*R, cy - 0.18*R);
    topoCtx.quadraticCurveTo(cx + 1.12*R, cy, cx + 1.02*R, cy + 0.18*R);
    topoCtx.stroke();

    // Electrodes.
    const showLabels = (lblSel.value || 'on') === 'on';
    state.bp.electrodesPx = [];
    topoCtx.font = `${Math.round(11*dpr)}px system-ui`;
    topoCtx.textAlign = 'center';
    topoCtx.textBaseline = 'middle';
    for(let i=0;i<nCh;i++){
      const pos = meta.positions[i];
      if(!pos) continue;
      const ex = cx + pos[0]*R;
      const ey = cy - pos[1]*R;
      state.bp.electrodesPx.push([ex, ey, i]);
      topoCtx.beginPath();
      topoCtx.arc(ex, ey, 7*dpr, 0, Math.PI*2);
      topoCtx.fillStyle = 'rgba(0,0,0,0.35)';
      topoCtx.fill();
      const src = (meta.positions_source && meta.positions_source[i]) ? meta.positions_source[i] : '';
      topoCtx.strokeStyle = (src === 'fallback') ? 'rgba(255,207,92,0.65)' : 'rgba(255,255,255,0.35)';
      topoCtx.lineWidth = 1.5*dpr;
      topoCtx.stroke();
      if(showLabels){
        topoCtx.fillStyle = 'rgba(255,255,255,0.78)';
        topoCtx.fillText(meta.channels[i], ex, ey);
      }
    }
  }

  function drawBandpowerSeries(){
    const meta = state.bp.meta;
    resizeCanvasTo(bpCanvas);
    const w = bpCanvas.width, h = bpCanvas.height;
    bpCtx.clearRect(0,0,w,h);
    bpCtx.fillStyle = '#071018';
    bpCtx.fillRect(0,0,w,h);

    if(!meta || !state.bp.frames.length){
      bpCtx.fillStyle = '#9fb0c3';
      bpCtx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      bpCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    const band = bandSel.value || (meta.bands[0] || '');
    const bIdx = Math.max(0, meta.bands.indexOf(band));
    const chName = chSel.value || (meta.channels[0] || '');
    const cIdx = Math.max(0, meta.channels.indexOf(chName));
    const nCh = meta.channels.length;

    const frames = state.bp.frames;
    const tNow = frames[frames.length-1].t;
    const tMin = Math.max(frames[0].t, tNow - state.winSec);
    const vis = [];
    for(const f of frames){
      if(f.t >= tMin && f.values && f.values.length >= meta.bands.length*nCh){
        vis.push({t:f.t, v:xformValue(f.values[bIdx*nCh + cIdx])});
      }
    }
    if(vis.length < 2) return;

    let yMin = Infinity, yMax = -Infinity;
    for(const p of vis){
      if(p.v!==null && p.v!==undefined && isFinite(p.v)){
        yMin = Math.min(yMin, p.v);
        yMax = Math.max(yMax, p.v);
      }
    }
    if(!(yMax>yMin)){ yMax = yMin + 1; }
    const pad = 0.05*(yMax-yMin);
    yMin -= pad; yMax += pad;

    const x = (t) => (t - tMin) / (tNow - tMin) * (w-20) + 10;
    const y = (v) => (h-20) - (v - yMin)/(yMax-yMin) * (h-30);

    // Grid
    bpCtx.strokeStyle = 'rgba(255,255,255,0.06)';
    bpCtx.lineWidth = 1;
    for(let i=0;i<=4;i++){
      const yy = 10 + i*(h-30)/4;
      bpCtx.beginPath(); bpCtx.moveTo(10,yy); bpCtx.lineTo(w-10,yy); bpCtx.stroke();
    }

    // Line
    bpCtx.strokeStyle = '#e8eef6';
    bpCtx.lineWidth = 2;
    bpCtx.beginPath();
    for(let i=0;i<vis.length;i++){
      const p = vis[i];
      const xx = x(p.t);
      const yy = y(p.v);
      if(i===0) bpCtx.moveTo(xx,yy); else bpCtx.lineTo(xx,yy);
    }
    bpCtx.stroke();

    // Labels
    bpCtx.fillStyle = '#9fb0c3';
    bpCtx.font = `${Math.round(11*(window.devicePixelRatio||1))}px system-ui`;
    const xm = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    const xLabel = (xm && xm !== 'linear') ? ` (${xm})` : '';
    bpCtx.fillText(`${band}:${chName}${xLabel}  y: ${fmt(yMin)}–${fmt(yMax)}`, 12, 22);
    bpCtx.fillText(`t: ${tMin.toFixed(1)}–${tNow.toFixed(1)}s`, 12, h-6);
  }

  function connectSSE(path, badgeEl, label){
    if(!window.EventSource){
      setBadge(badgeEl, `${label}: no EventSource`, 'warn');
      return null;
    }
    const url = `${path}?token=${encodeURIComponent(TOKEN)}`;
    const es = new EventSource(url);
    setBadge(badgeEl, `${label}: connecting`, 'warn');
    es.onopen = () => { setBadge(badgeEl, `${label}: connected`, 'good'); };
    // Browsers automatically reconnect EventSource streams; treat onerror as "reconnecting".
    es.onerror = () => { setBadge(badgeEl, `${label}: reconnecting`, 'warn'); };
    return es;
  }

  function unpackMsg(msg){
    if(msg && typeof msg === 'object' && msg.type === 'batch' && Array.isArray(msg.frames)){
      return {frames: msg.frames, reset: !!msg.reset};
    }
    return {frames: [msg], reset: false};
  }

  function applyMeta(meta){
    if(!meta || typeof meta !== 'object') return;
    updateFileStatus(meta);
    // Update band/channel selectors if the header becomes available mid-run.
    if(meta.bandpower && meta.bandpower.bands && meta.bandpower.channels){
      const bp = meta.bandpower;
      const prev = state.bp.meta;
      const prevBands = prev && prev.bands ? prev.bands.join('|') : '';
      const prevCh = prev && prev.channels ? prev.channels.join('|') : '';
      const newBands = bp.bands.join('|');
      const newCh = bp.channels.join('|');
      const bandWas = bandSel.value;
      const chWas = chSel.value;
      state.bp.meta = bp;
      if(newBands !== prevBands){
        bandSel.innerHTML = '';
        for(const b of bp.bands){
          const opt = document.createElement('option');
          opt.value = b; opt.textContent = b;
          bandSel.appendChild(opt);
        }
      }
      if(newCh !== prevCh){
        chSel.innerHTML = '';
        for(const c of bp.channels){
          const opt = document.createElement('option');
          opt.value = c; opt.textContent = c;
          chSel.appendChild(opt);
        }
      }
      // Restore previous selections if still valid.
      if(bandWas && bp.bands.includes(bandWas)) bandSel.value = bandWas;
      if(chWas && bp.channels.includes(chWas)) chSel.value = chWas;
    }
  }

  function start(){
    if(!TOKEN){
      showStatus('Missing token. Re-open using the printed URL (it includes <code>?token=…</code>).');
      return;
    }

    fetch(`/api/meta?token=${encodeURIComponent(TOKEN)}`)
      .then(r => r.ok ? r.json() : Promise.reject(r.status))
      .then(meta => {
        applyMeta(meta);

        // Prefer streaming meta updates (file status + bandpower header).
        const metaSse = connectSSE('/api/sse/meta', metaConn, 'meta');
        if(metaSse){
          metaSse.onmessage = (ev) => {
            try{
              const msg = JSON.parse(ev.data);
              const un = unpackMsg(msg);
              let dirty = false;
              for(const m of un.frames){ applyMeta(m); dirty = true; }
              if(dirty){ drawTopo(); drawBandpowerSeries(); }
            }catch(e){}
          };
        } else {
          // Fallback polling.
          setBadge(metaConn, 'meta: polling', 'warn');
          setInterval(() => {
            fetch(`/api/meta?token=${encodeURIComponent(TOKEN)}`)
              .then(r => r.ok ? r.json() : null)
              .then(m => { if(m){ applyMeta(m); drawTopo(); drawBandpowerSeries(); } })
              .catch(() => {});
          }, 2000);
        }

        // Initial render.
        drawTopo();
        drawBandpowerSeries();
        updateStats();
        drawChart();

        // Streaming.
        const nf = connectSSE('/api/sse/nf', nfConn, 'nf');
        if(nf){
          nf.onmessage = (ev) => {
            if(state.paused) return;
            try{
              const msg = JSON.parse(ev.data);
              const un = unpackMsg(msg);
              if(un.reset){ state.nf.frames = []; state.nf.lastT = -Infinity; }
              for(const f of un.frames){
                if(!f || typeof f !== 'object') continue;
                if(f.t !== null && f.t !== undefined && isFinite(f.t)){
                  if(f.t <= state.nf.lastT) continue;
                  state.nf.lastT = f.t;
                }
                state.nf.frames.push(f);
                // sync artifact status from nf frames if present
                if(f.artifact !== undefined) state.art.latest = {bad: !!f.artifact, bad_channels: f.bad_channels||0};
              }
              if(state.nf.frames.length > 20000) state.nf.frames.splice(0, state.nf.frames.length-20000);
              updateStats();
            }catch(e){}
          };
        }

        const bp = connectSSE('/api/sse/bandpower', bpConn, 'bandpower');
        if(bp){
          bp.onmessage = (ev) => {
            if(state.paused) return;
            try{
              const msg = JSON.parse(ev.data);
              const un = unpackMsg(msg);
              if(un.reset){ state.bp.frames = []; state.bp.lastT = -Infinity; state.bp.rollingMin = null; state.bp.rollingMax = null; }
              for(const f of un.frames){
                if(!f || typeof f !== 'object') continue;
                if(f.t !== null && f.t !== undefined && isFinite(f.t)){
                  if(f.t <= state.bp.lastT) continue;
                  state.bp.lastT = f.t;
                }
                state.bp.frames.push(f);
              }
              if(state.bp.frames.length > 20000) state.bp.frames.splice(0, state.bp.frames.length-20000);
              drawTopo();
              drawBandpowerSeries();
            }catch(e){}
          };
        }

        const art = connectSSE('/api/sse/artifact', artConn, 'artifact');
        if(art){
          art.onmessage = (ev) => {
            if(state.paused) return;
            try{
              const msg = JSON.parse(ev.data);
              const un = unpackMsg(msg);
              if(un.reset){ state.art.frames = []; state.art.lastT = -Infinity; state.art.latest = null; }
              for(const f of un.frames){
                if(!f || typeof f !== 'object') continue;
                if(f.t !== null && f.t !== undefined && isFinite(f.t)){
                  if(f.t <= state.art.lastT) continue;
                  state.art.lastT = f.t;
                }
                state.art.frames.push(f);
                state.art.latest = f;
              }
              if(state.art.frames.length > 40000) state.art.frames.splice(0, state.art.frames.length-40000);
              updateStats();
            }catch(e){}
          };
        }

        // Re-render loop.
        const tick = () => {
          if(!state.paused){
            drawChart();
            drawBandpowerSeries();
          }
          requestAnimationFrame(tick);
        };
        tick();

        bandSel.onchange = () => { drawTopo(); drawBandpowerSeries(); };
        chSel.onchange = () => drawBandpowerSeries();
        lblSel.onchange = () => drawTopo();
        xformSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; drawTopo(); drawBandpowerSeries(); };
        scaleSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; drawTopo(); };

        topoCanvas.addEventListener('click', (ev) => {
          const meta = state.bp.meta;
          if(!meta || !meta.channels || !meta.positions) return;
          const rect = topoCanvas.getBoundingClientRect();
          const dpr = window.devicePixelRatio || 1;
          const x = (ev.clientX - rect.left) * dpr;
          const y = (ev.clientY - rect.top) * dpr;
          const pts = state.bp.electrodesPx || [];
          let best = -1;
          let bestD = Infinity;
          for(const p of pts){
            const dx = x - p[0];
            const dy = y - p[1];
            const d2 = dx*dx + dy*dy;
            if(d2 < bestD){ bestD = d2; best = p[2]; }
          }
          const thresh = (32*dpr) * (32*dpr);
          if(best >= 0 && bestD <= thresh){
            const name = meta.channels[best];
            if(name){ chSel.value = name; drawBandpowerSeries(); }
          }
        });
      })
      .catch(err => {
        showStatus('Failed to load metadata from the server. Is the process still running?');
        setBadge(nfConn, 'nf: offline', 'bad');
        setBadge(bpConn, 'bandpower: offline', 'bad');
        setBadge(artConn, 'artifact: offline', 'bad');
        setBadge(metaConn, 'meta: offline', 'bad');
        console.error(err);
      });
  }

  start();
})();
</script>
</body>
</html>
"""



KIOSK_HTML = r'''<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>qEEG Kiosk</title>
  <style>
    html,body{height:100%;}
    body{margin:0; font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Cantarell,Noto Sans,sans-serif; background:#000; color:#fff;}
    #wrap{min-height:100%; display:flex; flex-direction:column; align-items:center; justify-content:center; padding:16px; box-sizing:border-box;}
    .big{font-size:64px; font-weight:700; letter-spacing:0.5px;}
    .mid{font-size:18px; color:#bbb; margin-top:10px; text-align:center;}
    .row{display:flex; gap:12px; flex-wrap:wrap; align-items:center; justify-content:center; margin-top:16px;}
    .badge{display:inline-block; padding:6px 12px; border-radius:999px; border:1px solid #444; background:#111; font-size:16px; color:#ddd;}
    .badge.reward{border-color:#2e5b33; color:#b9fbc0;}
    .badge.noreward{border-color:#555; color:#ddd;}
    .badge.artifact{border-color:#5b2e2e; color:#ff6b6b;}
    button{font:inherit; font-size:16px; padding:10px 14px; border-radius:10px; border:1px solid #444; background:#111; color:#fff;}
    button:active{transform:translateY(1px);}
    .small{font-size:14px; color:#aaa; margin-top:18px; text-align:center; max-width:720px; line-height:1.35;}
    code{color:#fff;}
  </style>
</head>
<body>
<div id="wrap">
  <div class="mid">qEEG Neurofeedback</div>
  <div id="metric" class="big">—</div>
  <div id="thr" class="mid">threshold: —</div>
  <div class="row">
    <span id="state" class="badge">connecting…</span>
    <span id="age" class="badge">age: —</span>
  </div>
  <div class="row" style="margin-top:20px">
    <button id="fsBtn" type="button">Fullscreen</button>
    <button id="pauseBtn" type="button">Pause</button>
  </div>
  <div class="small">
    Lightweight view for tablets / second screens. Requires the <code>?token=…</code> in the URL.
    (If you run the server with <code>--host 0.0.0.0 --allow-remote</code>, you can open the LAN URL on a Nexus/Android tablet.)
  </div>
</div>
<script>
(function(){
  var mEl = document.getElementById('metric');
  var tEl = document.getElementById('thr');
  var sEl = document.getElementById('state');
  var aEl = document.getElementById('age');
  var fsBtn = document.getElementById('fsBtn');
  var pauseBtn = document.getElementById('pauseBtn');

  var paused = false;

  function getToken(){
    var m = window.location.search.match(/(?:\?|&)token=([^&]+)/);
    return m ? decodeURIComponent(m[1]) : "";
  }
  var TOKEN = getToken();
  if(!TOKEN){
    sEl.className = "badge artifact";
    sEl.innerHTML = "missing token";
  }

  function fmt(v){
    if(v===null || v===undefined || !isFinite(v)) return "—";
    var av = Math.abs(v);
    var d = (av >= 100) ? 1 : (av >= 10 ? 2 : 3);
    return v.toFixed(d);
  }

  function setBadge(cls, txt){
    sEl.className = "badge " + cls;
    sEl.innerHTML = txt;
  }

  function setAge(age){
    if(age===null || age===undefined || !isFinite(age)){
      aEl.innerHTML = "age: —";
      return;
    }
    if(age < 1){ aEl.innerHTML = "age: <1s"; return; }
    if(age < 60){ aEl.innerHTML = "age: " + Math.round(age) + "s"; return; }
    var m = Math.floor(age/60);
    var s = Math.round(age - 60*m);
    aEl.innerHTML = "age: " + m + "m " + s + "s";
  }

  function handleFrame(fr){
    if(!fr || typeof fr !== "object") return;
    if(paused) return;

    // Unwrap batch events: use the last frame (freshest).
    if(fr.type === "batch" && fr.frames && fr.frames.length){
      fr = fr.frames[fr.frames.length - 1];
    }

    if(typeof fr.t === "number"){
      var now = (new Date()).getTime()/1000.0;
      setAge(now - fr.t);
    }

    if(typeof fr.metric === "number"){
      mEl.innerHTML = fmt(fr.metric);
    }
    if(typeof fr.threshold === "number"){
      tEl.innerHTML = "threshold: " + fmt(fr.threshold);
    }

    var reward = fr.reward ? 1 : 0;
    var artifact = fr.artifact ? 1 : 0;
    if(artifact){
      setBadge("artifact", "artifact");
    } else if(reward){
      setBadge("reward", "reward");
    } else {
      setBadge("noreward", "no reward");
    }
  }

  function connect(){
    if(!window.EventSource){
      setBadge("artifact", "no EventSource support");
      return;
    }
    var url = "/api/sse/nf?token=" + encodeURIComponent(TOKEN);
    var es = new EventSource(url);
    setBadge("noreward", "connecting…");

    es.onmessage = function(ev){
      try{
        var obj = JSON.parse(ev.data);
        handleFrame(obj);
      }catch(e){}
    };
    es.onerror = function(){
      setBadge("artifact", "disconnected");
    };
  }

  pauseBtn.onclick = function(){
    paused = !paused;
    pauseBtn.innerHTML = paused ? "Resume" : "Pause";
  };

  fsBtn.onclick = function(){
    var el = document.documentElement;
    var req = el.requestFullscreen || el.webkitRequestFullscreen || el.mozRequestFullScreen || el.msRequestFullscreen;
    if(req){ req.call(el); }
  };

  connect();
})();
</script>
</body>
</html>
'''


class DashboardHandler(BaseHTTPRequestHandler):
    server: "DashboardServer"  # type: ignore

    def log_message(self, fmt: str, *args: object) -> None:
        # Quiet by default; uncomment for debugging.
        return

    def _send_headers(
        self,
        code: int,
        *,
        content_type: Optional[str] = "text/plain; charset=utf-8",
        cache_control: Optional[str] = "no-store",
        etag: Optional[str] = None,
        last_modified: Optional[str] = None,
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> None:
        self.send_response(code)
        if content_type:
            self.send_header("Content-Type", content_type)
        if cache_control:
            self.send_header("Cache-Control", cache_control)
        self.send_header("X-Content-Type-Options", "nosniff")
        if etag:
            self.send_header("ETag", etag)
        if last_modified:
            self.send_header("Last-Modified", last_modified)
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()

    def _send(
        self,
        code: int,
        body: bytes = b"",
        content_type: str = "text/plain; charset=utf-8",
        *,
        cache_control: str = "no-store",
        etag: Optional[str] = None,
        last_modified: Optional[str] = None,
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> None:
        self._send_headers(
            code,
            content_type=content_type,
            cache_control=cache_control,
            etag=etag,
            last_modified=last_modified,
            extra_headers=extra_headers,
        )
        if body:
            try:
                self.wfile.write(body)
            except BrokenPipeError:
                pass

    def _send_not_modified(
        self, *, cache_control: str = "no-cache", etag: Optional[str] = None, last_modified: Optional[str] = None
    ) -> None:
        self._send_headers(
            HTTPStatus.NOT_MODIFIED, content_type=None, cache_control=cache_control, etag=etag, last_modified=last_modified
        )

    def _send_json(
        self,
        code: int,
        obj: object,
        *,
        cache_control: str = "no-store",
        etag: Optional[str] = None,
        last_modified: Optional[str] = None,
    ) -> None:
        self._send(code, _json_bytes(obj), "application/json; charset=utf-8", cache_control=cache_control, etag=etag, last_modified=last_modified)

    def _send_json_error(self, code: int, message: str, *, error_code: str = "error") -> None:
        self._send_json(
            code,
            {"error": {"code": str(error_code), "message": str(message)}, "server_time_utc": float(time.time())},
            cache_control="no-store",
        )

    def _require_token(self) -> bool:
        q = parse_qs(urlparse(self.path).query)
        tok = (q.get("token") or [""])[0]
        if tok != self.server.config.token:
            p = urlparse(self.path).path
            if p.startswith("/api/") and not p.startswith("/api/sse/"):
                self._send_json_error(HTTPStatus.FORBIDDEN, "Forbidden", error_code="forbidden")
            else:
                self._send(HTTPStatus.FORBIDDEN, b"Forbidden\n")
            return False
        return True

    def _read_json_body(self, *, max_bytes: int = 64 * 1024) -> Optional[object]:
        try:
            n = int(self.headers.get("Content-Length", "0") or "0")
        except Exception:
            n = 0
        if n <= 0:
            return None
        if n > max_bytes:
            self._send(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, b"Body too large\n")
            return None
        try:
            data = self.rfile.read(n)
            return json.loads(data.decode("utf-8"))
        except Exception:
            self._send(HTTPStatus.BAD_REQUEST, b"Invalid JSON\n")
            return None


    def do_GET(self) -> None:
        try:
            u = urlparse(self.path)
            path = u.path

            # Static frontend assets (prefer external files if present).
            if path in _FRONTEND_PATH_MAP:
                # HTML routes are token-protected.
                if path in ("/", "/index.html"):
                    if "token=" not in u.query:
                        url = f"/?token={self.server.config.token}"
                        self.send_response(HTTPStatus.FOUND)
                        self.send_header("Location", url)
                        self.end_headers()
                        return
                    if not self._require_token():
                        return
                    body, ctype = self.server.frontend_asset_bytes("/")
                    self._send(HTTPStatus.OK, body, ctype)
                    return

                if path in ("/kiosk", "/kiosk.html"):
                    if "token=" not in u.query:
                        url = f"/kiosk?token={self.server.config.token}"
                        self.send_response(HTTPStatus.FOUND)
                        self.send_header("Location", url)
                        self.end_headers()
                        return
                    if not self._require_token():
                        return
                    body, ctype = self.server.frontend_asset_bytes("/kiosk")
                    self._send(HTTPStatus.OK, body, ctype)
                    return

                # JS/CSS assets are safe to serve without the token.
                asset = self.server.try_frontend_asset(path)
                if asset is None:
                    self._send(HTTPStatus.NOT_FOUND, b"Not found\n")
                    return
                body, ctype, etag, last_mod = asset
                cache_cc = "public, max-age=0, must-revalidate"
                if etag and _if_none_match_matches(self.headers.get("If-None-Match", ""), etag):
                    self._send_not_modified(cache_control=cache_cc, etag=etag, last_modified=last_mod)
                    return
                self._send(HTTPStatus.OK, body, ctype, cache_control=cache_cc, etag=etag, last_modified=last_mod)
                return

            if path == "/api/meta":
                if not self._require_token():
                    return
                self._send_json(HTTPStatus.OK, self.server.build_meta())
                return

            if path == "/api/config":
                if not self._require_token():
                    return
                self._send_json(HTTPStatus.OK, self.server.build_config())
                return

            if path == "/api/state":
                if not self._require_token():
                    return
                self._send_json(HTTPStatus.OK, self.server.get_ui_state())
                return

            if path == "/api/run_meta":
                if not self._require_token():
                    return
                q = parse_qs(u.query)
                fmt = ((q.get("format") or [""])[0] or "").strip().lower()
                raw_flag = ((q.get("raw") or [""])[0] or "").strip().lower()
                want_raw = fmt in ("raw", "file") or raw_flag in ("1", "true", "yes", "y")

                rm_path = self.server.config.outdir / "nf_run_meta.json"
                st = _stat_dict(rm_path)

                etag: Optional[str] = None
                last_mod: Optional[str] = None
                if st.get("exists"):
                    try:
                        etag = _weak_etag_from_stat(int(st.get("size_bytes", 0) or 0), float(st.get("mtime_utc", 0.0) or 0.0))
                        last_mod = _http_date(float(st.get("mtime_utc", 0.0) or 0.0))
                    except Exception:
                        etag = None
                        last_mod = None

                # Conditional GET.
                if etag and _if_none_match_matches(self.headers.get("If-None-Match", ""), etag):
                    self._send_not_modified(cache_control="no-cache", etag=etag, last_modified=last_mod)
                    return

                if want_raw:
                    data = _safe_read_file(rm_path, max_bytes=5_000_000)
                    if data is None:
                        self._send_json_error(HTTPStatus.NOT_FOUND, "nf_run_meta.json not found", error_code="not_found")
                        return
                    self._send(
                        HTTPStatus.OK,
                        data,
                        "application/json; charset=utf-8",
                        cache_control="no-cache",
                        etag=etag,
                        last_modified=last_mod,
                    )
                    return

                run_obj: Optional[object] = None
                parse_error: Optional[str] = None
                if st.get("exists"):
                    try:
                        run_obj = json.loads(rm_path.read_text(encoding="utf-8"))
                    except Exception as e:
                        parse_error = str(e)

                resp = {
                    "schema_version": 1,
                    "server_time_utc": float(time.time()),
                    "server_instance_id": self.server.instance_id,
                    "path": str(rm_path),
                    "stat": st,
                    "etag": etag,
                    "data": run_obj,
                    "parse_error": parse_error,
                }
                self._send_json(HTTPStatus.OK, resp, cache_control="no-cache", etag=etag, last_modified=last_mod)
                return

            if path == "/api/stats":
                if not self._require_token():
                    return
                self._send_json(HTTPStatus.OK, self.server.build_stats())
                return

            if path == "/api/snapshot":
                if not self._require_token():
                    return
                q = parse_qs(u.query)
                topics = (q.get("topics") or [""])[0]
                wait_s = (q.get("wait") or q.get("wait_sec") or [""])[0]
                limit_s = (q.get("limit") or [""])[0]

                def _get_int(keys, default=0):
                    for k in keys:
                        try:
                            if k in q and q[k]:
                                return int(q[k][0])
                        except Exception:
                            pass
                    return default

                cursors = {
                    "nf": _get_int(["nf", "cursor_nf"], 0),
                    "bandpower": _get_int(["bandpower", "bp", "cursor_bandpower", "cursor_bp"], 0),
                    "artifact": _get_int(["artifact", "art", "cursor_artifact", "cursor_art"], 0),
                    "meta": _get_int(["meta", "cursor_meta"], 0),
                    "state": _get_int(["state", "cursor_state"], 0),
                }
                wait_sec = 0.0
                if wait_s:
                    try:
                        wait_sec = float(wait_s)
                    except Exception:
                        wait_sec = 0.0
                limit = 2500
                if limit_s:
                    try:
                        limit = int(limit_s)
                    except Exception:
                        limit = 2500
                resp = self.server.build_snapshot(topics=topics, cursors=cursors, wait_sec=wait_sec, limit=limit)
                self._send_json(HTTPStatus.OK, resp)
                return

            if path == "/api/sse/nf":
                if not self._require_token():
                    return
                self.server.stream_nf(self)
                return

            if path == "/api/sse/bandpower":
                if not self._require_token():
                    return
                self.server.stream_bandpower(self)
                return

            if path == "/api/sse/artifact":
                if not self._require_token():
                    return
                self.server.stream_artifact(self)
                return

            if path == "/api/sse/meta":
                if not self._require_token():
                    return
                self.server.stream_meta(self)
                return

            if path == "/api/sse/state":
                if not self._require_token():
                    return
                self.server.stream_state(self)
                return

            if path == "/api/sse/stream":
                if not self._require_token():
                    return
                q = parse_qs(u.query)
                topics = (q.get("topics") or [""])[0]
                hz_s = (q.get("hz") or [""])[0]
                hz: Optional[float] = None
                if hz_s:
                    try:
                        hz = float(hz_s)
                    except Exception:
                        hz = None
                self.server._conn_inc("stream")
                try:
                    self.server.stream_stream(self, topics=topics, max_hz=hz)
                finally:
                    self.server._conn_dec("stream")
                return

            self._send(HTTPStatus.NOT_FOUND, b"Not found\n")
        except Exception:
            tb = traceback.format_exc()
            self._send(HTTPStatus.INTERNAL_SERVER_ERROR, tb.encode("utf-8"))

    def do_PUT(self) -> None:
        # Only state updates are supported.
        try:
            u = urlparse(self.path)
            if u.path != "/api/state":
                self._send(HTTPStatus.NOT_FOUND, b"Not found\n")
                return
            if not self._require_token():
                return
            body = self._read_json_body()
            if body is None:
                return
            if not isinstance(body, dict):
                self._send(HTTPStatus.BAD_REQUEST, b"Expected a JSON object\n")
                return
            updated = self.server.update_ui_state(body)
            self._send_json(HTTPStatus.OK, updated)
        except Exception:
            tb = traceback.format_exc()
            self._send(HTTPStatus.INTERNAL_SERVER_ERROR, tb.encode("utf-8"))

    def do_POST(self) -> None:
        # Alias POST -> PUT for browser compatibility.
        return self.do_PUT()


class DashboardServer(ThreadingHTTPServer):
    def __init__(self, config: ServerConfig):
        super().__init__((config.host, config.port), DashboardHandler)
        self.config = config
        d = config.frontend_dir or _default_frontend_dir()
        self.frontend_dir: Optional[Path] = d if d.exists() else None

        self.hub = LiveHub(config.outdir, history_rows=config.history_rows, meta_interval_sec=config.meta_interval_sec)
        self.instance_id = secrets.token_hex(8)
        self._started_utc = float(time.time())

        # Lightweight server-side stats for debugging / UI diagnostics.
        self._conn_lock = threading.Lock()
        self._conn_counts: Dict[str, int] = {
            "nf": 0,
            "bandpower": 0,
            "artifact": 0,
            "meta": 0,
            "state": 0,
            "stream": 0,
        }

        self.hub.start()

        # UI state synchronization between clients (optional).
        self._state_lock = threading.Lock()
        self._ui_state: Dict[str, object] = self._load_ui_state()
        self._ui_state_buf = StreamBuffer(maxlen=2000)

    def server_close(self) -> None:
        try:
            self.hub.stop()
        except Exception:
            pass
        return super().server_close()

    def _sse_preamble(self, handler: DashboardHandler) -> None:
        handler.send_response(HTTPStatus.OK)
        handler.send_header("Content-Type", "text/event-stream; charset=utf-8")
        handler.send_header("Cache-Control", "no-cache")
        handler.send_header("Connection", "keep-alive")
        # Hint to reverse proxies not to buffer SSE. Harmless for direct localhost use.
        handler.send_header("X-Accel-Buffering", "no")
        handler.end_headers()
        # Set a reconnect delay (milliseconds) for EventSource clients.
        try:
            handler.wfile.write(b"retry: 1500\n\n")
            handler.wfile.flush()
        except Exception:
            pass

    def _sse_send(self, handler: DashboardHandler, obj: object, *, event: Optional[str] = None) -> bool:
        """Send an SSE message.

        If event is provided, the message is sent as a named event:
          event: <name>\n
          data: <json>\n\n
        """
        try:
            data = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
            parts: List[str] = []
            if event:
                # Defensive: EventSource event names are line-oriented.
                ev = str(event).replace("\n", " ").replace("\r", " ").strip()
                if ev:
                    parts.append(f"event: {ev}\n")
            parts.append(f"data: {data}\n\n")
            payload = "".join(parts).encode("utf-8")
            handler.wfile.write(payload)
            handler.wfile.flush()
            return True
        except BrokenPipeError:
            return False
        except Exception:
            return False

    def _sse_keepalive(self, handler: DashboardHandler) -> bool:
        try:
            handler.wfile.write(b": keepalive\n\n")
            handler.wfile.flush()
            return True
        except BrokenPipeError:
            return False
        except Exception:
            return False


    # ------------------------ server stats ------------------------

    def _conn_inc(self, key: str) -> None:
        with self._conn_lock:
            self._conn_counts[key] = int(self._conn_counts.get(key, 0)) + 1

    def _conn_dec(self, key: str) -> None:
        with self._conn_lock:
            cur = int(self._conn_counts.get(key, 0))
            self._conn_counts[key] = max(0, cur - 1)

    def build_stats(self) -> Dict[str, object]:
        """Lightweight diagnostic stats for the UI."""
        with self._conn_lock:
            conns = dict(self._conn_counts)

        def _buf_info(buf: StreamBuffer) -> Dict[str, int]:
            return {"oldest_seq": int(buf.oldest_seq()), "latest_seq": int(buf.latest_seq()), "size": int(buf.size())}

        return {
            "schema_version": 1,
            "server_time_utc": float(time.time()),
            "server_instance_id": self.instance_id,
            "uptime_sec": float(max(0.0, time.time() - float(self._started_utc))),
            "frontend": "external" if self.frontend_dir else "embedded",
            "connections": conns,
            "buffers": {
                "nf": _buf_info(self.hub.nf),
                "bandpower": _buf_info(self.hub.bandpower),
                "artifact": _buf_info(self.hub.artifact),
                "meta": _buf_info(self.hub.meta),
                "state": _buf_info(self._ui_state_buf),
            },
        }

    def build_meta(self) -> Dict[str, object]:
        return self.hub.latest_meta()

    def build_config(self) -> Dict[str, object]:
        """Configuration info for the frontend UI."""
        return {
            "schema_version": 2,
            "api_version": 1,
            "outdir": str(self.config.outdir),
            "max_hz": float(self.config.max_hz),
            "history_rows": int(self.config.history_rows),
            "meta_interval_sec": float(self.config.meta_interval_sec),
            "frontend": "external" if self.frontend_dir else "embedded",
            "server_time_utc": float(time.time()),
            "server_instance_id": self.instance_id,
            "supports": {
                "ui_state": True,
                "sse_stream": True,
                "snapshot": True,
                "run_meta": True,
                "stats": True,
                "asset_etag": True,
            },
        }

    

    # ------------------------ snapshot (polling) API ------------------------

    def _parse_snapshot_topics(self, topics: str) -> List[str]:
        """Parse topics for /api/snapshot.

        Uses the same topic names as /api/sse/stream (nf/bandpower/artifact/meta/state/config).
        """
        return self._parse_stream_topics(topics)

    def build_snapshot(
        self,
        *,
        topics: str = "",
        cursors: Optional[Dict[str, int]] = None,
        wait_sec: float = 0.0,
        limit: int = 2500,
    ) -> Dict[str, object]:
        """Build a JSON snapshot for polling clients.

        This endpoint is meant as a fallback for environments where EventSource/SSE
        is not available or not reliable.

        Query model:
          - The client supplies per-topic cursors (sequence numbers).
          - The server returns batches of frames since those cursors and updated cursors.
          - If wait_sec > 0, the server long-polls up to that many seconds waiting for new data.
        """
        wanted = self._parse_snapshot_topics(topics)
        cur = cursors or {}

        def _clamp_int(v: int, lo: int, hi: int) -> int:
            try:
                iv = int(v)
            except Exception:
                iv = lo
            return max(lo, min(hi, iv))

        wait_sec = float(wait_sec or 0.0)
        wait_sec = max(0.0, min(10.0, wait_sec))
        limit = _clamp_int(limit or 2500, 1, 10000)

        # Best-effort long-poll: wait until any selected topic has new data.
        if wait_sec > 0.0:
            t_end = time.time() + wait_sec
            while True:
                if time.time() >= t_end:
                    break

                any_new = False
                if "nf" in wanted and self.hub.nf.latest_seq() > int(cur.get("nf", 0) or 0):
                    any_new = True
                elif "bandpower" in wanted and self.hub.bandpower.latest_seq() > int(cur.get("bandpower", 0) or 0):
                    any_new = True
                elif "artifact" in wanted and self.hub.artifact.latest_seq() > int(cur.get("artifact", 0) or 0):
                    any_new = True
                elif "meta" in wanted and self.hub.meta.latest_seq() > int(cur.get("meta", 0) or 0):
                    any_new = True
                elif "state" in wanted and self._ui_state_buf.latest_seq() > int(cur.get("state", 0) or 0):
                    any_new = True

                if any_new:
                    break
                time.sleep(0.05)

        resp: Dict[str, object] = {
            "schema_version": 1,
            "server_time_utc": float(time.time()),
            "server_instance_id": self.instance_id,
        }

        if "config" in wanted:
            resp["config"] = self.build_config()

        def _topic_payload(buf: StreamBuffer, cursor: int) -> Dict[str, object]:
            last = int(cursor or 0)
            new_last, frames, reset = buf.get_since(last, limit=limit)
            return {
                "cursor": int(new_last),
                "batch": {"type": "batch", "reset": bool(reset), "frames": frames},
            }

        # Meta and state: always send a snapshot when cursor==0 to avoid an empty UI.
        if "meta" in wanted:
            meta_cur = int(cur.get("meta", 0) or 0)
            if meta_cur == 0:
                resp["meta"] = {
                    "cursor": int(self.hub.meta.latest_seq()),
                    "batch": {"type": "batch", "reset": False, "frames": [self.build_meta()]},
                }
            else:
                resp["meta"] = _topic_payload(self.hub.meta, meta_cur)

        if "state" in wanted:
            state_cur = int(cur.get("state", 0) or 0)
            if state_cur == 0:
                resp["state"] = {
                    "cursor": int(self._ui_state_buf.latest_seq()),
                    "batch": {"type": "batch", "reset": False, "frames": [self.get_ui_state()]},
                }
            else:
                resp["state"] = _topic_payload(self._ui_state_buf, state_cur)

        if "nf" in wanted:
            resp["nf"] = _topic_payload(self.hub.nf, int(cur.get("nf", 0) or 0))

        if "artifact" in wanted:
            resp["artifact"] = _topic_payload(self.hub.artifact, int(cur.get("artifact", 0) or 0))

        if "bandpower" in wanted:
            resp["bandpower"] = _topic_payload(self.hub.bandpower, int(cur.get("bandpower", 0) or 0))

        return resp
    # ------------------------ frontend asset serving ------------------------

    def try_frontend_asset(self, url_path: str) -> Optional[Tuple[bytes, str, Optional[str], Optional[str]]]:
        """Return (bytes, content_type, etag, last_modified_http) for a frontend asset, or None."""
        fname = _FRONTEND_PATH_MAP.get(url_path)
        if not fname:
            return None
        if self.frontend_dir is None:
            return None
        base = self.frontend_dir
        p = (base / fname).resolve()
        if p != base and base not in p.parents:
            return None
        data = _safe_read_file(p)
        if data is None:
            return None

        st = _stat_dict(p)
        etag: Optional[str] = None
        last_mod: Optional[str] = None
        if st.get("exists"):
            try:
                etag = _weak_etag_from_stat(int(st.get("size_bytes", 0) or 0), float(st.get("mtime_utc", 0.0) or 0.0))
                last_mod = _http_date(float(st.get("mtime_utc", 0.0) or 0.0))
            except Exception:
                etag = None
                last_mod = None
        else:
            # Fallback for unusual filesystems.
            try:
                etag = _strong_etag_from_bytes(data)
            except Exception:
                etag = None

        return data, _guess_mime(p), etag, last_mod

    def frontend_asset_bytes(self, url_path: str) -> Tuple[bytes, str]:
        """Get bytes for HTML routes, falling back to embedded HTML."""
        asset = self.try_frontend_asset(url_path)
        if asset is not None:
            body, ctype, _etag, _lm = asset
            return body, ctype
        if url_path in ("/", "/index.html"):
            return DASH_HTML.encode("utf-8"), "text/html; charset=utf-8"
        if url_path in ("/kiosk", "/kiosk.html"):
            return KIOSK_HTML.encode("utf-8"), "text/html; charset=utf-8"
        return b"Not found\n", "text/plain; charset=utf-8"

    # ------------------------ UI state sync ------------------------

    def _ui_state_path(self) -> Path:
        return self.config.outdir / "rt_dashboard_state.json"

    def _default_ui_state(self) -> Dict[str, object]:
        return {
            "schema_version": 1,
            "win_sec": 60.0,
            "paused": False,
            "band": None,
            "channel": None,
            "transform": "linear",
            "scale": "auto",
            "labels": "on",
            "updated_utc": float(time.time()),
            "updated_by": None,
        }

    def _load_ui_state(self) -> Dict[str, object]:
        path = self._ui_state_path()
        st = self._default_ui_state()
        try:
            if path.exists():
                obj = json.loads(path.read_text(encoding="utf-8"))
                if isinstance(obj, dict):
                    st.update(obj)
        except Exception:
            pass
        # Normalize/validate in case file contents are malformed.
        return self._sanitize_ui_state(st)

    def _save_ui_state(self, state: Dict[str, object]) -> None:
        path = self._ui_state_path()
        try:
            tmp = path.with_suffix(path.suffix + ".tmp")
            tmp.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            os.replace(str(tmp), str(path))
        except Exception:
            pass

    def _sanitize_ui_state(self, st: Dict[str, object]) -> Dict[str, object]:
        out = self._default_ui_state()

        def _num(v, lo, hi, default):
            try:
                f = float(v)
                if not (f == f):
                    return default
                return min(hi, max(lo, f))
            except Exception:
                return default

        # Keep schema_version if provided.
        try:
            sv = int(st.get("schema_version", 1) or 1)
        except Exception:
            sv = 1
        out["schema_version"] = sv
        out["win_sec"] = _num(st.get("win_sec", out["win_sec"]), 5.0, 3600.0, float(out["win_sec"]))
        out["paused"] = bool(st.get("paused", out["paused"]))

        def _s(v, max_len=64):
            if v is None:
                return None
            if not isinstance(v, str):
                return None
            v2 = v.strip()
            if not v2:
                return None
            return v2[:max_len]

        out["band"] = _s(st.get("band"), 64)
        out["channel"] = _s(st.get("channel"), 64)

        xform = st.get("transform", out["transform"])
        if xform not in ("linear", "log10", "db"):
            xform = "linear"
        out["transform"] = xform

        scale = st.get("scale", out["scale"])
        if scale not in ("auto", "fixed"):
            scale = "auto"
        out["scale"] = scale

        labels = st.get("labels", out["labels"])
        if labels not in ("on", "off"):
            labels = "on"
        out["labels"] = labels

        out["updated_utc"] = _num(st.get("updated_utc", time.time()), 0.0, 1e12, float(time.time()))
        out["updated_by"] = _s(st.get("updated_by"), 96)
        return out

    def get_ui_state(self) -> Dict[str, object]:
        with self._state_lock:
            st = dict(self._ui_state)
        st["server_time_utc"] = float(time.time())
        return st

    def update_ui_state(self, patch: Dict[str, object]) -> Dict[str, object]:
        # Only allow specific keys.
        allowed = {"win_sec", "paused", "band", "channel", "transform", "scale", "labels", "client_id"}
        clean_patch: Dict[str, object] = {k: patch[k] for k in patch.keys() if k in allowed}
        client_id = clean_patch.get("client_id")
        updated_by = None
        if isinstance(client_id, str) and client_id.strip():
            updated_by = client_id.strip()[:96]

        with self._state_lock:
            st = dict(self._ui_state)
            st.update(clean_patch)
            st["updated_utc"] = float(time.time())
            st["updated_by"] = updated_by
            st = self._sanitize_ui_state(st)
            self._ui_state = st
            self._save_ui_state(st)

        # Broadcast to any listeners.
        payload = dict(st)
        payload["server_time_utc"] = float(time.time())
        payload["client_id"] = updated_by
        self._ui_state_buf.append(payload)
        return payload

    def _stream_buffer(
        self,
        handler: DashboardHandler,
        buf: StreamBuffer,
        *,
        initial: Optional[object] = None,
        topic: str = "",
    ) -> None:
        self._sse_preamble(handler)
        if topic:
            self._conn_inc(topic)

        try:
            # If requested, send a single immediate frame (useful for meta/state).
            if initial is not None:
                if not self._sse_send(handler, {"type": "batch", "reset": False, "frames": [initial]}):
                    return

            last_seq = 0
            last_keep = time.time()
            last_send = 0.0
            max_hz = float(self.config.max_hz) if self.config.max_hz else 15.0
            interval = 1.0 / max(0.5, max_hz)

            while True:
                new_last, frames, reset = buf.get_since(last_seq, limit=2500)
                if frames:
                    # Throttle sends to reduce browser/UI overhead.
                    now = time.time()
                    dt = now - last_send
                    if dt < interval:
                        time.sleep(interval - dt)
                    ok = self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames})
                    if not ok:
                        break
                    last_send = time.time()
                    last_keep = last_send
                    last_seq = new_last
                    continue

                # Wait for new data (or timeout for keepalives).
                buf.wait_for_new(last_seq, timeout=0.5)
                if (time.time() - last_keep) > 2.0:
                    if not self._sse_keepalive(handler):
                        break
                    last_keep = time.time()
        finally:
            if topic:
                self._conn_dec(topic)

    def stream_nf(self, handler: DashboardHandler) -> None:
        self._stream_buffer(handler, self.hub.nf, topic="nf")

    def stream_bandpower(self, handler: DashboardHandler) -> None:
        self._stream_buffer(handler, self.hub.bandpower, topic="bandpower")

    def stream_artifact(self, handler: DashboardHandler) -> None:
        self._stream_buffer(handler, self.hub.artifact, topic="artifact")

    def stream_meta(self, handler: DashboardHandler) -> None:
        # Send immediate meta snapshot, then stream changes.
        initial = self.build_meta()
        self._stream_buffer(handler, self.hub.meta, initial=initial, topic="meta")

    def stream_state(self, handler: DashboardHandler) -> None:
        # Send immediate UI state snapshot, then stream updates.
        initial = self.get_ui_state()
        self._stream_buffer(handler, self._ui_state_buf, initial=initial, topic="state")

    # ------------------------ multiplexed SSE stream ------------------------

    def _parse_stream_topics(self, topics: str) -> List[str]:
        """Parse a comma-separated topic list.

        If topics is empty, returns the default set.
        """
        default = ["config", "meta", "state", "nf", "artifact", "bandpower"]
        t = (topics or "").strip()
        if not t:
            return default
        out: List[str] = []
        for part in t.split(","):
            p = (part or "").strip().lower()
            if not p:
                continue
            if p in ("bp", "band"):  # small aliases
                p = "bandpower"
            if p in ("art", "artifact_gate"):
                p = "artifact"
            if p not in default:
                continue
            if p not in out:
                out.append(p)
        return out or default

    def stream_stream(self, handler: DashboardHandler, *, topics: str = "", max_hz: Optional[float] = None) -> None:
        """Multiplex all dashboard topics onto a single EventSource connection.

        This improves compatibility with browsers that have low per-origin SSE
        connection limits (e.g., tablet browsers), since the UI can keep one
        streaming connection instead of several.
        """
        wanted = self._parse_stream_topics(topics)

        # Per-connection throttle (clamped to server max_hz).
        eff_hz = float(self.config.max_hz) if self.config.max_hz else 15.0
        if max_hz is not None:
            try:
                mhz = float(max_hz)
                if mhz > 0.0:
                    eff_hz = min(eff_hz, mhz)
            except Exception:
                pass
        eff_hz = max(0.5, min(60.0, eff_hz))
        interval = 1.0 / eff_hz

        self._sse_preamble(handler)

        # Send a small "hello" burst (config/state/meta) so the UI can render
        # without separate fetches.
        if "config" in wanted:
            if not self._sse_send(handler, self.build_config(), event="config"):
                return

        # Meta + state are sent as batches for a consistent client parser.
        meta_last = self.hub.meta.latest_seq()
        if "meta" in wanted:
            if not self._sse_send(handler, {"type": "batch", "reset": False, "frames": [self.build_meta()]}, event="meta"):
                return
            meta_last = self.hub.meta.latest_seq()

        state_last = self._ui_state_buf.latest_seq()
        if "state" in wanted:
            if not self._sse_send(handler, {"type": "batch", "reset": False, "frames": [self.get_ui_state()]}, event="state"):
                return
            state_last = self._ui_state_buf.latest_seq()

        nf_last = 0
        art_last = 0
        bp_last = 0

        last_keep = time.time()
        last_send = 0.0

        def _maybe_sleep() -> None:
            nonlocal last_send
            now = time.time()
            dt = now - last_send
            if dt < interval:
                time.sleep(interval - dt)
            last_send = time.time()

        while True:
            any_sent = False

            if "nf" in wanted:
                new_last, frames, reset = self.hub.nf.get_since(nf_last, limit=2500)
                if frames:
                    _maybe_sleep()
                    if not self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames}, event="nf"):
                        return
                    nf_last = new_last
                    last_keep = time.time()
                    any_sent = True

            if "artifact" in wanted:
                new_last, frames, reset = self.hub.artifact.get_since(art_last, limit=2500)
                if frames:
                    _maybe_sleep()
                    if not self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames}, event="artifact"):
                        return
                    art_last = new_last
                    last_keep = time.time()
                    any_sent = True

            if "bandpower" in wanted:
                new_last, frames, reset = self.hub.bandpower.get_since(bp_last, limit=2500)
                if frames:
                    _maybe_sleep()
                    if not self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames}, event="bandpower"):
                        return
                    bp_last = new_last
                    last_keep = time.time()
                    any_sent = True

            if "meta" in wanted:
                new_last, frames, reset = self.hub.meta.get_since(meta_last, limit=50)
                if frames:
                    _maybe_sleep()
                    if not self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames}, event="meta"):
                        return
                    meta_last = new_last
                    last_keep = time.time()
                    any_sent = True

            if "state" in wanted:
                new_last, frames, reset = self._ui_state_buf.get_since(state_last, limit=200)
                if frames:
                    _maybe_sleep()
                    if not self._sse_send(handler, {"type": "batch", "reset": reset, "frames": frames}, event="state"):
                        return
                    state_last = new_last
                    last_keep = time.time()
                    any_sent = True

            if not any_sent:
                time.sleep(0.05)
                if (time.time() - last_keep) > 2.0:
                    if not self._sse_keepalive(handler):
                        return
                    last_keep = time.time()


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Real-time dashboard for qeeg_nf_cli outputs (SSE streaming).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--outdir", required=True, help="Directory containing nf_feedback.csv, bandpower_timeseries.csv, etc.")
    ap.add_argument(
        "--host",
        default="127.0.0.1",
        help="Bind host. Default is local-only (127.0.0.1). Use 0.0.0.0 with --allow-remote to view from another device on your LAN.",
    )
    ap.add_argument(
        "--allow-remote",
        action="store_true",
        help="Allow binding to non-loopback hosts (e.g., 0.0.0.0 or a LAN IP). Use only on trusted networks.",
    )
    ap.add_argument("--port", type=int, default=0, help="Bind port (0 picks an ephemeral port)")
    ap.add_argument(
        "--token",
        default="",
        help="Fixed token for URL access (default: generate a random token).",
    )
    ap.add_argument(
        "--max-hz",
        type=float,
        default=15.0,
        help="Max SSE send rate per stream (frames are batched to stay under this rate)",
    )
    ap.add_argument(
        "--history-rows",
        type=int,
        default=1200,
        help="How many recent rows to keep/send to new clients (per CSV stream)",
    )
    ap.add_argument(
        "--meta-interval",
        type=float,
        default=1.0,
        help="How often to recompute file status metadata (seconds)",
    )
    ap.add_argument(
        "--frontend-dir",
        default="",
        help="Directory containing dashboard frontend assets (index.html/app.js/style.css). Default: scripts/rt_dashboard_frontend.",
    )
    ap.add_argument("--open", action="store_true", help="Open the dashboard URL in your default browser")
    ap.add_argument(
        "--open-kiosk",
        action="store_true",
        help="Open the lightweight /kiosk view (implies --open)",
    )
    return ap.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    ns = parse_args(argv)
    outdir = Path(ns.outdir)
    if not outdir.exists():
        print(f"Error: outdir does not exist: {outdir}", file=sys.stderr)
        return 2

    host = str(ns.host or "127.0.0.1")
    if ns.open_kiosk:
        ns.open = True

    # Safety: require an explicit opt-in to bind to non-loopback hosts.
    if (not _is_loopback_host(host)) and (not ns.allow_remote):
        print(
            "Error: refusing to bind to a non-loopback host without --allow-remote. "
            "If you want to view the dashboard from a tablet/phone on your LAN, re-run with:\n"
            "  --host 0.0.0.0 --allow-remote\n"
            "Note: the token helps, but it is not a full security boundary.",
            file=sys.stderr,
        )
        return 2

    token = (ns.token or "").strip() or secrets.token_urlsafe(16)

    frontend_dir = Path(ns.frontend_dir).expanduser() if (ns.frontend_dir or "").strip() else _default_frontend_dir()
    config = ServerConfig(
        outdir=outdir,
        host=host,
        port=int(ns.port),
        token=token,
        max_hz=float(ns.max_hz),
        history_rows=int(ns.history_rows),
        meta_interval_sec=float(ns.meta_interval),
        frontend_dir=frontend_dir,
    )

    httpd = DashboardServer(config)
    # Grab the real port if 0.
    bind_host, port = str(httpd.server_address[0]), int(httpd.server_address[1])

    urls = _build_urls(bind_host, port, token)

    # flush=True so scripts that capture stdout can reliably parse the URL.
    print(f"Dashboard: {urls['dashboard_url']}", flush=True)
    if "dashboard_url_lan" in urls:
        print(f"Dashboard (LAN): {urls['dashboard_url_lan']}", flush=True)

    print(f"Kiosk:     {urls['kiosk_url']}", flush=True)
    if "kiosk_url_lan" in urls:
        print(f"Kiosk (LAN): {urls['kiosk_url_lan']}", flush=True)

    print(f"Watching:  {outdir}", flush=True)

    if ns.open:
        url = urls["kiosk_url"] if ns.open_kiosk else urls["dashboard_url"]
        try:
            webbrowser.open(url)
        except Exception:
            pass

    try:
        httpd.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()

    return 0



if __name__ == "__main__":
    raise SystemExit(main())
