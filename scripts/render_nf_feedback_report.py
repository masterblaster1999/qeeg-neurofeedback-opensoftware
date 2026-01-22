#!/usr/bin/env python3
"""Render qeeg_nf_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - nf_feedback.csv (required)
  - nf_summary.json (optional; adds extra metadata)
  - nf_run_meta.json (optional; adds build/run metadata)

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

    chart_metric = _svg_timeseries(
        t,
        [
            ("metric", metric, "line-metric"),
            ("threshold", thr, "line-thr"),
        ],
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

    summary = _read_json_if_exists(summary_path) if summary_path else None
    run_meta = _read_json_if_exists(run_meta_path) if run_meta_path else None

    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src_rel = os.path.relpath(os.path.abspath(csv_path), out_dir)
    summary_rel = os.path.relpath(os.path.abspath(summary_path), out_dir) if summary_path else ""
    run_meta_rel = os.path.relpath(os.path.abspath(run_meta_path), out_dir) if run_meta_path else ""

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
        cols.threshold,
        cols.reward or "",
        cols.reward_rate or "",
        cols.raw_reward or "",
        cols.artifact_ready or "",
        cols.artifact or "",
        cols.bad_channels or "",
        cols.phase or "",
    ]:
        if c and c in headers and c not in table_cols:
            table_cols.append(c)

    table_html = _build_table(table_cols, [d["_row"] for d in parsed], max_rows=args.table_rows) if table_cols else ""

    # Helpful "columns used" summary.
    used = {
        "time": cols.t,
        "metric": cols.metric,
        "threshold": cols.threshold,
        "reward": cols.reward or "(missing)",
        "reward_rate": cols.reward_rate or "(missing)",
        "raw_reward": cols.raw_reward or "(missing)",
        "artifact": cols.artifact or "(missing)",
        "artifact_ready": cols.artifact_ready or "(missing)",
        "bad_channels": cols.bad_channels or "(missing)",
        "phase": cols.phase or "(missing)",
    }

    css = BASE_CSS + r"""

/* NF report specifics */
.kv { display: grid; grid-template-columns: 260px 1fr; gap: 6px 12px; font-size: 13px; }
.kv div:nth-child(odd) { color: var(--muted); }

.ts { width: 100%; height: auto; }
.frame { fill: #0c131f; stroke: #20324a; stroke-width: 1; }
.grid { stroke: rgba(255,255,255,0.06); stroke-width: 1; }

.ts-title { fill: #dce7ff; font-weight: 700; font-size: 13px; }
.axis-label { fill: var(--muted); font-size: 12px; }

.line-metric { fill: none; stroke: var(--accent); stroke-width: 2; opacity: 0.95; }
.line-thr { fill: none; stroke: #ffd37f; stroke-width: 2; opacity: 0.9; stroke-dasharray: 6 4; }
.line-rr { fill: none; stroke: #8cffaa; stroke-width: 2; opacity: 0.95; }

.line-metric-legend { fill: var(--accent); }
.line-thr-legend { fill: #ffd37f; }
.line-rr-legend { fill: #8cffaa; }

.shade-reward { fill: rgba(127, 179, 255, 0.14); }
.shade-artifact { fill: rgba(255, 120, 150, 0.14); }

.phase-baseline { fill: rgba(200, 200, 200, 0.12); }
.phase-train { fill: rgba(127, 179, 255, 0.22); }
.phase-rest { fill: rgba(255, 211, 127, 0.18); }
.phase-other { fill: rgba(155, 176, 208, 0.14); }

.legend-text { fill: #dce7ff; font-size: 12px; }
.legend-reward { fill: rgba(127, 179, 255, 0.55); }
.legend-artifact { fill: rgba(255, 120, 150, 0.6); }
"""

    js = JS_SORT_TABLE

    links: List[str] = [f'<a href="{_e(src_rel)}"><code>{_e(src_rel)}</code></a>']
    if summary_rel:
        links.append(f'<a href="{_e(summary_rel)}"><code>{_e(summary_rel)}</code></a>')
    if run_meta_rel:
        links.append(f'<a href="{_e(run_meta_rel)}"><code>{_e(run_meta_rel)}</code></a>')

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
      If a train/rest schedule was enabled, a thin band at the top shows baseline/train/rest/other phases.
    </div>
  </div>

  {f'<div class="card"><h2>Reward rate</h2>{chart_rr}</div>' if chart_rr else ''}

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
