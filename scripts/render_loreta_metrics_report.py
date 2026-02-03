#!/usr/bin/env python3
"""Render LORETA ROI metrics CSV into a self-contained HTML report.

This report is a lightweight companion to qeeg_loreta_metrics_cli.

Inputs:
  - An outdir created by qeeg_loreta_metrics_cli (containing loreta_metrics.csv), or
  - A direct path to a CSV/TSV table.

The resulting HTML makes **no network requests** and is safe to open locally.

Usage:
  python3 scripts/render_loreta_metrics_report.py --input out_loreta
  python3 scripts/render_loreta_metrics_report.py --input out_loreta --out out_loreta/loreta_metrics_report_embedded.html
"""

from __future__ import annotations

import argparse
import os
import re
import webbrowser
from typing import Dict, List, Optional, Sequence

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE_BASE,
    JS_THEME_TOGGLE,
    e as _e,
    is_dir as _is_dir,
    read_csv_dict as _read_csv_dict,
    read_json_if_exists as _read_json_if_exists,
    utc_now_iso,
)


def _detect_roi_col(headers: Sequence[str]) -> str:
    cands = ["roi", "ROI", "region", "Region", "label", "Label", "name", "Name", "ba", "BA"]
    for c in cands:
        if c in headers:
            return c
    return headers[0] if headers else "roi"


def _is_z_metric(name: str) -> bool:
    s = (name or "").strip().lower()
    if not s:
        return False
    # Heuristic: common z-score column names.
    return ("zscore" in s) or re.search(r"(^|[^a-z])z([^a-z]|$)", s) is not None or s.endswith("_z")


def _parse_num(s: str) -> Optional[float]:
    t = (s or "").strip()
    if not t:
        return None
    try:
        v = float(t)
    except Exception:
        return None
    if v != v:  # NaN
        return None
    return v



def _render_protocol(protocol: Dict[str, object], *, max_rows: int = 50) -> str:
    targets = protocol.get("targets")
    if not isinstance(targets, list) or not targets:
        return ""

    # Limit rows to keep report snappy.
    targets = targets[: max_rows if max_rows > 0 else len(targets)]

    parts: List[str] = []
    parts.append('<div class="card">')
    parts.append("<h2>Protocol candidates (heuristic)</h2>")
    parts.append(
        '<div class="note">Ranked ROI × metric values sorted by <code>|value|</code>. '
        'For z-score-like metrics, <code>suggested_direction</code> indicates movement toward 0.</div>'
    )
    parts.append('<div class="table-wrap">')
    parts.append('<table class="qeeg-table" id="protocol_table">')
    parts.append(
        "<thead><tr>"
        "<th>#</th><th>ROI</th><th>Metric</th><th>Value</th><th>|Value|</th><th>Kind</th><th>Direction</th>"
        "</tr></thead>"
    )
    parts.append("<tbody>")
    for t in targets:
        if not isinstance(t, dict):
            continue
        rank = t.get("rank")
        roi = t.get("roi")
        metric = t.get("metric")
        value = t.get("value")
        abs_value = t.get("abs_value")
        mk = t.get("metric_kind")
        vk = t.get("value_kind")
        band = t.get("band")
        direction = t.get("suggested_direction")

        kind = f"{mk}/{vk}" if isinstance(mk, str) and isinstance(vk, str) else ""
        if isinstance(band, str) and band:
            kind = f"{kind} ({band})" if kind else band

        parts.append("<tr>")
        parts.append(f"<td>{_e(str(rank) if rank is not None else '')}</td>")
        parts.append(f"<td>{_e(str(roi) if roi is not None else '')}</td>")
        parts.append(f"<td>{_e(str(metric) if metric is not None else '')}</td>")
        parts.append(f"<td>{_e(str(value) if value is not None else '')}</td>")
        parts.append(f"<td>{_e(str(abs_value) if abs_value is not None else '')}</td>")
        parts.append(f"<td>{_e(kind)}</td>")
        parts.append(f"<td>{_e(str(direction) if direction is not None else '')}</td>")
        parts.append("</tr>")
    parts.append("</tbody></table></div></div>")
    return "\n".join(parts)


def _find_protocol(outdir: str, index: Optional[Dict[str, object]]) -> Optional[Dict[str, object]]:
    # Prefer protocol_json path in the index when present.
    if isinstance(index, dict):
        pj = index.get("protocol_json")
        if isinstance(pj, str) and pj:
            p = os.path.join(outdir, pj)
            obj = _read_json_if_exists(p)
            if isinstance(obj, dict):
                return obj

    # Fallback: common default name.
    p = os.path.join(outdir, "loreta_protocol.json")
    obj = _read_json_if_exists(p)
    return obj if isinstance(obj, dict) else None


