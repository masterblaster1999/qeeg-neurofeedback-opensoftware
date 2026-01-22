#!/usr/bin/env python3
"""Render qeeg_bandpower_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (e.g., CI artifacts, lab workstations).

Inputs:
  - bandpowers.csv (required)
  - bandpowers.json (optional; adds per-column descriptions/units)
  - bandpower_run_meta.json (optional; adds build/run metadata)
  - topomap_*.bmp (optional; embedded if present)

Typical usage:

  # Pass an output directory produced by qeeg_bandpower_cli
  python3 scripts/render_bandpowers_report.py --input out_bp

  # Or pass bandpowers.csv directly
  python3 scripts/render_bandpowers_report.py --input out_bp/bandpowers.csv --out report.html

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.
"""

from __future__ import annotations

import argparse
import base64
import math
import mimetypes
import os
import re
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    try_float as _try_float,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, Optional[str], Optional[str]]:
    """Return (csv_path, html_path, sidecar_json_path_or_None, run_meta_json_path_or_None)."""
    csv_path = inp
    sidecar: Optional[str] = None
    run_meta: Optional[str] = None

    if _is_dir(inp):
        csv_path = os.path.join(inp, "bandpowers.csv")
        sidecar = os.path.join(inp, "bandpowers.json")
        run_meta = os.path.join(inp, "bandpower_run_meta.json")
        if out is None:
            out = os.path.join(inp, "bandpowers_report.html")
    else:
        # If a file is passed, default output is next to it.
        base_dir = os.path.dirname(os.path.abspath(inp)) or "."
        sidecar = os.path.join(base_dir, "bandpowers.json")
        run_meta = os.path.join(base_dir, "bandpower_run_meta.json")
        if out is None:
            out = os.path.join(base_dir, "bandpowers_report.html")

    if sidecar and not os.path.exists(sidecar):
        sidecar = None
    if run_meta and not os.path.exists(run_meta):
        run_meta = None

    return csv_path, out or "bandpowers_report.html", sidecar, run_meta



_TOPOMAP_PATTERN = re.compile(
    r"^topomap_(?P<band>.+?)(?P<z>_z)?\.(?P<ext>bmp|png|jpe?g|gif|svg)$",
    re.IGNORECASE,
)

def _guess_mime(path: str) -> str:
    # Some platforms may not have bmp registered; fall back safely.
    mt, _ = mimetypes.guess_type(path)
    if mt:
        return mt
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bmp":
        return "image/bmp"
    if ext == ".svg":
        return "image/svg+xml"
    if ext in (".jpg", ".jpeg"):
        return "image/jpeg"
    if ext == ".png":
        return "image/png"
    if ext == ".gif":
        return "image/gif"
    return "application/octet-stream"

def _read_file_b64_data_uri(path: str) -> str:
    mime = _guess_mime(path)
    with open(path, "rb") as f:
        b = f.read()
    return f"data:{mime};base64,{base64.b64encode(b).decode('ascii')}"

def _find_topomap_files(dir_path: str) -> Dict[str, Dict[str, str]]:
    """Return mapping: band -> {raw: path, z: path}.

    Files are recognized as:
      - topomap_<band>.bmp
      - topomap_<band>_z.bmp
    (also supports png/jpg/gif/svg extensions for future compatibility).
    """
    out: Dict[str, Dict[str, str]] = {}
    try:
        names = os.listdir(dir_path)
    except Exception:
        return out

    for name in names:
        m = _TOPOMAP_PATTERN.match(name)
        if not m:
            continue
        band = m.group("band")
        is_z = bool(m.group("z"))
        full = os.path.join(dir_path, name)
        if not os.path.isfile(full):
            continue
        bucket = out.setdefault(band, {})
        bucket["z" if is_z else "raw"] = full
    return out

