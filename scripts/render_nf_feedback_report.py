#!/usr/bin/env python3
"""Render qeeg_nf_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - nf_feedback.csv (required)
  - nf_summary.json (optional; adds extra metadata)
  - nf_run_meta.json (optional; adds build/run metadata)
  - nf_derived_events.tsv / nf_derived_events.csv (optional; BIDS-style segments)
  - nf_derived_events.json (optional; BIDS sidecar describing the events table)

Typical usage:

  # Pass an output directory produced by qeeg_nf_cli
  python3 scripts/render_nf_feedback_report.py --input out_nf

  # Or pass nf_feedback.csv directly
  python3 scripts/render_nf_feedback_report.py --input out_nf/nf_feedback.csv --out nf_report.html

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.

Notes:
- This is a convenience visualization for research/educational inspection only.
- It is not a medical device and is not intended for clinical decision-making.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import webbrowser
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    downsample_indices as _downsample_indices,
    e as _e,
    finite_minmax as _finite_minmax,
    is_dir as _is_dir,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    try_bool_int as _try_bool_int,
    try_float as _try_float,
    utc_now_iso,
)


@dataclass
class Cols:
    t: str
    metric: str
    threshold: str
    reward: Optional[str]
    reward_rate: Optional[str]
    artifact: Optional[str]
    artifact_ready: Optional[str]
    bad_channels: Optional[str]
    phase: Optional[str]
    raw_reward: Optional[str]

    # Optional advanced columns (newer qeeg_nf_cli versions)
    threshold_desired: Optional[str]
    metric_raw: Optional[str]
    feedback_raw: Optional[str]
    reward_value: Optional[str]

    metric_z: Optional[str]
    threshold_z: Optional[str]
    metric_z_ref: Optional[str]
    threshold_z_ref: Optional[str]


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, Optional[str], Optional[str]]:
    """Return (csv_path, html_path, summary_json_or_None, run_meta_json_or_None)."""
    csv_path = inp
    out_dir = os.path.dirname(os.path.abspath(inp)) or "."
    if _is_dir(inp):
        out_dir = os.path.abspath(inp)
        csv_path = os.path.join(out_dir, "nf_feedback.csv")
        if out is None:
            out = os.path.join(out_dir, "nf_feedback_report.html")
    else:
        if out is None:
            out = os.path.join(out_dir, "nf_feedback_report.html")

    summary = os.path.join(out_dir, "nf_summary.json")
    run_meta = os.path.join(out_dir, "nf_run_meta.json")
    if not os.path.exists(summary):
        summary = None
    if not os.path.exists(run_meta):
        run_meta = None
    return csv_path, os.path.abspath(out), summary, run_meta


def _guess_derived_events_paths(out_dir: str) -> Tuple[Optional[str], Optional[str]]:
    """Return (events_table_or_None, events_sidecar_json_or_None)."""
    out_dir = os.path.abspath(out_dir or ".")
    p_tsv = os.path.join(out_dir, "nf_derived_events.tsv")
    p_csv = os.path.join(out_dir, "nf_derived_events.csv")
    p_json = os.path.join(out_dir, "nf_derived_events.json")
    table: Optional[str] = None
    if os.path.exists(p_tsv):
        table = p_tsv
    elif os.path.exists(p_csv):
        table = p_csv
    sidecar: Optional[str] = p_json if os.path.exists(p_json) else None
    return table, sidecar


def _pick_col(headers: Sequence[str], candidates: Sequence[str], *, required: bool = False) -> Optional[str]:
    """Pick a column from headers, case-insensitively.

    Strategy:
      1) exact case-insensitive match
      2) contains match (candidate is a substring of header)
    """
    hmap = {h.lower(): h for h in headers}
    for cand in candidates:
        h = hmap.get(cand.lower())
        if h:
            return h

    # contains match
    lower_headers = [(h.lower(), h) for h in headers]
    for cand in candidates:
        cl = cand.lower()
        for hl, h in lower_headers:
            if cl and cl in hl:
                return h

    if required:
        raise SystemExit(f"Missing required column; expected one of: {', '.join(candidates)}")
    return None


def _detect_cols(headers: Sequence[str]) -> Cols:
    # These are stable in the C++ tool, but we keep this flexible for forwards-compat.
    t = _pick_col(headers, ["t_end_sec", "time_sec", "t_sec", "t"], required=True)
    metric = _pick_col(headers, ["metric"], required=True)
    threshold = _pick_col(headers, ["threshold", "thr"], required=True)

    reward = _pick_col(headers, ["reward"])
    reward_rate = _pick_col(headers, ["reward_rate"])

    artifact = _pick_col(headers, ["artifact"])
    artifact_ready = _pick_col(headers, ["artifact_ready"])
    bad_channels = _pick_col(headers, ["bad_channels", "bad_channel_count"])

    phase = _pick_col(headers, ["phase"])
    raw_reward = _pick_col(headers, ["raw_reward"])

    # Newer optional fields
    threshold_desired = _pick_col(headers, ["threshold_desired"])
    metric_raw = _pick_col(headers, ["metric_raw"])
    feedback_raw = _pick_col(headers, ["feedback_raw"])
    reward_value = _pick_col(headers, ["reward_value"])
    metric_z = _pick_col(headers, ["metric_z"])
    threshold_z = _pick_col(headers, ["threshold_z"])
    metric_z_ref = _pick_col(headers, ["metric_z_ref"])
    threshold_z_ref = _pick_col(headers, ["threshold_z_ref"])

    return Cols(
        t=t,
        metric=metric,
        threshold=threshold,
        reward=reward,
        reward_rate=reward_rate,
        artifact=artifact,
        artifact_ready=artifact_ready,
        bad_channels=bad_channels,
        phase=phase,
        raw_reward=raw_reward,
        threshold_desired=threshold_desired,
        metric_raw=metric_raw,
        feedback_raw=feedback_raw,
        reward_value=reward_value,
        metric_z=metric_z,
        threshold_z=threshold_z,
        metric_z_ref=metric_z_ref,
        threshold_z_ref=threshold_z_ref,
    )


def _polyline(points: Sequence[Tuple[float, float]]) -> str:
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points)


def _svg_timeseries(
    t: Sequence[float],
    series: Sequence[Tuple[str, Sequence[float], str]],
    *,
    shade_reward: Optional[Sequence[int]] = None,
    shade_artifact: Optional[Sequence[int]] = None,
    phase: Optional[Sequence[str]] = None,
    title: str,
    y_label: str,
) -> str:
    """Simple multi-line chart with optional shaded segments."""
    if not t:
        return ""

    w, h = 980, 380
    pad_l, pad_r, pad_t, pad_b = 60, 18, 38, 44
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    t_min = min(t)
    t_max = max(t)
    if not (math.isfinite(t_min) and math.isfinite(t_max)) or t_max <= t_min:
        t_min, t_max = 0.0, 1.0

    # Compute y-range over all series.
    all_vals: List[float] = []
    for _, ys, _ in series:
        all_vals.extend([v for v in ys if math.isfinite(v)])
    y_min, y_max = _finite_minmax(all_vals)

    def x_of(tt: float) -> float:
        return pad_l + (tt - t_min) / (t_max - t_min) * inner_w

    def y_of(v: float) -> float:
        return pad_t + (1.0 - (v - y_min) / (y_max - y_min)) * inner_h

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="ts">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    # Shading
    def shade_segments(flag: Optional[Sequence[int]], cls: str) -> None:
        if not flag or len(flag) != len(t):
            return
        for i in range(len(t) - 1):
            if flag[i] != 1:
                continue
            x1 = x_of(t[i])
            x2 = x_of(t[i + 1])
            wrect = max(1.0, x2 - x1)
            parts.append(f'<rect x="{x1:.2f}" y="{pad_t}" width="{wrect:.2f}" height="{inner_h}" class="{cls}" />')

    shade_segments(shade_reward, "shade-reward")
    shade_segments(shade_artifact, "shade-artifact")

    # Optional phase shading: thin band at top
    if phase is not None and len(phase) == len(t):
        for i in range(len(t) - 1):
            p = (phase[i] or "").strip().lower()
            if p == "":
                continue
            x1 = x_of(t[i])
            x2 = x_of(t[i + 1])
            wrect = max(1.0, x2 - x1)
            cls = "phase-other"
            if p == "baseline":
                cls = "phase-baseline"
            elif p == "train":
                cls = "phase-train"
            elif p == "rest":
                cls = "phase-rest"
            parts.append(f'<rect x="{x1:.2f}" y="{pad_t}" width="{wrect:.2f}" height="8" class="{cls}" />')

    # Grid lines (y)
    for k in range(5):
        yy = pad_t + k / 4.0 * inner_h
        parts.append(f'<line x1="{pad_l}" y1="{yy:.2f}" x2="{pad_l+inner_w}" y2="{yy:.2f}" class="grid" />')

    # Lines
    for name, ys, cls in series:
        pts: List[Tuple[float, float]] = []
        for tt, vv in zip(t, ys):
            if math.isfinite(tt) and math.isfinite(vv):
                pts.append((x_of(tt), y_of(vv)))
        if pts:
            parts.append(f'<polyline points="{_polyline(pts)}" class="{cls}" />')
        else:
            # still show legend item even if empty
            pass

    # Axes labels
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+30}" class="axis-label">time (s)</text>')
    parts.append(
        f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">{_e(f"{y_label} (min={y_min:.4g}, max={y_max:.4g})")}</text>'
    )
    parts.append(
        f'<text x="{pad_l+inner_w}" y="{pad_t+inner_h+30}" text-anchor="end" class="axis-label">{_e(f"{t_max:.2f}s")}</text>'
    )

    # Legend
    lx, ly = pad_l + 10, pad_t + inner_h + 12
    xcur = lx
    for name, _, cls in series:
        parts.append(f'<rect x="{xcur}" y="{ly}" width="12" height="12" class="{cls}-legend" />')
        parts.append(f'<text x="{xcur+18}" y="{ly+11}" class="legend-text">{_e(name)}</text>')
        xcur += 18 + 8 * max(3, len(name))
    if shade_reward is not None:
        parts.append(f'<rect x="{xcur}" y="{ly}" width="12" height="12" class="legend-reward" />')
        parts.append(f'<text x="{xcur+18}" y="{ly+11}" class="legend-text">reward</text>')
        xcur += 90
    if shade_artifact is not None:
        parts.append(f'<rect x="{xcur}" y="{ly}" width="12" height="12" class="legend-artifact" />')
        parts.append(f'<text x="{xcur+18}" y="{ly+11}" class="legend-text">artifact</text>')

    parts.append("</svg>")
    return "".join(parts)


def _safe_mean(values: Sequence[float]) -> float:
    xs = [v for v in values if math.isfinite(v)]
    return (sum(xs) / len(xs)) if xs else math.nan


def _fraction_ones(flags: Optional[Sequence[int]]) -> float:
    if not flags:
        return math.nan
    if len(flags) == 0:
        return math.nan
    return float(sum(1 for x in flags if x == 1)) / float(len(flags))


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, max_rows: int) -> str:
    if not rows:
        return '<div class="note">No rows available.</div>'
    max_rows = max(0, int(max_rows))
    if max_rows == 0:
        return '<div class="note">Table disabled (max rows = 0).</div>'

    # Downsample to at most max_rows rows.
    idx = _downsample_indices(len(rows), max_rows)
    use_rows = [rows[i] for i in idx]

    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body: List[str] = []
    for r in use_rows:
        tds: List[str] = []
        for h in headers:
            v = (r.get(h) or "").strip()
            # Numeric hint for sorting
            fv = _try_float(v)
            if math.isfinite(fv) and v != "":
                tds.append(f'<td data-num="{_e(f"{fv:.12g}")}">{_e(v)}</td>')
            else:
                tds.append(f"<td>{_e(v)}</td>")
        body.append("<tr>" + "".join(tds) + "</tr>")

    return (
        '<table class="data-table sticky">'
        f"<thead><tr>{ths}</tr></thead>"
        f"<tbody>{''.join(body)}</tbody>"
        "</table>"
    )


def _pretty_json_snippet(obj: Dict[str, Any], *, pick: Sequence[str], max_chars: int = 12000) -> str:
    picked: Dict[str, Any] = {k: obj.get(k) for k in pick if k in obj}
    if not picked:
        return ""
    s = json.dumps(picked, indent=2, sort_keys=True)
    if len(s) > max_chars:
        s = s[:max_chars] + "\n… (truncated)"
    return "<pre>" + _e(s) + "</pre>"


def _detect_event_cols(headers: Sequence[str]) -> Tuple[str, Optional[str], str]:
    """Return (onset_col, duration_col_or_None, label_col)."""
    onset = _pick_col(headers, ["onset", "onset_sec"], required=True)
    duration = _pick_col(headers, ["duration", "duration_sec"], required=False)
    label = _pick_col(headers, ["trial_type", "text", "label", "event"], required=True)
    return onset, duration, label


def _evt_sort_key(label: str) -> Tuple[int, str]:
    ll = (label or "").strip().lower()
    if "baseline" in ll:
        return (0, ll)
    if "train" in ll:
        return (1, ll)
    if "rest" in ll:
        return (2, ll)
    if "reward" in ll:
        return (3, ll)
    if "artifact" in ll:
        return (4, ll)
    if ll.startswith("nf:"):
        return (5, ll)
    return (6, ll)


def _evt_css_class(label: str) -> str:
    ll = (label or "").strip().lower()
    if "baseline" in ll:
        return "evt evt-baseline"
    if "train" in ll:
        return "evt evt-train"
    if "rest" in ll:
        return "evt evt-rest"
    if "reward" in ll:
        return "evt evt-reward"
    if "artifact" in ll:
        return "evt evt-artifact"
    if ll.startswith("nf:"):
        return "evt evt-nf"
    return "evt evt-other"


def _svg_events_timeline(events: Sequence[Dict[str, Any]], *, title: str) -> str:
    """Render a simple multi-lane event timeline SVG.

    events: list of {onset: float, duration: float, label: str}
    """
    if not events:
        return ""

    # Lanes by label (trial_type/text).
    labels: List[str] = []
    seen = set()
    for ev in events:
        lab = str(ev.get("label") or "")
        if lab not in seen:
            seen.add(lab)
            labels.append(lab)
    labels.sort(key=_evt_sort_key)

    # Time range.
    t0 = math.inf
    t1 = -math.inf
    for ev in events:
        onset = float(ev.get("onset") or math.nan)
        dur = float(ev.get("duration") or 0.0)
        if not math.isfinite(onset):
            continue
        if dur < 0.0 or not math.isfinite(dur):
            dur = 0.0
        t0 = min(t0, onset)
        t1 = max(t1, onset + dur)
    if not (math.isfinite(t0) and math.isfinite(t1)) or t1 <= t0:
        t0, t1 = 0.0, 1.0

    w = 980
    lane_h = 18
    lane_gap = 6
    pad_l, pad_r, pad_t, pad_b = 160, 18, 38, 42
    inner_h = len(labels) * lane_h + max(0, len(labels) - 1) * lane_gap
    h = pad_t + inner_h + pad_b
    inner_w = w - pad_l - pad_r

    def x_of(tt: float) -> float:
        return pad_l + (tt - t0) / (t1 - t0) * inner_w

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="evt-ts">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    # Lane labels + faint separators
    for i, lab in enumerate(labels):
        y = pad_t + i * (lane_h + lane_gap)
        ymid = y + lane_h / 2.0 + 4.0
        parts.append(f'<text x="{pad_l-10}" y="{ymid:.2f}" text-anchor="end" class="evt-label">{_e(lab)}</text>')
        parts.append(f'<line x1="{pad_l}" y1="{y + lane_h + lane_gap/2.0:.2f}" x2="{pad_l+inner_w}" y2="{y + lane_h + lane_gap/2.0:.2f}" class="evt-grid" />')

    # Events
    lane_index = {lab: i for i, lab in enumerate(labels)}
    for ev in events:
        onset = float(ev.get("onset") or math.nan)
        dur = float(ev.get("duration") or 0.0)
        lab = str(ev.get("label") or "")
        if not math.isfinite(onset):
            continue
        if dur < 0.0 or not math.isfinite(dur):
            dur = 0.0
        i = lane_index.get(lab, 0)
        y = pad_t + i * (lane_h + lane_gap)
        x1 = x_of(onset)
        x2 = x_of(onset + dur)
        wrect = max(1.0, x2 - x1)
        parts.append(f'<rect x="{x1:.2f}" y="{y:.2f}" width="{wrect:.2f}" height="{lane_h}" class="{_evt_css_class(lab)}" />')

    # Axis labels
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+30}" class="axis-label">time (s)</text>')
    parts.append(f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">{_e(f"{t0:.2f}s")}</text>')
    parts.append(f'<text x="{pad_l+inner_w}" y="{pad_t-10}" text-anchor="end" class="axis-label">{_e(f"{t1:.2f}s")}</text>')

    parts.append("</svg>")
    return "".join(parts)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Render nf_feedback.csv to a self-contained HTML report (stdlib only)."
    )
    ap.add_argument("--input", required=True, help="Path to nf_feedback.csv, or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/nf_feedback_report.html).")
    ap.add_argument("--title", default="Neurofeedback run report", help="Report title.")
    ap.add_argument(
        "--max-points",
        type=int,
        default=2000,
        help="Max points to plot (downsamples for long runs; default: 2000).",
    )
    ap.add_argument(
        "--table-rows",
        type=int,
        default=250,
        help="Max rows of nf_feedback.csv to include in the sortable table (default: 250; 0 disables).",
    )
    ap.add_argument(
        "--events-table-rows",
        type=int,
        default=500,
        help="Max rows of nf_derived_events.* to include in the report (default: 500; 0 disables).",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    csv_path, html_path, summary_path, run_meta_path = _guess_paths(args.input, args.out)
    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find nf_feedback.csv at: {csv_path}")

    headers, rows = _read_csv(csv_path)
    if not rows:
        raise SystemExit(f"No rows in: {csv_path}")

    cols = _detect_cols(headers)

    # Parse and sort by time (robust to out-of-order frames).
    parsed: List[Dict[str, Any]] = []
    for r in rows:
        tt = _try_float(r.get(cols.t, ""))
        if not math.isfinite(tt):
            continue
        parsed.append(
            {
                "t": tt,
                "metric": _try_float(r.get(cols.metric, "")),
                "threshold": _try_float(r.get(cols.threshold, "")),
                "reward": _try_bool_int(r.get(cols.reward, "")) if cols.reward else 0,
                "reward_rate": _try_float(r.get(cols.reward_rate, "")) if cols.reward_rate else math.nan,
                "artifact": _try_bool_int(r.get(cols.artifact, "")) if cols.artifact else None,
                "artifact_ready": _try_bool_int(r.get(cols.artifact_ready, "")) if cols.artifact_ready else None,
                "bad_channels": _try_float(r.get(cols.bad_channels, "")) if cols.bad_channels else math.nan,
                "phase": (r.get(cols.phase, "") or "").strip() if cols.phase else "",
                "raw_reward": _try_bool_int(r.get(cols.raw_reward, "")) if cols.raw_reward else None,
                "threshold_desired": _try_float(r.get(cols.threshold_desired, "")) if cols.threshold_desired else math.nan,
                "metric_raw": _try_float(r.get(cols.metric_raw, "")) if cols.metric_raw else math.nan,
                "feedback_raw": _try_float(r.get(cols.feedback_raw, "")) if cols.feedback_raw else math.nan,
                "reward_value": _try_float(r.get(cols.reward_value, "")) if cols.reward_value else math.nan,
                "metric_z": _try_float(r.get(cols.metric_z, "")) if cols.metric_z else math.nan,
                "threshold_z": _try_float(r.get(cols.threshold_z, "")) if cols.threshold_z else math.nan,
                "metric_z_ref": _try_float(r.get(cols.metric_z_ref, "")) if cols.metric_z_ref else math.nan,
                "threshold_z_ref": _try_float(r.get(cols.threshold_z_ref, "")) if cols.threshold_z_ref else math.nan,
                "_row": r,  # original row for table
            }
        )

    if not parsed:
        raise SystemExit(f"All rows had non-finite time column '{cols.t}' in: {csv_path}")

    parsed.sort(key=lambda d: d["t"])

    # Full-resolution stats (before downsampling)
    t_all = [d["t"] for d in parsed]
    metric_all = [d["metric"] for d in parsed]
    thr_all = [d["threshold"] for d in parsed]
    reward_all = [int(d["reward"]) for d in parsed]
    artifact_all = [int(d["artifact"]) for d in parsed] if cols.artifact else None
    artifact_ready_all = [int(d["artifact_ready"]) for d in parsed] if cols.artifact_ready else None
    raw_reward_all = [int(d["raw_reward"]) for d in parsed] if cols.raw_reward else None
    reward_rate_all = [d["reward_rate"] for d in parsed] if cols.reward_rate else None
    phase_all = [d["phase"] for d in parsed] if cols.phase else None

    metric_raw_all = [d["metric_raw"] for d in parsed] if cols.metric_raw else None
    thr_desired_all = [d["threshold_desired"] for d in parsed] if cols.threshold_desired else None
    feedback_raw_all = [d["feedback_raw"] for d in parsed] if cols.feedback_raw else None
    reward_value_all = [d["reward_value"] for d in parsed] if cols.reward_value else None
    metric_z_all = [d["metric_z"] for d in parsed] if cols.metric_z else None
    threshold_z_all = [d["threshold_z"] for d in parsed] if cols.threshold_z else None

    t_end = max(t_all) if t_all else 0.0
    duration = max(t_all) - min(t_all) if t_all else 0.0

    reward_frames = sum(reward_all)
    reward_frac = reward_frames / max(1, len(reward_all))
    artifact_frac = _fraction_ones(artifact_all) if artifact_all is not None else math.nan
    raw_reward_frac = _fraction_ones(raw_reward_all) if raw_reward_all is not None else math.nan
    artifact_ready_frac = _fraction_ones(artifact_ready_all) if artifact_ready_all is not None else math.nan

    metric_last = next((v for v in reversed(metric_all) if math.isfinite(v)), math.nan)
    thr_last = next((v for v in reversed(thr_all) if math.isfinite(v)), math.nan)

    reward_rate_mean = _safe_mean(reward_rate_all) if reward_rate_all is not None else math.nan
    reward_rate_last = next((v for v in reversed(reward_rate_all or []) if math.isfinite(v)), math.nan)

    # Downsample for plotting
    idx = _downsample_indices(len(parsed), max(50, int(args.max_points)))
    t = [parsed[i]["t"] for i in idx]
    metric = [parsed[i]["metric"] for i in idx]
    thr = [parsed[i]["threshold"] for i in idx]
    reward = [int(parsed[i]["reward"]) for i in idx]
    artifact = [int(parsed[i]["artifact"]) for i in idx] if cols.artifact else None
    phase = [parsed[i]["phase"] for i in idx] if cols.phase else None
    metric_raw = [parsed[i]["metric_raw"] for i in idx] if cols.metric_raw else None
    thr_desired = [parsed[i]["threshold_desired"] for i in idx] if cols.threshold_desired else None

    series_metric: List[Tuple[str, Sequence[float], str]] = [
        ("metric", metric, "line-metric"),
        ("threshold", thr, "line-thr"),
    ]
    if metric_raw is not None and any(math.isfinite(v) for v in metric_raw):
        series_metric.insert(1, ("metric_raw", metric_raw, "line-metricraw"))
    if thr_desired is not None and any(math.isfinite(v) for v in thr_desired):
        series_metric.append(("threshold_desired", thr_desired, "line-thrdes"))

    chart_metric = _svg_timeseries(
        t,
        series_metric,
        shade_reward=reward if cols.reward else None,
        shade_artifact=artifact,
        phase=phase,
        title="Metric & threshold over time (reward/artifact shaded)",
        y_label="metric / threshold",
    )

    chart_rr = ""
    if reward_rate_all is not None:
        rr = [parsed[i]["reward_rate"] for i in idx]
        chart_rr = _svg_timeseries(
            t,
            [("reward_rate", rr, "line-rr")],
            shade_reward=reward if cols.reward else None,
            shade_artifact=artifact,
            phase=phase,
            title="Reward rate over time (downsampled)",
            y_label="reward_rate",
        )

    chart_z = ""
    if (metric_z_all is not None) or (threshold_z_all is not None):
        mz = [parsed[i]["metric_z"] for i in idx] if metric_z_all is not None else []
        tz = [parsed[i]["threshold_z"] for i in idx] if threshold_z_all is not None else []
        if any(math.isfinite(v) for v in mz) or any(math.isfinite(v) for v in tz):
            series_z: List[Tuple[str, Sequence[float], str]] = []
            if metric_z_all is not None:
                series_z.append(("metric_z", mz, "line-metricz"))
            if threshold_z_all is not None:
                series_z.append(("threshold_z", tz, "line-thrz"))
            chart_z = _svg_timeseries(
                t,
                series_z,
                shade_reward=reward if cols.reward else None,
                shade_artifact=artifact,
                phase=phase,
                title="Z-scores over time (downsampled)",
                y_label="z",
            )

    chart_feedback = ""
    if (feedback_raw_all is not None) or (reward_value_all is not None):
        fb = [parsed[i]["feedback_raw"] for i in idx] if feedback_raw_all is not None else []
        rv = [parsed[i]["reward_value"] for i in idx] if reward_value_all is not None else []
        if any(math.isfinite(v) for v in fb) or any(math.isfinite(v) for v in rv):
            series_fb: List[Tuple[str, Sequence[float], str]] = []
            if feedback_raw_all is not None:
                series_fb.append(("feedback_raw", fb, "line-feedback"))
            if reward_value_all is not None:
                series_fb.append(("reward_value", rv, "line-rewardvalue"))
            chart_feedback = _svg_timeseries(
                t,
                series_fb,
                shade_reward=reward if cols.reward else None,
                shade_artifact=artifact,
                phase=phase,
                title="Continuous feedback (downsampled)",
                y_label="feedback",
            )

    summary = _read_json_if_exists(summary_path) if summary_path else None
    run_meta = _read_json_if_exists(run_meta_path) if run_meta_path else None

    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src_dir = os.path.dirname(os.path.abspath(csv_path)) or "."

    src_rel = os.path.relpath(os.path.abspath(csv_path), out_dir)
    summary_rel = os.path.relpath(os.path.abspath(summary_path), out_dir) if summary_path else ""
    run_meta_rel = os.path.relpath(os.path.abspath(run_meta_path), out_dir) if run_meta_path else ""

    # Derived events (optional).
    events_table_path, events_json_path = _guess_derived_events_paths(src_dir)
    events_table_rel = os.path.relpath(os.path.abspath(events_table_path), out_dir) if events_table_path else ""
    events_json_rel = os.path.relpath(os.path.abspath(events_json_path), out_dir) if events_json_path else ""

    derived_events_block = ""
    if events_table_path:
        ev_headers, ev_rows = _read_csv(events_table_path)
        ev_table_html = _build_table(ev_headers, ev_rows, max_rows=args.events_table_rows) if ev_headers else ""

        ev_sidecar = _read_json_if_exists(events_json_path) if events_json_path else None
        desc_map: Dict[str, str] = {}
        if isinstance(ev_sidecar, dict):
            trial_type = ev_sidecar.get("trial_type")
            if isinstance(trial_type, dict):
                levels = trial_type.get("Levels")
                if isinstance(levels, dict):
                    for k, v in levels.items():
                        if isinstance(k, str) and isinstance(v, str):
                            desc_map[k] = v

        # Parse into onset/duration/label for summary + timeline (best-effort).
        parsed_events: List[Dict[str, Any]] = []
        parse_error = ""
        try:
            col_onset, col_dur, col_label = _detect_event_cols(ev_headers)
            for r in ev_rows:
                onset = _try_float(r.get(col_onset, ""))
                if not math.isfinite(onset):
                    continue
                dur = _try_float(r.get(col_dur, "")) if col_dur else 0.0
                if not math.isfinite(dur) or dur < 0.0:
                    dur = 0.0
                lab = (r.get(col_label) or "").strip()
                parsed_events.append({"onset": onset, "duration": dur, "label": lab, "_row": r})
            parsed_events.sort(key=lambda d: (d["onset"], d["duration"]))
        except Exception as e:
            parse_error = str(e)

        ev_svg = ""
        ev_summary_html = ""
        if parsed_events and not parse_error:
            # Summary per label
            by_label: Dict[str, Dict[str, float]] = {}
            t0 = math.inf
            t1 = -math.inf
            for ev in parsed_events:
                onset = float(ev["onset"])
                dur = float(ev["duration"])
                lab = str(ev["label"])
                t0 = min(t0, onset)
                t1 = max(t1, onset + max(0.0, dur))
                st = by_label.get(lab)
                if st is None:
                    st = {"count": 0.0, "dur": 0.0}
                    by_label[lab] = st
                st["count"] += 1.0
                st["dur"] += max(0.0, dur)
            total = (t1 - t0) if (math.isfinite(t0) and math.isfinite(t1) and t1 > t0) else float(t_end)

            summary_rows: List[Dict[str, str]] = []
            for lab, st in by_label.items():
                d = st.get("dur", 0.0)
                frac = (d / total) if (total and total > 0.0 and math.isfinite(d)) else math.nan
                desc = desc_map.get(lab, "")
                summary_rows.append(
                    {
                        "trial_type": lab,
                        "count": str(int(st.get("count", 0.0))),
                        "total_duration_sec": f"{d:.6g}",
                        "fraction": (f"{frac:.3%}" if math.isfinite(frac) else ""),
                        "description": desc,
                    }
                )
            # Sort by known categories then by duration desc.
            summary_rows.sort(
                key=lambda r: (_evt_sort_key(r.get("trial_type", "")), -_try_float(r.get("total_duration_sec", "")))
            )
            ev_summary_html = _build_table(
                ["trial_type", "count", "total_duration_sec", "fraction", "description"],
                summary_rows,
                max_rows=len(summary_rows) if summary_rows else 0,
            )

            ev_svg = _svg_events_timeline(parsed_events, title="Derived events timeline (BIDS-style)")

        ev_sidecar_block = ""
        if isinstance(ev_sidecar, dict):
            ev_sidecar_block = _pretty_json_snippet(ev_sidecar, pick=["onset", "duration", "trial_type", "sample", "value"]) or ""

        err_html = f'<div class="note warn">Could not parse derived events for summary/timeline: {_e(parse_error)}</div>' if parse_error else ""

        derived_events_block = f"""<div class="card">
    <h2>Derived events</h2>
    <div class="note">
      If you enabled <code>--export-derived-events</code> or <code>--biotrace-ui</code>, <code>qeeg_nf_cli</code> writes
      <code>nf_derived_events.tsv</code>/<code>.csv</code> and a BIDS-style sidecar <code>nf_derived_events.json</code>.
      These are useful for offline annotation (baseline, train/rest schedule, reward, artifact gating).
    </div>
    {ev_svg if ev_svg else ''}
    {err_html}
    {f'<h3>Summary</h3><div class="note">Counts and total duration by <code>trial_type</code>.</div><div class="table-wrap">{ev_summary_html}</div>' if ev_summary_html else ''}
    <h3>Events table (sortable)</h3>
    <div class="table-filter">
      <div class="table-controls">
        <input type="search" placeholder="Filter events…" oninput="filterTable(this)" />
        <button type="button" onclick="downloadTableCSV(this, 'nf_derived_events_filtered.csv', true)">Download CSV</button>
        <span class="filter-count muted"></span>
      </div>
      <div class="table-wrap">{ev_table_html}</div>
    </div>
    <div class="note">
      Tip: see the source files:
      {f'<a href="{_e(events_table_rel)}"><code>{_e(events_table_rel)}</code></a>' if events_table_rel else ''}
      {(' · ' + f'<a href="{_e(events_json_rel)}"><code>{_e(events_json_rel)}</code></a>') if events_json_rel else ''}
    </div>
    {f'<h3>Optional events sidecar snippet</h3>{ev_sidecar_block}' if ev_sidecar_block else ''}
  </div>"""

    # Show a small set of common keys if present.
    summary_block = ""
    if summary:
        pick_keys = [
            "protocol",
            "metric_spec",
            "reward_direction",
            "threshold_mode",
            "threshold",
            "threshold_source",
            "threshold_init",
            "threshold_final",
            "threshold_strategy",
            "feedback_mode",
            "dwell_seconds",
            "refractory_seconds",
            "metric_smooth_seconds",
            "train_block_seconds",
            "rest_block_seconds",
            "start_with_rest",
            "baseline_seconds",
            "duration_seconds",
            "fs_hz",
        ]
        summary_block = _pretty_json_snippet(summary, pick=pick_keys) or ""

    run_meta_block = ""
    if run_meta:
        pick_keys = [
            "Tool",
            "Version",
            "GitDescribe",
            "BuildType",
            "Compiler",
            "CppStandard",
            "TimestampLocal",
            "TimestampUTC",
            "input_path",
            "OutputDir",
            "Outputs",
        ]
        run_meta_block = _pretty_json_snippet(run_meta, pick=pick_keys) or ""

    # Build a lightweight table with a subset of columns (if present).
    table_cols: List[str] = []
    for c in [
        cols.t,
        cols.metric,
        cols.metric_raw or "",
        cols.threshold,
        cols.threshold_desired or "",
        cols.reward or "",
        cols.reward_rate or "",
        cols.raw_reward or "",
        cols.artifact_ready or "",
        cols.artifact or "",
        cols.bad_channels or "",
        cols.phase or "",
        cols.feedback_raw or "",
        cols.reward_value or "",
        cols.metric_z or "",
        cols.threshold_z or "",
        cols.metric_z_ref or "",
        cols.threshold_z_ref or "",
    ]:
        if c and c in headers and c not in table_cols:
            table_cols.append(c)

    table_html = _build_table(table_cols, [d["_row"] for d in parsed], max_rows=args.table_rows) if table_cols else ""

    # Helpful "columns used" summary.
    used = {
        "time": cols.t,
        "metric": cols.metric,
        "metric_raw": cols.metric_raw or "(missing)",
        "threshold": cols.threshold,
        "threshold_desired": cols.threshold_desired or "(missing)",
        "reward": cols.reward or "(missing)",
        "reward_rate": cols.reward_rate or "(missing)",
        "raw_reward": cols.raw_reward or "(missing)",
        "artifact": cols.artifact or "(missing)",
        "artifact_ready": cols.artifact_ready or "(missing)",
        "bad_channels": cols.bad_channels or "(missing)",
        "phase": cols.phase or "(missing)",
        "feedback_raw": cols.feedback_raw or "(missing)",
        "reward_value": cols.reward_value or "(missing)",
        "metric_z": cols.metric_z or "(missing)",
        "threshold_z": cols.threshold_z or "(missing)",
        "metric_z_ref": cols.metric_z_ref or "(missing)",
        "threshold_z_ref": cols.threshold_z_ref or "(missing)",
    }


    # Optional phase stats table (requires a non-empty phase column).
    phase_stats_block = ""
    if phase_all is not None and any((p or "").strip() for p in phase_all):
        # Estimate dt from the median positive delta of t_end_sec (robust to a few outliers).
        diffs: List[float] = []
        for i in range(1, len(t_all)):
            a = float(t_all[i - 1])
            b = float(t_all[i])
            d = b - a
            if math.isfinite(d) and d > 0.0:
                diffs.append(d)
        diffs.sort()
        dt_est = diffs[len(diffs) // 2] if diffs else (duration / max(1, len(t_all)))
        if not (math.isfinite(dt_est) and dt_est > 0.0):
            dt_est = 0.0

        buckets: Dict[str, Dict[str, float]] = {}

        def _norm_phase(p: str) -> str:
            s = (p or "").strip().lower()
            if not s:
                return "other"
            if s in ("baseline", "train", "rest"):
                return s
            # common aliases
            if s.startswith("base"):
                return "baseline"
            if s.startswith("tr"):
                return "train"
            if s.startswith("re"):
                return "rest"
            return "other"

        for i, p in enumerate(phase_all):
            key = _norm_phase(str(p))
            b = buckets.get(key)
            if b is None:
                b = {
                    "frames": 0.0,
                    "seconds": 0.0,
                    "reward_sum": 0.0,
                    "artifact_sum": 0.0,
                    "metric_sum": 0.0,
                    "metric_n": 0.0,
                    "thr_sum": 0.0,
                    "thr_n": 0.0,
                }
                buckets[key] = b

            b["frames"] += 1.0

            # dt weight (best-effort): use delta to previous frame, fallback to dt_est.
            dt = dt_est
            if i > 0:
                d = float(t_all[i]) - float(t_all[i - 1])
                if math.isfinite(d) and d > 0.0:
                    dt = d
            if math.isfinite(dt) and dt > 0.0:
                b["seconds"] += dt

            if i < len(reward_all):
                b["reward_sum"] += float(reward_all[i])

            if artifact_all is not None and i < len(artifact_all):
                b["artifact_sum"] += float(artifact_all[i])

            mv = metric_all[i] if i < len(metric_all) else math.nan
            if math.isfinite(mv):
                b["metric_sum"] += float(mv)
                b["metric_n"] += 1.0

            tv = thr_all[i] if i < len(thr_all) else math.nan
            if math.isfinite(tv):
                b["thr_sum"] += float(tv)
                b["thr_n"] += 1.0

        headers_phase = [
            "phase",
            "frames",
            "seconds",
            "reward_frac",
            "artifact_frac",
            "metric_mean",
            "threshold_mean",
        ]
        rows_phase: List[Dict[str, str]] = []
        for key in ["baseline", "train", "rest", "other"]:
            b = buckets.get(key)
            if not b:
                continue
            frames = int(b.get("frames", 0.0) or 0)
            sec = float(b.get("seconds", 0.0) or 0.0)
            reward_frac_p = (float(b.get("reward_sum", 0.0) or 0.0) / frames) if frames > 0 else math.nan
            artifact_frac_p = (
                (float(b.get("artifact_sum", 0.0) or 0.0) / frames) if (artifact_all is not None and frames > 0) else math.nan
            )
            m_mean = (
                (float(b.get("metric_sum", 0.0) or 0.0) / float(b.get("metric_n", 0.0) or 0.0))
                if float(b.get("metric_n", 0.0) or 0.0) > 0.0
                else math.nan
            )
            t_mean = (
                (float(b.get("thr_sum", 0.0) or 0.0) / float(b.get("thr_n", 0.0) or 0.0))
                if float(b.get("thr_n", 0.0) or 0.0) > 0.0
                else math.nan
            )

            rows_phase.append(
                {
                    "phase": key,
                    "frames": str(frames),
                    "seconds": f"{sec:.3f}" if math.isfinite(sec) else "N/A",
                    "reward_frac": f"{reward_frac_p:.3%}" if math.isfinite(reward_frac_p) else "N/A",
                    "artifact_frac": (
                        f"{artifact_frac_p:.3%}"
                        if math.isfinite(artifact_frac_p)
                        else ("(n/a)" if artifact_all is None else "N/A")
                    ),
                    "metric_mean": f"{m_mean:.6g}" if math.isfinite(m_mean) else "N/A",
                    "threshold_mean": f"{t_mean:.6g}" if math.isfinite(t_mean) else "N/A",
                }
            )

        if rows_phase:
            phase_table_html = _build_table(headers_phase, rows_phase, max_rows=50)
            phase_stats_block = (
                f'<div class="card"><h2>Phase stats</h2>'
                f'<div class="note">Grouped by <code>{_e(cols.phase or "phase")}</code>; '
                f'approx dt={dt_est:.3f}s. (Seconds are a best-effort estimate.)</div>'
                f'<div class="table-wrap">{phase_table_html}</div></div>'
            )

    css = BASE_CSS + r"""

