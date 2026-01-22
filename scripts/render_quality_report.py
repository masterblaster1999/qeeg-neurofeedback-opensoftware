#!/usr/bin/env python3
"""Render qeeg_quality_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (e.g., CI artifacts, lab workstations).

Inputs:
  - line_noise_per_channel.csv (required)
  - quality_report.json (optional; richer summary + parameters)
  - quality_summary.txt (optional; embedded)
  - quality_run_meta.json (optional; build/run metadata)

Typical usage:

  # Pass an output directory produced by qeeg_quality_cli
  python3 scripts/render_quality_report.py --input out_quality

  # Or pass the per-channel CSV directly
  python3 scripts/render_quality_report.py --input out_quality/line_noise_per_channel.csv --out report.html

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
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    read_text_if_exists as _read_text_if_exists,
    try_float as _try_float,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str, Optional[str], Optional[str], Optional[str]]:
    """Return (outdir, csv_path, html_path, report_json_path, summary_txt_path, run_meta_json_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        csv_path = os.path.join(outdir, "line_noise_per_channel.csv")
    else:
        p = os.path.abspath(inp)
        outdir = os.path.dirname(p) or "."
        # Allow passing any of the sidecar files; default to the CSV in that folder.
        if os.path.basename(p).lower().endswith(".csv"):
            csv_path = p
        else:
            csv_path = os.path.join(outdir, "line_noise_per_channel.csv")

    if out is None:
        out = os.path.join(outdir, "quality_report.html")

    report_json = os.path.join(outdir, "quality_report.json")
    summary_txt = os.path.join(outdir, "quality_summary.txt")
    run_meta = os.path.join(outdir, "quality_run_meta.json")

    return (
        outdir,
        os.path.abspath(csv_path),
        os.path.abspath(out),
        report_json if os.path.exists(report_json) else None,
        summary_txt if os.path.exists(summary_txt) else None,
        run_meta if os.path.exists(run_meta) else None,
    )


def _finite(values: Iterable[float]) -> List[float]:
    out: List[float] = []
    for v in values:
        if math.isfinite(v):
            out.append(v)
    return out


def _median(values: Iterable[float]) -> float:
    vals = _finite(values)
    if not vals:
        return math.nan
    try:
        return float(statistics.median(vals))
    except Exception:
        vals.sort()
        n = len(vals)
        if n == 0:
            return math.nan
        if n % 2 == 1:
            return float(vals[n // 2])
        return float(0.5 * (vals[n // 2 - 1] + vals[n // 2]))


def _svg_two_bars(a_label: str, a_val: float, b_label: str, b_val: float, *, title: str, highlight: str = "") -> str:
    """Simple 2-bar comparison chart (vertical bars)."""
    w, h = 520, 240
    pad_l, pad_r, pad_t, pad_b = 60, 30, 36, 42
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    vmax = max(_finite([a_val, b_val]) + [1.0])
    if vmax <= 0:
        vmax = 1.0

    def bar_x(i: int) -> float:
        # Two bars centered
        slot = inner_w / 2.0
        return pad_l + i * slot + slot * 0.25

    bar_w = inner_w / 2.0 * 0.5

    def bar_h(v: float) -> float:
        if not math.isfinite(v):
            return 0.0
        return (v / vmax) * inner_h

    bars = [(a_label, a_val), (b_label, b_val)]

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')

    # axis
    y0 = pad_t + inner_h
    parts.append(f'<line x1="{pad_l}" y1="{y0}" x2="{pad_l + inner_w}" y2="{y0}" class="axis"/>')

    for i, (lab, v) in enumerate(bars):
        bh = bar_h(v)
        x = bar_x(i)
        y = y0 - bh
        cls = "bar" + (" hl" if highlight and lab == highlight else "")
        parts.append(f'<rect x="{x}" y="{y}" width="{bar_w}" height="{bh}" class="{cls}"/>')
        parts.append(f'<text x="{x + bar_w/2.0}" y="{y0 + 18}" text-anchor="middle" class="label">{_e(lab)}</text>')
        if math.isfinite(v):
            parts.append(f'<text x="{x + bar_w/2.0}" y="{y - 6}" text-anchor="middle" class="value">{_e(f"{v:.3g}")}</text>')
        else:
            parts.append(f'<text x="{x + bar_w/2.0}" y="{y - 6}" text-anchor="middle" class="nan">NaN</text>')

    parts.append('</svg>')
    return ''.join(parts)


def _svg_hbar(labels: Sequence[str], values: Sequence[float], *, title: str) -> str:
    """Simple horizontal bar chart SVG (top-N channels)."""
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

    parts.append('</svg>')
    return ''.join(parts)


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
        '<div class="note">Build/run metadata written by <code>qeeg_quality_cli</code> (if available).</div>'
        '<table class="kv">'
        + ''.join(rows)
        + '</table>'
        '</div>'
    )


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]]) -> str:
    ths = ''.join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)

    body: List[str] = []
    for r in rows:
        tds: List[str] = []
        for h in headers:
            v = r.get(h, "")
            if h.lower() != "channel":
                fv = _try_float(v)
                if math.isfinite(fv):
                    tds.append(f'<td data-num="{_e(f"{fv:.12g}")}">{_e(v)}</td>')
                else:
                    tds.append(f'<td>{_e(v)}</td>')
            else:
                tds.append(f'<td><code>{_e(v)}</code></td>')
        body.append('<tr>' + ''.join(tds) + '</tr>')

    return (
        '<table class="data-table sticky">'
        '<thead><tr>' + ths + '</tr></thead>'
        '<tbody>' + ''.join(body) + '</tbody>'
        '</table>'
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_quality_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to an outdir containing line_noise_per_channel.csv, or that CSV file itself.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/quality_report.html)")
    ap.add_argument("--top-n", type=int, default=12, help="Top-N channels to show in charts (default: 12)")
    ap.add_argument("--title", default="Quality report (line noise)", help="Report title")
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, csv_path, html_path, report_json_path, summary_txt_path, run_meta_path = _guess_paths(args.input, args.out)

    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find line_noise_per_channel.csv at: {csv_path}")

    headers, rows = _read_csv(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in: {csv_path}")

    report_json = _read_json_if_exists(report_json_path) if report_json_path else None
    summary_txt = _read_text_if_exists(summary_txt_path) if summary_txt_path else None
    run_meta = _read_json_if_exists(run_meta_path) if run_meta_path else None

    # Extract summary info, preferring the JSON report schema written by qeeg_quality_cli.
    fs_hz = _try_float(report_json.get("fs_hz")) if isinstance(report_json, dict) else math.nan
    n_channels = int(report_json.get("n_channels")) if isinstance(report_json, dict) and isinstance(report_json.get("n_channels"), int) else len(rows)
    duration_sec = _try_float(report_json.get("duration_sec")) if isinstance(report_json, dict) else math.nan

    params_min_ratio = math.nan
    if isinstance(report_json, dict):
        params = report_json.get("params")
        if isinstance(params, dict):
            params_min_ratio = _try_float(params.get("min_ratio"))

    ln = report_json.get("line_noise") if isinstance(report_json, dict) else None
    med50 = _try_float(ln.get("median_ratio_50")) if isinstance(ln, dict) else math.nan
    med60 = _try_float(ln.get("median_ratio_60")) if isinstance(ln, dict) else math.nan
    rec_hz = _try_float(ln.get("recommended_notch_hz")) if isinstance(ln, dict) else math.nan
    strength_ratio = _try_float(ln.get("strength_ratio")) if isinstance(ln, dict) else math.nan
    channels_used = int(ln.get("channels_used")) if isinstance(ln, dict) and isinstance(ln.get("channels_used"), int) else 0

    # Fallback summaries from CSV if JSON is missing.
    if not math.isfinite(med50):
        med50 = _median(_try_float(r.get("ratio_50", "")) for r in rows)
    if not math.isfinite(med60):
        med60 = _median(_try_float(r.get("ratio_60", "")) for r in rows)

    if not math.isfinite(rec_hz) or rec_hz <= 0:
        # If min_ratio is known (JSON), respect it; else just choose the stronger of the two medians.
        best = max((med50 if math.isfinite(med50) else 0.0), (med60 if math.isfinite(med60) else 0.0))
        min_ratio = params_min_ratio if math.isfinite(params_min_ratio) else 0.0
        if best >= min_ratio and best > 0.0:
            rec_hz = 60.0 if (med60 if math.isfinite(med60) else 0.0) > (med50 if math.isfinite(med50) else 0.0) else 50.0
        else:
            rec_hz = 0.0

    if not math.isfinite(strength_ratio):
        if rec_hz >= 59.0:
            strength_ratio = med60
        elif rec_hz >= 49.0:
            strength_ratio = med50
        else:
            strength_ratio = max(med50 if math.isfinite(med50) else 0.0, med60 if math.isfinite(med60) else 0.0)

    # Charts: top-N channels by ratio_50 and ratio_60.
    top_n = max(1, int(args.top_n))

    def top_by(col: str) -> Tuple[List[str], List[float]]:
        vals: List[Tuple[str, float]] = []
        for r in rows:
            ch = r.get("channel", "") or "?"
            v = _try_float(r.get(col, ""))
            if not math.isfinite(v):
                continue
            vals.append((ch, v))
        vals.sort(key=lambda kv: kv[1], reverse=True)
        vals = vals[: min(top_n, len(vals))]
        return [k for k, _ in vals], [v for _, v in vals]

    top50_labels, top50_vals = top_by("ratio_50")
    top60_labels, top60_vals = top_by("ratio_60")

    charts_html: List[str] = []
    if top50_labels:
        charts_html.append(_svg_hbar(top50_labels, top50_vals, title=f"Top {len(top50_labels)} channels by 50 Hz peak/baseline ratio"))
    if top60_labels:
        charts_html.append(_svg_hbar(top60_labels, top60_vals, title=f"Top {len(top60_labels)} channels by 60 Hz peak/baseline ratio"))

    # Table
    # Keep a stable header order if the CSV contains the expected columns.
    preferred = [
        "channel",
        "ratio_50",
        "peak_mean_50",
        "baseline_mean_50",
        "ratio_60",
        "peak_mean_60",
        "baseline_mean_60",
    ]
    if all(h in headers for h in preferred):
        headers = preferred

    table_html = _build_table(headers, rows)

    # Summary values for display
    rec_label = "none" if not (rec_hz > 0) else ("60 Hz" if rec_hz >= 59.0 else "50 Hz")
    rec_badge_cls = "badge-none"
    if rec_hz >= 59.0:
        rec_badge_cls = "badge-60"
    elif rec_hz >= 49.0:
        rec_badge_cls = "badge-50"

    now = utc_now_iso()
    src = os.path.abspath(args.input)

    def fmt(x: float, *, digits: int = 4) -> str:
        if not math.isfinite(x):
            return ""
        return f"{x:.{digits}g}"

    summary_grid = (
        '<div class="grid2">'
        '<div class="card">'
        '<h2>Recommendation</h2>'
        '<div class="note">Based on median peak/baseline ratios around 50 Hz and 60 Hz.</div>'
        f'<div class="rec"><span class="badge {rec_badge_cls}">{_e(rec_label)}</span>'
        f'<span class="muted">strength_ratio={_e(fmt(strength_ratio, digits=4) or "n/a")}</span></div>'
        '<div class="mini">'
        f'<div><span class="muted">median_ratio_50</span><br><code>{_e(fmt(med50, digits=4) or "n/a")}</code></div>'
        f'<div><span class="muted">median_ratio_60</span><br><code>{_e(fmt(med60, digits=4) or "n/a")}</code></div>'
        f'<div><span class="muted">channels_used</span><br><code>{_e(str(channels_used) if channels_used else str(n_channels))}</code></div>'
        '</div>'
        '</div>'
        '<div class="card">'
        '<h2>Median ratio comparison</h2>'
        '<div class="note">A higher ratio suggests stronger narrowband line-noise relative to local baseline.</div>'
        + _svg_two_bars("50 Hz", med50, "60 Hz", med60, title="Median peak/baseline ratio", highlight=rec_label)
        + '</div>'
        '</div>'
    )

    run_meta_card = _render_run_meta(run_meta)

    summary_block = ""
    if summary_txt:
        summary_block = (
            '<div class="card">'
            '<h2>quality_summary.txt</h2>'
            '<div class="note">Human-readable summary written by <code>qeeg_quality_cli</code>.</div>'
            f'<pre>{_e(summary_txt)}</pre>'
            '</div>'
        )

    css = BASE_CSS + r"""
.kv { width: 100%; border-collapse: collapse; font-size: 13px; }
.kv th, .kv td { border-bottom: 1px solid rgba(255,255,255,0.08); padding: 8px; text-align: left; vertical-align: top; }
.kv th { width: 220px; color: #d7e4ff; background: #0f1725; }

.grid2 { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
@media (max-width: 900px) { .grid2 { grid-template-columns: 1fr; } }

.rec { display:flex; gap:12px; align-items:center; margin-top:10px; }
.badge { display:inline-block; padding: 6px 10px; border-radius: 999px; font-size: 13px; border: 1px solid rgba(255,255,255,0.14); }
.badge-50 { color: var(--warn); border-color: rgba(255,184,107,0.55); background: rgba(255,184,107,0.08); }
.badge-60 { color: var(--bad); border-color: rgba(255,127,163,0.55); background: rgba(255,127,163,0.08); }
.badge-none { color: var(--muted); border-color: rgba(255,255,255,0.10); background: rgba(255,255,255,0.04); }

.mini { display:grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; margin-top: 12px; }

.chart { width: 100%; height: auto; margin-top: 8px; }
.chart-title { font-size: 13px; fill: #d7e4ff; font-family: ui-sans-serif, system-ui; }
.axis { stroke: rgba(255,255,255,0.20); stroke-width: 1; }
.bar { fill: rgba(143,183,255,0.70); }
.bar.hl { fill: rgba(255,184,107,0.80); }
.label { fill: rgba(255,255,255,0.72); font-size: 12px; font-family: ui-sans-serif, system-ui; }
.value { fill: rgba(255,255,255,0.85); font-size: 12px; font-family: ui-sans-serif, system-ui; }
.nan { fill: rgba(255,255,255,0.45); font-size: 12px; }
"""

    js = JS_SORT_TABLE

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
  <div class=\"meta\">Generated {now} — source <code>{_e(src)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This report visualizes the line-noise checks written by <code>qeeg_quality_cli</code>.
      It helps you choose an appropriate <code>--notch</code> frequency (50 or 60 Hz) for subsequent analyses.
      Click any column header to sort the table.
    </div>
  </div>

  {summary_grid}

  {run_meta_card}

  {summary_block}

  <div class=\"card\">
    <h2>Per-channel line noise table</h2>
    <div class=\"note\">Filter rows, sort columns, and export the visible subset to CSV.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'line_noise_per_channel_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{table_html}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>Top channels</h2>
    <div class=\"note\">These charts can help localize which channels carry the strongest line-noise peak.</div>
    {''.join(charts_html) if charts_html else '<div class="muted">No numeric ratios found.</div>'}
  </div>

  <div class=\"footer\">Tip: open this file in a browser. It is self-contained (no network requests).</div>
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