def _render_topomap_section(
    csv_dir: str,
    html_dir: str,
    *,
    link_only: bool = False,
    max_embed_bytes: int = 12 * 1024 * 1024,
) -> str:
    """Return HTML fragment for any topomap_* images found next to bandpowers.csv."""
    bands = _find_topomap_files(csv_dir)
    if not bands:
        return ""

    parts: List[str] = []
    parts.append('<div class="card">')
    parts.append("<h2>Topomaps</h2>")
    parts.append(
        '<div class="note">If your run exported <code>topomap_*.bmp</code> images, they are shown here. '
        'Use <code>--link-topomaps</code> to keep the HTML smaller (images will be loaded from disk).</div>'
    )
    parts.append('<div class="topomap-grid">')

    # Sort bands to keep output deterministic.
    for band in sorted(bands.keys()):
        paths = bands[band]
        parts.append('<div class="topomap-item">')
        parts.append(f'<div class="topomap-band">{_e(band)}</div>')
        parts.append('<div class="topomap-row">')

        for key, label in (("raw", "raw"), ("z", "z-score")):
            p = paths.get(key)
            if not p:
                parts.append(f'<div class="topomap-missing">No {label}</div>')
                continue

            try:
                if (not link_only) and os.path.getsize(p) <= max_embed_bytes:
                    src = _read_file_b64_data_uri(p)
                else:
                    # Link (or oversize fallback): keep local file reference.
                    src = _posix_relpath(p, html_dir)
            except Exception:
                parts.append(f'<div class="topomap-missing">Failed to load {label}</div>')
                continue

            alt = f"topomap {band} ({label})"
            parts.append(f'<figure class="topomap-fig"><img class="topomap-img" src="{_e(src)}" alt="{_e(alt)}">')
            parts.append(f'<figcaption class="topomap-cap">{_e(label)}</figcaption></figure>')

        parts.append("</div>")  # row
        parts.append("</div>")  # item

    parts.append("</div>")  # grid
    parts.append("</div>")  # card
    return "".join(parts)


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
        for r in rows[: min(50, len(rows))]:  # sample
            v = _try_float(r.get(h, ""))
            if math.isfinite(v):
                any_num = True
                break
        if any_num:
            cols.append(h)
    return cols


def _svg_bar_chart(labels: Sequence[str], values: Sequence[float], *, title: str, units: str) -> str:
    # Simple horizontal bar chart SVG. Bars are centered so positive/negative show direction.
    w, h = 820, 320
    pad_l, pad_r, pad_t, pad_b = 160, 20, 36, 30
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    vmax = max([abs(v) for v in values if math.isfinite(v)] + [1.0])
    vmax = vmax if vmax > 0 else 1.0

    n = max(1, len(labels))
    row_h = inner_h / n

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')
    if units:
        parts.append(f'<text x="{w - pad_r}" y="22" text-anchor="end" class="chart-units">{_e(units)}</text>')

    x_center = pad_l + inner_w / 2.0
    parts.append(f'<line x1="{x_center}" y1="{pad_t}" x2="{x_center}" y2="{pad_t + inner_h}" class="axis"/>')

    for i, (lab, v) in enumerate(zip(labels, values)):
        y = pad_t + i * row_h
        cy = y + row_h * 0.65
        parts.append(f'<text x="{pad_l - 8}" y="{cy}" text-anchor="end" class="label">{_e(lab)}</text>')
        if not math.isfinite(v):
            parts.append(f'<text x="{x_center + 4}" y="{cy}" class="nan">NaN</text>')
            continue

        half = inner_w / 2.0
        bw = (v / vmax) * half
        bar_y = y + row_h * 0.18
        bar_h = max(2.0, row_h * 0.55)
        if bw >= 0:
            parts.append(f'<rect x="{x_center}" y="{bar_y}" width="{bw}" height="{bar_h}" class="bar-pos"/>')
        else:
            parts.append(f'<rect x="{x_center + bw}" y="{bar_y}" width="{abs(bw)}" height="{bar_h}" class="bar-neg"/>')
        parts.append(
            f'<text x="{pad_l + inner_w - 2}" y="{cy}" text-anchor="end" class="value">{_e(f"{v:.6g}")}</text>'
        )

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


