#!/usr/bin/env python3
"""Render qeeg_epoch_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - events.csv (optional; event_id,onset_sec,duration_sec,text)
  - events_table.csv (optional; qeeg events table: onset_sec,duration_sec,text)
  - events_table.tsv (optional; BIDS-style events table: onset,duration,trial_type)
  - epoch_bandpowers.csv (optional; long table: event × channel × band)
  - epoch_bandpowers_summary.csv (optional; channel × band mean_power)
  - epoch_bandpowers_norm.csv (optional; baseline-normalized long table)
  - epoch_bandpowers_norm_summary.csv (optional; baseline-normalized channel × band summary)
  - epoch_run_meta.json (optional; build/run metadata)

Typical usage:

  # Pass an output directory produced by qeeg_epoch_cli
  python3 scripts/render_epoch_report.py --input out_epochs

  # Or pass a specific CSV/TSV inside that folder
  python3 scripts/render_epoch_report.py --input out_epochs/epoch_bandpowers.csv --out epoch_report.html

The generated HTML is self-contained (inline CSS + inline SVG charts) and safe to open locally.

Notes:
- This report is for research/educational inspection only. It is not a medical device.
- Large tables may be downsampled in the HTML for size. The original CSV/TSV files remain
  the source of truth.
"""

from __future__ import annotations

