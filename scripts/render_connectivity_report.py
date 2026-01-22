#!/usr/bin/env python3
"""Render qeeg connectivity outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Supported inputs (directory or file):

- Outputs from qeeg_coherence_cli (matrix mode):
    - coherence_pairs.csv / imcoh_pairs.csv
    - coherence_matrix_<band>.csv / imcoh_matrix_<band>.csv
    - coherence_run_meta.json (optional)

- Outputs from qeeg_plv_cli (matrix mode):
    - plv_pairs.csv / pli_pairs.csv / wpli_pairs.csv / wpli2_debiased_pairs.csv
    - <measure>_matrix_<band>.csv
    - plv_run_meta.json (optional)

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.

Typical usage:

  # Pass an output directory containing *_pairs.csv and/or *_matrix_*.csv
  python3 scripts/render_connectivity_report.py --input out_conn

  # Or pass a specific pairs or matrix CSV
  python3 scripts/render_connectivity_report.py --input out_conn/coherence_pairs.csv

Notes:
- This is a convenience visualization for research/educational inspection only.
- It does not perform statistical inference.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    read_json_if_exists as _read_json_if_exists,
    utc_now_iso,
)


def _try_float(s: str) -> float:
    ss = (s or "").strip()
    if ss == "":
        return math.nan
    try:
        return float(ss)
    except Exception:
        return math.nan


def _clamp(x: float, a: float, b: float) -> float:
    return a if x < a else b if x > b else x


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def _rgb(r: float, g: float, b: float) -> str:
    rr = int(_clamp(round(r), 0, 255))
    gg = int(_clamp(round(g), 0, 255))
    bb = int(_clamp(round(b), 0, 255))
    return f"#{rr:02x}{gg:02x}{bb:02x}"


def _color_for_value(v: float, vmin: float, vmax: float) -> str:
    """Sequential blue scale (dark -> light)."""
    if not math.isfinite(v):
        return "#000000"
    if not math.isfinite(vmin) or not math.isfinite(vmax) or vmax <= vmin:
        t = 1.0
    else:
        t = (v - vmin) / (vmax - vmin)
    t = _clamp(t, 0.0, 1.0)

    # Low/high colors chosen to match the other report scripts' dark theme.
    # low: deep blue-gray; high: accent blue.
    low = (27.0, 42.0, 65.0)
    high = (143.0, 183.0, 255.0)
    return _rgb(_lerp(low[0], high[0], t), _lerp(low[1], high[1], t), _lerp(low[2], high[2], t))

_SAFE_FILENAME_RE = re.compile(r"[^A-Za-z0-9._-]+")

def _safe_filename(s: str) -> str:
    """Sanitize a string to a safe-ish filename fragment."""
    s2 = _SAFE_FILENAME_RE.sub("_", str(s))
    s2 = s2.strip("._-")
    return s2 if s2 else "table"



@dataclass
class Edge:
    a: str
    b: str
    v: float


@dataclass
class Matrix:
    channels: List[str]
    values: List[List[float]]  # NxN


@dataclass
class MeasureData:
    measure_id: str
    value_col: str
    pairs_path: Optional[str]
    edges: List[Edge]
    matrices: Dict[str, Matrix]  # band -> matrix


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str]:
    """Return (scan_dir, html_path)."""
    if _is_dir(inp):
        scan_dir = inp
        if out is None:
            out = os.path.join(inp, "connectivity_report.html")
        return scan_dir, out

    # File input: scan its directory.
    scan_dir = os.path.dirname(os.path.abspath(inp)) or "."
    if out is None:
        out = os.path.join(scan_dir, "connectivity_report.html")
    return scan_dir, out


def _list_files(scan_dir: str) -> Tuple[List[str], List[str], List[str]]:
    pairs: List[str] = []
    mats: List[str] = []
    metas: List[str] = []

    try:
        for name in sorted(os.listdir(scan_dir)):
            path = os.path.join(scan_dir, name)
            if not os.path.isfile(path):
                continue
            if name.endswith("_pairs.csv"):
                pairs.append(path)
            elif "_matrix_" in name and name.endswith(".csv"):
                mats.append(path)
            elif name.endswith("_run_meta.json"):
                metas.append(path)
    except FileNotFoundError:
        pass
    return pairs, mats, metas


def _parse_pairs_csv(path: str) -> Tuple[str, List[Edge]]:
    with open(path, "r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        if not r.fieldnames:
            raise RuntimeError(f"Expected header row in CSV: {path}")
        headers = [h.strip() for h in r.fieldnames]

        # Required channel columns.
        if "channel_a" not in headers or "channel_b" not in headers:
            raise RuntimeError(f"Pairs CSV missing channel_a/channel_b columns: {path}")

        # Value column: first non-channel column.
        value_cols = [h for h in headers if h not in ("channel_a", "channel_b")]
        if not value_cols:
            raise RuntimeError(f"Pairs CSV missing value column: {path}")
        value_col = value_cols[0]

        edges: List[Edge] = []
        for row in r:
            a = (row.get("channel_a") or "").strip()
            b = (row.get("channel_b") or "").strip()
            v = _try_float(row.get(value_col, ""))
            if a == "" or b == "":
                continue
            if not math.isfinite(v):
                continue
            edges.append(Edge(a=a, b=b, v=v))

    return value_col, edges


def _parse_matrix_csv(path: str) -> Matrix:
    # Matrix CSV format emitted by this repo:
    #   ,C1,C2,...
    #   C1,v11,v12,...
    #   C2,v21,v22,...
    # ...
    with open(path, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))

    if not rows or len(rows) < 2:
        raise RuntimeError(f"Matrix CSV is empty: {path}")

    header = [c.strip() for c in rows[0]]
    if len(header) < 2:
        raise RuntimeError(f"Matrix CSV header too short: {path}")

    channels = [c for c in header[1:] if c != ""]
    n = len(channels)
    if n == 0:
        raise RuntimeError(f"Matrix CSV has no channel names: {path}")

    values: List[List[float]] = []
    row_labels: List[str] = []

    for ri in range(1, len(rows)):
        row = rows[ri]
        if not row:
            continue
        name = (row[0] or "").strip()
        if name == "":
            continue

        row_labels.append(name)
        vals = [_try_float(c) for c in row[1 : 1 + n]]
        # Pad missing values.
        while len(vals) < n:
            vals.append(math.nan)
        values.append(vals[:n])

    if len(values) != n:
        # Best-effort: If row count differs, keep what we have but ensure square-ish.
        # Prefer the overlap between header channels and row labels.
        m = min(len(values), n)
        channels = channels[:m]
        values = [row[:m] for row in values[:m]]
        n = m

    # Replace non-finite with NaN (already) and ensure rows are correct length.
    for i in range(len(values)):
        if len(values[i]) != n:
            values[i] = (values[i] + [math.nan] * n)[:n]

    return Matrix(channels=channels, values=values)


def _measure_id_from_pairs_path(path: str) -> str:
    base = os.path.basename(path)
    if base.endswith("_pairs.csv"):
        return base[: -len("_pairs.csv")]
    return os.path.splitext(base)[0]


def _measure_band_from_matrix_path(path: str) -> Tuple[str, str]:
    base = os.path.basename(path)
    stem = os.path.splitext(base)[0]
    # Expect <measure>_matrix_<band>
    if "_matrix_" not in stem:
        return stem, ""
    meas, band = stem.split("_matrix_", 1)
    return meas, band


def _group_measure_data(pairs_paths: List[str], matrix_paths: List[str]) -> List[MeasureData]:
    measures: Dict[str, MeasureData] = {}

    # First parse pairs (edges).
    for p in pairs_paths:
        measure_id = _measure_id_from_pairs_path(p)
        try:
            value_col, edges = _parse_pairs_csv(p)
        except Exception:
            # Skip malformed files.
            continue
        measures[measure_id] = MeasureData(
            measure_id=measure_id,
            value_col=value_col,
            pairs_path=p,
            edges=edges,
            matrices={},
        )

    # Parse matrices.
    for mpath in matrix_paths:
        measure_id, band = _measure_band_from_matrix_path(mpath)
        if band == "":
            continue
        try:
            mat = _parse_matrix_csv(mpath)
        except Exception:
            continue

        if measure_id not in measures:
            measures[measure_id] = MeasureData(
                measure_id=measure_id,
                value_col=measure_id,
                pairs_path=None,
                edges=[],
                matrices={},
            )
        measures[measure_id].matrices[band] = mat

    # If an entry has no edges but has matrices, synthesize edges from upper triangle for ranking.
    for md in measures.values():
        if md.edges or not md.matrices:
            continue
        # Use first matrix as representative.
        band0 = sorted(md.matrices.keys())[0]
        mat = md.matrices[band0]
        ch = mat.channels
        n = len(ch)
        edges: List[Edge] = []
        for i in range(n):
            for j in range(i + 1, n):
                v = mat.values[i][j]
                if math.isfinite(v):
                    edges.append(Edge(a=ch[i], b=ch[j], v=v))
        md.edges = edges
        md.value_col = md.measure_id

    # Sort edges descending.
    for md in measures.values():
        md.edges.sort(key=lambda e: e.v, reverse=True)

    # Return in stable order.
    return sorted(measures.values(), key=lambda m: m.measure_id)


def _stats(values: Sequence[float]) -> Dict[str, float]:
    xs = [v for v in values if math.isfinite(v)]
    if not xs:
        return {"count": 0.0}
    xs.sort()
    n = len(xs)
    mean = sum(xs) / n

    def q(p: float) -> float:
        if n == 1:
            return xs[0]
        t = _clamp(p, 0.0, 1.0) * (n - 1)
        i = int(math.floor(t))
        j = int(math.ceil(t))
        if i == j:
            return xs[i]
        return _lerp(xs[i], xs[j], t - i)

    return {
        "count": float(n),
        "min": xs[0],
        "max": xs[-1],
        "mean": mean,
        "median": q(0.5),
        "p90": q(0.9),
        "p95": q(0.95),
    }


def _svg_matrix_heatmap(mat: Matrix, *, title: str, vmin: float, vmax: float) -> str:
    ch = mat.channels
    n = len(ch)
    cell = 14
    pad = 120
    pad_t = 90
    pad_r = 24
    pad_b = 70
    w = pad + n * cell + pad_r
    h = pad_t + n * cell + pad_b

    # Downsample labels to avoid unreadable clutter.
    max_labels = 24
    step = 1 if n <= max_labels else int(math.ceil(n / max_labels))

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="matrix">')
    parts.append(f'<text x="{pad}" y="24" class="chart-title">{_e(title)}</text>')

    # Axes labels.
    for j, name in enumerate(ch):
        if j % step != 0:
            continue
        x = pad + j * cell + cell * 0.5
        y = pad_t - 8
        parts.append(
            f'<text x="{x}" y="{y}" transform="rotate(-60 {x} {y})" text-anchor="end" class="axis-label">{_e(name)}</text>'
        )

    for i, name in enumerate(ch):
        if i % step != 0:
            continue
        x = pad - 8
        y = pad_t + i * cell + cell * 0.75
        parts.append(f'<text x="{x}" y="{y}" text-anchor="end" class="axis-label">{_e(name)}</text>')

    # Cells.
    for i in range(n):
        for j in range(n):
            v = mat.values[i][j] if i < len(mat.values) and j < len(mat.values[i]) else math.nan
            fill = _color_for_value(v, vmin, vmax)
            x = pad + j * cell
            y = pad_t + i * cell
            tooltip = f"{ch[i]} ↔ {ch[j]}: {v:.6g}" if math.isfinite(v) else f"{ch[i]} ↔ {ch[j]}: NaN"
            parts.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" fill="{fill}" class="cell">')
            parts.append(f"<title>{_e(tooltip)}</title></rect>")

    # Frame.
    parts.append(
        f'<rect x="{pad}" y="{pad_t}" width="{n*cell}" height="{n*cell}" fill="none" class="frame"/>'
    )

    # Colorbar (simple rectangle strip).
    cb_x = pad
    cb_y = pad_t + n * cell + 20
    cb_w = min(320, n * cell)
    cb_h = 12
    steps = 48
    for k in range(steps):
        t0 = k / max(1, steps - 1)
        v = vmin + t0 * (vmax - vmin) if math.isfinite(vmin) and math.isfinite(vmax) else t0
        fill = _color_for_value(v, vmin, vmax)
        x = cb_x + (cb_w * k / steps)
        parts.append(f'<rect x="{x}" y="{cb_y}" width="{cb_w/steps + 0.5}" height="{cb_h}" fill="{fill}"/>')
    parts.append(f'<rect x="{cb_x}" y="{cb_y}" width="{cb_w}" height="{cb_h}" fill="none" class="frame"/>')
    parts.append(
        f'<text x="{cb_x}" y="{cb_y + 28}" class="cb-label">{_e(f"vmin {vmin:.4g}")}</text>'
    )
    parts.append(
        f'<text x="{cb_x + cb_w}" y="{cb_y + 28}" text-anchor="end" class="cb-label">{_e(f"vmax {vmax:.4g}")}</text>'
    )

    parts.append("</svg>")
    return "".join(parts)


def _build_edges_table(edges: Sequence[Edge], *, value_col: str, top_n: int) -> str:
    rows = edges[: max(0, int(top_n))]
    ths = "".join(
        f'<th onclick="sortTable(this)">{_e(h)}</th>'
        for h in ("rank", "channel_a", "channel_b", value_col)
    )

    body: List[str] = []
    for i, e in enumerate(rows, start=1):
        body.append(
            "<tr>"
            f"<td>{i}</td>"
            f"<td>{_e(e.a)}</td>"
            f"<td>{_e(e.b)}</td>"
            f"<td data-num=\"{_e(f'{e.v:.12g}')}\">{_e(f'{e.v:.6g}')}</td>"
            "</tr>"
        )

    return (
        '<table class="data-table sticky">'
        f"<thead><tr>{ths}</tr></thead>"
        f"<tbody>{''.join(body)}</tbody>"
        "</table>"
    )


def _human_path(path: str, out_dir: str) -> str:
    try:
        return os.path.relpath(os.path.abspath(path), out_dir)
    except Exception:
        return path


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Render qeeg coherence/PLV connectivity outputs into a self-contained HTML report (stdlib only)."
    )
    ap.add_argument("--input", required=True, help="Path to an output directory, or a *_pairs.csv / *_matrix_*.csv file.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/connectivity_report.html).")
    ap.add_argument("--top-n", type=int, default=120, help="Top-N edges to show in each measure table (default: 120).")
    ap.add_argument("--title", default="Connectivity report", help="Report title.")
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    scan_dir, html_path = _guess_paths(args.input, args.out)
    if not os.path.isdir(scan_dir):
        raise SystemExit(f"Input directory not found: {scan_dir}")

    pairs_paths, matrix_paths, meta_paths = _list_files(scan_dir)

    # If input is a file, ensure it is included even if it doesn't match our patterns.
    if os.path.isfile(args.input):
        p = os.path.abspath(args.input)
        if p not in pairs_paths and p.endswith("_pairs.csv"):
            pairs_paths.append(p)
        if p not in matrix_paths and "_matrix_" in os.path.basename(p) and p.endswith(".csv"):
            matrix_paths.append(p)
        if p not in meta_paths and p.endswith("_run_meta.json"):
            meta_paths.append(p)

    measures = _group_measure_data(pairs_paths, matrix_paths)
    if not measures:
        raise SystemExit(
            f"No connectivity outputs found in {scan_dir}. Expected *_pairs.csv and/or *_matrix_*.csv files."
        )

    # Load run meta (best-effort). Note: coherence uses coherence_run_meta.json even for imcoh;
    # plv uses plv_run_meta.json for plv/pli/wpli variants.
    meta_by_name: Dict[str, Dict[str, Any]] = {}
    for mp in meta_paths:
        meta = _read_json_if_exists(mp)
        if meta:
            meta_by_name[os.path.basename(mp)] = meta

    # Choose a "header" meta: prefer coherence/plv run_meta if present.
    header_meta = meta_by_name.get("coherence_run_meta.json") or meta_by_name.get("plv_run_meta.json")

    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."
    src = _human_path(scan_dir, out_dir)

    # Build content.
    measure_tabs: List[str] = []
    measure_sections: List[str] = []

    for idx, md in enumerate(measures):
        m_id = md.measure_id
        tab_id = f"tab_{idx}"
        section_id = f"section_{idx}"

        # Basic stats from edges.
        st = _stats([e.v for e in md.edges])
        stats_line = ""
        if st.get("count", 0.0) > 0:
            stats_line = (
                f"n={int(st['count'])} · min={st['min']:.4g} · max={st['max']:.4g} · "
                f"mean={st['mean']:.4g} · median={st['median']:.4g} · p95={st['p95']:.4g}"
            )
        else:
            stats_line = "No finite values found."

        pairs_rel = _human_path(md.pairs_path, out_dir) if md.pairs_path else "(synthesized from matrix)"
        dl_name = f"connectivity_top_edges_{_safe_filename(m_id)}.csv"

        # Matrices: render each band.
        matrices_html: List[str] = []
        for band in sorted(md.matrices.keys()):
            mat = md.matrices[band]
            # Range: ignore diagonal where it is often fixed (1 or 0) to reduce color washout.
            vals: List[float] = []
            n = len(mat.channels)
            for i in range(n):
                for j in range(n):
                    if i == j:
                        continue
                    v = mat.values[i][j] if i < len(mat.values) and j < len(mat.values[i]) else math.nan
                    if math.isfinite(v):
                        vals.append(v)
            if not vals:
                vmin, vmax = 0.0, 1.0
            else:
                vmin, vmax = min(vals), max(vals)
                # If all values identical, widen a bit.
                if vmax <= vmin:
                    vmax = vmin + 1e-9

            title = f"{m_id} matrix — {band}"
            matrices_html.append(
                '<div class="card">'
                f"<h3>{_e(title)}</h3>"
                '<div class="note">Tip: hover cells to see tooltips; scroll if needed.</div>'
                '<div class="matrix-wrap">'
                f"{_svg_matrix_heatmap(mat, title=title, vmin=vmin, vmax=vmax)}"
                "</div>"
                "</div>"
            )

        edges_table = _build_edges_table(md.edges, value_col=md.value_col, top_n=int(args.top_n))

        measure_tabs.append(
            f'<button class="tab" id="{_e(tab_id)}" onclick="showSection({idx})">{_e(m_id)}</button>'
        )

        measure_sections.append(
            f'<section class="measure" id="{_e(section_id)}">'
            '<div class="card">'
            f"<h2>{_e(m_id)}</h2>"
            f'<div class="note"><div><b>Source:</b> <code>{_e(pairs_rel)}</code></div>'
            f"<div><b>Stats:</b> { _e(stats_line) }</div></div>"
            "</div>"
            '<div class="card">'
            f"<h3>Top edges ({int(args.top_n)})</h3>"
            '<div class="note">Click column headers to sort. Values are shown as emitted (no inference).</div>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
            f'<button type="button" onclick="downloadTableCSV(this, \'{_e(dl_name)}\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            '</div>'
            f'<div class="table-wrap">{edges_table}</div>'
            '</div>'
            "</div>"
            + "".join(matrices_html)
            + "</section>"
        )

    # Render meta JSON summary (if present).
    meta_html = ""
    if header_meta:
        tool = header_meta.get("Tool")
        inp_path = header_meta.get("InputPath") or header_meta.get("input_path")
        outd = header_meta.get("OutputDir")
        outs = header_meta.get("Outputs")
        outs_list = ""
        if isinstance(outs, list):
            outs_list = "".join(f"<li><code>{_e(str(x))}</code></li>" for x in outs[:80])
            if len(outs) > 80:
                outs_list += f"<li>… ({len(outs) - 80} more)</li>"
        meta_html = (
            '<div class="card">'
            "<h2>Run metadata</h2>"
            '<div class="note">Best-effort preview from <code>*_run_meta.json</code> if present.</div>'
            "<ul class=\"meta-list\">"
            + (f"<li><b>Tool:</b> <code>{_e(tool)}</code></li>" if tool else "")
            + (f"<li><b>Input:</b> <code>{_e(inp_path)}</code></li>" if inp_path else "")
            + (f"<li><b>OutputDir:</b> <code>{_e(outd)}</code></li>" if outd else "")
            + "</ul>"
            + (f"<details><summary>Outputs ({len(outs) if isinstance(outs,list) else 0})</summary><ul>{outs_list}</ul></details>" if outs_list else "")
            + "</div>"
        )

    css = BASE_CSS + r"""

