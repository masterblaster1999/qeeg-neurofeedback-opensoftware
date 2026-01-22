#!/usr/bin/env python3
"""Generate an HTML dashboard for QEEG CLI outputs (dependency-free).

This script scans one or more directories *recursively* looking for well-known
output files produced by this repo's CLIs, and then generates a single
dashboard HTML that links to per-run HTML reports.

By default it will also (re)generate any missing reports using:

  - scripts/render_quality_report.py
  - scripts/render_trace_plot_report.py
  - scripts/render_bandpowers_report.py
  - scripts/render_bandratios_report.py
  - scripts/render_spectral_features_report.py
  - scripts/render_pac_report.py
  - scripts/render_epoch_report.py
  - scripts/render_nf_feedback_report.py
  - scripts/render_connectivity_report.py
  - scripts/render_microstates_report.py
  - scripts/render_iaf_report.py
  - scripts/render_bids_scan_report.py
  - scripts/render_channel_qc_report.py
  - scripts/render_artifacts_report.py

Recognized outputs:

  - Quality:      quality_report.json / line_noise_per_channel.csv (qeeg_quality_cli)
  - Trace plot:   trace_plot_run_meta.json (qeeg_trace_plot_cli)
  - Bandpowers:   bandpowers.csv
  - Band ratios:  bandratios.csv
  - Spectral features: spectral_features.csv
  - Neurofeedback: nf_feedback.csv
  - PAC:          pac_timeseries.csv (qeeg_pac_cli)
  - Epochs:       epoch_bandpowers_summary.csv / epoch_bandpowers.csv (qeeg_epoch_cli)
  - Connectivity: coherence_pairs.csv / imcoh_pairs.csv / plv_pairs.csv / etc
                   and/or <measure>_matrix_<band>.csv (matrix mode)
  - Microstates:  microstate_state_stats.csv (qeeg_microstates_cli)
  - IAF:          iaf_by_channel.csv (qeeg_iaf_cli)
  - BIDS scan:    bids_index.csv / bids_index.json (qeeg_bids_scan_cli)
  - Channel QC:   channel_qc.csv (qeeg_channel_qc_cli)
  - Artifacts:    artifact_windows.csv / artifact_segments.csv (qeeg_artifacts_cli)

Examples:

  # Scan a study folder containing out_* subfolders, render missing reports,
  # and write qeeg_reports_dashboard.html in the study folder.
  python3 scripts/render_reports_dashboard.py ./study_folder

  # Scan multiple roots and write dashboard to an explicit path.
  python3 scripts/render_reports_dashboard.py out_bp out_nf out_conn --out dashboard.html

  # Only link to reports if they already exist (do not render).
  python3 scripts/render_reports_dashboard.py . --no-render
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import re
import sys
import webbrowser
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from report_common import BASE_CSS, JS_SORT_TABLE, e as _e, posix_relpath as _posix_relpath, utc_now_iso


# ---- Detection -------------------------------------------------------------

_KNOWN_CONN_PAIR_FILES: Set[str] = {
    "coherence_pairs.csv",
    "imcoh_pairs.csv",
    "plv_pairs.csv",
    "pli_pairs.csv",
    "wpli_pairs.csv",
    "wpli2_debiased_pairs.csv",
}

_CONN_MATRIX_RE = re.compile(
    r"^(coherence|imcoh|plv|pli|wpli|wpli2_debiased)_matrix_.+\\.csv$",
    re.IGNORECASE,
)

_ARTIFACT_HINT_FILES: Set[str] = {
    "artifact_windows.csv",
    "artifact_segments.csv",
    "artifact_run_meta.json",
}

_BIDS_SCAN_HINT_FILES: Set[str] = {
    "bids_index.csv",
    "bids_index.json",
    "bids_scan_report.txt",
    "bids_scan_run_meta.json",
}

_QUALITY_HINT_FILES: Set[str] = {
    "quality_report.json",
    "line_noise_per_channel.csv",
    "quality_summary.txt",
    "quality_run_meta.json",
}

_TRACE_PLOT_HINT_FILES: Set[str] = {
    "trace_plot_run_meta.json",
    "trace_plot_meta.txt",
    # The SVG filename is configurable, so we avoid hard-coding "traces.svg".
}


def _walk_dirs(roots: Sequence[str]) -> Iterable[Tuple[str, List[str]]]:
    """Yield (dirpath, filenames) for every directory under each root."""
    for root in roots:
        for dirpath, _dirnames, filenames in os.walk(root):
            yield dirpath, filenames


def _looks_like_connectivity(filenames: Sequence[str]) -> bool:
    s = set(filenames)
    if any(name in s for name in _KNOWN_CONN_PAIR_FILES):
        return True
    return any(_CONN_MATRIX_RE.match(name or "") for name in filenames)


# ---- Rendering -------------------------------------------------------------

@dataclass(frozen=True)
class ReportItem:
    kind: str
    outdir: str
    report_path: str
    status: str  # ok | skipped | error
    message: str = ""


def _try_import_renderers() -> Dict[str, object]:
    """Try to import renderer modules from the scripts directory.

    Returns a dict of {kind -> module} for the kinds it could load.
    """
    mods: Dict[str, object] = {}

    try:
        import render_quality_report as _qr  # type: ignore
        mods["quality"] = _qr
    except Exception:
        pass

    try:
        import render_trace_plot_report as _tr  # type: ignore
        mods["trace_plot"] = _tr
    except Exception:
        pass

    try:
        import render_bandpowers_report as _bp  # type: ignore
        mods["bandpowers"] = _bp
    except Exception:
        pass

    try:
        import render_bandratios_report as _br  # type: ignore
        mods["bandratios"] = _br
    except Exception:
        pass

    try:
        import render_nf_feedback_report as _nf  # type: ignore
        mods["nf"] = _nf
    except Exception:
        pass

    try:
        import render_pac_report as _pac  # type: ignore
        mods["pac"] = _pac
    except Exception:
        pass

    try:
        import render_epoch_report as _ep  # type: ignore
        mods["epoch"] = _ep
    except Exception:
        pass

    try:
        import render_connectivity_report as _conn  # type: ignore
        mods["connectivity"] = _conn
    except Exception:
        pass

    try:
        import render_microstates_report as _ms  # type: ignore
        mods["microstates"] = _ms
    except Exception:
        pass

    try:
        import render_iaf_report as _iaf  # type: ignore
        mods["iaf"] = _iaf
    except Exception:
        pass

    try:
        import render_bids_scan_report as _bids  # type: ignore
        mods["bids_scan"] = _bids
    except Exception:
        pass

    try:
        import render_channel_qc_report as _qc  # type: ignore
        mods["channel_qc"] = _qc
    except Exception:
        pass

    try:
        import render_artifacts_report as _ar  # type: ignore
        mods["artifacts"] = _ar
    except Exception:
        pass

    try:
        import render_spectral_features_report as _sf  # type: ignore
        mods["spectral_features"] = _sf
    except Exception:
        pass

    return mods


def _render_report(kind: str, outdir: str, *, force: bool, renderer_mods: Dict[str, object]) -> ReportItem:
    """Generate one report (or skip if already present)."""

    if kind == "quality":
        report = os.path.join(outdir, "quality_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("quality")

    elif kind == "trace_plot":
        report = os.path.join(outdir, "trace_plot_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("trace_plot")

    elif kind == "bandpowers":
        report = os.path.join(outdir, "bandpowers_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("bandpowers")

    elif kind == "bandratios":
        report = os.path.join(outdir, "bandratios_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("bandratios")

    elif kind == "spectral_features":
        report = os.path.join(outdir, "spectral_features_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("spectral_features")

    elif kind == "nf":
        report = os.path.join(outdir, "nf_feedback_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("nf")

    elif kind == "pac":
        report = os.path.join(outdir, "pac_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("pac")


    elif kind == "epoch":
        report = os.path.join(outdir, "epoch_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("epoch")
    elif kind == "connectivity":
        report = os.path.join(outdir, "connectivity_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("connectivity")

    elif kind == "microstates":
        report = os.path.join(outdir, "microstates_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("microstates")

    elif kind == "iaf":
        report = os.path.join(outdir, "iaf_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("iaf")

    elif kind == "bids_scan":
        report = os.path.join(outdir, "bids_scan_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("bids_scan")

    elif kind == "channel_qc":
        report = os.path.join(outdir, "channel_qc_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("channel_qc")

    elif kind == "artifacts":
        report = os.path.join(outdir, "artifacts_report.html")
        argv = ["--input", outdir, "--out", report]
        mod = renderer_mods.get("artifacts")

    else:
        return ReportItem(kind=kind, outdir=outdir, report_path="", status="error", message="Unknown report kind")

    if (not force) and os.path.exists(report):
        return ReportItem(kind=kind, outdir=outdir, report_path=report, status="skipped", message="exists")

    if mod is None or not hasattr(mod, "main"):
        return ReportItem(
            kind=kind,
            outdir=outdir,
            report_path=report,
            status="error",
            message=(
                "Renderer module not importable (run from repo root: python3 scripts/render_reports_dashboard.py ...)"
            ),
        )

    try:
        rc = int(mod.main(argv))  # type: ignore[attr-defined]
    except SystemExit as e:
        code = getattr(e, "code", None)
        if code is None:
            rc = 0
        elif isinstance(code, int):
            rc = code
        else:
            rc = 1
    except Exception as e:
        return ReportItem(kind=kind, outdir=outdir, report_path=report, status="error", message=str(e))

    if rc != 0:
        return ReportItem(kind=kind, outdir=outdir, report_path=report, status="error", message=f"renderer returned {rc}")
    if not os.path.exists(report):
        return ReportItem(kind=kind, outdir=outdir, report_path=report, status="error", message="report not created")

    return ReportItem(kind=kind, outdir=outdir, report_path=report, status="ok")


# ---- Dashboard HTML --------------------------------------------------------

def _file_mtime_iso(path: str) -> str:
    try:
        ts = os.path.getmtime(path)
    except Exception:
        return ""
    return _dt.datetime.utcfromtimestamp(ts).replace(microsecond=0).isoformat() + "Z"


def _build_dashboard(items: Sequence[ReportItem], out_path: str, roots: Sequence[str]) -> str:
    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(out_path)) or "."

    # Group items by kind
    by_kind: Dict[str, List[ReportItem]] = {}
    for it in items:
        by_kind.setdefault(it.kind, []).append(it)

    kind_order: List[Tuple[str, str]] = [
        ("quality", "Quality runs"),
        ("trace_plot", "Trace plot runs"),
        ("bids_scan", "BIDS scan runs"),
        ("nf", "Neurofeedback runs"),
        ("pac", "PAC runs"),
        ("epoch", "Epoch runs"),
        ("iaf", "IAF runs"),
        ("bandpowers", "Bandpower runs"),
        ("bandratios", "Band ratio runs"),
        ("spectral_features", "Spectral feature runs"),
        ("connectivity", "Connectivity runs"),
        ("microstates", "Microstate runs"),
        ("channel_qc", "Channel QC runs"),
        ("artifacts", "Artifacts runs"),
    ]

    def _counts(its: Sequence[ReportItem]) -> Dict[str, int]:
        out = {"total": 0, "ok": 0, "skipped": 0, "error": 0}
        out["total"] = len(its)
        for it in its:
            if it.status in out:
                out[it.status] += 1
        return out

    overall = _counts(items)

    def _badge(status: str) -> str:
        cls = "ok" if status == "ok" else ("skip" if status == "skipped" else "err")
        return f'<span class="badge {cls}">{_e(status)}</span>'

    def section(kind: str, title: str) -> str:
        its = by_kind.get(kind, [])
        c = _counts(its)
        anchor = f"sec_{kind}"

        if not its:
            return (
                f'<div class="card">'
                f'<h2 id="{_e(anchor)}">{_e(title)} <span class="pill">0</span></h2>'
                '<div class="note">None found.</div>'
                '</div>'
            )

        rows: List[str] = []
        for it in sorted(its, key=lambda x: (x.status != "ok", x.outdir)):
            rel_dir = _posix_relpath(it.outdir, out_dir)
            folder_href = rel_dir
            if folder_href and not folder_href.endswith("/"):
                folder_href += "/"
            folder_link = (
                f'<a href="{_e(folder_href)}"><code>{_e(rel_dir)}</code></a>' if rel_dir else '<code>.</code>'
            )

            rel_report = _posix_relpath(it.report_path, out_dir) if it.report_path else ""
            mtime = _file_mtime_iso(it.report_path) if it.report_path else ""
            msg = it.message or ""

            link = (
                f'<a href="{_e(rel_report)}">open</a>'
                if rel_report and it.report_path and os.path.exists(it.report_path)
                else '<span class="muted">missing</span>'
            )

            rows.append(
                "<tr>"
                f"<td data-csv=\"{_e(it.status)}\">{_badge(it.status)}</td>"
                f"<td data-csv=\"{_e(rel_dir)}\">{folder_link}</td>"
                f"<td data-csv=\"{_e(rel_report)}\">{link}</td>"
                f"<td data-csv=\"{_e(mtime)}\"><code>{_e(mtime)}</code></td>"
                f"<td class=\"muted\" data-csv=\"{_e(msg)}\">{_e(msg)}</td>"
                "</tr>"
            )

        ths = (
            '<th onclick="sortTable(this)" aria-sort="none">Status</th>'
            '<th onclick="sortTable(this)" aria-sort="none">Output folder</th>'
            '<th onclick="sortTable(this)" aria-sort="none">Report</th>'
            '<th onclick="sortTable(this)" aria-sort="none">Report mtime (UTC)</th>'
            '<th onclick="sortTable(this)" aria-sort="none">Note</th>'
        )

        download_name = f"{kind}_runs_filtered.csv"

        return (
            f'<div class="card">'
            f'<h2 id="{_e(anchor)}">{_e(title)} <span class="pill">{c["total"]}</span>'
            f' <span class="muted" style="font-size:12px;">(ok {c["ok"]} · skipped {c["skipped"]} · error {c["error"]})</span>'
            '</h2>'
            '<div class="table-filter">'
            '<div class="table-controls">'
            '<input class="section-filter" type="search" placeholder="Filter rows…" oninput="filterTable(this)" />'
            f'<button type="button" onclick="downloadTableCSV(this, \'{_e(download_name)}\', true)">Download CSV</button>'
            '<span class="filter-count muted"></span>'
            '</div>'
            '<div class="table-wrap">'
            '<table class="data-table sticky">'
            f'<thead><tr>{ths}</tr></thead>'
            '<tbody>'
            + "".join(rows)
            + '</tbody></table>'
            '</div>'
            '</div>'
            '</div>'
        )

    # Table of contents with counts.
    toc_rows: List[str] = []
    for kind, title in kind_order:
        c = _counts(by_kind.get(kind, []))
        pill = f'<span class="pill">{c["total"]}</span>'
        toc_rows.append(f'<li><a href="#{_e("sec_" + kind)}">{_e(title)}</a> {pill}</li>')

    toc_html = "".join(toc_rows)
    roots_html = "".join(f"<li><code>{_e(os.path.abspath(r))}</code></li>" for r in roots)

    css = BASE_CSS + r"""
