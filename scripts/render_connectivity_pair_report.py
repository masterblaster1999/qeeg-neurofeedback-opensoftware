#!/usr/bin/env python3
"""Render pair-mode connectivity outputs into a self-contained HTML report.

This repository already includes a connectivity renderer for matrix-mode and
edge-list outputs (see scripts/render_connectivity_report.py). Some connectivity
CLIs also support a *pair mode* that produces a small summary CSV:

  - qeeg_coherence_cli --pair ... writes:
      coherence_band.csv
      [optional] coherence_spectrum.csv
      [optional] coherence_timeseries.csv + coherence_timeseries.json

  - qeeg_plv_cli --pair ... writes:
      <measure>_band.csv  (plv / pli / wpli / wpli2_debiased)

This script bundles those artifacts into a single, easy-to-share HTML file.

The generated HTML is dependency-free (Python stdlib only) and safe to open
locally.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import statistics
import webbrowser
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    downsample_indices as _downsample_indices,
    e as _e,
    finite_minmax as _finite_minmax,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    try_float as _try_float,
    utc_now_iso,
)


_DEFAULT_EEG_BANDS: Dict[str, Tuple[float, float]] = {
    # Keep in sync with qeeg::default_eeg_bands() (src/bandpower.cpp).
    "delta": (0.5, 4.0),
    "theta": (4.0, 7.0),
    "alpha": (8.0, 12.0),
    "beta": (13.0, 30.0),
    "gamma": (30.0, 80.0),
}


_MEASURE_STEMS: Tuple[str, ...] = (
    "coherence",
    "imcoh",
    "plv",
    "pli",
    "wpli",
    "wpli2_debiased",
)


@dataclass(frozen=True)
class _PairRunFiles:
    stem: str
    band_csv: str
    spectrum_csv: Optional[str]
    timeseries_csv: Optional[str]
    timeseries_json: Optional[str]


def _guess_outdir(inp: str) -> str:
    if _is_dir(inp):
        return os.path.abspath(inp)
    p = os.path.abspath(inp)
    return os.path.dirname(p) or "."


def _find_pair_runs(outdir: str) -> List[_PairRunFiles]:
    runs: List[_PairRunFiles] = []
    for stem in _MEASURE_STEMS:
        band_csv = os.path.join(outdir, f"{stem}_band.csv")
        if not os.path.exists(band_csv):
            continue

        spectrum_csv = os.path.join(outdir, f"{stem}_spectrum.csv")
        if not os.path.exists(spectrum_csv):
            spectrum_csv = None

        timeseries_csv = os.path.join(outdir, f"{stem}_timeseries.csv")
        if not os.path.exists(timeseries_csv):
            timeseries_csv = None

        timeseries_json = os.path.join(outdir, f"{stem}_timeseries.json")
        if not os.path.exists(timeseries_json):
            timeseries_json = None

        runs.append(
            _PairRunFiles(
                stem=stem,
                band_csv=band_csv,
                spectrum_csv=spectrum_csv,
                timeseries_csv=timeseries_csv,
                timeseries_json=timeseries_json,
            )
        )

    return runs


def _find_run_meta(outdir: str) -> Optional[Dict[str, Any]]:
    # Prefer tool-specific meta files.
    candidates = [
        "coherence_run_meta.json",
        "plv_run_meta.json",
        "nf_run_meta.json",
    ]
    for name in candidates:
        p = os.path.join(outdir, name)
        obj = _read_json_if_exists(p)
        if isinstance(obj, dict):
            return obj
    # Fallback: any *_run_meta.json file.
    try:
        for fn in sorted(os.listdir(outdir)):
            if fn.lower().endswith("_run_meta.json"):
                obj = _read_json_if_exists(os.path.join(outdir, fn))
                if isinstance(obj, dict):
                    return obj
    except Exception:
        pass
    return None


def _pick_value_col(headers: Sequence[str]) -> str:
    # Prefer known value columns.
    for c in ("coherence", "imcoh", "plv", "pli", "wpli", "wpli2_debiased"):
        if c in headers:
            return c
    # Fallback: first non-identifier column.
    for h in headers:
        hl = (h or "").strip().lower()
        if hl in ("band", "channel_a", "channel_b"):
            continue
        if hl:
            return h
    return headers[-1] if headers else ""


_RE_RANGE = re.compile(r"^\s*([0-9]*\.?[0-9]+)\s*[-–]\s*([0-9]*\.?[0-9]+)\s*$")
_RE_DESC_RANGE = re.compile(r"from\s+([0-9]*\.?[0-9]+)\s+to\s+([0-9]*\.?[0-9]+)\s+hz", re.IGNORECASE)


def _infer_band_range(band_name: str, sidecar: Optional[Dict[str, Any]], *, value_key: str) -> Optional[Tuple[float, float]]:
    s = (band_name or "").strip()
    if not s:
        return None

    m = _RE_RANGE.match(s)
    if m:
        f1 = float(m.group(1))
        f2 = float(m.group(2))
        if f2 > f1 >= 0.0:
            return (f1, f2)

    key = s.strip().lower()
    if key in _DEFAULT_EEG_BANDS:
        return _DEFAULT_EEG_BANDS[key]

    # Try to parse from the timeseries sidecar JSON (qeeg_coherence_cli writes this).
    if isinstance(sidecar, dict) and value_key:
        ent = sidecar.get(value_key)
        if isinstance(ent, dict):
            desc = ent.get("Description")
            if isinstance(desc, str):
                m2 = _RE_DESC_RANGE.search(desc)
                if m2:
                    f1 = float(m2.group(1))
                    f2 = float(m2.group(2))
                    if f2 > f1 >= 0.0:
                        return (f1, f2)

    return None


def _polyline(points: Sequence[Tuple[float, float]]) -> str:
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points)


def _svg_line_chart(
    x: Sequence[float],
    y: Sequence[float],
    *,
    title: str,
    x_label: str,
    y_label: str,
    highlight_x: Optional[Tuple[float, float]] = None,
) -> str:
    if not x or not y or len(x) != len(y):
        return ""

    w, h = 980, 380
    pad_l, pad_r, pad_t, pad_b = 60, 18, 38, 44
    inner_w = w - pad_l - pad_r
    inner_h = h - pad_t - pad_b

    x_min = min(x)
    x_max = max(x)
    if not (math.isfinite(x_min) and math.isfinite(x_max)) or x_max <= x_min:
        x_min, x_max = 0.0, 1.0

    y_min, y_max = _finite_minmax([v for v in y if math.isfinite(v)])

    def x_of(xx: float) -> float:
        return pad_l + (xx - x_min) / (x_max - x_min) * inner_w

    def y_of(vv: float) -> float:
        return pad_t + (1.0 - (vv - y_min) / (y_max - y_min)) * inner_h

    pts: List[Tuple[float, float]] = []
    for xx, vv in zip(x, y):
        if math.isfinite(xx) and math.isfinite(vv):
            pts.append((x_of(xx), y_of(vv)))

    parts: List[str] = []
    parts.append(f'<svg viewBox="0 0 {w} {h}" class="ts">')
    parts.append(f'<text x="{pad_l}" y="22" class="ts-title">{_e(title)}</text>')
    parts.append(f'<rect x="{pad_l}" y="{pad_t}" width="{inner_w}" height="{inner_h}" class="frame" />')

    # Grid
    for k in range(5):
        yy = pad_t + k / 4.0 * inner_h
        parts.append(f'<line x1="{pad_l}" y1="{yy:.2f}" x2="{pad_l+inner_w}" y2="{yy:.2f}" class="grid" />')

    # Highlight region (e.g., band range on spectrum)
    if highlight_x is not None:
        hx0, hx1 = highlight_x
        if math.isfinite(hx0) and math.isfinite(hx1) and hx1 > hx0:
            # Clamp to axis range.
            hx0c = max(x_min, min(x_max, hx0))
            hx1c = max(x_min, min(x_max, hx1))
            if hx1c > hx0c:
                rx = x_of(hx0c)
                rw = x_of(hx1c) - rx
                parts.append(
                    f'<rect x="{rx:.2f}" y="{pad_t}" width="{rw:.2f}" height="{inner_h}" class="hl" />'
                )

    if pts:
        parts.append(f'<polyline points="{_polyline(pts)}" class="line" />')
    else:
        parts.append(f'<text x="{pad_l + 12}" y="{pad_t + 18}" class="warn">No finite samples</text>')

    # Labels
    parts.append(f'<text x="{pad_l}" y="{pad_t+inner_h+30}" class="axis-label">{_e(x_label)}</text>')
    parts.append(
        f'<text x="{pad_l}" y="{pad_t-10}" class="axis-label">{_e(f"{y_label} (min={y_min:.4g}, max={y_max:.4g})")}</text>'
    )
    parts.append(
        f'<text x="{pad_l+inner_w}" y="{pad_t+inner_h+30}" text-anchor="end" class="axis-label">{_e(f"{x_max:.2f}")}</text>'
    )
    parts.append("</svg>")
    return "".join(parts)


def _render_run_meta_card(run_meta: Optional[Dict[str, Any]]) -> str:
    if not isinstance(run_meta, dict):
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
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(run_meta.get(k)))}</code></td></tr>")
            seen.add(k)

    extra = 0
    for k, v in run_meta.items():
        if k in seen:
            continue
        if extra >= 10:
            break
        if isinstance(v, (str, int, float, bool)):
            rows.append(f"<tr><th>{_e(k)}</th><td><code>{_e(str(v))}</code></td></tr>")
            extra += 1

    if not rows:
        return ""

    return (
        '<div class="card">'
        '<h2>Run metadata</h2>'
        '<div class="note">Build/run metadata written by the CLI (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + "</table>"
        + "</div>"
    )


def _build_table(
    headers: Sequence[str],
    rows: Sequence[Dict[str, str]],
    *,
    max_rows: int,
    download_name: str,
) -> str:
    if not headers:
        return ""

    n = len(rows)
    max_rows = max(1, int(max_rows))
    idx = _downsample_indices(n, max_rows)

    ths = "".join(f'<th onclick="sortTable(this)" aria-sort="none">{_e(h)}</th>' for h in headers)
    body: List[str] = []
    for i in idx:
        r = rows[i]
        tds: List[str] = []
        for h in headers:
            v = r.get(h, "")
            # Attach a numeric attribute when it parses cleanly.
            num = _try_float(v)
            if math.isfinite(num):
                tds.append(f'<td data-num="{num:.12g}">{_e(v)}</td>')
            else:
                tds.append(f"<td>{_e(v)}</td>")
        body.append("<tr>" + "".join(tds) + "</tr>")

    sampled_note = ""
    if n > len(idx):
        sampled_note = f"<div class=\"muted\" style=\"font-size:12px;margin-top:6px\">Showing {len(idx)} sampled rows out of {n} (use the raw CSV for full data).</div>"

    return (
        '<div class="table-filter">'
        '<div class="table-controls">'
        '<input type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
        f'<button type="button" onclick="downloadTableCSV(this, \'{_e(download_name)}\', true)">Download CSV</button>'
        '<span class="filter-count muted"></span>'
        '</div>'
        '<div class="table-wrap">'
        '<table class="data-table sticky">'
        f"<thead><tr>{ths}</tr></thead>"
        "<tbody>"
        + "".join(body)
        + "</tbody></table></div>"
        + sampled_note
        + "</div>"
    )


def _safe_mean(values: Sequence[float]) -> float:
    vals = [v for v in values if math.isfinite(v)]
    if not vals:
        return math.nan
    return float(statistics.mean(vals))


def _safe_std(values: Sequence[float]) -> float:
    vals = [v for v in values if math.isfinite(v)]
    if len(vals) < 2:
        return math.nan
    try:
        return float(statistics.pstdev(vals))
    except Exception:
        return math.nan


def _parse_xy_from_csv(
    path: str,
    *,
    x_col_candidates: Sequence[str],
    y_col_candidates: Sequence[str],
    max_points: int,
) -> Tuple[List[str], List[Dict[str, str]], List[float], List[float], str, str]:
    headers, rows = _read_csv(path)
    if not headers:
        return headers, rows, [], [], "", ""

    # pick columns
    x_col = ""
    for c in x_col_candidates:
        if c in headers:
            x_col = c
            break
    if not x_col:
        x_col = headers[0]

    y_col = ""
    for c in y_col_candidates:
        if c in headers:
            y_col = c
            break
    if not y_col:
        # pick the first non-x column
        for h in headers:
            if h != x_col:
                y_col = h
                break
    if not y_col:
        y_col = headers[-1]

    xs: List[float] = []
    ys: List[float] = []
    for r in rows:
        xx = _try_float(r.get(x_col, ""))
        yy = _try_float(r.get(y_col, ""))
        if math.isfinite(xx) and math.isfinite(yy):
            xs.append(xx)
            ys.append(yy)

    # Downsample for plotting
    idx = _downsample_indices(len(xs), max(50, int(max_points)))
    xs2 = [xs[i] for i in idx] if idx else xs
    ys2 = [ys[i] for i in idx] if idx else ys
    return headers, rows, xs2, ys2, x_col, y_col


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render pair-mode connectivity outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to an output directory containing *_band.csv, or the *_band.csv itself.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/connectivity_pair_report.html).")
    ap.add_argument("--title", default="Connectivity pair report", help="Report title.")
    ap.add_argument(
        "--max-chart-points",
        type=int,
        default=2000,
        help="Max points to plot in charts (default: 2000).",
    )
    ap.add_argument(
        "--max-table-rows",
        type=int,
        default=600,
        help="Max rows to include per table (sampled) (default: 600).",
    )
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir = _guess_outdir(args.input)
    runs = _find_pair_runs(outdir)
    if not runs:
        raise SystemExit(
            "Could not find any *_band.csv pair-mode connectivity outputs under: "
            + outdir
            + "\nExpected one of: "
            + ", ".join([f"{s}_band.csv" for s in _MEASURE_STEMS])
        )

    html_path = args.out
    if html_path is None:
        html_path = os.path.join(outdir, "connectivity_pair_report.html")
    html_path = os.path.abspath(html_path)

    out_dir_for_links = os.path.dirname(html_path) or "."
    now = utc_now_iso()

    run_meta = _find_run_meta(outdir)
    run_meta_card = _render_run_meta_card(run_meta)

    # Build sections for each measure stem found.
    sections: List[str] = []

    for run in runs:
        band_headers, band_rows = _read_csv(run.band_csv)
        if not band_rows:
            continue

        # Try to find the typical identifiers.
        band_col = "band" if "band" in band_headers else (band_headers[0] if band_headers else "band")
        cha_col = "channel_a" if "channel_a" in band_headers else "channel_a"
        chb_col = "channel_b" if "channel_b" in band_headers else "channel_b"
        val_col = _pick_value_col(band_headers)

        # Prefer the first row for summary.
        r0 = band_rows[0]
        band_name = r0.get(band_col, "")
        ch_a = r0.get(cha_col, "")
        ch_b = r0.get(chb_col, "")
        v0 = _try_float(r0.get(val_col, ""))

        sidecar = _read_json_if_exists(run.timeseries_json) if run.timeseries_json else None
        band_range = _infer_band_range(band_name, sidecar if isinstance(sidecar, dict) else None, value_key=val_col)

        # Links
        rel_band = _posix_relpath(run.band_csv, out_dir_for_links)
        links = [f'<a href="{_e(rel_band)}">{_e(os.path.basename(run.band_csv))}</a>']
        if run.spectrum_csv:
            rel_sp = _posix_relpath(run.spectrum_csv, out_dir_for_links)
            links.append(f'<a href="{_e(rel_sp)}">{_e(os.path.basename(run.spectrum_csv))}</a>')
        if run.timeseries_csv:
            rel_ts = _posix_relpath(run.timeseries_csv, out_dir_for_links)
            links.append(f'<a href="{_e(rel_ts)}">{_e(os.path.basename(run.timeseries_csv))}</a>')
        if run.timeseries_json:
            rel_tsj = _posix_relpath(run.timeseries_json, out_dir_for_links)
            links.append(f'<a href="{_e(rel_tsj)}">{_e(os.path.basename(run.timeseries_json))}</a>')

        # Stats cards (timeseries)
        ts_chart = ""
        ts_stats_html = ""
        ts_table_html = ""

        if run.timeseries_csv and os.path.exists(run.timeseries_csv):
            ts_headers, ts_rows, t, y, t_col, y_col = _parse_xy_from_csv(
                run.timeseries_csv,
                x_col_candidates=("t_end_sec", "t_sec", "time_sec", "t"),
                y_col_candidates=(val_col, run.stem, "value"),
                max_points=int(args.max_chart_points),
            )

            if t and y:
                ts_chart = _svg_line_chart(
                    t,
                    y,
                    title=f"{run.stem}: time series",
                    x_label=f"{t_col} (s)",
                    y_label=y_col,
                )

                ts_mean = _safe_mean(y)
                ts_std = _safe_std(y)
                ts_min, ts_max = _finite_minmax([vv for vv in y if math.isfinite(vv)])

                ts_stats_html = (
                    '<div class="stats">'
                    f'<div class="stat"><div class="num">{len(y)}</div><div class="muted">samples (finite)</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{ts_mean:.4g}" if math.isfinite(ts_mean) else "NaN")}</div><div class="muted">mean</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{ts_std:.4g}" if math.isfinite(ts_std) else "NaN")}</div><div class="muted">std</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{ts_min:.4g}")}</div><div class="muted">min</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{ts_max:.4g}")}</div><div class="muted">max</div></div>'
                    '</div>'
                )

            ts_table_html = _build_table(
                ts_headers,
                ts_rows,
                max_rows=int(args.max_table_rows),
                download_name=f"{run.stem}_timeseries_table_filtered.csv",
            )

        sp_chart = ""
        sp_stats_html = ""
        sp_table_html = ""
        if run.spectrum_csv and os.path.exists(run.spectrum_csv):
            sp_headers, sp_rows, fx, fy, fx_col, fy_col = _parse_xy_from_csv(
                run.spectrum_csv,
                x_col_candidates=("freq_hz", "f_hz", "freq"),
                y_col_candidates=(val_col, run.stem, "value"),
                max_points=int(args.max_chart_points),
            )
            if fx and fy:
                hl = band_range
                sp_chart = _svg_line_chart(
                    fx,
                    fy,
                    title=f"{run.stem}: spectrum",
                    x_label=f"{fx_col} (Hz)",
                    y_label=fy_col,
                    highlight_x=hl,
                )

                sp_mean = _safe_mean(fy)
                sp_std = _safe_std(fy)
                sp_min, sp_max = _finite_minmax([vv for vv in fy if math.isfinite(vv)])
                sp_stats_html = (
                    '<div class="stats">'
                    f'<div class="stat"><div class="num">{len(fy)}</div><div class="muted">samples (finite)</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{sp_mean:.4g}" if math.isfinite(sp_mean) else "NaN")}</div><div class="muted">mean</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{sp_std:.4g}" if math.isfinite(sp_std) else "NaN")}</div><div class="muted">std</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{sp_min:.4g}")}</div><div class="muted">min</div></div>'
                    f'<div class="stat"><div class="num">{_e(f"{sp_max:.4g}")}</div><div class="muted">max</div></div>'
                    '</div>'
                )

            sp_table_html = _build_table(
                sp_headers,
                sp_rows,
                max_rows=int(args.max_table_rows),
                download_name=f"{run.stem}_spectrum_table_filtered.csv",
            )

        # Band table
        band_table_html = _build_table(
            band_headers,
            band_rows,
            max_rows=max(10, min(int(args.max_table_rows), 2000)),
            download_name=f"{run.stem}_band_table_filtered.csv",
        )

        # Summary line
        summary_bits: List[str] = []
        if ch_a and ch_b:
            summary_bits.append(f"pair: <code>{_e(ch_a)}</code> ↔ <code>{_e(ch_b)}</code>")
        if band_name:
            summary_bits.append(f"band: <code>{_e(band_name)}</code>")
        if math.isfinite(v0):
            summary_bits.append(f"value: <code>{_e(f'{v0:.6g}')}</code>")
        if band_range is not None:
            summary_bits.append(f"range: <code>{_e(f'{band_range[0]:.4g}-{band_range[1]:.4g} Hz')}</code>")

        sections.append(
            '<div class="card">'
            f'<h2>{_e(run.stem)} pair</h2>'
            '<div class="note">'
            + (" · ".join(summary_bits) if summary_bits else "")
            + '<div style="margin-top:6px">Files: '
            + " · ".join(links)
            + "</div>"
            + "</div>"
            + band_table_html
            + (f'<h3 style="margin-top:14px">Spectrum</h3>{sp_chart}{sp_stats_html}{sp_table_html}' if (sp_chart or sp_table_html) else "")
            + (f'<h3 style="margin-top:14px">Time series</h3>{ts_chart}{ts_stats_html}{ts_table_html}' if (ts_chart or ts_table_html) else "")
            + "</div>"
        )

    if not sections:
        raise SystemExit(f"Found *_band.csv under {outdir}, but could not parse any rows.")

    css = BASE_CSS + r"""
