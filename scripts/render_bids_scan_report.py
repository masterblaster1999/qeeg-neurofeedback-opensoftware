#!/usr/bin/env python3
"""Render qeeg_bids_scan_cli outputs into a self-contained HTML report.

This script is intentionally dependency-free (Python stdlib only) so it can run
in minimal environments (CI artifacts, lab workstations, offline bundles).

Inputs (written under --outdir by qeeg_bids_scan_cli):
  - bids_index.csv (required)
  - bids_index.json (optional; richer structured summary)
  - bids_scan_report.txt (optional; embedded verbatim)
  - bids_scan_run_meta.json (optional; build/run metadata)

Typical usage:

  # Pass an output directory produced by qeeg_bids_scan_cli
  python3 scripts/render_bids_scan_report.py --input out_bids_scan

  # Or pass the CSV directly
  python3 scripts/render_bids_scan_report.py --input out_bids_scan/bids_index.csv

The generated HTML is self-contained (inline CSS + JS; no network requests) and
safe to open locally.

Notes:
- This is a convenience visualization for research/educational inspection only.
- It is not a medical device and is not intended for clinical decision-making.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import statistics
import webbrowser
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e as _e,
    is_dir as _is_dir,
    read_csv_dict as _read_csv,
    read_json_if_exists as _read_json_if_exists,
    read_text_if_exists as _read_text_if_exists,
    try_bool_int as _try_bool_int,
    utc_now_iso,
)


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str, Optional[str], Optional[str], Optional[str]]:
    """Return (outdir, csv_path, html_path, index_json_path, report_txt_path, run_meta_path)."""
    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        csv_path = os.path.join(outdir, "bids_index.csv")
    else:
        p = os.path.abspath(inp)
        outdir = os.path.dirname(p) or "."
        if os.path.basename(p).lower().endswith(".csv"):
            csv_path = p
        else:
            csv_path = os.path.join(outdir, "bids_index.csv")

    if out is None:
        out = os.path.join(outdir, "bids_scan_report.html")

    index_json = os.path.join(outdir, "bids_index.json")
    report_txt = os.path.join(outdir, "bids_scan_report.txt")
    run_meta = os.path.join(outdir, "bids_scan_run_meta.json")

    return (
        outdir,
        os.path.abspath(csv_path),
        os.path.abspath(out),
        index_json if os.path.exists(index_json) else None,
        report_txt if os.path.exists(report_txt) else None,
        run_meta if os.path.exists(run_meta) else None,
    )


def _count_issue_kinds(issues: str) -> Tuple[int, int]:
    """Return (warn_count, error_count) by scanning the issues string."""
    if not issues:
        return 0, 0
    s = str(issues)
    # The CLI prefixes issues with "[WARN] " / "[ERROR] ". Be tolerant to case.
    u = s.upper()
    return u.count("[WARN]"), u.count("[ERROR]")


def _median(values: List[float]) -> float:
    try:
        return float(statistics.median(values))
    except Exception:
        if not values:
            return 0.0
        values = sorted(values)
        n = len(values)
        if n % 2 == 1:
            return float(values[n // 2])
        return float(0.5 * (values[n // 2 - 1] + values[n // 2]))


def _svg_sidecar_coverage(counts: Dict[str, Tuple[int, int]]) -> str:
    """Return a compact horizontal bar chart for sidecar coverage.

    counts maps key -> (present, total).
    """
    keys = list(counts.keys())
    if not keys:
        return ""

    w = 920
    row_h = 22
    pad_l, pad_r, pad_t, pad_b = 190, 24, 18, 22
    h = pad_t + pad_b + row_h * len(keys)

    def pct(p: int, t: int) -> float:
        return 0.0 if t <= 0 else max(0.0, min(1.0, p / float(t)))

    inner_w = w - pad_l - pad_r
    out: List[str] = []
    out.append(f'<svg viewBox="0 0 {w} {h}" width="100%" height="{h}" role="img" aria-label="Sidecar coverage">')
    out.append(
        f'<text x="{pad_l}" y="{pad_t - 4}" fill="#9fb0c8" font-size="12" text-anchor="start">present / total</text>'
    )

    for i, k in enumerate(keys):
        present, total = counts[k]
        y = pad_t + i * row_h
        frac = pct(present, total)
        bw = inner_w * frac
        # background bar
        out.append(f'<rect x="{pad_l}" y="{y}" width="{inner_w}" height="{row_h - 6}" rx="6" fill="#0f1725" />')
        # filled bar
        out.append(
            f'<rect x="{pad_l}" y="{y}" width="{bw}" height="{row_h - 6}" rx="6" fill="#8fb7ff" opacity="0.85" />'
        )
        out.append(
            f'<text x="{pad_l - 10}" y="{y + 12}" fill="#e7eefc" font-size="12" text-anchor="end">{_e(k)}</text>'
        )
        out.append(
            f'<text x="{pad_l + inner_w + 10}" y="{y + 12}" fill="#9fb0c8" font-size="12" text-anchor="start">{present}/{total}</text>'
        )

    out.append("</svg>")
    return "".join(out)


def _html_doc(
    *,
    outdir: str,
    csv_path: str,
    html_path: str,
    index_json: Optional[Dict[str, Any]],
    report_txt: Optional[str],
    run_meta: Optional[Dict[str, Any]],
    headers: List[str],
    rows: List[Dict[str, str]],
) -> str:
    now = utc_now_iso()

    dataset_root = ""
    global_warnings: List[str] = []
    global_errors: List[str] = []
    if index_json:
        dataset_root = str(index_json.get("DatasetRoot", "") or "")
        ws = index_json.get("Warnings")
        es = index_json.get("Errors")
        if isinstance(ws, list):
            global_warnings = [str(x) for x in ws if str(x).strip()]
        if isinstance(es, list):
            global_errors = [str(x) for x in es if str(x).strip()]

    # Compute derived per-row stats.
    n = len(rows)
    warn_counts: List[int] = []
    err_counts: List[int] = []
    missing_sidecar_counts: List[int] = []

    # Sidecar presence columns expected from qeeg_bids_scan_cli.
    sidecar_cols = [
        ("eeg_json", "eeg.json"),
        ("channels_tsv", "channels.tsv"),
        ("events_tsv", "events.tsv"),
        ("events_json", "events.json"),
        ("electrodes_tsv", "electrodes.tsv"),
        ("coordsystem_json", "coordsystem.json"),
    ]
    sidecar_counts: Dict[str, Tuple[int, int]] = {}
    for key, _label in sidecar_cols:
        present = 0
        for r in rows:
            present += _try_bool_int(r.get(key, ""), default=0)
        sidecar_counts[key] = (present, n)

    # Build recording table rows (with derived columns appended).
    tbody_rows: List[str] = []
    for r in rows:
        issues = r.get("issues", "")
        wcnt, ecnt = _count_issue_kinds(issues)
        warn_counts.append(wcnt)
        err_counts.append(ecnt)

        miss = 0
        for key, _label in sidecar_cols:
            miss += 1 - _try_bool_int(r.get(key, ""), default=0)
        missing_sidecar_counts.append(miss)

        # Build cells in a stable order.
        def cell(name: str) -> str:
            v = r.get(name, "")
            # Display 1/0 sidecars as ✓ / ✕ but preserve raw value in data-csv.
            if name in {c[0] for c in sidecar_cols}:
                b = _try_bool_int(v, default=0)
                sym = "✓" if b else "✕"
                cls = "ok" if b else "bad"
                return f'<td data-num="{b}" data-csv="{_e(str(b))}"><span class="yn {cls}">{sym}</span></td>'
            if name == "issues":
                # Keep the raw issue text searchable.
                short = v
                return f'<td class="muted" data-csv="{_e(v)}">{_e(short)}</td>'
            return f'<td data-csv="{_e(v)}">{_e(v)}</td>'

        base_order = [
            "path",
            "format",
            "sub",
            "ses",
            "task",
            "acq",
            "run",
        ] + [k for k, _ in sidecar_cols] + ["issues"]

        cells = [cell(h) for h in base_order]
        cells.append(f'<td data-num="{wcnt}" data-csv="{wcnt}">{wcnt}</td>')
        cells.append(f'<td data-num="{ecnt}" data-csv="{ecnt}">{ecnt}</td>')
        cells.append(f'<td data-num="{miss}" data-csv="{miss}">{miss}</td>')

        severity = "ok"
        if ecnt > 0:
            severity = "error"
        elif wcnt > 0 or miss > 0:
            severity = "warn"
        sev_badge = f'<span class="sev {severity}">{_e(severity)}</span>'

        tbody_rows.append("<tr>" + f"<td data-csv=\"{_e(severity)}\">{sev_badge}</td>" + "".join(cells) + "</tr>")

    n_with_err = sum(1 for c in err_counts if c > 0)
    n_with_warn = sum(1 for c in warn_counts if c > 0)
    # Consider missing sidecars as "incomplete" (not necessarily a warning).
    n_incomplete = sum(1 for c in missing_sidecar_counts if c > 0)
    median_missing = _median([float(x) for x in missing_sidecar_counts]) if missing_sidecar_counts else 0.0

    # If the JSON wasn't present, try to derive global warnings/errors from per-row issues.
    if not global_warnings and not global_errors:
        # Keep it small; a full expansion is still available in the table.
        for r in rows:
            issues = str(r.get("issues", "") or "")
            parts = [p.strip() for p in issues.split("|") if p.strip()]
            for p in parts:
                if p.upper().startswith("[ERROR]"):
                    global_errors.append(p)
                elif p.upper().startswith("[WARN]"):
                    global_warnings.append(p)

        # Deduplicate while preserving order.
        def _uniq(xs: List[str]) -> List[str]:
            seen = set()
            out: List[str] = []
            for x in xs:
                if x in seen:
                    continue
                seen.add(x)
                out.append(x)
            return out

        global_warnings = _uniq(global_warnings)[:30]
        global_errors = _uniq(global_errors)[:30]

    # Sidecar coverage chart and small table.
    sidecar_labels = {
        "eeg_json": "eeg.json",
        "channels_tsv": "channels.tsv",
        "events_tsv": "events.tsv",
        "events_json": "events.json",
        "electrodes_tsv": "electrodes.tsv",
        "coordsystem_json": "coordsystem.json",
    }
    sidecar_counts_pretty: Dict[str, Tuple[int, int]] = {
        sidecar_labels.get(k, k): v for k, v in sidecar_counts.items()
    }
    svg_cov = _svg_sidecar_coverage(sidecar_counts_pretty)

    cov_rows: List[str] = []
    for label, (present, total) in sidecar_counts_pretty.items():
        missing = max(0, total - present)
        pct = 0.0 if total <= 0 else 100.0 * (present / float(total))
        cov_rows.append(
            "<tr>"
            f"<td>{_e(label)}</td>"
            f"<td data-num=\"{present}\">{present}</td>"
            f"<td data-num=\"{missing}\">{missing}</td>"
            f"<td data-num=\"{pct:.3f}\"><code>{pct:.1f}%</code></td>"
            "</tr>"
        )

    warn_list = "".join(f"<li>{_e(x)}</li>" for x in global_warnings)
    err_list = "".join(f"<li>{_e(x)}</li>" for x in global_errors)

    report_txt_html = ""
    if report_txt:
        report_txt_html = (
            '<div class="card">'
            '<h2>CLI text report</h2>'
            '<pre>'
            + _e(report_txt)
            + '</pre>'
            '</div>'
        )

    run_meta_html = ""
    if run_meta:
        import json

        run_meta_html = (
            '<div class="card">'
            '<h2>Run metadata</h2>'
            '<pre>'
            + _e(json.dumps(run_meta, indent=2, sort_keys=True))
            + '</pre>'
            '</div>'
        )

    # Build the main recordings table header.
    ths: List[str] = []
    ths.append('<th onclick="sortTable(this)" aria-sort="none">severity</th>')
    ths.extend(
        [
            '<th onclick="sortTable(this)" aria-sort="none">path</th>',
            '<th onclick="sortTable(this)" aria-sort="none">format</th>',
            '<th onclick="sortTable(this)" aria-sort="none">sub</th>',
            '<th onclick="sortTable(this)" aria-sort="none">ses</th>',
            '<th onclick="sortTable(this)" aria-sort="none">task</th>',
            '<th onclick="sortTable(this)" aria-sort="none">acq</th>',
            '<th onclick="sortTable(this)" aria-sort="none">run</th>',
        ]
    )
    for k, _label in sidecar_cols:
        ths.append(f'<th onclick="sortTable(this)" aria-sort="none">{_e(k)}</th>')
    ths.append('<th onclick="sortTable(this)" aria-sort="none">issues</th>')
    ths.append('<th onclick="sortTable(this)" aria-sort="none">warn_count</th>')
    ths.append('<th onclick="sortTable(this)" aria-sort="none">error_count</th>')
    ths.append('<th onclick="sortTable(this)" aria-sort="none">missing_sidecars</th>')

    css = BASE_CSS + r"""