/* Dashboard-specific tweaks */
.data-table { min-width: 760px; }

.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 999px;
  font-size: 12px;
  border: 1px solid var(--grid);
}
.badge.ok { color: var(--ok); border-color: rgba(127,179,255,0.6); }
.badge.skip { color: var(--warn); border-color: rgba(255,184,107,0.6); }
.badge.err { color: var(--bad); border-color: rgba(255,127,163,0.6); }

.pill {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 999px;
  font-size: 12px;
  border: 1px solid var(--grid);
  color: var(--muted);
  margin-left: 6px;
}

.stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  gap: 10px;
  margin-top: 12px;
}
.stat {
  border: 1px solid var(--grid);
  border-radius: 12px;
  padding: 10px 12px;
  background: rgba(255,255,255,0.02);
}
.stat .num { font-size: 20px; font-weight: 700; }
.stat .lbl { font-size: 12px; color: var(--muted); margin-top: 2px; }

.toc { columns: 2; column-gap: 22px; }
.toc li { break-inside: avoid; margin: 6px 0; }
@media (max-width: 900px) { .toc { columns: 1; } }

.small { font-size: 12px; }
"""

    extra_js = r"""
function _countVisibleRows() {
  let total = 0;
  let shown = 0;
  try {
    document.querySelectorAll('table.data-table tbody tr').forEach(tr => {
      total++;
      if (!tr.style || tr.style.display !== 'none') shown++;
    });
  } catch(e) {}
  const el = document.getElementById('global_count');
  if (!el) return;
  const q = (document.getElementById('global_q')?.value || '').trim();
  if (!q) el.textContent = `${total} total rows`;
  else el.textContent = `shown ${shown} / ${total}`;
}

