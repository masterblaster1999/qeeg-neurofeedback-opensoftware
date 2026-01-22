#!/usr/bin/env python3
"""Render qeeg_microstates_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - microstate_state_stats.csv (required)
  - microstate_transition_probs.csv (optional; preferred)
  - microstate_transition_counts.csv (optional)
  - microstate_segments.csv (optional; efficient label timeline)
  - microstate_timeseries.csv (optional; enables GFP/corr timeline + segment extraction)
  - microstate_summary.txt (optional)
  - topomap_microstate_<state>.(bmp|png|jpg|jpeg|gif|svg) (optional; embedded unless --link-topomaps)

Typical usage:

  # Pass an output directory produced by qeeg_microstates_cli
  python3 scripts/render_microstates_report.py --input out_ms

  # Or pass a specific CSV
  python3 scripts/render_microstates_report.py --input out_ms/microstate_state_stats.csv

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.
"""

from __future__ import annotations

import argparse
import base64
import csv
import datetime as _dt
import html
import math
import mimetypes
import os
import pathlib
import webbrowser
import re
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import JS_SORT_TABLE


def _e(x: Any) -> str:
    return html.escape(str(x), quote=True)


def _is_dir(path: str) -> bool:
    try:
        return os.path.isdir(path)
    except Exception:
        return False


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str]:
    """Return (outdir, state_stats_csv_path, html_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        csv_path = os.path.join(outdir, "microstate_state_stats.csv")
        if out is None:
            out = os.path.join(outdir, "microstates_report.html")
    else:
        csv_path = os.path.abspath(inp)
        outdir = os.path.dirname(csv_path) or "."
        if out is None:
            out = os.path.join(outdir, "microstates_report.html")
    return outdir, csv_path, os.path.abspath(out or "microstates_report.html")


def _try_float(s: str) -> float:
    ss = (s or "").strip()
    if ss == "":
        return math.nan
    try:
        return float(ss)
    except Exception:
        return math.nan


def _finite_minmax(values: Iterable[float]) -> Tuple[float, float]:
    mn = math.inf
    mx = -math.inf
    for v in values:
        if math.isfinite(v):
            mn = min(mn, v)
            mx = max(mx, v)
    if not math.isfinite(mn) or not math.isfinite(mx):
        return 0.0, 1.0
    if mn == mx:
        return mn - 1.0, mx + 1.0
    return mn, mx


def _downsample_indices(n: int, max_points: int) -> List[int]:
    if n <= max_points:
        return list(range(n))
    step = max(1, int(math.ceil(n / max_points)))
    idx = list(range(0, n, step))
    if idx and idx[-1] != n - 1:
        idx.append(n - 1)
    return idx


def _hsl_to_rgb(h: float, s: float, l: float) -> Tuple[int, int, int]:
    """Convert HSL in [0,1] to RGB 0-255."""

    def hue_to_rgb(p: float, q: float, t: float) -> float:
        if t < 0:
            t += 1
        if t > 1:
            t -= 1
        if t < 1 / 6:
            return p + (q - p) * 6 * t
        if t < 1 / 2:
            return q
        if t < 2 / 3:
            return p + (q - p) * (2 / 3 - t) * 6
        return p

    if s <= 1e-9:
        v = int(round(l * 255))
        return v, v, v
    q = l * (1 + s) if l < 0.5 else l + s - l * s
    p = 2 * l - q
    r = hue_to_rgb(p, q, h + 1 / 3)
    g = hue_to_rgb(p, q, h)
    b = hue_to_rgb(p, q, h - 1 / 3)
    return int(round(r * 255)), int(round(g * 255)), int(round(b * 255))


def _label_color_map(labels: Sequence[str]) -> Dict[str, str]:
    """Stable, reasonably distinct colors per label."""
    uniq = [x for x in labels if x]
    # Keep order stable and unique
    seen: set[str] = set()
    ordered: List[str] = []
    for x in uniq:
        if x in seen:
            continue
        seen.add(x)
        ordered.append(x)

    n = max(1, len(ordered))
    out: Dict[str, str] = {}
    for i, lab in enumerate(ordered):
        # Golden-angle hue progression for nicer spacing.
        h = ((i * 0.61803398875) % 1.0)
        r, g, b = _hsl_to_rgb(h, 0.62, 0.56)
        out[lab] = f"#{r:02x}{g:02x}{b:02x}"
    return out


def _read_text_if_exists(path: str, *, max_bytes: int = 200_000) -> Optional[str]:
    try:
        with open(path, "rb") as f:
            data = f.read(max_bytes + 1)
        if len(data) > max_bytes:
            data = data[:max_bytes] + b"\n... (truncated)\n"
        return data.decode("utf-8", errors="replace")
    except FileNotFoundError:
        return None
    except Exception:
        return None


def _read_csv_rows(path: str) -> List[Dict[str, str]]:
    with open(path, "r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        if not r.fieldnames:
            raise SystemExit(f"Expected header row in: {path}")
        return list(r)


def _read_state_stats(path: str) -> List[Dict[str, Any]]:
    rows = _read_csv_rows(path)
    if not rows:
        raise SystemExit(f"No rows in: {path}")
    need = {
        "microstate",
        "coverage",
        "mean_duration_sec",
        "occurrence_per_sec",
        "gev_contrib",
        "gev_frac",
    }
    got = set((rows[0].keys()))
    missing = sorted(need - got)
    if missing:
        raise SystemExit(f"Missing required column(s) {missing} in: {path}")

    out: List[Dict[str, Any]] = []
    for row in rows:
        lab = (row.get("microstate") or "").strip()
        out.append(
            {
                "microstate": lab,
                "coverage": _try_float(row.get("coverage", "")),
                "mean_duration_sec": _try_float(row.get("mean_duration_sec", "")),
                "occurrence_per_sec": _try_float(row.get("occurrence_per_sec", "")),
                "gev_contrib": _try_float(row.get("gev_contrib", "")),
                "gev_frac": _try_float(row.get("gev_frac", "")),
            }
        )

    def sort_key(d: Dict[str, Any]) -> Tuple[int, str]:
        s = str(d.get("microstate") or "")
        if len(s) == 1 and "A" <= s.upper() <= "Z":
            return (0, s.upper())
        return (1, s)

    out.sort(key=sort_key)
    return out


def _read_matrix_csv(path: str) -> Tuple[List[str], List[str], List[List[float]]]:
    """Parse a square-ish matrix CSV with a row label in col0 and headers in row0."""
    with open(path, "r", encoding="utf-8", newline="") as f:
        r = csv.reader(f)
        rows = list(r)
    if not rows or len(rows) < 2:
        raise SystemExit(f"Matrix CSV is empty: {path}")

    header = rows[0]
    col_labels = [c.strip() for c in header[1:] if (c or "").strip() != ""]

    row_labels: List[str] = []
    mat: List[List[float]] = []
    for rr in rows[1:]:
        if not rr:
            continue
        row_lab = (rr[0] or "").strip()
        if row_lab == "":
            continue
        vals = [_try_float(x) for x in rr[1:1 + len(col_labels)]]
        # Pad if short
        if len(vals) < len(col_labels):
            vals.extend([math.nan] * (len(col_labels) - len(vals)))
        row_labels.append(row_lab)
        mat.append(vals)
    return row_labels, col_labels, mat


def _rgb(r: float, g: float, b: float) -> str:
    rr = int(max(0, min(255, round(r))))
    gg = int(max(0, min(255, round(g))))
    bb = int(max(0, min(255, round(b))))
    return f"#{rr:02x}{gg:02x}{bb:02x}"


def _color_for_value(v: float, vmin: float, vmax: float) -> str:
    """Sequential blue scale (dark -> light)."""
    if not math.isfinite(v):
        return "#000000"
    if not math.isfinite(vmin) or not math.isfinite(vmax) or vmax <= vmin:
        t = 1.0
    else:
        t = (v - vmin) / (vmax - vmin)
    t = max(0.0, min(1.0, t))
    # dark-navy to light-blue
    r = (1 - t) * 20 + t * 200
    g = (1 - t) * 36 + t * 235
    b = (1 - t) * 64 + t * 255
    return _rgb(r, g, b)


def _svg_heatmap(
    row_labels: Sequence[str],
    col_labels: Sequence[str],
    mat: Sequence[Sequence[float]],
    *,
    title: str,
    value_format: str,
    vmin: Optional[float] = None,
    vmax: Optional[float] = None,
) -> str:
    k = min(len(row_labels), len(col_labels), len(mat))
    if k <= 0:
        return ""

    cell = 34
    pad_l, pad_t = 90, 50
    w = pad_l + cell * k + 18
    h = pad_t + cell * k + 28

    # Determine scaling
    all_vals: List[float] = []
    for i in range(k):
        for j in range(min(k, len(mat[i]))):
            v = float(mat[i][j])
            if math.isfinite(v):
                all_vals.append(v)
    auto_min, auto_max = _finite_minmax(all_vals)
    vvmin = auto_min if vmin is None else vmin
    vvmax = auto_max if vmax is None else vmax
    if vvmin == vvmax:
        vvmin -= 1.0
        vvmax += 1.0

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="hm">')
    parts.append(f'<text x="{pad_l}" y="26" class="hm-title">{_e(title)}</text>')

    # Column labels
    for j in range(k):
        x = pad_l + j * cell + cell / 2
        parts.append(f'<text x="{x:.2f}" y="{pad_t-10}" text-anchor="middle" class="hm-axis">{_e(col_labels[j])}</text>')

    # Row labels
    for i in range(k):
        y = pad_t + i * cell + cell / 2 + 4
        parts.append(f'<text x="{pad_l-10}" y="{y:.2f}" text-anchor="end" class="hm-axis">{_e(row_labels[i])}</text>')

    # Cells
    for i in range(k):
        for j in range(k):
            v = float(mat[i][j]) if j < len(mat[i]) else math.nan
            x = pad_l + j * cell
            y = pad_t + i * cell
            fill = _color_for_value(v, vvmin, vvmax)
            parts.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" fill="{fill}" class="hm-cell" />')
            if math.isfinite(v):
                try:
                    txt = value_format.format(v)
                except Exception:
                    txt = f"{v:.3g}"
                parts.append(f'<text x="{x+cell/2:.2f}" y="{y+cell/2+4:.2f}" text-anchor="middle" class="hm-val">{_e(txt)}</text>')

    parts.append(f'<text x="{pad_l}" y="{h-8}" class="hm-note">scale: {vvmin:.3g} → {vvmax:.3g}</text>')
    parts.append("</svg>")
    return "".join(parts)


@dataclass
class TimeSeries:
    t: List[float]
    gfp: List[float]
    corr: List[float]
    segments: List[Tuple[str, float, float]]  # (label, start, end)
    n_rows: int
    t_min: float
    t_max: float


def _count_csv_rows(path: str) -> int:
    # Count data rows (excluding header). Best-effort.
    try:
        with open(path, "r", encoding="utf-8", newline="") as f:
            # header
            _ = f.readline()
            return sum(1 for _ in f)
    except Exception:
        return 0


def _read_segments_csv(path: str) -> List[Tuple[str, float, float]]:
    rows = _read_csv_rows(path)
    out: List[Tuple[str, float, float]] = []
    for row in rows:
        lab = (row.get("label") or "").strip()
        if lab == "":
            continue
        st = _try_float(row.get("start_sec", ""))
        en = _try_float(row.get("end_sec", ""))
        if not (math.isfinite(st) and math.isfinite(en) and en >= st):
            continue
        out.append((lab, st, en))
    out.sort(key=lambda x: (x[1], x[2]))
    return out


def _read_timeseries_csv(path: str, *, max_points: int) -> TimeSeries:
    n = _count_csv_rows(path)
    if n <= 0:
        # fall back to reading fully
        rows = _read_csv_rows(path)
        n = len(rows)
        idx = _downsample_indices(n, max_points)
        t: List[float] = []
        g: List[float] = []
        c: List[float] = []
        segs: List[Tuple[str, float, float]] = []
        cur_lab: Optional[str] = None
        seg_start = 0.0
        prev_t = math.nan
        for ii, row in enumerate(rows):
            tt = _try_float(row.get("time_sec", ""))
            lab = (row.get("label") or "").strip() or None
            if ii == 0:
                cur_lab = lab
                seg_start = tt if math.isfinite(tt) else 0.0
            if lab != cur_lab and cur_lab is not None and math.isfinite(prev_t):
                segs.append((cur_lab, seg_start, prev_t))
                cur_lab = lab
                seg_start = tt if math.isfinite(tt) else seg_start
            prev_t = tt
            if ii in idx:
                t.append(tt)
                g.append(_try_float(row.get("gfp", "")))
                c.append(_try_float(row.get("corr", "")))
        if cur_lab is not None and math.isfinite(prev_t):
            segs.append((cur_lab, seg_start, prev_t))
        t_min = min((x for x in t if math.isfinite(x)), default=0.0)
        t_max = max((x for x in t if math.isfinite(x)), default=1.0)
        return TimeSeries(t=t, gfp=g, corr=c, segments=[s for s in segs if s[0]], n_rows=n, t_min=t_min, t_max=t_max)

    step = max(1, int(math.ceil(n / max(10, max_points))))

    t_s: List[float] = []
    g_s: List[float] = []
    c_s: List[float] = []
    segs: List[Tuple[str, float, float]] = []

    cur_lab: Optional[str] = None
    seg_start = 0.0
    prev_t: Optional[float] = None
    first_t: Optional[float] = None
    last_t: Optional[float] = None

    with open(path, "r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        if not r.fieldnames:
            raise SystemExit(f"Expected header row in: {path}")
        if "time_sec" not in r.fieldnames or "label" not in r.fieldnames:
            raise SystemExit(f"Expected columns time_sec,label in: {path}")

        for idx, row in enumerate(r):
            tt = _try_float(row.get("time_sec", ""))
            lab = (row.get("label") or "").strip() or None
            if first_t is None and math.isfinite(tt):
                first_t = tt
            if math.isfinite(tt):
                last_t = tt

            if idx == 0:
                cur_lab = lab
                seg_start = tt if math.isfinite(tt) else 0.0
            else:
                if lab != cur_lab:
                    if cur_lab is not None and prev_t is not None and math.isfinite(prev_t):
                        segs.append((cur_lab, seg_start, prev_t))
                    cur_lab = lab
                    if math.isfinite(tt):
                        seg_start = tt

            prev_t = tt

            # sample
            if (idx % step) == 0 or idx == n - 1:
                t_s.append(tt)
                g_s.append(_try_float(row.get("gfp", "")))
                c_s.append(_try_float(row.get("corr", "")))

    if cur_lab is not None and prev_t is not None and math.isfinite(prev_t):
        segs.append((cur_lab, seg_start, prev_t))

    t_min = first_t if first_t is not None else (min((x for x in t_s if math.isfinite(x)), default=0.0))
    t_max = last_t if last_t is not None else (max((x for x in t_s if math.isfinite(x)), default=1.0))
    if t_min is None or not math.isfinite(t_min):
        t_min = 0.0
    if t_max is None or not math.isfinite(t_max) or t_max == t_min:
        t_max = t_min + 1.0

    segs2 = [s for s in segs if s[0] and math.isfinite(s[1]) and math.isfinite(s[2]) and s[2] >= s[1]]
    return TimeSeries(t=t_s, gfp=g_s, corr=c_s, segments=segs2, n_rows=n, t_min=float(t_min), t_max=float(t_max))


def _svg_two_series(
    t: Sequence[float],
    y1: Sequence[float],
    y2: Sequence[float],
    *,
    title: str,
    y1_label: str,
    y2_label: str,
) -> str:
    w, h = 980, 320
    pad_l, pad_r, pad_t, pad_b = 60, 18, 38, 40
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    # Determine x range
    t_finite = [x for x in t if math.isfinite(x)]
    t_min = min(t_finite) if t_finite else 0.0
    t_max = max(t_finite) if t_finite else 1.0
    if not math.isfinite(t_min) or not math.isfinite(t_max) or t_min == t_max:
        t_min, t_max = 0.0, 1.0

    y_min, y_max = _finite_minmax([*y1, *y2])

    def x_of(tt: float) -> float:
        return pad_l + (tt - t_min) / (t_max - t_min) * inner_w

    def y_of(v: float) -> float:
        return pad_t + (1.0 - (v - y_min) / (y_max - y_min)) * inner_h

    def poly(vals: Sequence[float]) -> str:
        pts: List[str] = []
        for tt, vv in zip(t, vals):
            if math.isfinite(tt) and math.isfinite(vv):
                pts.append(f"{x_of(tt):.2f},{y_of(vv):.2f}")
        return " ".join(pts)

    p1 = poly(y1)
    p2 = poly(y2)

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="ts">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')
    for k in range(5):
        yy = pad_t + k / 4.0 * inner_h
        parts.append(f'<line x1="{pad_l}" y1="{yy:.2f}" x2="{pad_l+inner_w}" y2="{yy:.2f}" class="grid" />')
    if p1:
        parts.append(f'<polyline points="{p1}" class="line1" />')
    if p2:
        parts.append(f'<polyline points="{p2}" class="line2" />')
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+26}" class="axis-label">time (s)</text>')
    parts.append(f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">{_e(f"range {y_min:.4g}..{y_max:.4g}")}</text>')
    parts.append(f'<text x="{pad_l+inner_w}" y="{pad_t+inner_h+26}" text-anchor="end" class="axis-label">{_e(f"{t_max:.2f}s")}</text>')

    # Legend
    lx, ly = pad_l + 10, pad_t + inner_h + 10
    parts.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" class="leg1" /><text x="{lx+18}" y="{ly+11}" class="legend-text">{_e(y1_label)}</text>')
    parts.append(f'<rect x="{lx+110}" y="{ly}" width="12" height="12" class="leg2" /><text x="{lx+128}" y="{ly+11}" class="legend-text">{_e(y2_label)}</text>')
    parts.append("</svg>")
    return "".join(parts)


def _svg_state_ribbon(
    segments: Sequence[Tuple[str, float, float]],
    *,
    title: str,
    color_map: Dict[str, str],
) -> str:
    if not segments:
        return ""
    w, h = 980, 96
    pad_l, pad_r, pad_t, pad_b = 60, 18, 34, 22
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    t_min = min(s[1] for s in segments)
    t_max = max(s[2] for s in segments)
    if not (math.isfinite(t_min) and math.isfinite(t_max)) or t_max <= t_min:
        t_min, t_max = 0.0, 1.0

    # Pixelize to keep SVG light even with many segments.
    px = int(inner_w)

    # Precondition: segments sorted by start.
    segs = sorted([s for s in segments if s[0]], key=lambda x: (x[1], x[2]))
    if not segs:
        return ""

    labels_by_px: List[str] = []
    si = 0
    for p in range(px):
        tt = t_min + (p + 0.5) / px * (t_max - t_min)
        while si < len(segs) and tt > segs[si][2]:
            si += 1
        lab = segs[si][0] if si < len(segs) else ""
        labels_by_px.append(lab)

    # Compress consecutive pixels.
    runs: List[Tuple[str, int, int]] = []  # label, start_px, end_px(exclusive)
    cur = labels_by_px[0]
    start = 0
    for i in range(1, len(labels_by_px)):
        if labels_by_px[i] != cur:
            runs.append((cur, start, i))
            cur = labels_by_px[i]
            start = i
    runs.append((cur, start, len(labels_by_px)))

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="rib">')
    parts.append(f'<text x="{pad_l}" y="22" class="rib-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    for lab, a, b in runs:
        if not lab:
            continue
        x = pad_l + a
        ww = max(1, b - a)
        col = color_map.get(lab, "#7fb3ff")
        parts.append(f'<rect x="{x}" y="{pad_t}" width="{ww}" height="{inner_h}" fill="{col}" opacity="0.55" />')

    parts.append(f'<text x="{pad_l}" y="{h-8}" class="axis-label">{_e(f"{t_min:.2f}s → {t_max:.2f}s")}</text>')
    parts.append("</svg>")
    return "".join(parts)


def _embed_image_data_uri(path: str) -> Optional[str]:
    try:
        mime, _ = mimetypes.guess_type(path)
        if not mime:
            mime = "application/octet-stream"
        with open(path, "rb") as f:
            data = f.read()
        b64 = base64.b64encode(data).decode("ascii")
        return f"data:{mime};base64,{b64}"
    except Exception:
        return None


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_microstates_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to an outdir produced by qeeg_microstates_cli, or a specific microstate_*.csv file.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/microstates_report.html).")
    ap.add_argument("--title", default="EEG microstates report", help="Report title.")
    ap.add_argument(
        "--max-points",
        type=int,
        default=2500,
        help="Max points to plot from microstate_timeseries.csv (downsamples for long runs; default: 2500).",
    )
    ap.add_argument(
        "--link-topomaps",
        action="store_true",
        help="Link to topomap images instead of embedding them (smaller HTML).",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, stats_path, html_path = _guess_paths(args.input, args.out)
    if not os.path.exists(stats_path):
        raise SystemExit(f"Could not find microstate_state_stats.csv at: {stats_path}")

    state_stats = _read_state_stats(stats_path)
    labels = [str(d.get("microstate") or "") for d in state_stats if str(d.get("microstate") or "")]  # ordered
    color_map = _label_color_map(labels)

    # Optional files
    summary_path = os.path.join(outdir, "microstate_summary.txt")
    summary_txt = _read_text_if_exists(summary_path)

    probs_path = os.path.join(outdir, "microstate_transition_probs.csv")
    counts_path = os.path.join(outdir, "microstate_transition_counts.csv")
    trans_kind = ""
    trans_svg = ""
    if os.path.exists(probs_path):
        rl, cl, mat = _read_matrix_csv(probs_path)
        trans_kind = "Transition probabilities"
        trans_svg = _svg_heatmap(rl, cl, mat, title=trans_kind, value_format="{:.2f}", vmin=0.0, vmax=1.0)
    elif os.path.exists(counts_path):
        rl, cl, mat = _read_matrix_csv(counts_path)
        trans_kind = "Transition counts"
        # counts usually include zeros; let vmax float
        trans_svg = _svg_heatmap(rl, cl, mat, title=trans_kind, value_format="{:.0f}")

    # Timeline / time series
    ts_path = os.path.join(outdir, "microstate_timeseries.csv")
    seg_path = os.path.join(outdir, "microstate_segments.csv")
    segments: List[Tuple[str, float, float]] = []
    ts: Optional[TimeSeries] = None
    if os.path.exists(seg_path):
        segments = _read_segments_csv(seg_path)

    if os.path.exists(ts_path):
        try:
            ts = _read_timeseries_csv(ts_path, max_points=max(50, int(args.max_points)))
            # If we didn't have segments.csv, use the derived segments.
            if not segments:
                segments = ts.segments
        except Exception:
            ts = None

    ribbon_svg = _svg_state_ribbon(
        segments,
        title="Microstate label timeline (overview)",
        color_map=color_map,
    )

    ts_svg = ""
    ts_note = ""
    if ts is not None and ts.t and (ts.gfp or ts.corr):
        ts_svg = _svg_two_series(ts.t, ts.gfp, ts.corr, title="GFP and corr over time (downsampled)", y1_label="GFP", y2_label="corr")
        ts_note = f"Read {ts.n_rows} rows from microstate_timeseries.csv; plotted {len(ts.t)} points." 
    elif os.path.exists(ts_path):
        ts_note = "microstate_timeseries.csv found, but could not be parsed for plotting."
    else:
        ts_note = "microstate_timeseries.csv not found; timeline plots omitted."

    # Topomap images
    topo_paths: List[Tuple[str, str]] = []  # (label, path)
    topo_re = re.compile(r"^topomap_microstate_(.+?)\.(bmp|png|jpg|jpeg|gif|svg)$", re.IGNORECASE)
    try:
        for fn in os.listdir(outdir):
            m = topo_re.match(fn)
            if not m:
                continue
            lab = m.group(1)
            topo_paths.append((lab, os.path.join(outdir, fn)))
    except Exception:
        topo_paths = []

    # Sort topomaps by label
    def _lab_key(x: Tuple[str, str]) -> Tuple[int, str]:
        lab = x[0]
        if len(lab) == 1 and "A" <= lab.upper() <= "Z":
            return (0, lab.upper())
        return (1, lab)

    topo_paths.sort(key=_lab_key)

    topo_cards: List[str] = []
    for lab, p in topo_paths:
        if args.link_topomaps:
            rel = os.path.basename(p)
            img_html = f'<a href="{_e(rel)}"><img class="topo" alt="{_e(lab)}" src="{_e(rel)}"></a>'
        else:
            uri = _embed_image_data_uri(p)
            if uri:
                img_html = f'<img class="topo" alt="{_e(lab)}" src="{_e(uri)}">'
            else:
                rel = os.path.basename(p)
                img_html = f'<a href="{_e(rel)}">{_e(rel)}</a>'
        badge = f'<span class="ms-badge" style="background:{_e(color_map.get(lab, "#7fb3ff"))}">{_e(lab)}</span>'
        topo_cards.append(f'<div class="topo-card">{badge}{img_html}</div>')

    topo_block = (
        '<div class="topo-grid">' + "".join(topo_cards) + "</div>" if topo_cards else '<div class="note">No topomap_microstate_*.bmp found.</div>'
    )

    # Stats table with bars
    cov_max = max((d["coverage"] for d in state_stats if math.isfinite(d["coverage"])), default=1.0)
    dur_max = max((d["mean_duration_sec"] for d in state_stats if math.isfinite(d["mean_duration_sec"])), default=1.0)
    occ_max = max((d["occurrence_per_sec"] for d in state_stats if math.isfinite(d["occurrence_per_sec"])), default=1.0)
    gevf_max = max((d["gev_frac"] for d in state_stats if math.isfinite(d["gev_frac"])), default=1.0)
    cov_max = cov_max if cov_max > 0 else 1.0
    dur_max = dur_max if dur_max > 0 else 1.0
    occ_max = occ_max if occ_max > 0 else 1.0
    gevf_max = gevf_max if gevf_max > 0 else 1.0

    rows_html: List[str] = []
    for d in state_stats:
        lab = str(d.get("microstate") or "")
        col = color_map.get(lab, "#7fb3ff")

        def bar(val: float, vmax: float) -> str:
            if not math.isfinite(val):
                return '<div class="bar-wrap"><div class="bar" style="width:0%"></div></div>'
            pct = max(0.0, min(100.0, 100.0 * val / vmax))
            return f'<div class="bar-wrap"><div class="bar" style="width:{pct:.1f}%;background:{_e(col)}"></div></div>'

        cov = float(d.get("coverage", math.nan))
        dur = float(d.get("mean_duration_sec", math.nan))
        occ = float(d.get("occurrence_per_sec", math.nan))
        gevf = float(d.get("gev_frac", math.nan))

        rows_html.append(
            "<tr>"
            f"<td><span class=\"ms-badge\" style=\"background:{_e(col)}\">{_e(lab)}</span></td>"
            f"<td>{_e(f'{cov:.4g}' if math.isfinite(cov) else 'NaN')}</td>"
            f"<td>{bar(cov, cov_max)}</td>"
            f"<td>{_e(f'{dur:.4g}' if math.isfinite(dur) else 'NaN')}</td>"
            f"<td>{bar(dur, dur_max)}</td>"
            f"<td>{_e(f'{occ:.4g}' if math.isfinite(occ) else 'NaN')}</td>"
            f"<td>{bar(occ, occ_max)}</td>"
            f"<td>{_e(f'{gevf:.4g}' if math.isfinite(gevf) else 'NaN')}</td>"
            f"<td>{bar(gevf, gevf_max)}</td>"
            "</tr>"
        )

    stats_table = (
        '<div class="table-filter"><div class="table-controls"><input type="search" placeholder="Filter rows…" oninput="filterTable(this)" /><button type="button" onclick="downloadTableCSV(this, \'microstate_state_stats_table_filtered.csv\', true)">Download CSV</button><span class="filter-count muted"></span></div><div class="table-wrap"><table class="tbl">'
        '<thead><tr>'
        '<th onclick="sortTable(this)">State</th><th onclick="sortTable(this)">Coverage</th><th></th><th onclick="sortTable(this)">Mean duration (s)</th><th></th><th onclick="sortTable(this)">Occurrence (/s)</th><th></th><th onclick="sortTable(this)">GEV frac</th><th></th>'
        '</tr></thead><tbody>'
        + "".join(rows_html)
        + "</tbody></table></div></div>"
    )

    # Quick derived stats
    k = len(state_stats)
    gev_total = sum(float(d.get("gev_contrib", 0.0)) for d in state_stats if math.isfinite(float(d.get("gev_contrib", 0.0))))
    gev_frac_sum = sum(float(d.get("gev_frac", 0.0)) for d in state_stats if math.isfinite(float(d.get("gev_frac", 0.0))))

    seg_count = len(segments)
    seg_durs = [s[2] - s[1] for s in segments if math.isfinite(s[2] - s[1]) and (s[2] >= s[1])]
    seg_mean = (sum(seg_durs) / len(seg_durs)) if seg_durs else math.nan
    seg_min = min(seg_durs) if seg_durs else math.nan
    seg_max = max(seg_durs) if seg_durs else math.nan
    t_span = (max(s[2] for s in segments) - min(s[1] for s in segments)) if segments else math.nan

    now = _dt.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src_rel = os.path.relpath(os.path.abspath(stats_path), out_dir)

    css = r"""