import argparse
import math
import os
import statistics
import webbrowser
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    downsample_indices as _downsample_indices,
    e as _e,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_csv_dict as _read_csv_dict,
    read_json_if_exists as _read_json_if_exists,
    try_float as _try_float,
    try_int as _try_int,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str]:
    """Return (outdir, html_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        if out is None:
            out = os.path.join(outdir, "epoch_report.html")
        return outdir, os.path.abspath(out)

    p = os.path.abspath(inp)
    outdir = os.path.dirname(p) or "."
    if out is None:
        out = os.path.join(outdir, "epoch_report.html")
    return outdir, os.path.abspath(out)


def _safe_rel(path: str, base_dir: str) -> str:
    try:
        return _posix_relpath(path, base_dir)
    except Exception:
        return os.path.relpath(path, base_dir)


def _kv_table(run_meta: Optional[Dict[str, Any]]) -> str:
    if not isinstance(run_meta, dict) or not run_meta:
        return ""
    preferred = [
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
    seen: set[str] = set()
    for k in preferred:
        if k in run_meta:
            v = run_meta.get(k)
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            seen.add(k)

    # Include a few extra scalar keys for debugging.
    extra = 0
    for k, v in run_meta.items():
        if k in seen:
            continue
        if extra >= 14:
            break
        if isinstance(v, (str, int, float, bool)):
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            extra += 1

    if not rows:
        return ""

    return (
        '<div class="card">'
        '<h2>Run metadata</h2>'
        '<div class="note">Build/run metadata written by <code>qeeg_epoch_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + "</table>"
        + "</div>"
    )


def _maybe_sample_rows(rows: List[Dict[str, str]], max_rows: int) -> Tuple[List[Dict[str, str]], bool]:
    if max_rows <= 0:
        return rows, False
    if len(rows) <= max_rows:
        return rows, False
    idx = _downsample_indices(len(rows), max_rows)
    return [rows[i] for i in idx], True


def _build_table(
    headers: Sequence[str],
    rows: Sequence[Dict[str, str]],
    *,
    code_cols: Sequence[str] = (),
    max_rows: int = 0,
) -> Tuple[str, bool]:
    sampled_rows, sampled = _maybe_sample_rows(list(rows), max_rows)

    code_set = {c.lower() for c in code_cols}
    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)

    body: List[str] = []
    for r in sampled_rows:
        tds: List[str] = []
        for h in headers:
            v = r.get(h, "")
            hl = h.lower()
            if hl in code_set:
                tds.append(f"<td><code>{_e(v)}</code></td>")
                continue

            fv = _try_float(v)
            if math.isfinite(fv):
                tds.append(f'<td data-num="{_e(f"{fv:.12g}")}">{_e(v)}</td>')
            else:
                tds.append(f"<td>{_e(v)}</td>")
        body.append("<tr>" + "".join(tds) + "</tr>")

    table_html = (
        '<table class="data-table sticky">'
        "<thead><tr>"
        + ths
        + "</tr></thead>"
        "<tbody>"
        + "".join(body)
        + "</tbody>"
        "</table>"
    )
    return table_html, sampled


def _finite(values: Iterable[float]) -> List[float]:
    out: List[float] = []
    for v in values:
        if math.isfinite(v):
            out.append(v)
    return out


def _svg_bar_chart(labels: Sequence[str], values: Sequence[float], *, title: str, units: str) -> str:
    """Horizontal bar chart centered at 0 (supports +/-)."""

    w, h = 860, 340
    pad_l, pad_r, pad_t, pad_b = 170, 20, 44, 30
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    vmax = max([abs(v) for v in values if math.isfinite(v)] + [1.0])
    vmax = vmax if vmax > 0 else 1.0

    n = max(1, len(labels))
    row_h = inner_h / n

    # Axis at x=0 in data space.
    x0 = pad_l + inner_w * 0.5

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart">')
    parts.append(f'<text x="{pad_l}" y="24" class="chart-title">{_e(title)}</text>')
    if units:
        parts.append(f'<text x="{w - pad_r}" y="24" text-anchor="end" class="chart-units">{_e(units)}</text>')

    # Vertical center line (0)
    parts.append(f'<line x1="{x0:.2f}" y1="{pad_t}" x2="{x0:.2f}" y2="{pad_t + inner_h}" class="chart-axis" />')

    # Bars + labels
    for i, (lab, val) in enumerate(zip(labels, values)):
        y = pad_t + i * row_h + row_h * 0.15
        bh = row_h * 0.7
        if not math.isfinite(val):
            parts.append(f'<text x="{pad_l - 8}" y="{y + bh * 0.72:.2f}" text-anchor="end" class="chart-label">{_e(lab)}</text>')
            parts.append(f'<text x="{x0 + 6:.2f}" y="{y + bh * 0.72:.2f}" class="chart-muted">n/a</text>')
            continue

        t = (abs(val) / vmax) if vmax > 0 else 0.0
        bw = inner_w * 0.5 * max(0.0, min(1.0, t))
        if val >= 0:
            x = x0
        else:
            x = x0 - bw
        parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bw:.2f}" height="{bh:.2f}" rx="4" class="chart-bar" />')
        parts.append(f'<text x="{pad_l - 8}" y="{y + bh * 0.72:.2f}" text-anchor="end" class="chart-label">{_e(lab)}</text>')
        anc = "start" if val >= 0 else "end"
        x_text = x0 + (bw + 6 if val >= 0 else -(bw + 6))
        parts.append(f'<text x="{x_text:.2f}" y="{y + bh * 0.72:.2f}" text-anchor="{anc}" class="chart-value">{_e(f"{val:.4g}")}</text>')

    parts.append("</svg>")
    return "".join(parts)


def _summary_charts(summary_rows: Sequence[Dict[str, str]], *, value_key: str, title_prefix: str, units: str) -> str:
    # Group by band.
    by_band: Dict[str, List[Tuple[str, float]]] = {}
    for r in summary_rows:
        band = (r.get("band") or r.get("Band") or "").strip()
        ch = (r.get("channel") or r.get("Channel") or "").strip()
        if not band or not ch:
            continue
        v = _try_float(r.get(value_key, ""))
        by_band.setdefault(band, []).append((ch, v))

    if not by_band:
        return ""

    # Stable band ordering: common EEG order first.
    band_order = ["delta", "theta", "alpha", "beta", "gamma"]
    bands = sorted(by_band.keys(), key=lambda b: (band_order.index(b.lower()) if b.lower() in band_order else 999, b.lower()))

    parts: List[str] = []
    parts.append('<div class="card">')
    parts.append(f"<h2>{_e(title_prefix)} summary charts</h2>")
    parts.append('<div class="note">Top channels per band (from the summary CSV).</div>')
    parts.append('<div class="grid2">')

    for band in bands:
        items = by_band[band]
        # Sort descending, then take top 10.
        items_sorted = sorted(items, key=lambda x: (-(x[1]) if math.isfinite(x[1]) else math.inf, x[0]))
        top = items_sorted[:10]
        labels = [ch for ch, _v in top]
        values = [_v for _ch, _v in top]
        parts.append('<div class="card tight">')
        parts.append(f"<h3>{_e(band)}</h3>")
        parts.append(_svg_bar_chart(labels, values, title=f"{title_prefix}: {band}", units=units))
        parts.append("</div>")

    parts.append("</div></div>")
    return "".join(parts)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_epoch_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to an outdir produced by qeeg_epoch_cli, or a file within it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/epoch_report.html).")
    ap.add_argument("--title", default="Epoch bandpower report", help="Report title.")
    ap.add_argument(
        "--max-long-rows",
        type=int,
        default=50000,
        help="Max rows to embed for long tables (epoch_bandpowers*.csv). If exceeded, the HTML table is downsampled.",
    )
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, html_path = _guess_paths(args.input, args.out)

    # Optional files.
    run_meta = _read_json_if_exists(os.path.join(outdir, "epoch_run_meta.json"))

    events_csv = os.path.join(outdir, "events.csv")
    events_table_csv = os.path.join(outdir, "events_table.csv")
    events_table_tsv = os.path.join(outdir, "events_table.tsv")

    ep_long_csv = os.path.join(outdir, "epoch_bandpowers.csv")
    ep_sum_csv = os.path.join(outdir, "epoch_bandpowers_summary.csv")
    ep_norm_long_csv = os.path.join(outdir, "epoch_bandpowers_norm.csv")
    ep_norm_sum_csv = os.path.join(outdir, "epoch_bandpowers_norm_summary.csv")

    # Load tables if present.
    ev_headers: List[str] = []
    ev_rows: List[Dict[str, str]] = []
    if os.path.exists(events_csv):
        ev_headers, ev_rows = _read_csv_dict(events_csv)

    ev2_headers: List[str] = []
    ev2_rows: List[Dict[str, str]] = []
    if os.path.exists(events_table_csv):
        ev2_headers, ev2_rows = _read_csv_dict(events_table_csv)
    elif os.path.exists(events_table_tsv):
        ev2_headers, ev2_rows = _read_csv_dict(events_table_tsv)

    sum_headers: List[str] = []
    sum_rows: List[Dict[str, str]] = []
    if os.path.exists(ep_sum_csv):
        sum_headers, sum_rows = _read_csv_dict(ep_sum_csv)

    long_headers: List[str] = []
    long_rows: List[Dict[str, str]] = []
    if os.path.exists(ep_long_csv):
        long_headers, long_rows = _read_csv_dict(ep_long_csv)

    norm_sum_headers: List[str] = []
    norm_sum_rows: List[Dict[str, str]] = []
    if os.path.exists(ep_norm_sum_csv):
        norm_sum_headers, norm_sum_rows = _read_csv_dict(ep_norm_sum_csv)

    norm_long_headers: List[str] = []
    norm_long_rows: List[Dict[str, str]] = []
    if os.path.exists(ep_norm_long_csv):
        norm_long_headers, norm_long_rows = _read_csv_dict(ep_norm_long_csv)

    # Basic KPIs
    n_events = len(ev_rows) if ev_rows else (len(ev2_rows) if ev2_rows else 0)
    n_long = len(long_rows)
    n_long_norm = len(norm_long_rows)
    bands: set[str] = set()
    chans: set[str] = set()

    for r in sum_rows:
        b = (r.get("band") or "").strip()
        c = (r.get("channel") or "").strip()
        if b:
            bands.add(b)
        if c:
            chans.add(c)
    if not chans or not bands:
        # Fall back to long table.
        for r in long_rows[:5000]:
            b = (r.get("band") or "").strip()
            c = (r.get("channel") or "").strip()
            if b:
                bands.add(b)
            if c:
                chans.add(c)

    band_list = sorted(bands)
    chan_list = sorted(chans)

    now = utc_now_iso()
    src_abs = os.path.abspath(args.input)
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src_rel = _safe_rel(src_abs, out_dir)

    # Charts
    charts_html = ""
    if sum_rows:
        charts_html += _summary_charts(sum_rows, value_key="mean_power", title_prefix="Mean epoch power", units="mean_power")
    if norm_sum_rows:
        # Mode might be in the table; use as units/label.
        modes = {str(r.get("mode") or "").strip() for r in norm_sum_rows}
        mode_label = ""
        modes2 = [m for m in modes if m]
        if modes2:
            mode_label = modes2[0]
        units = f"mean_value ({mode_label})" if mode_label else "mean_value"
        charts_html += _summary_charts(norm_sum_rows, value_key="mean_value", title_prefix="Baseline-normalized", units=units)

    # Tables
    tables_html: List[str] = []

    if ev_rows and ev_headers:
        ev_table, _ = _build_table(ev_headers, ev_rows, code_cols=["event_id"], max_rows=0)
        tables_html.append(
            '<div class="card">'
            "<h2>Events</h2>"
            '<div class="note">Event list exported by <code>qeeg_epoch_cli</code>.</div>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter events…" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'events_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{ev_table}</div>'
            "</div>"
            "</div>"
        )
    elif ev2_rows and ev2_headers:
        ev2_table, _ = _build_table(ev2_headers, ev2_rows, max_rows=0)
        tables_html.append(
            '<div class="card">'
            "<h2>Events table</h2>"
            '<div class="note">Events table (CSV or TSV). If you have <code>events.csv</code>, that will be preferred.</div>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter events…" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'events_table_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{ev2_table}</div>'
            "</div>"
            "</div>"
        )

    if sum_rows and sum_headers:
        sum_table, _ = _build_table(sum_headers, sum_rows, code_cols=["channel", "band"], max_rows=0)
        tables_html.append(
            '<div class="card">'
            "<h2>Epoch bandpower summary</h2>"
            '<div class="note">Mean power per channel/band across processed epochs.</div>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'epoch_bandpowers_summary_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{sum_table}</div>'
            "</div>"
            "</div>"
        )

    if norm_sum_rows and norm_sum_headers:
        nsum_table, _ = _build_table(norm_sum_headers, norm_sum_rows, code_cols=["channel", "band", "mode"], max_rows=0)
        tables_html.append(
            '<div class="card">'
            "<h2>Baseline-normalized summary</h2>"
            '<div class="note">Mean baseline-normalized value per channel/band across processed epochs (when baseline normalization is enabled).</div>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'epoch_bandpowers_norm_summary_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{nsum_table}</div>'
            "</div>"
            "</div>"
        )

    # Long tables (potentially huge)
    if long_rows and long_headers:
        long_table, sampled = _build_table(
            long_headers,
            long_rows,
            code_cols=["event_id", "channel", "band"],
            max_rows=int(args.max_long_rows),
        )
        note = ""
        if sampled:
            note = (
                f'<div class="note warn">This table has {len(long_rows):,} rows and was downsampled to '
                f'{min(len(long_rows), int(args.max_long_rows)):,} rows for HTML size. Open <code>{_e(_safe_rel(ep_long_csv, out_dir))}</code> for the full dataset.</div>'
            )
        tables_html.append(
            '<div class="card">'
            "<h2>Epoch bandpowers (long table)</h2>"
            '<div class="note">One row per <code>event × channel × band</code>.</div>'
            + note
            + '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter rows (e.g., band=alpha, channel:Cz, event_id=3) …" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'epoch_bandpowers_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{long_table}</div>'
            "</div>"
            "</div>"
        )

    if norm_long_rows and norm_long_headers:
        nlong_table, sampled = _build_table(
            norm_long_headers,
            norm_long_rows,
            code_cols=["event_id", "channel", "band", "mode"],
            max_rows=int(args.max_long_rows),
        )
        note = ""
        if sampled:
            note = (
                f'<div class="note warn">This table has {len(norm_long_rows):,} rows and was downsampled to '
                f'{min(len(norm_long_rows), int(args.max_long_rows)):,} rows for HTML size. Open <code>{_e(_safe_rel(ep_norm_long_csv, out_dir))}</code> for the full dataset.</div>'
            )
        tables_html.append(
            '<div class="card">'
            "<h2>Baseline-normalized epoch bandpowers (long table)</h2>"
            '<div class="note">One row per <code>event × channel × band</code>, including baseline values and normalized values.</div>'
            + note
            + '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
            '<button type="button" onclick="downloadTableCSV(this, \'epoch_bandpowers_norm_filtered.csv\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            "</div>"
            f'<div class="table-wrap">{nlong_table}</div>'
            "</div>"
            "</div>"
        )

    if not tables_html:
        tables_html.append(
            '<div class="card">'
            "<h2>No tables found</h2>"
            '<div class="note">This folder does not appear to contain epoch outputs (no <code>events.csv</code> / <code>epoch_bandpowers*.csv</code> files were found).</div>'
            "</div>"
        )

    # KPI chip row
    kpi_bits: List[str] = []
    if n_events:
        kpi_bits.append(f'<span class="kpi"><span class="k">events</span><span class="v">{n_events:,}</span></span>')
    if chan_list:
        kpi_bits.append(f'<span class="kpi"><span class="k">channels</span><span class="v">{len(chan_list):,}</span></span>')
    if band_list:
        kpi_bits.append(f'<span class="kpi"><span class="k">bands</span><span class="v">{len(band_list):,}</span></span>')
    if n_long:
        kpi_bits.append(f'<span class="kpi"><span class="k">epoch rows</span><span class="v">{n_long:,}</span></span>')
    if n_long_norm:
        kpi_bits.append(f'<span class="kpi"><span class="k">norm rows</span><span class="v">{n_long_norm:,}</span></span>')

    kpis_html = ""
    if kpi_bits:
        kpis_html = '<div class="kpis">' + "".join(kpi_bits) + "</div>"

    extra_css = r"""