/* NF report specifics */
.kv { display: grid; grid-template-columns: 260px 1fr; gap: 6px 12px; font-size: 13px; }
.kv div:nth-child(odd) { color: var(--muted); }

.ts { width: 100%; height: auto; }
.evt-ts { width: 100%; height: auto; }
.frame { fill: #0c131f; stroke: #20324a; stroke-width: 1; }
.grid { stroke: rgba(255,255,255,0.06); stroke-width: 1; }
.evt-grid { stroke: rgba(255,255,255,0.04); stroke-width: 1; }

.ts-title { fill: #dce7ff; font-weight: 700; font-size: 13px; }
.axis-label { fill: var(--muted); font-size: 12px; }
.evt-label { fill: #dce7ff; font-size: 12px; opacity: 0.9; }

.line-metric { fill: none; stroke: var(--accent); stroke-width: 2; opacity: 0.95; }
.line-metricraw { fill: none; stroke: rgba(143,183,255,0.45); stroke-width: 1.5; opacity: 0.85; stroke-dasharray: 4 4; }
.line-thr { fill: none; stroke: #ffd37f; stroke-width: 2; opacity: 0.9; stroke-dasharray: 6 4; }
.line-thrdes { fill: none; stroke: rgba(255,211,127,0.55); stroke-width: 1.5; opacity: 0.85; stroke-dasharray: 2 6; }
.line-rr { fill: none; stroke: #8cffaa; stroke-width: 2; opacity: 0.95; }
.line-metricz { fill: none; stroke: #c6a8ff; stroke-width: 2; opacity: 0.95; }
.line-thrz { fill: none; stroke: #ffd37f; stroke-width: 2; opacity: 0.85; stroke-dasharray: 6 4; }
.line-feedback { fill: none; stroke: #8fb7ff; stroke-width: 2; opacity: 0.95; }
.line-rewardvalue { fill: none; stroke: #ffb86b; stroke-width: 2; opacity: 0.95; }

.line-metric-legend { fill: var(--accent); }
.line-metricraw-legend { fill: rgba(143,183,255,0.45); }
.line-thr-legend { fill: #ffd37f; }
.line-thrdes-legend { fill: rgba(255,211,127,0.55); }
.line-rr-legend { fill: #8cffaa; }
.line-metricz-legend { fill: #c6a8ff; }
.line-thrz-legend { fill: #ffd37f; }
.line-feedback-legend { fill: #8fb7ff; }
.line-rewardvalue-legend { fill: #ffb86b; }

.shade-reward { fill: rgba(127, 179, 255, 0.14); }
.shade-artifact { fill: rgba(255, 120, 150, 0.14); }

.phase-baseline { fill: rgba(200, 200, 200, 0.12); }
.phase-train { fill: rgba(127, 179, 255, 0.22); }
.phase-rest { fill: rgba(255, 211, 127, 0.18); }
.phase-other { fill: rgba(155, 176, 208, 0.14); }

.legend-text { fill: #dce7ff; font-size: 12px; }
.legend-reward { fill: rgba(127, 179, 255, 0.55); }
.legend-artifact { fill: rgba(255, 120, 150, 0.6); }

/* Derived events timeline */
.evt { stroke: rgba(255,255,255,0.10); stroke-width: 1; }
.evt-baseline { fill: rgba(200,200,200,0.22); }
.evt-train { fill: rgba(127, 179, 255, 0.30); }
.evt-rest { fill: rgba(255, 211, 127, 0.26); }
.evt-reward { fill: rgba(127, 179, 255, 0.45); }
.evt-artifact { fill: rgba(255, 120, 150, 0.42); }
.evt-nf { fill: rgba(155, 176, 208, 0.28); }
.evt-other { fill: rgba(155, 176, 208, 0.20); }

.warn { color: var(--warn); }
"""

    js = JS_SORT_TABLE

    links: List[str] = [f'<a href="{_e(src_rel)}"><code>{_e(src_rel)}</code></a>']
    if summary_rel:
        links.append(f'<a href="{_e(summary_rel)}"><code>{_e(summary_rel)}</code></a>')
    if run_meta_rel:
        links.append(f'<a href="{_e(run_meta_rel)}"><code>{_e(run_meta_rel)}</code></a>')
    if events_table_rel:
        links.append(f'<a href="{_e(events_table_rel)}"><code>{_e(events_table_rel)}</code></a>')
    if events_json_rel:
        links.append(f'<a href="{_e(events_json_rel)}"><code>{_e(events_json_rel)}</code></a>')

    html_doc = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{_e(args.title)}</title>
<style>{css}</style>
</head>
<body>
<header>
  <h1>{_e(args.title)}</h1>
  <div class="meta">Generated {now} — sources: {' · '.join(links)}</div>
</header>
<main>
  <div class="card">
    <h2>About</h2>
    <div class="note">
      This report is a convenience visualization of <code>nf_feedback.csv</code> written by <code>qeeg_nf_cli</code>.
      It is for research/educational inspection only and is not a medical device.
    </div>
  </div>

  <div class="card">
    <h2>Quick stats</h2>
    <div class="kv">
      <div>Frames (rows)</div><div>{len(parsed)}</div>
      <div>t_end max (s)</div><div>{t_end:.3f}</div>
      <div>Duration (s)</div><div>{duration:.3f}</div>
      <div>Reward fraction</div><div>{reward_frac:.3%} ({reward_frames} frames)</div>
      <div>Raw reward fraction</div><div>{_e(f"{raw_reward_frac:.3%}" if math.isfinite(raw_reward_frac) else "N/A")}</div>
      <div>Artifact fraction</div><div>{_e(f"{artifact_frac:.3%}" if math.isfinite(artifact_frac) else "N/A")}</div>
      <div>Artifact-ready fraction</div><div>{_e(f"{artifact_ready_frac:.3%}" if math.isfinite(artifact_ready_frac) else "N/A")}</div>
      <div>Reward rate mean / last</div><div>{_e(f"{reward_rate_mean:.6g} / {reward_rate_last:.6g}" if (math.isfinite(reward_rate_mean) and math.isfinite(reward_rate_last)) else "N/A")}</div>
      <div>Last metric</div><div>{_e(f"{metric_last:.6g}" if math.isfinite(metric_last) else "NaN")}</div>
      <div>Last threshold</div><div>{_e(f"{thr_last:.6g}" if math.isfinite(thr_last) else "NaN")}</div>
    </div>
  </div>

  {phase_stats_block}

  <div class="card">
    <h2>Columns used</h2>
    <div class="note">This renderer is flexible; it will use these columns when present.</div>
    <div class="kv">
      {''.join(f'<div>{_e(k)}</div><div><code>{_e(v)}</code></div>' for k, v in used.items())}
    </div>
  </div>

  <div class="card">
    <h2>Timeline</h2>
    {chart_metric}
    <div class="note">
      Reward segments are shaded blue. If artifact gating was enabled, artifact segments are shaded red.
      If phase information is available, a thin band at the top shows baseline/train/rest/other phases.
    </div>
  </div>

  {f'<div class="card"><h2>Reward rate</h2>{chart_rr}</div>' if chart_rr else ''}
  {f'<div class="card"><h2>Z-scores</h2>{chart_z}</div>' if chart_z else ''}
  {f'<div class="card"><h2>Continuous feedback</h2>{chart_feedback}</div>' if chart_feedback else ''}

  {derived_events_block}

  <div class="card">
    <h2>Sampled rows (sortable)</h2>
    <div class="note">Downsampled view of the CSV to keep the report lightweight. Click any header to sort.</div>
    <div class="table-filter">
      <div class="table-controls">
        <input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />
        <button type="button" onclick="downloadTableCSV(this, 'nf_feedback_table_filtered.csv', true)">Download CSV</button>
        <span class="filter-count muted"></span>
      </div>
      <div class="table-wrap">{table_html or '<div class="note">No table columns detected.</div>'}</div>
    </div>
  </div>

  <div class="card">
    <h2>Optional nf_summary.json snippet</h2>
    <div class="note">Only a small set of common keys is shown (when present) to keep this HTML lightweight.</div>
    {summary_block or '<div class="note">No nf_summary.json found (or no common keys present).</div>'}
  </div>

  <div class="card">
    <h2>Optional nf_run_meta.json snippet</h2>
    <div class="note">Build/run metadata (when present).</div>
    {run_meta_block or '<div class="note">No nf_run_meta.json found (or no common keys present).</div>'}
  </div>

  <div class="footer">
    Tip: open this file in a browser. It is self-contained (no network requests).
  </div>
</main>
<script>{js}</script>
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
