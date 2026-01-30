#!/usr/bin/env python3
"""Render a multi-session neurofeedback dashboard (dependency-free HTML).

This script scans one or more directories (recursively) for ``qeeg_nf_cli``
outputs and builds a single self-contained HTML dashboard that summarizes
*multiple* neurofeedback runs.

It is intentionally dependency-free (Python stdlib only) so it can run in minimal
environments (CI artifacts, lab workstations, offline bundles).

Inputs (per session folder):
  - nf_feedback.csv (required)
  - nf_summary.json (optional; protocol + metric spec)
  - nf_run_meta.json (optional; tool/build metadata + timestamps)
  - nf_derived_events.tsv/.csv (optional; BIDS-ish derived events)

Outputs:
  - nf_sessions_dashboard.html (default output name; configurable via --out)

Typical usage:

  # Scan a folder containing multiple out_nf* directories
  python3 scripts/render_nf_sessions_dashboard.py results_root

  # Or provide session directories directly
  python3 scripts/render_nf_sessions_dashboard.py out_nf out_nf2 out_nf3

Notes:
  - This dashboard is for research/educational inspection only and is not a
    medical device.
  - The dashboard makes no network requests and is safe to open locally.

"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import math
import os
import sys
import webbrowser
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

# Reuse the shared theme + table JS.
try:
    from report_common import (
        BASE_CSS,
        JS_SORT_TABLE,
        e as _e,
        finite_minmax,
        posix_relpath,
        read_json_if_exists,
        try_bool_int,
        try_float,
        utc_now_iso,
    )
except Exception as _exc:  # pragma: no cover
    raise SystemExit(f"ERROR: failed to import scripts/report_common.py: {_exc}")


@dataclass
class _Cols:
    t: str
    metric: str
    threshold: str
    reward: Optional[str]
    reward_rate: Optional[str]
    artifact: Optional[str]
    artifact_ready: Optional[str]
    bad_channels: Optional[str]
    phase: Optional[str]


@dataclass
class _SessionStats:
    n_frames: int
    duration_sec: float
    dt_median_sec: float

    reward_frac: float
    artifact_frac: float
    artifact_ready_frac: float

    metric_mean: float
    metric_min: float
    metric_max: float
    metric_last: float

    threshold_mean: float
    threshold_min: float
    threshold_max: float
    threshold_last: float

    reward_rate_mean: float
    reward_rate_last: float

    bad_channels_mean: float

    phase_counts: Dict[str, int]
    derived_durations: Dict[str, float]


@dataclass
class _Session:
    outdir: Path
    csv_path: Path
    summary: Optional[Dict[str, Any]]
    run_meta: Optional[Dict[str, Any]]
    derived_events_path: Optional[Path]
    stats: _SessionStats
    timestamp_utc: str
    protocol: str
    metric_spec: str
    report_html: Optional[Path]
    note: str


def _mtime_iso_utc(path: Path) -> str:
    try:
        ts = path.stat().st_mtime
        return _dt.datetime.utcfromtimestamp(ts).replace(microsecond=0).isoformat() + "Z"
    except Exception:
        return ""


def _pick_col(headers: Sequence[str], candidates: Sequence[str], *, required: bool = False) -> Optional[str]:
    hset = {str(h).strip().lower(): str(h).strip() for h in headers if str(h).strip() != ""}
    for cand in candidates:
        c = str(cand).strip().lower()
        if c in hset:
            return hset[c]
    if required:
        raise RuntimeError(f"Missing required column: one of {list(candidates)}")
    return None


def _detect_cols(headers: Sequence[str]) -> _Cols:
    t = _pick_col(headers, ["t_end_sec", "t_sec", "t", "time_sec", "time_s", "time"], required=True) or "t_end_sec"
    metric = _pick_col(headers, ["metric", "value", "score"], required=True) or "metric"
    threshold = _pick_col(headers, ["threshold", "thr"], required=True) or "threshold"
    reward = _pick_col(headers, ["reward", "reward_on", "reward_state", "reinforce"])
    reward_rate = _pick_col(headers, ["reward_rate", "rr", "reinforcement_rate"])
    artifact = _pick_col(headers, ["artifact", "artifact_on", "artifact_state"])
    artifact_ready = _pick_col(headers, ["artifact_ready", "artifact_gate_ready", "gate_ready"])
    bad_channels = _pick_col(headers, ["bad_channels", "bad_channels_count", "n_bad_channels", "n_bad_channel"])
    phase = _pick_col(headers, ["phase", "block", "state"])
    return _Cols(
        t=t,
        metric=metric,
        threshold=threshold,
        reward=reward,
        reward_rate=reward_rate,
        artifact=artifact,
        artifact_ready=artifact_ready,
        bad_channels=bad_channels,
        phase=phase,
    )


def _safe_mean(sum_v: float, n: int) -> float:
    if n <= 0:
        return math.nan
    return sum_v / float(n)


def _median(vals: List[float]) -> float:
    vv = [v for v in vals if math.isfinite(v)]
    if not vv:
        return math.nan
    vv.sort()
    m = len(vv) // 2
    if len(vv) % 2 == 1:
        return float(vv[m])
    return 0.5 * (float(vv[m - 1]) + float(vv[m]))


def _summarize_nf_feedback(csv_path: Path) -> Tuple[_SessionStats, str]:
    """Return (_SessionStats, note)."""

    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f, restkey="__extra__", restval="")
        if not r.fieldnames:
            raise RuntimeError("missing header row")
        headers = [str(h or "").strip() for h in r.fieldnames if str(h or "").strip() != ""]
        cols = _detect_cols(headers)

        # Streaming stats.
        n = 0
        t_first = math.nan
        t_last = math.nan
        prev_t = math.nan
        dt_samples: List[float] = []

        reward_sum = 0
        artifact_sum = 0
        artifact_ready_sum = 0

        metric_sum = 0.0
        metric_n = 0
        metric_min = math.inf
        metric_max = -math.inf
        metric_last = math.nan

        thr_sum = 0.0
        thr_n = 0
        thr_min = math.inf
        thr_max = -math.inf
        thr_last = math.nan

        rr_sum = 0.0
        rr_n = 0
        rr_last = math.nan

        bad_sum = 0.0
        bad_n = 0

        phase_counts: Dict[str, int] = {}

        for row in r:
            tt = try_float(row.get(cols.t, ""))
            if not math.isfinite(tt):
                continue

            if not math.isfinite(t_first):
                t_first = tt
            t_last = tt

            if math.isfinite(prev_t):
                dt = tt - prev_t
                if math.isfinite(dt) and dt > 0 and dt < 60 and len(dt_samples) < 600:
                    dt_samples.append(float(dt))
            prev_t = tt

            n += 1

            mv = try_float(row.get(cols.metric, ""))
            if math.isfinite(mv):
                metric_sum += mv
                metric_n += 1
                metric_min = min(metric_min, mv)
                metric_max = max(metric_max, mv)
                metric_last = mv

            tv = try_float(row.get(cols.threshold, ""))
            if math.isfinite(tv):
                thr_sum += tv
                thr_n += 1
                thr_min = min(thr_min, tv)
                thr_max = max(thr_max, tv)
                thr_last = tv

            if cols.reward:
                reward_sum += int(try_bool_int(row.get(cols.reward, ""), default=0))
            if cols.artifact:
                artifact_sum += int(try_bool_int(row.get(cols.artifact, ""), default=0))
            if cols.artifact_ready:
                artifact_ready_sum += int(try_bool_int(row.get(cols.artifact_ready, ""), default=0))

            if cols.reward_rate:
                rv = try_float(row.get(cols.reward_rate, ""))
                if math.isfinite(rv):
                    rr_sum += rv
                    rr_n += 1
                    rr_last = rv

            if cols.bad_channels:
                bv = try_float(row.get(cols.bad_channels, ""))
                if math.isfinite(bv):
                    bad_sum += bv
                    bad_n += 1

            if cols.phase:
                ph = str(row.get(cols.phase, "") or "").strip()
                if ph:
                    phase_counts[ph] = phase_counts.get(ph, 0) + 1

        if n <= 0:
            raise RuntimeError("no valid rows (time column missing/NaN?)")

        duration = float(t_last) if math.isfinite(t_last) else math.nan
        dt_med = _median(dt_samples)

        reward_frac = (reward_sum / float(n)) if (n > 0 and cols.reward) else math.nan
        artifact_frac = (artifact_sum / float(n)) if (n > 0 and cols.artifact) else math.nan
        artifact_ready_frac = (artifact_ready_sum / float(n)) if (n > 0 and cols.artifact_ready) else math.nan

        metric_mean = _safe_mean(metric_sum, metric_n)
        thr_mean = _safe_mean(thr_sum, thr_n)
        rr_mean = _safe_mean(rr_sum, rr_n)
        bad_mean = _safe_mean(bad_sum, bad_n)

        # Normalize min/max when missing.
        if metric_n <= 0:
            metric_min, metric_max = math.nan, math.nan
        if thr_n <= 0:
            thr_min, thr_max = math.nan, math.nan

        note_parts: List[str] = []
        if cols.reward is None:
            note_parts.append("no reward column")
        if cols.artifact is None:
            note_parts.append("no artifact column")
        if cols.phase is None:
            note_parts.append("no phase column")
        note = "; ".join(note_parts)

        return (
            _SessionStats(
                n_frames=n,
                duration_sec=duration,
                dt_median_sec=dt_med,
                reward_frac=reward_frac,
                artifact_frac=artifact_frac,
                artifact_ready_frac=artifact_ready_frac,
                metric_mean=metric_mean,
                metric_min=float(metric_min) if math.isfinite(metric_min) else math.nan,
                metric_max=float(metric_max) if math.isfinite(metric_max) else math.nan,
                metric_last=metric_last,
                threshold_mean=thr_mean,
                threshold_min=float(thr_min) if math.isfinite(thr_min) else math.nan,
                threshold_max=float(thr_max) if math.isfinite(thr_max) else math.nan,
                threshold_last=thr_last,
                reward_rate_mean=rr_mean,
                reward_rate_last=rr_last,
                bad_channels_mean=bad_mean,
                phase_counts=phase_counts,
                derived_durations={},
            ),
            note,
        )


def _find_derived_events(outdir: Path) -> Optional[Path]:
    # Prefer BIDS-ish TSV if present.
    for name in ["nf_derived_events.tsv", "nf_derived_events.csv"]:
        p = outdir / name
        if p.is_file():
            return p
    # Some older outputs might have a generic name.
    for name in ["derived_events.tsv", "derived_events.csv", "events.tsv", "events.csv"]:
        p = outdir / name
        if p.is_file():
            return p
    return None


def _summarize_derived_events(path: Path) -> Dict[str, float]:
    """Return total duration per trial_type (or legacy text column)."""

    try:
        # TSV/CSV handled by the shared reader.
        from report_common import read_csv_dict  # local import to keep top-level small

        headers, rows = read_csv_dict(str(path))
    except Exception:
        return {}

    # BIDS columns: onset, duration, trial_type
    trial = _pick_col(headers, ["trial_type", "trial", "type", "text"], required=False)
    dur = _pick_col(headers, ["duration", "duration_sec", "dur", "len_sec"], required=False)

    if not trial or not dur:
        return {}

    out: Dict[str, float] = {}
    for row in rows:
        label = str(row.get(trial, "") or "").strip()
        dv = try_float(row.get(dur, ""))
        if not (label and math.isfinite(dv) and dv >= 0):
            continue
        out[label] = out.get(label, 0.0) + float(dv)
    return out


def _metric_spec_string(summary: Optional[Dict[str, Any]]) -> str:
    if not summary:
        return ""
    ms = summary.get("metric_spec")
    if not isinstance(ms, dict):
        return ""
    t = str(ms.get("type") or "").strip().lower()
    if t == "band":
        b = str(ms.get("band") or "")
        ch = str(ms.get("channel") or "")
        return f"{b}:{ch}".strip(":")
    if t == "ratio":
        bn = str(ms.get("band_num") or "")
        bd = str(ms.get("band_den") or "")
        ch = str(ms.get("channel") or "")
        base = f"{bn}/{bd}".strip("/")
        return f"{base}:{ch}".strip(":")
    if t == "asymmetry":
        b = str(ms.get("band") or "")
        a = str(ms.get("channel_a") or "")
        bch = str(ms.get("channel_b") or "")
        return f"{b}:{a}-{bch}".strip(":")
    if t == "coherence":
        b = str(ms.get("band") or "")
        a = str(ms.get("channel_a") or "")
        bch = str(ms.get("channel_b") or "")
        meas = str(ms.get("measure") or "")
        left = meas if meas else "coh"
        return f"{left}:{b}:{a}-{bch}".strip(":")
    if t == "pac":
        ph = str(ms.get("phase_band") or "")
        am = str(ms.get("amp_band") or "")
        ch = str(ms.get("channel") or "")
        meth = str(ms.get("method") or "")
        left = f"PAC({ph},{am})"
        right = f"{ch}"
        if meth:
            right += f" [{meth}]"
        return f"{left} {right}".strip()
    # Unknown spec; best-effort compact JSON-ish string.
    try:
        keys = sorted(ms.keys())
        short = {
            k: ms[k]
            for k in keys
            if k
            in (
                "type",
                "band",
                "band_num",
                "band_den",
                "channel",
                "channel_a",
                "channel_b",
            )
        }
        return str(short)
    except Exception:
        return ""


def _timestamp_utc(run_meta: Optional[Dict[str, Any]], csv_path: Path) -> str:
    # Prefer nf_run_meta.json TimestampUTC if present.
    if run_meta:
        ts = run_meta.get("TimestampUTC")
        if isinstance(ts, str) and ts.strip():
            return ts.strip()
    # Fall back to file mtime.
    return _mtime_iso_utc(csv_path)


def _parse_iso_to_epoch(ts: str) -> Optional[float]:
    s = str(ts or "").strip()
    if not s:
        return None
    # Handle trailing Z.
    if s.endswith("Z"):
        s2 = s[:-1] + "+00:00"
    else:
        s2 = s
    try:
        return _dt.datetime.fromisoformat(s2).timestamp()
    except Exception:
        return None


def _scan_sessions(roots: Sequence[str]) -> List[Path]:
    out: List[Path] = []

    def _add_dir(d: Path) -> None:
        try:
            d = d.resolve()
        except Exception:
            d = Path(d)
        if (d / "nf_feedback.csv").is_file():
            out.append(d)

    for r in roots:
        p = Path(r)
        if p.is_file():
            if p.name == "nf_feedback.csv":
                _add_dir(p.parent)
            continue
        if not p.is_dir():
            continue

        for dirpath, dirnames, filenames in os.walk(str(p)):
            # Skip common noisy folders.
            dn = [d for d in dirnames if d not in (".git", "__pycache__", "build", "dist")]
            dirnames[:] = dn

            if "nf_feedback.csv" in set(filenames):
                out.append(Path(dirpath).resolve())

    # De-duplicate deterministically.
    out = sorted(set(out), key=lambda q: str(q))
    return out


def _try_generate_nf_report(outdir: Path) -> Optional[Path]:
    """Best-effort ensure nf_feedback_report.html exists for this session."""

    rep = outdir / "nf_feedback_report.html"
    if rep.is_file():
        return rep

    # Try in-process import (fast; no subprocess).
    try:
        import render_nf_feedback_report as _r  # type: ignore

        rc = _r.main(["--input", str(outdir)])
        if rc == 0 and rep.is_file():
            return rep
    except Exception:
        pass

    # Fall back to subprocess to be resilient to import path issues.
    try:
        import subprocess

        here = Path(__file__).resolve().parent
        script = here / "render_nf_feedback_report.py"
        if script.is_file():
            subprocess.check_call([sys.executable, str(script), "--input", str(outdir)])
            if rep.is_file():
                return rep
    except Exception:
        pass

    return None


def _fmt_frac(v: float) -> str:
    return f"{v * 100.0:.1f}%" if math.isfinite(v) else "—"


def _fmt_num(v: float) -> str:
    if not math.isfinite(v):
        return "—"
    # Keep compact while preserving readability.
    return f"{v:.6g}"


def _svg_spark(values: Sequence[float], labels: Sequence[str], *, title: str, yfmt: str) -> str:
    vals = [float(v) for v in values]
    if not any(math.isfinite(v) for v in vals):
        return ""

    W, H = 1000, 220
    pad_l, pad_r, pad_t, pad_b = 60, 24, 26, 44
    x0, x1 = pad_l, W - pad_r
    y0, y1 = pad_t, H - pad_b
    n = len(vals)
    if n <= 1:
        return ""

    mn, mx = finite_minmax(vals)
    # Slight padding so a flat line is still visible.
    if mx - mn <= 0:
        mx = mn + 1.0

    def x(i: int) -> float:
        return x0 + (x1 - x0) * (i / float(n - 1))

    def y(v: float) -> float:
        if not math.isfinite(v):
            return float(y1)
        t = (v - mn) / (mx - mn)
        t = min(1.0, max(0.0, t))
        return y1 - t * (y1 - y0)

    # Build polyline points (skip NaNs).
    pts: List[str] = []
    for i, v in enumerate(vals):
        if not math.isfinite(v):
            continue
        pts.append(f"{x(i):.2f},{y(v):.2f}")

    if not pts:
        return ""

    # Labels for min/max.
    mn_lbl = yfmt.format(mn)
    mx_lbl = yfmt.format(mx)

    # Points with tooltips.
    circles: List[str] = []
    for i, v in enumerate(vals):
        if not math.isfinite(v):
            continue
        lab = labels[i] if i < len(labels) else str(i + 1)
        circles.append(
            f'<circle class="spark-pt" cx="{x(i):.2f}" cy="{y(v):.2f}" r="4">'
            f"<title>{_e(lab)}: {yfmt.format(v)}</title>"
            "</circle>"
        )

    return f"""