.kv { width: 100%; border-collapse: collapse; font-size: 13px; }
.kv th, .kv td { border-bottom: 1px solid rgba(255,255,255,0.08); padding: 8px; text-align: left; vertical-align: top; }
.kv th { width: 240px; color: #d7e4ff; background: #0f1725; }

svg.ts { width: 100%; height: auto; display: block; margin: 10px 0 6px; }
.frame { fill: rgba(255,255,255,0.02); stroke: rgba(255,255,255,0.12); stroke-width: 1; }
.grid { stroke: rgba(255,255,255,0.08); stroke-width: 1; }
.line { fill: none; stroke: var(--accent); stroke-width: 2; }
.hl { fill: rgba(255,184,107,0.18); stroke: rgba(255,184,107,0.24); }
.axis-label { fill: var(--muted); font-size: 12px; }
.ts-title { fill: #d7e4ff; font-size: 14px; font-weight: 700; }
.warn { fill: var(--warn); font-size: 12px; }
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
  <div class=\"meta\">Generated {now} — source <code>{_e(os.path.abspath(args.input))}</code></div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      Pair-mode connectivity summary visualization for <code>qeeg_coherence_cli --pair</code> and
      <code>qeeg_plv_cli --pair</code> outputs.
      Charts are downsampled for readability; tables are sampled to keep the HTML size reasonable.
    </div>
  </div>

  {run_meta_card}

  {''.join(sections)}

  <div class=\"footer\">Generated by scripts/render_connectivity_pair_report.py (stdlib only).</div>
</main>
<script>{JS_SORT_TABLE}</script>
</body>
</html>
"""

    os.makedirs(os.path.dirname(html_path) or ".", exist_ok=True)
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {html_path}")

    if args.open:
        try:
            webbrowser.open_new_tab("file://" + os.path.abspath(html_path))
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
