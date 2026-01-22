#!/usr/bin/env python3
"""Render qeeg_iaf_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (e.g., CI artifacts, lab workstations).

Inputs (directory or file):
  - iaf_by_channel.csv (required)
  - iaf_summary.txt (optional; embedded + parsed key/value summary)
  - iaf_band_spec.txt (optional; embedded)
  - iaf_run_meta.json (optional; build/run metadata)
  - topomap_iaf.bmp (optional; embedded if present)

Typical usage:

  # Pass an output directory produced by qeeg_iaf_cli
  python3 scripts/render_iaf_report.py --input out_iaf

  # Or pass iaf_by_channel.csv directly
  python3 scripts/render_iaf_report.py --input out_iaf/iaf_by_channel.csv --out iaf_report.html

The generated HTML is self-contained (inline CSS + SVG) and safe to open locally.
"""

from __future__ import annotations

import argparse
import base64
import math
import mimetypes
import os
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    read_text_if_exists as _read_text_if_exists,
    try_bool_int as _try_bool_int,
    try_float as _try_float,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str]:
    """Return (outdir, csv_path, html_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        csv_path = os.path.join(outdir, "iaf_by_channel.csv")
    else:
        p = os.path.abspath(inp)
        outdir = os.path.dirname(p) or "."
        if os.path.basename(p).lower().endswith(".csv"):
            csv_path = p
        else:
            csv_path = os.path.join(outdir, "iaf_by_channel.csv")

    if out is None:
        out = os.path.join(outdir, "iaf_report.html")
    return outdir, csv_path, os.path.abspath(out)


def _guess_mime(path: str) -> str:
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


def _parse_kv_summary(txt: str) -> Dict[str, str]:
    """Parse a qeeg-style key=value text summary into a dict."""
    out: Dict[str, str] = {}
    for line in (txt or "").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if "=" not in s:
            continue
        k, v = s.split("=", 1)
        k = k.strip()
        v = v.strip()
        if not k:
            continue
        # Keep first occurrence; avoid noisy duplicates.
        if k not in out:
            out[k] = v
    return out


def _median(values: List[float]) -> float:
    v = [x for x in values if math.isfinite(x)]
    if not v:
        return math.nan
    v.sort()
    n = len(v)
    mid = n // 2
    if n % 2 == 1:
        return float(v[mid])
    return 0.5 * (float(v[mid - 1]) + float(v[mid]))


def _mean(values: List[float]) -> float:
    v = [x for x in values if math.isfinite(x)]
    if not v:
        return math.nan
    return float(sum(v) / len(v))


def _summary_stats(values: List[float]) -> Dict[str, float]:
    v = [x for x in values if math.isfinite(x)]
    if not v:
        return {"n": 0.0, "min": math.nan, "median": math.nan, "mean": math.nan, "max": math.nan}
    return {
        "n": float(len(v)),
        "min": float(min(v)),
        "median": float(_median(v)),
        "mean": float(_mean(v)),
        "max": float(max(v)),
    }


def _svg_histogram(values: List[float], *, title: str, units: str, bins: int = 12) -> str:
    v = [x for x in values if math.isfinite(x)]
    if not v:
        return '<div class="note">No finite values available for histogram.</div>'

    mn = min(v)
    mx = max(v)
    if mn == mx:
        mn -= 1.0
        mx += 1.0

    bins = max(3, int(bins))
    step = (mx - mn) / bins
    counts = [0] * bins
    for x in v:
        i = int((x - mn) / step)
        if i < 0:
            i = 0
        if i >= bins:
            i = bins - 1
        counts[i] += 1

    w, h = 820, 280
    pad_l, pad_r, pad_t, pad_b = 60, 16, 38, 36
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    max_c = max(counts) if counts else 1
    max_c = max(1, int(max_c))

    def x_of(i: int) -> float:
        return pad_l + (i / bins) * inner_w

    def y_of(c: int) -> float:
        return pad_t + inner_h * (1.0 - (c / max_c))

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="chart" role="img" aria-label="{_e(title)}">')
    parts.append(f'<text x="{pad_l}" y="22" class="chart-title">{_e(title)}</text>')
    parts.append(f'<text x="{pad_l}" y="{h - 10}" class="axis">{_e(units)}</text>')

    # grid + bars
    for i, c in enumerate(counts):
        x0 = x_of(i)
        x1 = x_of(i + 1)
        bw = max(1.0, x1 - x0 - 2)
        y = y_of(c)
        bh = (pad_t + inner_h) - y
        parts.append(f'<rect x="{x0 + 1}" y="{y}" width="{bw}" height="{bh}" class="bar" />')

    # axis ticks (min/mid/max)
    parts.append(f'<text x="{pad_l}" y="{pad_t + inner_h + 18}" class="axis">{_e(f"{mn:.2f}")}</text>')
    parts.append(
        f'<text x="{pad_l + inner_w/2}" y="{pad_t + inner_h + 18}" text-anchor="middle" class="axis">{_e(f"{(mn+mx)/2:.2f}")}</text>'
    )
    parts.append(
        f'<text x="{pad_l + inner_w}" y="{pad_t + inner_h + 18}" text-anchor="end" class="axis">{_e(f"{mx:.2f}")}</text>'
    )

    parts.append("</svg>")
    return "".join(parts)


def _detect_channel_column(headers: Sequence[str]) -> str:
    for cand in ("channel", "Channel", "ch", "Ch", "label", "Label", "name", "Name"):
        if cand in headers:
            return cand
    return headers[0] if headers else "channel"


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, sticky: bool = True) -> str:
    headers_l = [h.lower() for h in headers]
    has_found = "found" in headers_l
    ths = "".join(f'<th onclick="sortTable(this)">{_e(h)}</th>' for h in headers)
    body_rows: List[str] = []
    for r in rows:
        # Heuristic: if there is a 'found' column and it is falsey, mark the row.
        found_v = r.get("found", r.get("Found", ""))
        found = _try_bool_int(found_v, default=1)
        row_cls = "row-missing" if (has_found and found == 0) else ""
        tds = "".join(f"<td>{_e(r.get(h, ''))}</td>" for h in headers)
        body_rows.append(f"<tr class=\"{row_cls}\">{tds}</tr>")
    cls = "data-table sticky" if sticky else "data-table"
    return f'<table class="{cls}"><thead><tr>{ths}</tr></thead><tbody>' + "".join(body_rows) + "</tbody></table>"


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
        "Input",
        "OutputDir",
        "AggregateIAFHz",
    ]
    rows: List[str] = []
    seen = set()
    for k in keys:
        if k in run_meta:
            v = run_meta.get(k)
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            seen.add(k)

    extra = 0
    for k, v in run_meta.items():
        if k in seen:
            continue
        if extra >= 12:
            break
        if isinstance(v, (str, int, float, bool)):
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            extra += 1

    if not rows:
        return ""
    return (
        '<div class="card">'
        '<h2>Run metadata</h2>'
        '<div class="note">Build/run metadata written by <code>qeeg_iaf_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + "</table>"
        + "</div>"
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_iaf_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to iaf_by_channel.csv, or the outdir containing it.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/iaf_report.html).")
    ap.add_argument("--title", default="IAF report", help="Report title.")
    ap.add_argument(
        "--link-topomap",
        action="store_true",
        help="Do not embed topomap_iaf.*; link to the local image file instead (keeps HTML smaller).",
    )
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, csv_path, html_path = _guess_paths(args.input, args.out)
    if not os.path.exists(csv_path):
        raise SystemExit(f"Could not find iaf_by_channel.csv at: {csv_path}")

    headers, rows = _read_csv(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in: {csv_path}")

    summary_txt = _read_text_if_exists(os.path.join(outdir, "iaf_summary.txt"))
    band_spec_txt = _read_text_if_exists(os.path.join(outdir, "iaf_band_spec.txt"))
    run_meta = _read_json_if_exists(os.path.join(outdir, "iaf_run_meta.json"))

    # Optional topomap image.
    topomap_path: Optional[str] = None
    for name in ("topomap_iaf.bmp", "topomap_iaf.png", "topomap_iaf.jpg", "topomap_iaf.jpeg", "topomap_iaf.gif", "topomap_iaf.svg"):
        cand = os.path.join(outdir, name)
        if os.path.exists(cand) and os.path.isfile(cand):
            topomap_path = cand
            break

    # Stats
    found_vals = [_try_bool_int(r.get("found", ""), default=1) for r in rows]
    n_total = len(rows)
    n_found = sum(1 for x in found_vals if x == 1)
    n_missing = n_total - n_found

    iaf_vals = [
        _try_float(r.get("iaf_hz", ""))
        for r in rows
        if _try_bool_int(r.get("found", ""), default=1) == 1
    ]
    cog_vals = [_try_float(r.get("cog_hz", "")) for r in rows]
    iaf_stats = _summary_stats(iaf_vals)
    cog_stats = _summary_stats(cog_vals)

    # If summary contains aggregate_iaf_hz, prefer showing that.
    summary_kv = _parse_kv_summary(summary_txt or "") if summary_txt else {}
    agg_iaf_txt = summary_kv.get("aggregate_iaf_hz")

    table_html = _build_table(headers, rows, sticky=True)
    now = utc_now_iso()
    src = os.path.abspath(args.input)
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."

    css = BASE_CSS + r"""