+<svg class=\"spark\" viewBox=\"0 0 {W} {H}\" role=\"img\" aria-label=\"{_e(title)}\">
+  <rect class=\"spark-frame\" x=\"0\" y=\"0\" width=\"{W}\" height=\"{H}\" />
+  <text class=\"spark-title\" x=\"{pad_l}\" y=\"18\">{_e(title)}</text>
+
+  <line class=\"spark-axis\" x1=\"{x0}\" y1=\"{y1}\" x2=\"{x1}\" y2=\"{y1}\" />
+  <line class=\"spark-axis\" x1=\"{x0}\" y1=\"{y0}\" x2=\"{x0}\" y2=\"{y1}\" />
+
+  <text class=\"spark-y\" x=\"10\" y=\"{y0 + 10:.2f}\">{_e(mx_lbl)}</text>
+  <text class=\"spark-y\" x=\"10\" y=\"{y1:.2f}\">{_e(mn_lbl)}</text>
+
+  <polyline class=\"spark-line\" points=\"{_e(' '.join(pts))}\" />
+  {''.join(circles)}
+</svg>
+""".strip()


def _build_dashboard(sessions: Sequence[_Session], out_path: Path, roots: Sequence[str]) -> str:
    now = utc_now_iso()
    out_dir = out_path.resolve().parent

    # Summary stats.
    n_sess = len(sessions)
    tot_dur = sum(s.stats.duration_sec for s in sessions if math.isfinite(s.stats.duration_sec))
    reward_vals = [s.stats.reward_frac for s in sessions if math.isfinite(s.stats.reward_frac)]
    art_vals = [s.stats.artifact_frac for s in sessions if math.isfinite(s.stats.artifact_frac)]

    avg_reward = sum(reward_vals) / len(reward_vals) if reward_vals else math.nan
    avg_art = sum(art_vals) / len(art_vals) if art_vals else math.nan

    # Trend charts (use session order).
    labels = [f"{i+1} · {posix_relpath(str(s.outdir), str(out_dir))}" for i, s in enumerate(sessions)]
    reward_chart = _svg_spark(
        [s.stats.reward_frac for s in sessions],
        labels,
        title="Reward fraction by session",
        yfmt="{:.1%}",
    )
    artifact_chart = _svg_spark(
        [s.stats.artifact_frac for s in sessions],
        labels,
        title="Artifact fraction by session",
        yfmt="{:.1%}",
    )
    thr_chart = _svg_spark(
        [s.stats.threshold_mean for s in sessions],
        labels,
        title="Mean threshold by session",
        yfmt="{:.6g}",
    )

    roots_html = "".join(f"<li><code>{_e(str(Path(r).resolve()))}</code></li>" for r in roots)

    # Sessions table.
    rows: List[str] = []
    for i, s in enumerate(sessions):
        rel_dir = posix_relpath(str(s.outdir), str(out_dir))
        folder_href = rel_dir
        if folder_href and not folder_href.endswith("/"):
            folder_href += "/"
        folder_link = f'<a href="{_e(folder_href)}"><code>{_e(rel_dir)}</code></a>' if rel_dir else '<code>.</code>'

        rep_link = '<span class="muted">missing</span>'
        if s.report_html and s.report_html.is_file():
            rep_href = posix_relpath(str(s.report_html), str(out_dir))
            rep_link = f'<a href="{_e(rep_href)}">open</a>'

        # Derived durations (best-effort; omit when missing).
        dd = s.stats.derived_durations
        base_s = dd.get("NF:Baseline", math.nan)
        train_s = dd.get("NF:Train", math.nan)
        rest_s = dd.get("NF:Rest", math.nan)

        rows.append(
            "<tr>"
            f"<td data-csv=\"{i+1}\"><code>{i+1}</code></td>"
            f"<td data-csv=\"{_e(rel_dir)}\">{folder_link}</td>"
            f"<td data-csv=\"{_e(s.timestamp_utc)}\"><code>{_e(s.timestamp_utc)}</code></td>"
            f"<td data-csv=\"{_e(s.protocol)}\"><code>{_e(s.protocol)}</code></td>"
            f"<td data-csv=\"{_e(s.metric_spec)}\"><code>{_e(s.metric_spec)}</code></td>"
            f"<td data-csv=\"{_fmt_num(s.stats.duration_sec)}\"><code>{_e(_fmt_num(s.stats.duration_sec))}</code></td>"
            f"<td data-csv=\"{_fmt_frac(s.stats.reward_frac)}\"><code>{_e(_fmt_frac(s.stats.reward_frac))}</code></td>"
            f"<td data-csv=\"{_fmt_frac(s.stats.artifact_frac)}\"><code>{_e(_fmt_frac(s.stats.artifact_frac))}</code></td>"
            f"<td data-csv=\"{_fmt_num(base_s)}\"><code>{_e(_fmt_num(base_s))}</code></td>"
            f"<td data-csv=\"{_fmt_num(train_s)}\"><code>{_e(_fmt_num(train_s))}</code></td>"
            f"<td data-csv=\"{_fmt_num(rest_s)}\"><code>{_e(_fmt_num(rest_s))}</code></td>"
            f"<td data-csv=\"{_fmt_num(s.stats.threshold_mean)}\"><code>{_e(_fmt_num(s.stats.threshold_mean))}</code></td>"
            f"<td data-csv=\"{_fmt_num(s.stats.metric_mean)}\"><code>{_e(_fmt_num(s.stats.metric_mean))}</code></td>"
            f"<td data-csv=\"{_e('ok' if s.report_html else 'missing')}\">{rep_link}</td>"
            f"<td class=\"muted\" data-csv=\"{_e(s.note)}\">{_e(s.note)}</td>"
            "</tr>"
        )

    ths = (
        '<th onclick="sortTable(this)" aria-sort="none">#</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Session folder</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Timestamp (UTC)</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Protocol</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Metric</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Duration (s)</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Reward %</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Artifact %</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Baseline (s)</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Train (s)</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Rest (s)</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Threshold mean</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Metric mean</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Report</th>'
        '<th onclick="sortTable(this)" aria-sort="none">Note</th>'
    )

    css = BASE_CSS + r"""