main { max-width: 1280px; }

.tabs { display: flex; gap: 8px; flex-wrap: wrap; margin: 12px 0; }
.tab { background: #0f1725; color: var(--text); border: 1px solid var(--grid); border-radius: 8px;
       padding: 8px 10px; cursor: pointer; font-size: 13px; }
.tab.active { outline: 2px solid rgba(143, 183, 255, 0.55); }
.measure { display: none; }
.measure.active { display: block; }

.matrix-wrap { overflow: auto; border: 1px solid var(--grid); border-radius: 10px; padding: 10px; background: rgba(15, 23, 37, 0.35); }
.matrix { width: 100%; height: auto; overflow: visible; }
.chart-title { fill: #dce7ff; font-size: 13px; font-weight: 600; }
.axis-label { fill: #cfe0ff; font-size: 11px; }
.cell { shape-rendering: crispEdges; }
.frame { stroke: #2b3d58; stroke-width: 1; }
.cb-label { fill: var(--muted); font-size: 12px; }

.meta-list { margin: 0; padding-left: 18px; }
.meta-list li { margin: 4px 0; }
"""

    js = JS_SORT_TABLE + r"""

function showSection(i) {
  const tabs = Array.from(document.querySelectorAll('.tab'));
  const sections = Array.from(document.querySelectorAll('.measure'));
  tabs.forEach((t, idx) => { t.classList.toggle('active', idx === i); });
  sections.forEach((s, idx) => { s.classList.toggle('active', idx === i); });
}

// Default: show first section.
window.addEventListener('DOMContentLoaded', () => {
  showSection(0);
});
"""

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
  <div class=\"meta\">Generated {now} — scanned <code>{_e(src)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This report is a convenience visualization of connectivity CSV outputs written by this repository’s CLI tools
      (for example <code>qeeg_coherence_cli</code> and <code>qeeg_plv_cli</code>). It is for research/educational inspection only
      and is not a medical device.
    </div>
  </div>

  {meta_html}

  <div class=\"card\">
    <h2>Measures</h2>
    <div class=\"note\">Select a measure to view its top edges and any available band matrices.</div>
    <div class=\"tabs\">{''.join(measure_tabs)}</div>
  </div>

  {''.join(measure_sections)}

  <div class=\"footer\">Tip: this file is self-contained (no network requests). Hover matrix cells for tooltips.</div>
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
