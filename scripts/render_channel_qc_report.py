#!/usr/bin/env python3
"""Render qeeg_channel_qc_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only).

Inputs (directory or file):
  - channel_qc.csv (required)
  - qc_summary.txt (optional; embedded as a preformatted block)
  - bad_channels.txt (optional; linked)
  - qc_run_meta.json (optional; used for extra metadata)

Typical usage:

  # Pass an output directory produced by qeeg_channel_qc_cli
  python3 scripts/render_channel_qc_report.py --input out_qc

  # Or pass channel_qc.csv directly
  python3 scripts/render_channel_qc_report.py --input out_qc/channel_qc.csv --out qc_report.html

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


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str, str, Optional[str]]:
    """Return (outdir, csv_path, html_path, bad_txt_path, meta_json_path_or_None)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        csv_path = os.path.join(outdir, "channel_qc.csv")
    else:
        p = os.path.abspath(inp)
        outdir = os.path.dirname(p) or "."
        if os.path.basename(p).lower().endswith(".csv"):
            csv_path = p
        else:
            # Allow passing qc_summary.txt, etc.
            csv_path = os.path.join(outdir, "channel_qc.csv")

    if out is None:
        out = os.path.join(outdir, "channel_qc_report.html")
    html_path = os.path.abspath(out)

    bad_txt = os.path.join(outdir, "bad_channels.txt")
    summary_txt = os.path.join(outdir, "qc_summary.txt")
    meta_json = os.path.join(outdir, "qc_run_meta.json")
    return outdir, csv_path, html_path, bad_txt, (meta_json if os.path.exists(meta_json) else None)


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
    # Simple horizontal bar chart.
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
        parts.append(f'<text x="{pad_l + inner_w - 2}" y="{cy}" text-anchor="end" class="value">{_e(f"{v:.6g}")}</text>')

    parts.append("</svg>")
    return "".join(parts)


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]]) -> str:
    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body_rows: List[str] = []
    for r in rows:
        bad = _try_int(r.get("bad", "0"))
        cls = "bad" if bad else ""
        tds = "".join(f"<td>{_e(r.get(h, ''))}</td>" for h in headers)
        body_rows.append(f'<tr class="{cls}">{tds}</tr>')
    return f'<table class="data-table sticky"><thead><tr>{ths}</tr></thead><tbody>' + "".join(body_rows) + "</tbody></table>"


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render channel_qc.csv to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to channel_qc.csv or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/channel_qc_report.html)")
    ap.add_argument("--top-n", type=int, default=12, help="Top-N channels to show per chart (default: 12)")
    ap.add_argument("--title", default="Channel QC report", help="Report title")
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, csv_path, html_path, bad_txt_path, meta_path = _guess_paths(args.input, args.out)
    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find channel_qc.csv at: {csv_path}")

    headers, rows = _read_csv(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in: {csv_path}")

    # Pull optional sidecars.
    summary_txt_path = os.path.join(outdir, "qc_summary.txt")
    summary_txt = _read_text_if_exists(summary_txt_path)
    meta = _read_json_if_exists(meta_path) if meta_path else None

    # Summary stats
    n_channels = len(rows)
    bad_rows = [r for r in rows if _try_int(r.get("bad", "0")) != 0]
    n_bad = len(bad_rows)

    counts = {
        "flatline": sum(_try_int(r.get("flatline", "0")) != 0 for r in rows),
        "noisy": sum(_try_int(r.get("noisy", "0")) != 0 for r in rows),
        "artifact_often_bad": sum(_try_int(r.get("artifact_often_bad", "0")) != 0 for r in rows),
        "corr_low": sum(_try_int(r.get("corr_low", "0")) != 0 for r in rows),
    }

    # Charts
    top_n = max(1, int(args.top_n))

    def top_by(col: str, *, reverse: bool = True, finite_only: bool = True) -> Tuple[List[str], List[float]]:
        vals: List[Tuple[str, float]] = []
        for r in rows:
            ch = r.get("channel", "") or r.get("name", "") or "?"
            v = _try_float(r.get(col, ""))
            if finite_only and not math.isfinite(v):
                continue
            vals.append((ch, v))
        vals.sort(key=lambda kv: kv[1], reverse=reverse)
        vals = vals[: min(top_n, len(vals))]
        return [k for k, _ in vals], [v for _, v in vals]

    labels_rs, values_rs = top_by("robust_scale", reverse=True)
    labels_abf, values_abf = top_by("artifact_bad_window_fraction", reverse=True)
    labels_corr, values_corr = top_by("abs_corr_with_mean", reverse=False)

    charts: List[str] = []
    if labels_rs:
        charts.append(_svg_hbar(labels_rs, values_rs, title=f"Top {len(labels_rs)} channels by robust_scale"))
    if labels_abf:
        charts.append(_svg_hbar(labels_abf, values_abf, title=f"Top {len(labels_abf)} channels by artifact_bad_window_fraction"))
    if labels_corr:
        charts.append(_svg_hbar(labels_corr, values_corr, title=f"Lowest {len(labels_corr)} channels by abs_corr_with_mean"))

    # Bad channels list
    bad_list_items: List[str] = []
    for r in bad_rows:
        ch = r.get("channel", "") or "?"
        reasons = r.get("reasons", "")
        if reasons:
            bad_list_items.append(f"<li><code>{_e(ch)}</code> — <span class=\"muted\">{_e(reasons)}</span></li>")
        else:
            bad_list_items.append(f"<li><code>{_e(ch)}</code></li>")
    bad_list_html = "<ul>" + "".join(bad_list_items) + "</ul>" if bad_list_items else "<div class=\"muted\">None</div>"

    # Meta info
    now = utc_now_iso()
    rel_csv = os.path.relpath(os.path.abspath(csv_path), os.path.dirname(html_path) or ".")

    meta_lines: List[str] = []
    if isinstance(meta, dict):
        tool = meta.get("Tool")
        ts = meta.get("TimestampLocal")
        if isinstance(tool, str):
            meta_lines.append(f"Tool: <code>{_e(tool)}</code>")
        if isinstance(ts, str):
            meta_lines.append(f"TimestampLocal: <code>{_e(ts)}</code>")

        opt = meta.get("Options")
        if isinstance(opt, dict):
            # Show a small subset of options if present.
            show_keys = [
                "Interpolate",
                "DropBad",
                "FlatlinePtp",
                "FlatlineScale",
                "FlatlineScaleFactor",
                "NoisyScaleFactor",
                "ArtifactBadFrac",
                "MinAbsCorr",
            ]
            kvs = []
            for k in show_keys:
                if k in opt:
                    kvs.append(f"<code>{_e(k)}</code>={_e(opt.get(k))}")
            if kvs:
                meta_lines.append("Options: " + ", ".join(kvs))

    meta_html = "<br>".join(meta_lines) if meta_lines else "<span class=\"muted\">(no qc_run_meta.json found)</span>"

    # Links
    rel_bad_txt = ""
    if os.path.exists(bad_txt_path):
        rel_bad_txt = os.path.relpath(os.path.abspath(bad_txt_path), os.path.dirname(html_path) or ".").replace(os.sep, "/")

    table_html = _build_table(headers, rows)

    css = BASE_CSS + r"""

.kpi { display:flex; gap: 14px; flex-wrap: wrap; }
.kpi .pill { border: 1px solid var(--grid); border-radius: 999px; padding: 6px 10px; font-size: 13px; }
.kpi .pill b { color: #dce7ff; }

.data-table tr.bad td { border-left: 3px solid var(--bad); }

.charts { display: grid; grid-template-columns: 1fr; gap: 12px; }
.chart { width: 100%; height: auto; }
.axis { stroke: #2b3d58; stroke-width: 1; }
.label, .value, .nan, .chart-title, .chart-units { fill: #dce7ff; font-size: 12px; }
.chart-title { font-size: 13px; font-weight: 600; }
.chart-units { fill: var(--muted); font-size: 12px; }
.label { fill: #cfe0ff; }
.value { fill: var(--muted); font-size: 12px; }
.nan { fill: var(--warn); }
.bar { fill: var(--ok); opacity: 0.92; }
"""

    js = JS_SORT_TABLE

    charts_html = "".join(f'<div class="card">{c}</div>' for c in charts) if charts else ""
    bad_link_html = (
        f'<a href="{_e(rel_bad_txt)}">bad_channels.txt</a>' if rel_bad_txt else '<span class="muted">(bad_channels.txt not found)</span>'
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
  <div class=\"meta\">Generated {now} — source: <code>{_e(rel_csv)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>Summary</h2>
    <div class=\"kpi\">
      <div class=\"pill\"><b>Channels</b>: {_e(n_channels)}</div>
      <div class=\"pill\"><b>Bad</b>: {_e(n_bad)}</div>
      <div class=\"pill\"><b>Flatline</b>: {_e(counts['flatline'])}</div>
      <div class=\"pill\"><b>Noisy</b>: {_e(counts['noisy'])}</div>
      <div class=\"pill\"><b>Artifact-often-bad</b>: {_e(counts['artifact_often_bad'])}</div>
      <div class=\"pill\"><b>Low-corr</b>: {_e(counts['corr_low'])}</div>
    </div>
    <div class=\"note\" style=\"margin-top:10px\">{meta_html}</div>
  </div>

  <div class=\"card\">
    <h2>Bad channels</h2>
    <div class=\"note\">{bad_link_html}</div>
    {bad_list_html}
  </div>

  {charts_html}

  <div class=\"card\">
    <h2>channel_qc.csv</h2>
    <div class=\"note\">Click a header to sort. Rows marked with a left red bar are flagged <code>bad=1</code>.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'channel_qc_table_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{table_html}</div>
    </div>
  </div>

  <div class=\"card\">
    <h2>qc_summary.txt</h2>
    <div class=\"note\">Raw CLI summary (if present).</div>
    <pre>{_e(summary_txt or 'qc_summary.txt not found')}</pre>
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
