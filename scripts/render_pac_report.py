#!/usr/bin/env python3
"""Render qeeg_pac_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (e.g., CI artifacts, lab workstations).

Inputs (produced by qeeg_pac_cli under --outdir):
  - pac_timeseries.csv (required)
  - pac_phase_distribution.csv (optional; only for MI mode when computed)
  - pac_summary.txt (optional)
  - pac_run_meta.json (optional; build/run metadata)

Typical usage:

  # Pass an output directory produced by qeeg_pac_cli
  python3 scripts/render_pac_report.py --input out_pac

  # Or pass pac_timeseries.csv directly
  python3 scripts/render_pac_report.py --input out_pac/pac_timeseries.csv --out report.html

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.

Notes:
- This is a convenience visualization for research/educational inspection only.
- It is not a medical device and is not intended for clinical decision-making.
"""

from __future__ import annotations

import argparse
import math
import os
import pathlib
import statistics
import webbrowser
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
    read_text_if_exists as _read_text_if_exists,
    try_float as _try_float,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, Optional[str], Optional[str], Optional[str]]:
    """Return (timeseries_csv, html_path, dist_csv_or_None, summary_txt_or_None, run_meta_json_or_None)."""
    csv_path = inp

    if _is_dir(inp):
        out_dir = os.path.abspath(inp)
        csv_path = os.path.join(out_dir, "pac_timeseries.csv")
        dist = os.path.join(out_dir, "pac_phase_distribution.csv")
        summ = os.path.join(out_dir, "pac_summary.txt")
        meta = os.path.join(out_dir, "pac_run_meta.json")
        if out is None:
            out = os.path.join(out_dir, "pac_report.html")
    else:
        base_dir = os.path.dirname(os.path.abspath(inp)) or "."
        # Allow passing pac_phase_distribution.csv by inferring the timeseries in the same folder.
        if os.path.basename(inp) == "pac_phase_distribution.csv":
            csv_path = os.path.join(base_dir, "pac_timeseries.csv")
        dist = os.path.join(base_dir, "pac_phase_distribution.csv")
        summ = os.path.join(base_dir, "pac_summary.txt")
        meta = os.path.join(base_dir, "pac_run_meta.json")
        if out is None:
            out = os.path.join(base_dir, "pac_report.html")

    dist = dist if os.path.exists(dist) else None
    summ = summ if os.path.exists(summ) else None
    meta = meta if os.path.exists(meta) else None

    return csv_path, out or "pac_report.html", dist, summ, meta


def _pick_col(headers: Sequence[str], candidates: Sequence[str]) -> str:
    for c in candidates:
        if c in headers:
            return c
    # Try case-insensitive match
    low = {h.lower(): h for h in headers}
    for c in candidates:
        if c.lower() in low:
            return low[c.lower()]
    return candidates[0] if candidates else (headers[0] if headers else "")


def _polyline(points: Sequence[Tuple[float, float]]) -> str:
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points)


def _svg_line_chart(t: Sequence[float], y: Sequence[float], *, title: str, y_label: str) -> str:
    if not t or not y or len(t) != len(y):
        return ""

    w, h = 980, 380
    pad_l, pad_r, pad_t, pad_b = 60, 18, 38, 44
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    t_min = min(t)
    t_max = max(t)
    if not (math.isfinite(t_min) and math.isfinite(t_max)) or t_max <= t_min:
        t_min, t_max = 0.0, 1.0

    y_min, y_max = _finite_minmax([v for v in y if math.isfinite(v)])

    def x_of(tt: float) -> float:
        return pad_l + (tt - t_min) / (t_max - t_min) * inner_w

    def y_of(vv: float) -> float:
        return pad_t + (1.0 - (vv - y_min) / (y_max - y_min)) * inner_h

    pts: List[Tuple[float, float]] = []
    for tt, vv in zip(t, y):
        if math.isfinite(tt) and math.isfinite(vv):
            pts.append((x_of(tt), y_of(vv)))

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="ts">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    # Grid lines
    for k in range(5):
        yy = pad_t + k / 4.0 * inner_h
        parts.append(f'<line x1="{pad_l}" y1="{yy:.2f}" x2="{pad_l+inner_w}" y2="{yy:.2f}" class="grid" />')

    if pts:
        parts.append(f'<polyline points="{_polyline(pts)}" class="line" />')
    else:
        parts.append(f'<text x="{pad_l + 12}" y="{pad_t + 18}" class="warn">No finite samples</text>')

    # Axis labels
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+30}" class="axis-label">time (s)</text>')
    parts.append(
        f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">{_e(f"{y_label} (min={y_min:.4g}, max={y_max:.4g})")}</text>'
    )
    parts.append(
        f'<text x="{pad_l+inner_w}" y="{pad_t+inner_h+30}" text-anchor="end" class="axis-label">{_e(f"{t_max:.2f}s")}</text>'
    )

    parts.append("</svg>")
    return "".join(parts)