function applyGlobalFilter() {
  const q = (document.getElementById('global_q')?.value || '');
  try {
    document.querySelectorAll('input.section-filter').forEach(inp => {
      inp.value = q;
      filterTable(inp);
    });
  } catch(e) {}
  _countVisibleRows();
}

function clearGlobalFilter() {
  const el = document.getElementById('global_q');
  if (el) el.value = '';
  applyGlobalFilter();
}

document.addEventListener('DOMContentLoaded', () => {
  try { applyGlobalFilter(); } catch(e) {}
  try {
    const el = document.getElementById('global_q');
    if (el) {
      el.addEventListener('keydown', (ev) => {
        if (ev.key === 'Escape') {
          clearGlobalFilter();
        }
      });
    }
  } catch(e) {}
});
"""

    sections_html = "".join(section(kind, title) for kind, title in kind_order)

    return f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<title>QEEG reports dashboard</title>
<style>{css}</style>
</head>
<body>
<header>
  <h1>QEEG reports dashboard</h1>
  <div class=\"meta\">Generated {now} — {overall['total']} entries (ok {overall['ok']} · skipped {overall['skipped']} · error {overall['error']})</div>
</header>
<main>
  <div class=\"card\">
    <h2>About</h2>
    <div class=\"note\">
      This dashboard links to self-contained HTML reports generated from CLI outputs in this repository.
      It is for research/educational inspection only and is not a medical device.
    </div>

    <div class=\"note\">
      Scanned roots:
      <ul>{roots_html}</ul>
    </div>

    <div class=\"stats\">
      <div class=\"stat\"><div class=\"num\">{overall['total']}</div><div class=\"lbl\">Total entries</div></div>
      <div class=\"stat\"><div class=\"num\">{overall['ok']}</div><div class=\"lbl\">OK</div></div>
      <div class=\"stat\"><div class=\"num\">{overall['skipped']}</div><div class=\"lbl\">Skipped</div></div>
      <div class=\"stat\"><div class=\"num\">{overall['error']}</div><div class=\"lbl\">Error</div></div>
    </div>

    <div class=\"card\" style=\"margin-top: 12px;\">
      <h3>Filter</h3>
      <div class=\"note\">Filters all run tables below. Tip: click table headers to sort. Use <code>-token</code> to exclude; use <code>status=ok</code> / <code>note:error</code> to filter a specific column. Columns with spaces can be quoted, e.g. <code>\"output folder\":subj01</code>.</div>
      <div class=\"table-controls\">
        <input id=\"global_q\" type=\"search\" placeholder=\"Filter all tables…\" oninput=\"applyGlobalFilter()\">
        <button type=\"button\" onclick=\"clearGlobalFilter()\">Clear</button>
        <span id=\"global_count\" class=\"muted\"></span>
      </div>
    </div>

    <div class=\"card\" style=\"margin-top: 12px;\">
      <h3>Sections</h3>
      <ul class=\"toc\">{toc_html}</ul>
    </div>
  </div>

  {sections_html}

  <div class=\"footer\">Tip: open report links in a browser. These HTML files make no network requests.</div>
</main>
<script>{JS_SORT_TABLE + extra_js}</script>
</body>
</html>
"""