def _column_meta(sidecar: Optional[Dict[str, Any]], key: str) -> Tuple[str, str]:
    if not sidecar:
        return "", ""
    info = sidecar.get(key)
    if not isinstance(info, dict):
        return "", ""
    desc = info.get("Description", "") if isinstance(info.get("Description"), str) else ""
    units = info.get("Units", "") if isinstance(info.get("Units"), str) else ""
    return desc, units


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
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(v)}</code></td></tr>")
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
        '<div class="note">Build/run metadata written by <code>qeeg_bandpower_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + '</table>'
        '</div>'
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render bandpowers.csv to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to bandpowers.csv, or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/bandpowers_report.html).")
    ap.add_argument(
        "--top-n",
        type=int,
        default=10,
        help="How many channels to show per-feature in the bar charts (default: 10).",
    )
    ap.add_argument("--title", default="Bandpower report", help="Report title.")
    ap.add_argument(
        "--link-topomaps",
        action="store_true",
        help="Do not embed topomap_* images; link to local files instead (keeps HTML smaller).",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    csv_path, html_path, sidecar_path, run_meta_path = _guess_paths(args.input, args.out)
    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find bandpowers.csv at: {csv_path}")

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
            ch = r.get(channel_col, "").strip() or "?"
            v = _try_float(r.get(col, ""))
            if math.isfinite(v):
                vals.append((ch, v))

        if not vals:
            continue

        vals.sort(key=lambda kv: abs(kv[1]), reverse=True)
        vals = vals[: min(top_n, len(vals))]
        labels = [k for k, _ in vals]
        values = [v for _, v in vals]
        desc, units = _column_meta(sidecar, col)

        subtitle = f"{col} (top {len(vals)} channels by |value|)"
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

/* Topomap gallery */
.topomap-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px; }
.topomap-item { border: 1px solid var(--grid); border-radius: 10px; padding: 10px; background: rgba(255,255,255,0.02); }
.topomap-band { font-weight: 600; margin-bottom: 8px; }
.topomap-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; align-items: start; }
.topomap-fig { margin: 0; }
.topomap-img { width: 100%; height: auto; border-radius: 8px; border: 1px solid var(--grid); background: #0b0f14; }
.topomap-cap { text-align: center; color: var(--muted); font-size: 12px; margin-top: 4px; }
.topomap-missing { color: var(--muted); font-size: 12px; padding: 10px; border: 1px dashed var(--grid); border-radius: 8px; text-align: center; }
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
    csv_dir = os.path.dirname(os.path.abspath(csv_path)) or "."
    topomap_html = _render_topomap_section(
        csv_dir,
        out_dir,
        link_only=bool(args.link_topomaps),
    )


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
  <div class="meta">Generated {now} — source <code>{_e(src)}</code>{sidecar_line}{run_meta_line}</div>
</header>
<main>
  <div class="card">
    <h2>About</h2>
    <div class="note">
      This report is a convenience visualization of <code>bandpowers.csv</code> written by
      <code>qeeg_bandpower_cli</code>. It is for research/educational inspection only and is not a medical device.
      Click any column header to sort the table.
    </div>
  </div>

  {run_meta_card}

  <div class="card">
    <h2>Bandpowers table</h2>
    <div class="table-filter">
      <div class="table-controls">
        <input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />
        <button type="button" onclick="downloadTableCSV(this, 'bandpowers_table_filtered.csv', true)">Download CSV</button>
        <span class="filter-count muted"></span>
      </div>
      <div class="table-wrap">{table_html}</div>
    </div>
  </div>


  {topomap_html}

  <div class="card">
    <h2>Top channels per feature</h2>
    <div class="note">Each chart shows the top-N channels by absolute value for that feature column.</div>
  </div>

  {charts_html}

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
            import webbrowser
            import pathlib

            webbrowser.open(pathlib.Path(os.path.abspath(html_path)).as_uri())
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