def _svg_distribution(bins: Sequence[int], probs: Sequence[float], *, title: str) -> str:
    if not bins or not probs or len(bins) != len(probs):
        return ""

    w, h = 980, 300
    pad_l, pad_r, pad_t, pad_b = 52, 18, 38, 40
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    pmax = max([p for p in probs if math.isfinite(p)] + [1.0])
    if not math.isfinite(pmax) or pmax <= 0:
        pmax = 1.0

    n = max(1, len(bins))
    bar_w = inner_w / n

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="dist">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    # Bars
    for i, (b, p) in enumerate(zip(bins, probs)):
        if not math.isfinite(p) or p < 0:
            continue
        x = pad_l + i * bar_w
        bh = (p / pmax) * inner_h
        y = pad_t + (inner_h - bh)
        parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{max(1.0, bar_w-1):.2f}" height="{bh:.2f}" class="bar" />')
        # Sparse bin labels
        if n <= 16 or (i % max(1, n // 8) == 0) or (i == n - 1):
            parts.append(
                f'<text x="{x + bar_w/2:.2f}" y="{pad_t+inner_h+18}" text-anchor="middle" class="axis-label">{_e(str(b))}</text>'
            )

    # y-axis label
    parts.append(f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">probability (max={pmax:.4g})</text>')
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+34}" class="axis-label">phase bin index</text>')

    parts.append("</svg>")
    return "".join(parts)


def _render_run_meta(run_meta: Optional[Dict[str, Any]]) -> str:
    if not run_meta:
        return ""

    keys = [
        "Tool",
        "Version",
        "GitDescribe",
        "BuildType",
        "Compiler",
        "CppStandard",
        "TimestampUTC",
        "input_path",
        "InputPath",
        "OutputDir",
    ]

    rows: List[str] = []
    seen = set()
    for k in keys:
        if k in run_meta:
            v = run_meta.get(k)
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(v)}</code></td></tr>")
            seen.add(k)

    extra = 0
    for k, v in run_meta.items():
        if k in seen:
            continue
        if extra >= 12:
            break
        if isinstance(v, (str, int, float, bool)):
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(v)}</code></td></tr>")
            extra += 1

    if not rows:
        return ""

    return (
        '<div class="card">'
        '<h2>Run metadata</h2>'
        '<div class="note">Build/run metadata written by <code>qeeg_pac_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + "</table>"
        "</div>"
    )


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, max_rows: int) -> str:
    if not headers:
        return ""

    n = len(rows)
    max_rows = max(1, int(max_rows))
    idx = _downsample_indices(n, max_rows)

    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body_rows: List[str] = []
    for i in idx:
        r = rows[i]
        tds = "".join(f"<td>{_e(r.get(h, ''))}</td>" for h in headers)
        body_rows.append(f"<tr>{tds}</tr>")

    return (
        '<table class="data-table sticky">'
        f"<thead><tr>{ths}</tr></thead>"
        "<tbody>"
        + "".join(body_rows)
        + "</tbody></table>"
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_pac_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to pac_timeseries.csv (or pac_phase_distribution.csv), or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/pac_report.html).")
    ap.add_argument(
        "--max-chart-points",
        type=int,
        default=2000,
        help="Max points to plot in the timeseries chart (default: 2000).",
    )
    ap.add_argument(
        "--max-table-rows",
        type=int,
        default=600,
        help="Max rows to include in the sampled table (default: 600).",
    )
    ap.add_argument("--title", default="PAC report", help="Report title.")
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    ts_csv, html_path, dist_csv, summary_txt, meta_json = _guess_paths(args.input, args.out)
    if not os.path.exists(ts_csv):
        raise SystemExit(f"Could not find pac_timeseries.csv at: {ts_csv}")

    headers, rows = _read_csv(ts_csv)
    if not rows:
        raise SystemExit(f"No rows found in: {ts_csv}")

    t_col = _pick_col(headers, ["t_end_sec", "t_sec", "time_sec", "time", "t"])
    v_col = _pick_col(headers, ["pac", "value", "Pac", "PAC"])

    t: List[float] = []
    y: List[float] = []
    for r in rows:
        tt = _try_float(r.get(t_col, ""))
        vv = _try_float(r.get(v_col, ""))
        if math.isfinite(tt) and math.isfinite(vv):
            t.append(tt)
            y.append(vv)

    duration = (max(t) - min(t)) if t else math.nan

    # Chart downsampling
    plot_idx = _downsample_indices(len(t), max(10, int(args.max_chart_points)))
    t_plot = [t[i] for i in plot_idx] if t else []
    y_plot = [y[i] for i in plot_idx] if y else []

    chart_html = _svg_line_chart(t_plot, y_plot, title=f"{v_col} over time", y_label=v_col)

    # Distribution
    dist_html = ""
    dist_stats = ""
    if dist_csv and os.path.exists(dist_csv):
        dh, dr = _read_csv(dist_csv)
        if dr:
            b_col = _pick_col(dh, ["bin_index", "bin", "index"])
            p_col = _pick_col(dh, ["prob", "p", "probability"])
            bins: List[int] = []
            probs: List[float] = []
            for rr in dr:
                bi = _try_float(rr.get(b_col, ""))
                pr = _try_float(rr.get(p_col, ""))
                if math.isfinite(bi) and math.isfinite(pr):
                    bins.append(int(bi))
                    probs.append(float(pr))

            if probs:
                s = sum(probs)
                # Normalize if slightly off.
                if s > 0 and math.isfinite(s):
                    probs = [p / s for p in probs]
                # Simple stats
                pmax = max(probs)
                ent = -sum(p * math.log(p) for p in probs if p > 0)
                dist_stats = f"bins={len(probs)}, sum={sum(probs):.6g}, max={pmax:.6g}, entropy={ent:.6g}"
                dist_html = _svg_distribution(bins, probs, title="Phase distribution (average)")

    # Summary text
    summary_text = _read_text_if_exists(summary_txt) if summary_txt else None

    # Run meta
    run_meta = _read_json_if_exists(meta_json) if meta_json else None
    run_meta_card = _render_run_meta(run_meta) if run_meta else ""

    # Stats
    def fmt(x: float) -> str:
        return "N/A" if not math.isfinite(x) else f"{x:.6g}"

    mean = statistics.fmean(y) if y else math.nan
    med = statistics.median(y) if y else math.nan
    mn = min(y) if y else math.nan
    mx = max(y) if y else math.nan

    # Links
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    ts_rel = os.path.relpath(os.path.abspath(ts_csv), out_dir)
    dist_rel = os.path.relpath(os.path.abspath(dist_csv), out_dir) if dist_csv else ""
    meta_rel = os.path.relpath(os.path.abspath(meta_json), out_dir) if meta_json else ""
    summ_rel = os.path.relpath(os.path.abspath(summary_txt), out_dir) if summary_txt else ""

    links = [f"<code>{_e(ts_rel)}</code>"]
    if dist_rel:
        links.append(f"<code>{_e(dist_rel)}</code>")
    if summ_rel:
        links.append(f"<code>{_e(summ_rel)}</code>")
    if meta_rel:
        links.append(f"<code>{_e(meta_rel)}</code>")

    table_html = _build_table(headers, rows, max_rows=int(args.max_table_rows))

    css = BASE_CSS + r"""

main { max-width: 1280px; }

/* Key/value table */
table.kv { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 13px; }
table.kv th, table.kv td { border-bottom: 1px solid var(--grid); padding: 8px; text-align: left; vertical-align: top; }
table.kv th { width: 220px; color: #cfe0ff; font-weight: 600; }

/* Charts */
.ts, .dist { width: 100%; height: auto; overflow: visible; margin-top: 8px; }
.frame { fill: rgba(15, 23, 37, 0.20); stroke: #2b3d58; stroke-width: 1; }
.grid { stroke: rgba(255,255,255,0.08); stroke-width: 1; }
.line { fill: none; stroke: var(--accent); stroke-width: 2; opacity: 0.95; }
.bar { fill: var(--ok); opacity: 0.9; }
.ts-title { fill: #dce7ff; font-size: 13px; font-weight: 600; }
.axis-label { fill: var(--muted); font-size: 12px; }
.warn { fill: var(--warn); font-size: 12px; }

.kvgrid { display: grid; grid-template-columns: 240px 1fr; gap: 6px 12px; font-size: 13px; }
.kvgrid div:nth-child(odd) { color: var(--muted); }
"""

    now = utc_now_iso()

    dist_card = ""
    if dist_html:
        dist_note = (
            '<div class="note">This file is produced only for MI mode when the distribution was computed. '
            'The distribution is shown as a probability mass function over phase bins.</div>'
        )
        dist_stats_line = f"<div class=\"note\"><b>Stats:</b> <code>{_e(dist_stats)}</code></div>" if dist_stats else ""
        dist_card = (
            '<div class="card">'
            '<h2>Phase distribution</h2>'
            + dist_note
            + dist_stats_line
            + dist_html
            + "</div>"
        )

    summary_card = ""
    if summary_text:
        summary_card = (
            '<div class="card">'
            '<h2>pac_summary.txt</h2>'
            '<div class="note">Exact text output from the CLI (if present).</div>'
            f'<pre>{_e(summary_text)}</pre>'
            "</div>"
        )

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
  <div class=\"meta\">Generated {now} — sources: {' · '.join(links)}</div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This report visualizes <code>pac_timeseries.csv</code> written by <code>qeeg_pac_cli</code>.
      It is for research/educational inspection only and is not a medical device.
      Click any table column header to sort.
    </div>
  </div>

  {run_meta_card}

  <div class=\"card\">
    <h2>Quick stats</h2>
    <div class=\"kvgrid\">
      <div>Frames (finite)</div><div>{len(y)}</div>
      <div>Duration (s)</div><div>{_e(fmt(duration))}</div>
      <div>Mean</div><div>{_e(fmt(mean))}</div>
      <div>Median</div><div>{_e(fmt(med))}</div>
      <div>Min / Max</div><div>{_e(fmt(mn))} / {_e(fmt(mx))}</div>
      <div>Chart points</div><div>{len(t_plot)} (max {int(args.max_chart_points)})</div>
      <div>Table rows</div><div>{min(len(rows), int(args.max_table_rows))} (downsampled)</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>Timeseries</h2>
    {chart_html or '<div class="note">No finite samples to plot.</div>'}
    <div class=\"note\">The chart is downsampled to keep the HTML lightweight.</div>
  </div>

  {dist_card}

  <div class=\"card\">
    <h2>Sampled rows (sortable)</h2>
    <div class=\"note\">Downsampled view of the CSV to keep the report lightweight.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'pac_timeseries_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{table_html or '<div class="note">No columns detected.</div>'}</div>
    </div>
  </div>

  {summary_card}

  <div class=\"footer\">
    Tip: open this file in a browser. It is self-contained (no network requests).
  </div>
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