/* epoch report additions */
.kpis { display:flex; gap:10px; flex-wrap:wrap; margin: 10px 0 0; }
.kpi { display:inline-flex; gap:8px; align-items:baseline; padding: 6px 10px; border-radius: 999px;
  background: rgba(143,183,255,0.08); border: 1px solid rgba(143,183,255,0.25); }
.kpi .k { color: var(--muted); font-size: 12px; }
.kpi .v { color: var(--text); font-weight: 600; font-size: 13px; }
.warn { color: #ffb86b; }
.grid2 { display:grid; grid-template-columns: repeat(auto-fit, minmax(360px, 1fr)); gap: 12px; }
.card.tight { padding: 12px; }
.chart { width: 100%; height: auto; }
.chart-title { fill: var(--text); font-size: 14px; font-weight: 600; }
.chart-units { fill: var(--muted); font-size: 12px; }
.chart-axis { stroke: rgba(155,176,208,0.45); stroke-width: 1; }
.chart-bar { fill: rgba(143,183,255,0.80); }
.chart-label { fill: var(--text); font-size: 12px; }
.chart-value { fill: var(--muted); font-size: 12px; }
.chart-muted { fill: var(--muted); font-size: 12px; }
"""

    html_doc = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{_e(args.title)}</title>
<style>{BASE_CSS}{extra_css}</style>
</head>
<body>
<header class="hero">
  <div class="wrap">
    <h1>{_e(args.title)}</h1>
    <div class="meta">Generated {now} — source <code>{_e(src_rel)}</code></div>
    {kpis_html}
  </div>
</header>
<main class="wrap">
  <div class="card">
    <h2>About</h2>
    <div class="note">This report visualizes <code>qeeg_epoch_cli</code> outputs for research/educational inspection only. It is not a medical device.</div>
    <div class="note">Primary outputs are <code>epoch_bandpowers.csv</code> / <code>epoch_bandpowers_summary.csv</code>, plus optional baseline-normalized variants.</div>
    <div class="note">Tip: tables support sortable headers, structured filtering (e.g. <code>band=alpha</code>, <code>channel:Cz</code>, <code>power&gt;1.0</code>), and <b>Download CSV</b> for the visible rows.</div>
  </div>

  {_kv_table(run_meta)}

  {charts_html}

  {''.join(tables_html)}

  <div class="card">
    <h2>Notes</h2>
    <div class="note">Open this file in a browser. It is self-contained (no network requests).</div>
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
            webbrowser.open("file://" + html_path)
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
