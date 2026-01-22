#!/usr/bin/env python3
"""Render qeeg_artifacts_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - artifact_windows.csv (required)
  - artifact_channel_summary.csv (optional; top channels chart)
  - artifact_segments.csv (optional; preferred timeline)
  - artifact_summary.txt (optional; embedded)
  - artifact_run_meta.json (optional; metadata)

Typical usage:

  # Pass an output directory produced by qeeg_artifacts_cli
  python3 scripts/render_artifacts_report.py --input out_art

  # Or pass a specific CSV
  python3 scripts/render_artifacts_report.py --input out_art/artifact_windows.csv

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.
"""

from __future__ import annotations

import argparse
import math
import os
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    read_text_if_exists as _read_text_if_exists,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str]:
    """Return (outdir, windows_csv_path, html_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        windows_csv = os.path.join(outdir, "artifact_windows.csv")
    else:
        p = os.path.abspath(inp)
        outdir = os.path.dirname(p) or "."
        if os.path.basename(p).lower().endswith(".csv"):
            windows_csv = p
        else:
            windows_csv = os.path.join(outdir, "artifact_windows.csv")

    if out is None:
        out = os.path.join(outdir, "artifacts_report.html")
    return outdir, windows_csv, os.path.abspath(out)


def _try_float(s: str) -> float:
    ss = (s or "").strip()
    if ss == "":
        return math.nan
    try:
        return float(ss)
    except Exception:
        return math.nan


def _try_int(s: str) -> int:
    ss = (s or "").strip()
    if ss == "":
        return 0
    try:
        return int(float(ss))
    except Exception:
        return 0


def _finite(values: Iterable[float]) -> List[float]:
    out: List[float] = []
    for v in values:
        if math.isfinite(v):
            out.append(v)
    return out


def _svg_hbar(labels: Sequence[str], values: Sequence[float], *, title: str, units: str = "") -> str:
    w, h = 860, 320
    pad_l, pad_r, pad_t, pad_b = 220, 18, 38, 28
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    vmax = max(_finite(values) + [1.0])
    vmax = vmax if vmax > 0 else 1.0

    n = max(1, len(labels))
    row_h = inner_h / n

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')
    if units:
        parts.append(f'<text x="{w - pad_r}" y="22" text-anchor="end" class="chart-units">{_e(units)}</text>')

    for i, (lab, v) in enumerate(zip(labels, values)):
        y = pad_t + i * row_h
        cy = y + row_h * 0.65
        parts.append(f'<text x="{pad_l - 8}" y="{cy}" text-anchor="end" class="label">{_e(lab)}</text>')
        if not math.isfinite(v):
            parts.append(f'<text x="{pad_l}" y="{cy}" class="nan">NaN</text>')
            continue

        bw = (v / vmax) * inner_w
        bar_y = y + row_h * 0.18
        bar_h = max(2.0, row_h * 0.55)
        parts.append(f'<rect x="{pad_l}" y="{bar_y}" width="{bw}" height="{bar_h}" class="bar"/>')
        parts.append(
            f'<text x="{pad_l + inner_w - 2}" y="{cy}" text-anchor="end" class="value">{_e(f"{v:.6g}")}</text>'
        )

    parts.append("</svg>")
    return "".join(parts)


def _svg_timeline(
    segments: Sequence[Tuple[float, float, int]],
    *,
    title: str,
    total_duration: float,
    width: int = 1000,
    height: int = 140,
) -> str:
    """Return an SVG timeline.

    segments: list of (t_start_sec, t_end_sec, kind)
      - kind=1 => bad
      - kind=0 => ok
    """
    w, h = width, height
    pad_l, pad_r, pad_t, pad_b = 60, 20, 34, 30
    inner_w = max(1.0, w - pad_l - pad_r)
    inner_h = max(1.0, h - pad_t - pad_b)
    dur = total_duration if total_duration > 0 else 1.0

    # Ticks: 0, 25, 50, 75, 100%
    ticks = [0.0, 0.25 * dur, 0.5 * dur, 0.75 * dur, dur]

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="timeline">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')

    y0 = pad_t + inner_h * 0.35
    bar_h = inner_h * 0.28
    parts.append(f'<rect x="{pad_l}" y="{y0}" width="{inner_w}" height="{bar_h}" class="tl-bg"/>')

    def x_of(t: float) -> float:
        return pad_l + (t / dur) * inner_w

    for t0, t1, kind in segments:
        if not (math.isfinite(t0) and math.isfinite(t1)):
            continue
        if t1 <= t0:
            continue
        x0 = x_of(max(0.0, t0))
        x1 = x_of(min(dur, t1))
        cls = "tl-bad" if kind else "tl-ok"
        parts.append(f'<rect x="{x0}" y="{y0}" width="{max(0.5, x1 - x0)}" height="{bar_h}" class="{cls}"/>')

    # Axis + ticks
    axis_y = pad_t + inner_h * 0.82
    parts.append(f'<line x1="{pad_l}" y1="{axis_y}" x2="{pad_l + inner_w}" y2="{axis_y}" class="axis"/>')
    for t in ticks:
        x = x_of(t)
        parts.append(f'<line x1="{x}" y1="{axis_y - 4}" x2="{x}" y2="{axis_y + 4}" class="axis"/>')
        parts.append(f'<text x="{x}" y="{axis_y + 18}" text-anchor="middle" class="tick">{_e(f"{t:.1f}s")}</text>')

    parts.append("</svg>")
    return "".join(parts)


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, bad_flag_col: Optional[str] = None) -> str:
    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body_rows: List[str] = []
    for r in rows:
        cls = ""
        if bad_flag_col is not None:
            cls = "bad" if _try_int(r.get(bad_flag_col, "0")) else ""
        tds = "".join(f"<td>{_e(r.get(h, ''))}</td>" for h in headers)
        body_rows.append(f'<tr class="{cls}">{tds}</tr>')
    return f'<table class="data-table sticky"><thead><tr>{ths}</tr></thead><tbody>' + "".join(body_rows) + "</tbody></table>"


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_artifacts_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to artifact_windows.csv or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/artifacts_report.html)")
    ap.add_argument("--top-n", type=int, default=12, help="Top-N channels to show in charts (default: 12)")
    ap.add_argument("--title", default="Artifacts report", help="Report title")
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, windows_csv, html_path = _guess_paths(args.input, args.out)
    if not os.path.exists(windows_csv):
        raise SystemExit(f"Could not find artifact_windows.csv at: {windows_csv}")

    win_headers, win_rows = _read_csv(windows_csv)
    if not win_rows:
        raise SystemExit(f"No rows found in: {windows_csv}")

    # Optional sidecars
    ch_sum_path = os.path.join(outdir, "artifact_channel_summary.csv")
    seg_path = os.path.join(outdir, "artifact_segments.csv")
    summary_txt_path = os.path.join(outdir, "artifact_summary.txt")
    meta_path = os.path.join(outdir, "artifact_run_meta.json")

    ch_headers: List[str] = []
    ch_rows: List[Dict[str, str]] = []
    if os.path.exists(ch_sum_path):
        try:
            ch_headers, ch_rows = _read_csv(ch_sum_path)
        except Exception:
            ch_headers, ch_rows = [], []

    seg_headers: List[str] = []
    seg_rows: List[Dict[str, str]] = []
    if os.path.exists(seg_path):
        try:
            seg_headers, seg_rows = _read_csv(seg_path)
        except Exception:
            seg_headers, seg_rows = [], []

    summary_txt = _read_text_if_exists(summary_txt_path)
    meta = _read_json_if_exists(meta_path) if os.path.exists(meta_path) else None

    # Basic summary
    n_windows = len(win_rows)
    n_bad_windows = sum(_try_int(r.get("bad", "0")) != 0 for r in win_rows)
    bad_frac = (float(n_bad_windows) / float(n_windows)) if n_windows > 0 else 0.0
    t_ends = [_try_float(r.get("t_end_sec", "")) for r in win_rows]
    t_starts = [_try_float(r.get("t_start_sec", "")) for r in win_rows]
    t_ends_f = [t for t in t_ends if math.isfinite(t)]
    t_starts_f = [t for t in t_starts if math.isfinite(t)]
    t_end_max = max(t_ends_f) if t_ends_f else 0.0
    t_start_min = min(t_starts_f) if t_starts_f else 0.0
    duration = max(0.0, t_end_max - t_start_min)

    # Build timeline segments
    timeline_segments: List[Tuple[float, float, int]] = []
    if seg_rows:
        for r in seg_rows:
            t0 = _try_float(r.get("t_start_sec", ""))
            t1 = _try_float(r.get("t_end_sec", ""))
            if math.isfinite(t0) and math.isfinite(t1) and t1 > t0:
                timeline_segments.append((t0, t1, 1))
    else:
        # Fall back to windows. Downsample if huge.
        max_windows = 2000
        rows_for_tl = win_rows
        if len(win_rows) > max_windows:
            step = max(1, int(len(win_rows) / max_windows))
            rows_for_tl = win_rows[::step]
        for r in rows_for_tl:
            t0 = _try_float(r.get("t_start_sec", ""))
            t1 = _try_float(r.get("t_end_sec", ""))
            bad = 1 if _try_int(r.get("bad", "0")) != 0 else 0
            if math.isfinite(t0) and math.isfinite(t1) and t1 > t0:
                timeline_segments.append((t0, t1, bad))

    tl_title = "Artifact segments" if seg_rows else "Artifact windows (downsampled)"
    timeline_svg = _svg_timeline(timeline_segments, title=tl_title, total_duration=max(duration, 1e-9))

    # Channel chart (top channels by bad_window_fraction)
    chart_html = ""
    if ch_rows and ("bad_window_fraction" in (h.lower() for h in ch_headers) or "bad_window_fraction" in ch_headers):
        # normalize header lookup
        def get_frac(r: Dict[str, str]) -> float:
            return _try_float(r.get("bad_window_fraction", r.get("bad_window_fraction ", r.get("bad_window_fraction".upper(), ""))))

        vals: List[Tuple[str, float]] = []
        for r in ch_rows:
            ch = r.get("channel", "") or "?"
            frac = _try_float(r.get("bad_window_fraction", ""))
            if math.isfinite(frac):
                vals.append((ch, frac))
        vals.sort(key=lambda kv: kv[1], reverse=True)
        vals = vals[: min(max(1, int(args.top_n)), len(vals))]
        if vals:
            labels = [k for k, _ in vals]
            values = [v for _, v in vals]
            chart_html = _svg_hbar(labels, values, title=f"Top {len(vals)} channels by bad_window_fraction")

    # Meta info
    now = utc_now_iso()
    rel_win = os.path.relpath(os.path.abspath(windows_csv), os.path.dirname(html_path) or ".").replace(os.sep, "/")

    meta_lines: List[str] = []
    if isinstance(meta, dict):
        tool = meta.get("Tool")
        ts = meta.get("TimestampLocal")
        if isinstance(tool, str):
            meta_lines.append(f"Tool: <code>{_e(tool)}</code>")
        if isinstance(ts, str):
            meta_lines.append(f"TimestampLocal: <code>{_e(ts)}</code>")
        inp = meta.get("Input")
        if isinstance(inp, dict) and isinstance(inp.get("Path"), str):
            meta_lines.append(f"Input: <code>{_e(inp.get('Path'))}</code>")
    meta_html = "<br>".join(meta_lines) if meta_lines else "<span class=\"muted\">(no artifact_run_meta.json found)</span>"

    # Tables
    win_table = _build_table(win_headers, win_rows, bad_flag_col="bad")
    seg_table = _build_table(seg_headers, seg_rows) if seg_rows else "<div class=\"muted\">artifact_segments.csv not found</div>"
    ch_table = _build_table(ch_headers, ch_rows) if ch_rows else "<div class=\"muted\">artifact_channel_summary.csv not found</div>"

    css = BASE_CSS + r"""