.data-table { min-width: 1180px; }

.sev {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 999px;
  font-size: 12px;
  border: 1px solid var(--grid);
}
.sev.ok { color: var(--ok); border-color: rgba(127,179,255,0.6); }
.sev.warn { color: var(--warn); border-color: rgba(255,184,107,0.6); }
.sev.error { color: var(--bad); border-color: rgba(255,127,163,0.6); }

.yn {
  display: inline-block;
  width: 18px;
  text-align: center;
  font-weight: 700;
}
.yn.ok { color: var(--ok); }
.yn.bad { color: var(--bad); }

.cols {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 14px;
}
@media (max-width: 980px) { .cols { grid-template-columns: 1fr; } }

.issues-list {
  margin: 0;
  padding-left: 18px;
}
.issues-list li { margin: 5px 0; }
"""

    rel_csv = os.path.relpath(csv_path, outdir).replace(os.sep, "/")
    rel_html = os.path.relpath(html_path, outdir).replace(os.sep, "/")

    subtitle_bits: List[str] = []
    if dataset_root:
        subtitle_bits.append(f"Dataset root: <code>{_e(dataset_root)}</code>")
    subtitle_bits.append(f"Input: <code>{_e(rel_csv)}</code>")
    subtitle_bits.append(f"Output: <code>{_e(rel_html)}</code>")
    subtitle_bits.append(f"Generated: <code>{_e(now)}</code>")
    subtitle = " · ".join(subtitle_bits)

    # Summary stats card.
    stats_html = (
        '<div class="stats">'
        f'<div class="stat"><div class="num">{n}</div><div class="lbl">recordings</div></div>'
        f'<div class="stat"><div class="num">{n_with_err}</div><div class="lbl">recordings with errors</div></div>'
        f'<div class="stat"><div class="num">{n_with_warn}</div><div class="lbl">recordings with warnings</div></div>'
        f'<div class="stat"><div class="num">{n_incomplete}</div><div class="lbl">recordings missing sidecars</div></div>'
        f'<div class="stat"><div class="num">{int(median_missing)}</div><div class="lbl">median missing sidecars</div></div>'
        '</div>'
    )

    # Issues cards.
    issues_cards = "".join(
        [
            '<div class="cols">',
            '<div class="card">',
            f'<h2>Global errors <span class="muted" style="font-size:12px;">({len(global_errors)})</span></h2>',
            (f'<ul class="issues-list">{err_list}</ul>' if global_errors else '<div class="note">None.</div>'),
            '</div>',
            '<div class="card">',
            f'<h2>Global warnings <span class="muted" style="font-size:12px;">({len(global_warnings)})</span></h2>',
            (f'<ul class="issues-list">{warn_list}</ul>' if global_warnings else '<div class="note">None.</div>'),
            '</div>',
            '</div>',
        ]
    )

    # Sidecar coverage section.
    cov_table = (
        '<div class="table-wrap">'
        '<table class="data-table">'
        '<thead><tr>'
        '<th onclick="sortTable(this)" aria-sort="none">sidecar</th>'
        '<th onclick="sortTable(this)" aria-sort="none">present</th>'
        '<th onclick="sortTable(this)" aria-sort="none">missing</th>'
        '<th onclick="sortTable(this)" aria-sort="none">coverage</th>'
        '</tr></thead>'
        '<tbody>'
        + "".join(cov_rows)
        + '</tbody></table>'
        '</div>'
    )
    cov_html = "".join(
        [
            '<div class="card">',
            '<h2>Sidecar coverage</h2>',
            '<div class="note">These columns come from <code>bids_index.csv</code>. Missing sidecars may be expected for some datasets, but are often useful for debugging BIDS compliance.</div>',
            (svg_cov if svg_cov else ""),
            cov_table,
            '</div>',
        ]
    )

    # Main recordings table.
    table_html = "".join(
        [
            '<div class="card">',
            '<h2>Recordings</h2>',
            '<div class="table-filter">',
            '<div class="table-controls">',
            '<input type="search" placeholder="Filter rows… (e.g., sub:01 -WARN error_count>0)" oninput="filterTable(this)" />',
            '<button type="button" onclick="downloadTableCSV(this, \'bids_index_filtered.csv\', true)">Download CSV</button>',
            '<span class="filter-count muted"></span>',
            '</div>',
            '<div class="table-wrap">',
            '<table class="data-table sticky">',
            f'<thead><tr>{"".join(ths)}</tr></thead>',
            '<tbody>',
            "".join(tbody_rows),
            '</tbody></table>',
            '</div>',
            '</div>',
            '<div class="footer">',
            'Tips: click headers to sort. Use column filters like <code>sub=01</code>, <code>events_tsv=0</code>, <code>error_count>0</code>, and exclude tokens with <code>-token</code>.',
            '</div>',
            '</div>',
        ]
    )

    # Minimal overall page wrapper (mirrors other reports).
    html_doc = "".join(
        [
            '<!doctype html>',
            '<html lang="en">',
            '<head>',
            '<meta charset="utf-8" />',
            '<meta name="viewport" content="width=device-width, initial-scale=1" />',
            '<title>qEEG BIDS scan report</title>',
            f'<style>{css}</style>',
            f'<script>{JS_SORT_TABLE}</script>',
            '</head>',
            '<body>',
            '<header>',
            '<h1>qEEG BIDS scan report</h1>',
            f'<div class="meta">{subtitle}</div>',
            '</header>',
            '<main>',
            '<div class="card">',
            '<h2>Summary</h2>',
            stats_html,
            '</div>',
            issues_cards,
            cov_html,
            table_html,
            report_txt_html,
            run_meta_html,
            '<div class="footer">Generated by <code>scripts/render_bids_scan_report.py</code> (stdlib only). No network requests. For research/educational inspection only.</div>',
            '</main>',
            '</body></html>',
        ]
    )

    return html_doc


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_bids_scan_cli outputs into a standalone HTML report.")
    ap.add_argument(
        "--input",
        required=True,
        help="Input directory from qeeg_bids_scan_cli (or bids_index.csv path).",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="Output HTML path (default: <outdir>/bids_scan_report.html).",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated HTML in your default browser.",
    )

    args = ap.parse_args(list(argv) if argv is not None else None)
    outdir, csv_path, html_path, index_json_path, report_txt_path, run_meta_path = _guess_paths(args.input, args.out)

    if not os.path.exists(csv_path):
        raise SystemExit(f"Missing required input CSV: {csv_path}")

    headers, rows = _read_csv(csv_path)
    index_json = _read_json_if_exists(index_json_path) if index_json_path else None
    report_txt = _read_text_if_exists(report_txt_path) if report_txt_path else None
    run_meta = _read_json_if_exists(run_meta_path) if run_meta_path else None

    html_doc = _html_doc(
        outdir=outdir,
        csv_path=csv_path,
        html_path=html_path,
        index_json=index_json,
        report_txt=report_txt,
        run_meta=run_meta,
        headers=headers,
        rows=rows,
    )

    pathlib.Path(html_path).parent.mkdir(parents=True, exist_ok=True)
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {html_path}")

    if args.open:
        try:
            webbrowser.open("file://" + os.path.abspath(html_path))
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