# ---- CLI ------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Scan directories and build an HTML dashboard linking to report HTML files.")
    ap.add_argument(
        "roots",
        nargs="+",
        help="One or more directories to scan recursively for qeeg_* CLI outputs.",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="Output HTML path (default: <first-root>/qeeg_reports_dashboard.html)",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="Regenerate per-run report HTML even if it already exists.",
    )
    ap.add_argument(
        "--no-render",
        action="store_true",
        help="Only build dashboard from existing report HTML; do not call render scripts.",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated dashboard in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    roots = [os.path.abspath(r) for r in args.roots]
    out_path = args.out
    if out_path is None:
        out_path = os.path.join(roots[0], "qeeg_reports_dashboard.html")
    out_path = os.path.abspath(out_path)
    out_dir = os.path.dirname(out_path) or "."
    os.makedirs(out_dir, exist_ok=True)

    renderer_mods = _try_import_renderers()

    quality_dirs: Set[str] = set()
    trace_dirs: Set[str] = set()
    band_dirs: Set[str] = set()
    br_dirs: Set[str] = set()
    sf_dirs: Set[str] = set()
    nf_dirs: Set[str] = set()
    pac_dirs: Set[str] = set()
    epoch_dirs: Set[str] = set()
    conn_dirs: Set[str] = set()
    ms_dirs: Set[str] = set()
    iaf_dirs: Set[str] = set()
    bids_dirs: Set[str] = set()
    qc_dirs: Set[str] = set()
    art_dirs: Set[str] = set()

    for dirpath, filenames in _walk_dirs(roots):
        s = set(filenames)

        if any(name in s for name in _QUALITY_HINT_FILES):
            # quality_report.json is the strongest signal; accept the others too.
            if ("quality_report.json" in s) or ("line_noise_per_channel.csv" in s):
                quality_dirs.add(os.path.abspath(dirpath))

        if any(name in s for name in _TRACE_PLOT_HINT_FILES):
            trace_dirs.add(os.path.abspath(dirpath))

        if "bandpowers.csv" in s:
            band_dirs.add(os.path.abspath(dirpath))
        if "bandratios.csv" in s:
            br_dirs.add(os.path.abspath(dirpath))
        if "spectral_features.csv" in s:
            sf_dirs.add(os.path.abspath(dirpath))
        if "nf_feedback.csv" in s:
            nf_dirs.add(os.path.abspath(dirpath))
        if "pac_timeseries.csv" in s:
            pac_dirs.add(os.path.abspath(dirpath))
        if ("epoch_bandpowers_summary.csv" in s) or ("epoch_bandpowers.csv" in s):
            epoch_dirs.add(os.path.abspath(dirpath))
        if _looks_like_connectivity(filenames):
            conn_dirs.add(os.path.abspath(dirpath))
        if "microstate_state_stats.csv" in s:
            ms_dirs.add(os.path.abspath(dirpath))
        if "iaf_by_channel.csv" in s:
            iaf_dirs.add(os.path.abspath(dirpath))

        if any(name in s for name in _BIDS_SCAN_HINT_FILES):
            # bids_index.csv is the strongest signal; accept the others too.
            if "bids_index.csv" in s:
                bids_dirs.add(os.path.abspath(dirpath))
        if "channel_qc.csv" in s:
            qc_dirs.add(os.path.abspath(dirpath))
        if any(name in s for name in _ARTIFACT_HINT_FILES):
            # artifact_windows.csv is the main signal; allow segments/meta too.
            art_dirs.add(os.path.abspath(dirpath))

    items: List[ReportItem] = []

    def add_existing(kind: str, outdir: str, report_name: str) -> None:
        report_path = os.path.join(outdir, report_name)
        if os.path.exists(report_path):
            items.append(ReportItem(kind=kind, outdir=outdir, report_path=report_path, status="ok"))
        else:
            items.append(ReportItem(kind=kind, outdir=outdir, report_path=report_path, status="error", message="missing"))

    if args.no_render:
        for d in sorted(quality_dirs):
            add_existing("quality", d, "quality_report.html")
        for d in sorted(trace_dirs):
            add_existing("trace_plot", d, "trace_plot_report.html")
        for d in sorted(nf_dirs):
            add_existing("nf", d, "nf_feedback_report.html")
        for d in sorted(pac_dirs):
            add_existing("pac", d, "pac_report.html")
        for d in sorted(epoch_dirs):
            add_existing("epoch", d, "epoch_report.html")
        for d in sorted(iaf_dirs):
            add_existing("iaf", d, "iaf_report.html")
        for d in sorted(bids_dirs):
            add_existing("bids_scan", d, "bids_scan_report.html")
        for d in sorted(band_dirs):
            add_existing("bandpowers", d, "bandpowers_report.html")
        for d in sorted(br_dirs):
            add_existing("bandratios", d, "bandratios_report.html")
        for d in sorted(sf_dirs):
            add_existing("spectral_features", d, "spectral_features_report.html")
        for d in sorted(conn_dirs):
            add_existing("connectivity", d, "connectivity_report.html")
        for d in sorted(ms_dirs):
            add_existing("microstates", d, "microstates_report.html")
        for d in sorted(qc_dirs):
            add_existing("channel_qc", d, "channel_qc_report.html")
        for d in sorted(art_dirs):
            add_existing("artifacts", d, "artifacts_report.html")
    else:
        for d in sorted(quality_dirs):
            items.append(_render_report("quality", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(trace_dirs):
            items.append(_render_report("trace_plot", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(nf_dirs):
            items.append(_render_report("nf", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(pac_dirs):
            items.append(_render_report("pac", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(epoch_dirs):
            items.append(_render_report("epoch", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(iaf_dirs):
            items.append(_render_report("iaf", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(bids_dirs):
            items.append(_render_report("bids_scan", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(band_dirs):
            items.append(_render_report("bandpowers", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(br_dirs):
            items.append(_render_report("bandratios", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(sf_dirs):
            items.append(_render_report("spectral_features", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(conn_dirs):
            items.append(_render_report("connectivity", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(ms_dirs):
            items.append(_render_report("microstates", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(qc_dirs):
            items.append(_render_report("channel_qc", d, force=bool(args.force), renderer_mods=renderer_mods))
        for d in sorted(art_dirs):
            items.append(_render_report("artifacts", d, force=bool(args.force), renderer_mods=renderer_mods))

    html_doc = _build_dashboard(items, out_path, roots)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {out_path}")

    if args.open:
        try:
            webbrowser.open("file://" + out_path)
        except Exception:
            pass

    any_err = any(it.status == "error" for it in items)
    return 1 if any_err else 0


if __name__ == "__main__":
    raise SystemExit(main())