:root { --bg:#0b0f14; --panel:#111823; --text:#e7eefc; --muted:#9bb0d0; --grid:#213046; --accent:#8fb7ff; }
* { box-sizing: border-box; }
body { font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif;
       margin: 0; background: var(--bg); color: var(--text); }
header { padding: 18px 22px; background: linear-gradient(90deg, #0e1623, #0b0f14); border-bottom: 1px solid var(--grid); }
h1 { margin: 0; font-size: 20px; letter-spacing: 0.2px; }
.meta { margin-top: 6px; color: var(--muted); font-size: 13px; }
main { padding: 18px 22px 40px; max-width: 1200px; margin: 0 auto; }
.card { background: var(--panel); border: 1px solid var(--grid); border-radius: 10px; padding: 14px; margin: 12px 0; }
.card h2 { margin: 0 0 10px; font-size: 16px; }
.note { color: var(--muted); font-size: 13px; line-height: 1.35; }
.muted { color: var(--muted); }

.table-controls { display:flex; gap:10px; flex-wrap:wrap; align-items:center; margin: 10px 0 10px; }
.table-controls input[type="search"] { width: min(520px, 100%); padding: 10px 12px; border-radius: 10px;
  border: 1px solid var(--grid); background: #0b0f14; color: var(--text); }
.table-controls button { padding: 10px 12px; border-radius: 10px; border: 1px solid var(--grid);
  background: #0f1725; color: var(--text); cursor: pointer; font-size: 13px; }
.table-controls button:hover { background: rgba(143,183,255,0.10); }
.table-controls button:active { transform: translateY(1px); }
.filter-count { font-size: 12px; color: var(--muted); }
.kv { display: grid; grid-template-columns: 240px 1fr; gap: 6px 12px; font-size: 13px; }
.kv div:nth-child(odd) { color: var(--muted); }
code { background: rgba(255,255,255,0.06); padding: 2px 6px; border-radius: 6px; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }

.table-wrap { overflow:auto; border-radius: 10px; border: 1px solid var(--grid); }
.tbl { width: 100%; border-collapse: collapse; font-size: 13px; min-width: 920px; }
.tbl th, .tbl td { border-bottom: 1px solid var(--grid); padding: 8px; vertical-align: middle; }
.tbl th { background: #0f1725; position: sticky; top: 0; z-index: 2; text-align:left; }
.tbl tr:hover td { background: rgba(143, 183, 255, 0.08); }

.bar-wrap { width: 160px; height: 10px; background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.08); border-radius: 999px; overflow:hidden; }
.bar { height: 100%; border-radius: 999px; }

.ms-badge { display:inline-block; min-width: 20px; text-align:center; padding: 2px 8px; border-radius: 999px; font-size: 12px; color: #0b0f14; font-weight: 700; }

.topo-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; }
.topo-card { border: 1px solid var(--grid); border-radius: 12px; padding: 10px; background: rgba(255,255,255,0.02); }
.topo-card .ms-badge { margin-bottom: 8px; }
.topo { width: 100%; height: auto; border-radius: 10px; border: 1px solid rgba(255,255,255,0.08); background: #0b0f14; }

.hm { width: 100%; height: auto; }
.hm-title { fill: #dce7ff; font-weight: 700; font-size: 13px; }
.hm-axis { fill: var(--muted); font-size: 12px; }
.hm-val { fill: #0b0f14; font-size: 11px; font-weight: 700; }
.hm-note { fill: var(--muted); font-size: 12px; }
.hm-cell { stroke: rgba(0,0,0,0.18); stroke-width: 1; }

.ts, .rib { width: 100%; height: auto; }
.frame { fill: #0c131f; stroke: #20324a; stroke-width: 1; }
.grid { stroke: rgba(255,255,255,0.06); stroke-width: 1; }
.ts-title, .rib-title { fill: #dce7ff; font-weight: 700; font-size: 13px; }
.axis-label { fill: var(--muted); font-size: 12px; }
.line1 { fill: none; stroke: #7fb3ff; stroke-width: 2; opacity: 0.95; }
.line2 { fill: none; stroke: #ffd37f; stroke-width: 2; opacity: 0.90; stroke-dasharray: 6 4; }
.legend-text { fill: #dce7ff; font-size: 12px; }
.leg1 { fill: #7fb3ff; }
.leg2 { fill: #ffd37f; }

pre { white-space: pre-wrap; background: rgba(255,255,255,0.04); border: 1px solid var(--grid); border-radius: 10px; padding: 10px; color: #dce7ff; font-size: 12px; }
.footer { color: var(--muted); font-size: 12px; margin-top: 16px; }
"""

    summary_html = (
        f"<pre>{_e(summary_txt)}</pre>" if summary_txt else '<div class="note">No microstate_summary.txt found.</div>'
    )

    trans_html = trans_svg if trans_svg else '<div class="note">No transition matrix CSV found (microstate_transition_probs.csv / microstate_transition_counts.csv).</div>'

    ribbon_html = ribbon_svg if ribbon_svg else '<div class="note">No segments/timeseries available for a label timeline.</div>'
    ts_plot_html = ts_svg if ts_svg else ''

    html_doc = f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<title>{_e(args.title)}</title>
<style>{css}</style>
</head>
<body>
<header>
  <h1>{_e(args.title)}</h1>
  <div class=\"meta\">Generated {now} — source <code>{_e(src_rel)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">This report visualizes <code>qeeg_microstates_cli</code> outputs for research/educational inspection only. It is not a medical device.</div>
    <div class=\"note\">Primary inputs: <code>microstate_state_stats.csv</code>, optional transition matrices, and optional timelines/topomaps.</div>
  </div>

  <div class=\"card\">
    <h2>Quick stats</h2>
    <div class=\"kv\">
      <div>States (k)</div><div>{k}</div>
      <div>GEV contrib (sum)</div><div>{_e(f"{gev_total:.6g}" if math.isfinite(gev_total) else "NaN")}</div>
      <div>GEV frac (sum)</div><div>{_e(f"{gev_frac_sum:.6g}" if math.isfinite(gev_frac_sum) else "NaN")}</div>
      <div>Segments (derived/loaded)</div><div>{seg_count}</div>
      <div>Segment duration mean/min/max (s)</div><div>{_e(f"{seg_mean:.4g} / {seg_min:.4g} / {seg_max:.4g}" if (math.isfinite(seg_mean) and math.isfinite(seg_min) and math.isfinite(seg_max)) else "N/A")}</div>
      <div>Timeline span (s)</div><div>{_e(f"{t_span:.4g}" if math.isfinite(t_span) else "N/A")}</div>
      <div>Timeseries note</div><div class=\"note\">{_e(ts_note)}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>Per-state stats</h2>
    <div class=\"note\">Bars are scaled relative to the maximum value across states for the given metric.</div>
    {stats_table}
  </div>

  <div class=\"card\">
    <h2>Template topomaps</h2>
    <div class=\"note\">If present, topomap images are embedded (or linked with <code>--link-topomaps</code>).</div>
    {topo_block}
  </div>

  <div class=\"card\">
    <h2>Transitions</h2>
    <div class=\"note\">{_e(trans_kind) if trans_kind else "Transition matrix"} (rows = from, columns = to).</div>
    {trans_html}
  </div>

  <div class=\"card\">
    <h2>Timeline</h2>
    {ribbon_html}
    {ts_plot_html}
    <div class=\"note\">The label ribbon is pixel-compressed to keep the SVG lightweight even for long recordings.</div>
  </div>

  <div class=\"card\">
    <h2>microstate_summary.txt (optional)</h2>
    {summary_html}
  </div>

  <div class=\"footer\">Tip: open this file in a browser. It is self-contained (no network requests).</div>
</main>
<script>{JS_SORT_TABLE}</script>
</body>
</html>
"""

    os.makedirs(os.path.dirname(os.path.abspath(html_path)) or ".", exist_ok=True)
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {html_path}")

    if args.open:
        try:
            webbrowser.open(pathlib.Path(os.path.abspath(html_path)).as_uri())
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