+/* Sessions dashboard tweaks */
+.data-table { min-width: 980px; }
+
+.stats {
+  display: grid;
+  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
+  gap: 10px;
+  margin-top: 12px;
+}
+.stat {
+  border: 1px solid var(--grid);
+  border-radius: 12px;
+  padding: 10px 12px;
+  background: rgba(255,255,255,0.02);
+}
+.stat .num { font-size: 20px; font-weight: 700; }
+.stat .lbl { font-size: 12px; color: var(--muted); margin-top: 2px; }
+
+.spark { width: 100%; height: auto; margin-top: 10px; }
+.spark-frame { fill: rgba(17,24,39,0.60); stroke: var(--grid); stroke-width: 1; }
+.spark-title { fill: #dce7ff; font-weight: 700; font-size: 13px; }
+.spark-axis { stroke: rgba(255,255,255,0.08); stroke-width: 1; }
+.spark-y { fill: var(--muted); font-size: 12px; }
+.spark-line { fill: none; stroke: var(--accent); stroke-width: 2.5; opacity: 0.92; }
+.spark-pt { fill: rgba(127,179,255,0.65); stroke: rgba(255,255,255,0.22); stroke-width: 1; }
+"""

    return f"""<!doctype html>
+<html lang=\"en\">
+<head>
+<meta charset=\"utf-8\">
+<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
+<title>Neurofeedback sessions dashboard</title>
+<style>{css}</style>
+</head>
+<body>
+<header>
+  <h1>Neurofeedback sessions dashboard</h1>
+  <div class=\"meta\">Generated {now} — {n_sess} sessions</div>
+</header>
+<main>
+  <div class=\"card\">
+    <h2>About</h2>
+    <div class=\"note\">
+      Aggregated view of <code>qeeg_nf_cli</code> outputs (<code>nf_feedback.csv</code>) across multiple sessions.
+      For research/educational inspection only; not a medical device.
+    </div>
+    <div class=\"note\">
+      Scanned roots:
+      <ul>{roots_html}</ul>
+    </div>
+
+    <div class=\"stats\">
+      <div class=\"stat\"><div class=\"num\">{n_sess}</div><div class=\"lbl\">Sessions</div></div>
+      <div class=\"stat\"><div class=\"num\">{_e(_fmt_num(tot_dur))}</div><div class=\"lbl\">Total duration (s)</div></div>
+      <div class=\"stat\"><div class=\"num\">{_e(_fmt_frac(avg_reward))}</div><div class=\"lbl\">Avg reward</div></div>
+      <div class=\"stat\"><div class=\"num\">{_e(_fmt_frac(avg_art))}</div><div class=\"lbl\">Avg artifact</div></div>
+    </div>
+  </div>
+
+  <div class=\"card\">
+    <h2>Trends</h2>
+    <div class=\"note\">Each point is one session (ordered by timestamp when available).</div>
+    {reward_chart if reward_chart else '<div class="note muted">Reward trend unavailable (missing reward columns).</div>'}
+    {artifact_chart if artifact_chart else '<div class="note muted">Artifact trend unavailable (missing artifact columns).</div>'}
+    {thr_chart if thr_chart else '<div class="note muted">Threshold trend unavailable (missing threshold).</div>'}
+  </div>
+
+  <div class=\"card\">
+    <h2>Sessions (sortable)</h2>
+    <div class=\"note\">Click headers to sort. Use the filter box to search across all columns.</div>
+    <div class=\"table-filter\">
+      <div class=\"table-controls\">
+        <input type=\"search\" placeholder=\"Filter rows…\" oninput=\"filterTable(this)\" />
+        <button type=\"button\" onclick=\"downloadTableCSV(this, 'nf_sessions_filtered.csv', true)\">Download CSV</button>
+        <span class=\"filter-count muted\"></span>
+      </div>
+      <div class=\"table-wrap\">
+        <table class=\"data-table sticky\">
+          <thead><tr>{ths}</tr></thead>
+          <tbody>{''.join(rows)}</tbody>
+        </table>
+      </div>
+    </div>
+  </div>
+
+  <div class=\"footer\">Tip: open links in a browser. This HTML makes no network requests.</div>
+</main>
+<script>{JS_SORT_TABLE}</script>
+</body>
+</html>
+"""


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Build an aggregated dashboard of multiple neurofeedback sessions.")
    ap.add_argument(
        "roots",
        nargs="+",
        help="One or more directories (or nf_feedback.csv files) to scan recursively for nf_feedback.csv.",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="Output HTML path (default: <first-root>/nf_sessions_dashboard.html).",
    )
    ap.add_argument(
        "--max-sessions",
        type=int,
        default=500,
        help="Maximum number of sessions to include (default: 500).",
    )
    ap.add_argument(
        "--no-generate-reports",
        action="store_true",
        help="Do not try to generate missing nf_feedback_report.html files.",
    )
    ap.add_argument(
        "--open",
        action="store_true",
        help="Open the generated dashboard in your default browser.",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    roots = [str(r) for r in args.roots]
    out_path = args.out
    if out_path is None:
        out_path = os.path.join(str(Path(roots[0]).resolve()), "nf_sessions_dashboard.html")
    out_p = Path(out_path).resolve()
    out_dir = out_p.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    sess_dirs = _scan_sessions(roots)

    sessions: List[_Session] = []
    for d in sess_dirs:
        csv_path = d / "nf_feedback.csv"
        if not csv_path.is_file():
            continue

        summary = read_json_if_exists(str(d / "nf_summary.json"))
        run_meta = read_json_if_exists(str(d / "nf_run_meta.json"))
        derived = _find_derived_events(d)

        try:
            stats, note = _summarize_nf_feedback(csv_path)
        except Exception as e:
            # If this session is unreadable, include a placeholder row.
            stats = _SessionStats(
                n_frames=0,
                duration_sec=math.nan,
                dt_median_sec=math.nan,
                reward_frac=math.nan,
                artifact_frac=math.nan,
                artifact_ready_frac=math.nan,
                metric_mean=math.nan,
                metric_min=math.nan,
                metric_max=math.nan,
                metric_last=math.nan,
                threshold_mean=math.nan,
                threshold_min=math.nan,
                threshold_max=math.nan,
                threshold_last=math.nan,
                reward_rate_mean=math.nan,
                reward_rate_last=math.nan,
                bad_channels_mean=math.nan,
                phase_counts={},
                derived_durations={},
            )
            note = f"ERROR reading nf_feedback.csv: {e}"

        # Derived events summary (best-effort).
        if derived and derived.is_file():
            try:
                stats.derived_durations = _summarize_derived_events(derived)
            except Exception:
                stats.derived_durations = {}

        ts = _timestamp_utc(run_meta, csv_path)
        protocol = str((summary or {}).get("protocol") or "").strip() if summary else ""
        metric_spec = _metric_spec_string(summary)

        report_html: Optional[Path] = None
        if not args.no_generate_reports:
            report_html = _try_generate_nf_report(d)
        else:
            cand = d / "nf_feedback_report.html"
            report_html = cand if cand.is_file() else None

        sessions.append(
            _Session(
                outdir=d,
                csv_path=csv_path,
                summary=summary,
                run_meta=run_meta,
                derived_events_path=derived,
                stats=stats,
                timestamp_utc=ts,
                protocol=protocol,
                metric_spec=metric_spec,
                report_html=report_html,
                note=note,
            )
        )

    # Sort sessions by timestamp (if parseable) then path.
    def _key(s: _Session) -> Tuple[int, float, str]:
        ep = _parse_iso_to_epoch(s.timestamp_utc)
        have = 1 if ep is not None else 0
        # Sort unknown timestamps last.
        return (0 if have else 1, float(ep) if ep is not None else 0.0, str(s.outdir))

    sessions.sort(key=_key)

    if args.max_sessions and len(sessions) > int(args.max_sessions):
        sessions = sessions[-int(args.max_sessions) :]

    html_doc = _build_dashboard(sessions, out_p, roots)
    out_p.write_text(html_doc, encoding="utf-8")
    print(f"Wrote: {out_p}")

    if args.open:
        try:
            webbrowser.open(f"file://{out_p}")
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