.kv { width: 100%; border-collapse: collapse; font-size: 13px; }
.kv th, .kv td { border-bottom: 1px solid rgba(255,255,255,0.08); padding: 8px; text-align: left; vertical-align: top; }
.kv th { width: 220px; color: #d7e4ff; background: #0f1725; }

.grid2 { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
@media (max-width: 900px) { .grid2 { grid-template-columns: 1fr; } }

.chart { width: 100%; height: auto; display: block; margin: 8px 0 0; }
.chart-title { fill: #dce7ff; font-size: 14px; font-weight: 600; }
.axis { fill: rgba(231,238,252,0.72); font-size: 12px; }
.bar { fill: rgba(143,183,255,0.65); }

.row-missing td { color: var(--bad); }

.img-wrap { overflow: auto; border: 1px solid var(--grid); border-radius: 10px; padding: 10px; background: rgba(255,255,255,0.02); }
.img-wrap img { max-width: 100%; height: auto; display: block; }
"""

    js = JS_SORT_TABLE

    # Topomap embed / link
    topomap_html = ""
    if topomap_path:
        try:
            if args.link_topomap:
                src_img = _posix_relpath(topomap_path, out_dir)
            else:
                # Guard against accidentally embedding huge images.
                if os.path.getsize(topomap_path) <= 12 * 1024 * 1024:
                    src_img = _read_file_b64_data_uri(topomap_path)
                else:
                    src_img = _posix_relpath(topomap_path, out_dir)
            topomap_html = (
                '<div class="card">'
                '<h2>Topomap</h2>'
                '<div class="note">If your run exported <code>topomap_iaf.bmp</code>, it is shown here. '
                'Use <code>--link-topomap</code> to keep the HTML smaller (image will be loaded from disk).</div>'
                f'<div class="img-wrap"><img src="{_e(src_img)}" alt="topomap IAF"></div>'
                '</div>'
            )
        except Exception:
            topomap_html = (
                '<div class="card">'
                '<h2>Topomap</h2>'
                '<div class="note">Failed to load <code>topomap_iaf.*</code>.</div>'
                '</div>'
            )

    summary_card = ""
    if summary_txt:
        rows_kv = []
        for k in sorted(summary_kv.keys()):
            rows_kv.append(f"<tr><th>{_e(k)}</th><td><code>{_e(summary_kv[k])}</code></td></tr>")
        kv_table = '<table class="kv">' + "".join(rows_kv) + "</table>" if rows_kv else ""
        summary_card = (
            '<div class="card">'
            '<h2>IAF summary</h2>'
            '<div class="note">Key/value snapshot written by <code>qeeg_iaf_cli</code>.</div>'
            + kv_table
            + '<div class="note" style="margin-top:10px">Raw <code>iaf_summary.txt</code>:</div>'
            + f"<pre>{_e(summary_txt)}</pre>"
            + "</div>"
        )

    band_spec_card = ""
    if band_spec_txt:
        band_spec_card = (
            '<div class="card">'
            '<h2>Recommended IAF-relative band spec</h2>'
            '<div class="note">If this file is present, you can pass it to tools that accept <code>--bands</code> (e.g., <code>qeeg_bandpower_cli</code>).</div>'
            f"<pre>{_e(band_spec_txt.strip())}</pre>"
            '</div>'
        )

    # Charts
    charts: List[str] = []
    charts.append(_svg_histogram(iaf_vals, title="IAF (peak) distribution across channels", units="Hz"))
    charts.append(_svg_histogram(cog_vals, title="Alpha CoG distribution across channels", units="Hz"))
    # We'll place the histograms in a 2-column grid.
    charts_html = (
        '<div class="card">'
        '<h2>Distributions</h2>'
        '<div class="note">These histograms summarize per-channel estimates. Use the table filter to inspect specific channels.</div>'
        '<div class="grid2">'
        + "".join(f'<div class="card" style="margin:0">{c}</div>' for c in charts)
        + '</div>'
        '</div>'
    )

    # Quick stats card.
    def fmt(x: float) -> str:
        return "" if not math.isfinite(x) else f"{x:.4g}"

    agg_line = f"<code>{_e(agg_iaf_txt)}</code> Hz" if agg_iaf_txt else f"median≈<code>{_e(fmt(iaf_stats['median']))}</code> Hz"

    stats_card = f"""
  <div class="card">
    <h2>At a glance</h2>
    <div class="note">
      Channels: <code>{n_total}</code> — peaks found: <code>{n_found}</code> — missing: <code>{n_missing}</code><br>
      Aggregate IAF: {agg_line}
    </div>
    <div class="note" style="margin-top:10px">
      <b>IAF (found channels)</b>: n={int(iaf_stats['n'])}, min={_e(fmt(iaf_stats['min']))}, median={_e(fmt(iaf_stats['median']))}, mean={_e(fmt(iaf_stats['mean']))}, max={_e(fmt(iaf_stats['max']))} (Hz)<br>
      <b>CoG</b>: n={int(cog_stats['n'])}, min={_e(fmt(cog_stats['min']))}, median={_e(fmt(cog_stats['median']))}, mean={_e(fmt(cog_stats['mean']))}, max={_e(fmt(cog_stats['max']))} (Hz)
    </div>
  </div>
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
  <div class=\"meta\">Generated {now} — source <code>{_e(src)}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This report is a convenience visualization of <code>qeeg_iaf_cli</code> outputs.
      It is for research/educational inspection only and is not a medical device.
      Click any column header to sort the table.
    </div>
  </div>

  {stats_card}

  {_render_run_meta(run_meta)}

  {summary_card}
  {band_spec_card}
  {topomap_html}

  {charts_html}

  <div class=\"card\">
    <h2>Per-channel estimates</h2>
    <div class=\"note\">Rows where <code>found=0</code> indicate no robust alpha peak was detected for that channel under the chosen parameters.</div>
    <div class=\"table-filter\">
      <div class=\"table-controls\">
        <input type=\"search\" placeholder=\"Filter rows… (e.g. found=1  iaf_hz>9.5  -fz)\" oninput=\"filterTable(this)\" />
        <button type=\"button\" onclick=\"downloadTableCSV(this, 'iaf_by_channel_filtered.csv', true)\">Download CSV</button>
        <span class=\"filter-count muted\"></span>
      </div>
      <div class=\"table-wrap\">{table_html}</div>
    </div>
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
            import pathlib
            import webbrowser

            webbrowser.open(pathlib.Path(os.path.abspath(html_path)).as_uri())
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
