#!/usr/bin/env python3
"""Render qeeg_spectral_features_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (e.g., CI artifacts, lab workstations).

Inputs:
  - spectral_features.csv (required)
  - spectral_features.json (optional; adds per-column descriptions/units)
  - spectral_features_run_meta.json (optional; adds build/run metadata)

Typical usage:

  # Pass an output directory produced by qeeg_spectral_features_cli
  python3 scripts/render_spectral_features_report.py --input out_sf

  # Or pass spectral_features.csv directly
  python3 scripts/render_spectral_features_report.py --input out_sf/spectral_features.csv --out report.html

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
import webbrowser
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    try_float as _try_float,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, Optional[str], Optional[str]]:
    """Return (csv_path, html_path, sidecar_json_path_or_None, run_meta_json_path_or_None)."""
    csv_path = inp
    sidecar: Optional[str]
    run_meta: Optional[str]

    if _is_dir(inp):
        out_dir = os.path.abspath(inp)
        csv_path = os.path.join(out_dir, "spectral_features.csv")
        sidecar = os.path.join(out_dir, "spectral_features.json")
        run_meta = os.path.join(out_dir, "spectral_features_run_meta.json")
        if out is None:
            out = os.path.join(out_dir, "spectral_features_report.html")
    else:
        out_dir = os.path.dirname(os.path.abspath(inp)) or "."
        sidecar = os.path.join(out_dir, "spectral_features.json")
        run_meta = os.path.join(out_dir, "spectral_features_run_meta.json")
        if out is None:
            out = os.path.join(out_dir, "spectral_features_report.html")

    if not os.path.exists(sidecar):
        sidecar = None
    if not os.path.exists(run_meta):
        run_meta = None

    return csv_path, out or "spectral_features_report.html", sidecar, run_meta


def _detect_channel_column(headers: Sequence[str]) -> str:
    for cand in ("channel", "Channel", "ch", "Ch", "label", "Label", "name", "Name"):
        if cand in headers:
            return cand
    return headers[0] if headers else "channel"


def _numeric_columns(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, channel_col: str) -> List[str]:
    cols: List[str] = []
    for h in headers:
        if h == channel_col:
            continue
        any_num = False
        for r in rows[: min(50, len(rows))]:
            v = _try_float(r.get(h, ""))
            if math.isfinite(v):
                any_num = True
                break
        if any_num:
            cols.append(h)
    return cols


def _column_meta(sidecar: Optional[Dict[str, Any]], key: str) -> Tuple[str, str]:
    if not sidecar:
        return "", ""
    info = sidecar.get(key)
    if not isinstance(info, dict):
        return "", ""
    desc = info.get("Description", "") if isinstance(info.get("Description"), str) else ""
    units = info.get("Units", "") if isinstance(info.get("Units"), str) else ""
    return desc, units


def _svg_bar_chart(labels: Sequence[str], values: Sequence[float], *, title: str, units: str) -> str:
    """Simple horizontal bar chart SVG.

    Scales to the min/max of the data with 0 included so negative values are
    displayed sensibly (even though most spectral features are non-negative).
    """
    w, h = 820, 320
    pad_l, pad_r, pad_t, pad_b = 160, 20, 36, 30
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    finite_vals = [v for v in values if math.isfinite(v)]
    vmin = min(finite_vals + [0.0]) if finite_vals else 0.0
    vmax = max(finite_vals + [0.0]) if finite_vals else 1.0
    if vmin == vmax:
        vmin -= 1.0
        vmax += 1.0

    def x_of(v: float) -> float:
        return pad_l + (v - vmin) / (vmax - vmin) * inner_w

    x0 = x_of(0.0)

    n = max(1, len(labels))
    row_h = inner_h / n

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')
    if units:
        parts.append(f'<text x="{w - pad_r}" y="22" text-anchor="end" class="chart-units">{_e(units)}</text>')

    # zero axis
    parts.append(f'<line x1="{x0}" y1="{pad_t}" x2="{x0}" y2="{pad_t + inner_h}" class="axis"/>')

    for i, (lab, v) in enumerate(zip(labels, values)):
        y = pad_t + i * row_h
        cy = y + row_h * 0.65
        parts.append(f'<text x="{pad_l - 8}" y="{cy}" text-anchor="end" class="label">{_e(lab)}</text>')
        if not math.isfinite(v):
            parts.append(f'<text x="{x0 + 4}" y="{cy}" class="nan">NaN</text>')
            continue

        xv = x_of(v)
        x_left = min(x0, xv)
        bw = abs(xv - x0)
        bar_y = y + row_h * 0.18
        bar_h = max(2.0, row_h * 0.55)
        cls = "bar-pos" if v >= 0 else "bar-neg"
        parts.append(f'<rect x="{x_left}" y="{bar_y}" width="{bw}" height="{bar_h}" class="{cls}"/>')

        parts.append(f'<text x="{pad_l + inner_w - 2}" y="{cy}" text-anchor="end" class="value">{_e(f"{v:.6g}")}</text>')

    parts.append("</svg>")
    return "".join(parts)


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, sticky: bool = True) -> str:
    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body_rows: List[str] = []
    for r in rows:
        tds = "".join(f"<td>{_e(r.get(h, ''))}</td>" for h in headers)
        body_rows.append(f"<tr>{tds}</tr>")
    cls = "data-table sticky" if sticky else "data-table"
    return f'<table class="{cls}"><thead><tr>{ths}</tr></thead><tbody>' + "".join(body_rows) + "</tbody></table>"


def _render_run_meta(run_meta: Optional[Dict[str, Any]]) -> str:
    if not run_meta:
        return ""
    # Prefer a small curated set of keys, but fall back to anything stringy/numbery.
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
            if isinstance(v, (str, int, float, bool)):
                rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(v)}</code></td></tr>")
            else:
                rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            seen.add(k)

    # Add a few additional simple keys (bounded) to help debugging.
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
        '<div class="note">Build/run metadata written by <code>qeeg_spectral_features_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + '</table>'
        '</div>'
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render spectral_features.csv to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to spectral_features.csv, or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/spectral_features_report.html).") 
    ap.add_argument(
        "--top-n",
        type=int,
        default=10,
        help="How many channels to show per-feature in the bar charts (default: 10).",
    )
    ap.add_argument("--title", default="Spectral features report", help="Report title.")
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    csv_path, html_path, sidecar_path, run_meta_path = _guess_paths(args.input, args.out)
    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find spectral_features.csv at: {csv_path}")

    headers, rows = _read_csv(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in: {csv_path}")

    channel_col = _detect_channel_column(headers)
    num_cols = _numeric_columns(headers, rows, channel_col=channel_col)

    sidecar = _read_json_if_exists(sidecar_path) if sidecar_path else None
    run_meta = _read_json_if_exists(run_meta_path) if run_meta_path else None

    charts: List[str] = []
    top_n = max(1, int(args.top_n))

    for col in num_cols:
        vals: List[Tuple[str, float]] = []
        for r in rows:
            ch = (r.get(channel_col, "") or "").strip() or "?"
            v = _try_float(r.get(col, ""))
            if math.isfinite(v):
                vals.append((ch, v))
        if not vals:
            continue

        # For most spectral features, larger is "more"; show top-N by value.
        vals.sort(key=lambda kv: kv[1], reverse=True)
        vals = vals[: min(top_n, len(vals))]
        labels = [k for k, _ in vals]
        values = [v for _, v in vals]

        desc, units = _column_meta(sidecar, col)
        subtitle = f"{col} (top {len(vals)} channels)"
        if desc:
            subtitle += f" — {desc}"
        charts.append(_svg_bar_chart(labels, values, title=subtitle, units=units))

    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src = os.path.relpath(os.path.abspath(csv_path), out_dir)
    sidecar_rel = os.path.relpath(os.path.abspath(sidecar_path), out_dir) if sidecar_path else ""
    run_meta_rel = os.path.relpath(os.path.abspath(run_meta_path), out_dir) if run_meta_path else ""

    css = BASE_CSS + r"""