.kpi { display:flex; gap: 14px; flex-wrap: wrap; }
.kpi .pill { border: 1px solid var(--grid); border-radius: 999px; padding: 6px 10px; font-size: 13px; }
.kpi .pill b { color: #dce7ff; }

.data-table tr.bad td { border-left: 3px solid var(--bad); }

.chart { width: 100%; height: auto; }
.timeline { width: 100%; height: auto; }
.axis { stroke: #2b3d58; stroke-width: 1; }
.label, .value, .nan, .chart-title, .chart-units, .tick { fill: #dce7ff; font-size: 12px; }
.chart-title { font-size: 13px; font-weight: 600; }
.chart-units { fill: var(--muted); font-size: 12px; }
.tick { fill: var(--muted); font-size: 11px; }
.label { fill: #cfe0ff; }
.value { fill: var(--muted); font-size: 12px; }
.nan { fill: var(--warn); }
.bar { fill: var(--ok); opacity: 0.92; }

.tl-bg { fill: rgba(255,255,255,0.04); stroke: rgba(255,255,255,0.06); }
.tl-bad { fill: var(--bad); opacity: 0.75; }
.tl-ok { fill: var(--ok); opacity: 0.25; }
"""

    js = JS_SORT_TABLE

    charts_block = ""
    if chart_html:
        charts_block = f'<div class="card"><h2>Channel summary</h2><div class="note">Based on artifact_channel_summary.csv</div>{chart_html}</div>'

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
  <div class=\"meta\">Generated {now} — source: <code>{_e(rel_win)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>Summary</h2>
    <div class=\"kpi\">
      <div class=\"pill\"><b>Windows</b>: {_e(n_windows)}</div>
      <div class=\"pill\"><b>Bad windows</b>: {_e(n_bad_windows)}</div>
      <div class=\"pill\"><b>Bad fraction</b>: {_e(f"{bad_frac:.4f}")}</div>
      <div class=\"pill\"><b>Duration</b>: {_e(f"{duration:.1f}s")}</div>
      <div class=\"pill\"><b>Segments</b>: {_e(len(seg_rows))}</div>
    </div>
    <div class=\"note\" style=\"margin-top:10px\">{meta_html}</div>
  </div>

  <div class=\"card\">
    <h2>Timeline</h2>
    <div class=\"note\">{_e('Segments are preferred (artifact_segments.csv). If missing, bad windows are shown (possibly downsampled).')}</div>
    {timeline_svg}
  </div>

  {charts_block}

  <div class=\"card\">
    <h2>artifact_segments.csv</h2>
    <div class=\"note\">Merged artifact segments (if exported). Click headers to sort.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'artifact_segments_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{seg_table}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>artifact_channel_summary.csv</h2>
    <div class=\"note\">Per-channel bad window fractions (if exported). Click headers to sort.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'artifact_channel_summary_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{ch_table}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>artifact_windows.csv</h2>
    <div class=\"note\">Per-window artifact flags and max z-scores. Rows with <code>bad=1</code> are marked with a left red bar.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'artifact_windows_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{win_table}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>artifact_summary.txt</h2>
    <div class=\"note\">Raw CLI summary (if present).</div>
    <pre>{_e(summary_txt or 'artifact_summary.txt not found')}</pre>
  </div>

  <div class=\"note\">Tip: open this file in a browser. It is self-contained (no network requests).</div>
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
            import webbrowser
            import pathlib

            webbrowser.open(pathlib.Path(os.path.abspath(html_path)).as_uri())
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