def _build_table(headers: Sequence[str], rows: Sequence[Dict[str, str]], *, z_threshold: float) -> str:
    # Filter UI
    parts: List[str] = []
    parts.append('<div class="table-filter">')
    parts.append(
        '<div class="row" style="gap:10px;align-items:center;flex-wrap:wrap">'
        '<input class="filter" type="search" placeholder="Filter… (e.g., ba24 z_theta>2 -beta)" oninput="filterTable(this)"/>'
        f'<span class="muted" id="qeeg-count"></span>'
        "</div>"
    )
    parts.append('<table class="data-table sticky" data-qeeg-count="#qeeg-count">')
    parts.append("<thead><tr>")
    for h in headers:
        parts.append(f'<th onclick="sortTable(this)">{_e(h)}</th>')
    parts.append("</tr></thead>")
    parts.append("<tbody>")

    roi_col = headers[0] if headers else "roi"
    z_cols = {h for h in headers if _is_z_metric(h)}

    for r in rows:
        parts.append("<tr>")
        for h in headers:
            v = r.get(h, "")
            cls = ""
            data_num = ""
            num = _parse_num(v)
            if num is not None:
                data_num = f' data-num="{num:.12g}"'
                if h in z_cols and abs(num) >= z_threshold:
                    cls = " class=\"warn\""
            parts.append(f"<td{cls}{data_num}>{_e(v)}</td>")
        parts.append("</tr>")
    parts.append("</tbody></table></div>")
    return "".join(parts)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render LORETA ROI metrics into a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to loreta_metrics.csv (or .tsv), or the outdir containing it.")
    ap.add_argument(
        "--out",
        default=None,
        help="Output HTML path (default: <outdir>/loreta_metrics_report.html or <csv_dir>/loreta_metrics_report.html).",
    )
    ap.add_argument("--title", default="LORETA ROI metrics", help="Report title")
    ap.add_argument(
        "--z-threshold",
        type=float,
        default=2.0,
        help="Highlight z-score-like columns where |value| >= threshold (default: 2.0).",
    )
    ap.add_argument("--open", action="store_true", help="Open the report in the default browser")
    args = ap.parse_args(argv)

    inp = os.path.abspath(args.input)
    outdir = inp if _is_dir(inp) else os.path.dirname(inp)

    csv_path = inp
    if _is_dir(inp):
        csv_path = os.path.join(inp, "loreta_metrics.csv")
        if not os.path.exists(csv_path):
            # fallback: accept common name
            csv_path = os.path.join(inp, "roi_metrics.csv")
    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"Input CSV not found: {csv_path}")

    if args.out:
        out_html = os.path.abspath(args.out)
    else:
        out_html = os.path.join(outdir, "loreta_metrics_report.html")

    headers, rows = _read_csv_dict(csv_path)
    if not rows:
        raise RuntimeError(f"No rows parsed from: {csv_path}")

    if not headers:
        raise RuntimeError(f"No headers parsed from: {csv_path}")

    roi_col = _detect_roi_col(headers)
    # Reorder: ROI column first.
    reordered = [roi_col] + [h for h in headers if h != roi_col]

    # atlas label (optional)
    index = _read_json_if_exists(os.path.join(outdir, "loreta_metrics_index.json")) if _is_dir(inp) else None
    atlas = "unknown"
    if isinstance(index, dict):
        a = index.get("atlas")
        if isinstance(a, dict) and isinstance(a.get("name"), str):
            atlas = str(a.get("name"))

    table_html = _build_table(reordered, rows, z_threshold=float(args.z_threshold))

    protocol = _find_protocol(outdir, index if isinstance(index, dict) else None)
    protocol_html = _render_protocol(protocol) if isinstance(protocol, dict) else ""

    html = f"""<!doctype html>
<html>
<head>
  <meta charset=\"utf-8\"/>
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>
  <title>{_e(args.title)}</title>
  <style>
{BASE_CSS}
  .warn {{ font-weight: 700; }}
  </style>
  <script>
{JS_SORT_TABLE_BASE}
{JS_THEME_TOGGLE}
  </script>
</head>
<body>
  <button class=\"qeeg-theme-toggle\" onclick=\"qeegToggleTheme()\" title=\"Toggle theme\">◐</button>
  <h1>{_e(args.title)}</h1>
  <div class=\"note\">Generated: <code>{_e(utc_now_iso())}</code></div>
  <div class=\"note\">Source: <code>{_e(os.path.basename(csv_path))}</code> &nbsp;·&nbsp; Rows: <code>{len(rows)}</code> &nbsp;·&nbsp; Metrics: <code>{max(0, len(reordered)-1)}</code></div>
  <div class=\"note\">Atlas: <code>{_e(atlas)}</code> &nbsp;·&nbsp; Highlight: z-like columns with |value| ≥ <code>{_e(str(args.z_threshold))}</code></div>
  {protocol_html}
  <div class=\"card\">
    <h2>ROI table</h2>
    {table_html}
  </div>
</body>
</html>
"""

    os.makedirs(os.path.dirname(out_html) or ".", exist_ok=True)
    with open(out_html, "w", encoding="utf-8") as f:
        f.write(html)

    print(f"Wrote: {out_html}")
    if args.open:
        webbrowser.open_new_tab("file://" + out_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