/* Charts */
.chart { width: 100%; height: auto; overflow: visible; }
.axis { stroke: #2b3d58; stroke-width: 1; }
.label, .value, .nan, .chart-title, .chart-units { fill: #dce7ff; font-size: 12px; }
.chart-title { font-size: 13px; font-weight: 600; }
.chart-units { fill: var(--muted); font-size: 12px; }
.label { fill: #cfe0ff; }
.value { fill: var(--muted); font-size: 12px; }
.nan { fill: var(--warn); }
.bar-pos { fill: var(--ok); opacity: 0.9; }
.bar-neg { fill: var(--bad); opacity: 0.9; }

/* Key/value table */
table.kv { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 13px; }
table.kv th, table.kv td { border-bottom: 1px solid var(--grid); padding: 8px; text-align: left; vertical-align: top; }
table.kv th { width: 220px; color: #cfe0ff; font-weight: 600; }
"""

    js = JS_SORT_TABLE

    table_html = _build_table(headers, rows, sticky=True)
    charts_html = (
        "".join(f'<div class="card">{c}</div>' for c in charts)
        if charts
        else '<div class="note">No numeric columns found to chart.</div>'
    )

    sidecar_line = f' (with sidecar <code>{_e(sidecar_rel)}</code>)' if sidecar_rel else ""
    run_meta_line = f' (with run meta <code>{_e(run_meta_rel)}</code>)' if run_meta_rel else ""

    run_meta_card = _render_run_meta(run_meta) if run_meta else ""

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
  <div class=\"meta\">Generated {now} — source <code>{_e(src)}</code>{sidecar_line}{run_meta_line}</div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This report visualizes <code>spectral_features.csv</code> written by <code>qeeg_spectral_features_cli</code>.
      It is for research/educational inspection only and is not a medical device.
      Click any column header to sort the table.
    </div>
  </div>

  {run_meta_card}

  <div class=\"card\">
    <h2>Spectral features table</h2>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'spectral_features_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{table_html}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>Top channels per feature</h2>
    <div class=\"note\">Each chart shows the top-N channels by value for that feature column.</div>
  </div>

  {charts_html}

  <div class=\"footer\">
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
