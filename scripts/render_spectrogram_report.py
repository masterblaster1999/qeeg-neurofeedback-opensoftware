#!/usr/bin/env python3
"""Render qeeg_spectrogram_cli outputs into a self-contained HTML report.

The qeeg_spectrogram_cli tool writes per-channel artifacts such as:

  - spectrogram_<channel>.bmp   (required for visualization)
  - spectrogram_<channel>.csv   (optional; wide matrix or long format)
  - spectrogram_run_meta.json   (optional; build/run metadata)

This script bundles those outputs into a single, dependency-free HTML report
(Python stdlib only) that is safe to open locally (no network requests).

Typical usage:

  # Point at an output directory produced by qeeg_spectrogram_cli
  python3 scripts/render_spectrogram_report.py --input out_spec

  # Or point at a specific artifact inside the output directory
  python3 scripts/render_spectrogram_report.py --input out_spec/spectrogram_Cz.bmp

Outputs (by default, written next to the inputs):

  - spectrogram_report.html
"""

from __future__ import annotations

import argparse
import base64
import csv
import math
import os
import re
import webbrowser
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    JS_SORT_TABLE,
    e,
    read_json_if_exists,
    read_text_if_exists,
    try_float,
    utc_now_iso,
)

# qeeg_spectrogram_cli produces BMP by default, but we allow a few common formats
# in case users convert images post-hoc.
_IMG_RE = re.compile(r"^spectrogram_(?P<ch>.+?)\.(?P<ext>bmp|png|jpe?g|gif|svg)$", re.IGNORECASE)


def _guess_mime(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bmp":
        # Some platforms may not have bmp registered; use a safe explicit mime.
        return "image/bmp"
    if ext == ".png":
        return "image/png"
    if ext in (".jpg", ".jpeg"):
        return "image/jpeg"
    if ext == ".gif":
        return "image/gif"
    if ext == ".svg":
        return "image/svg+xml"
    return "application/octet-stream"


def _file_to_data_uri(path: str) -> str:
    b = b""
    try:
        with open(path, "rb") as f:
            b = f.read()
    except Exception:
        return ""
    mime = _guess_mime(path)
    return f"data:{mime};base64,{base64.b64encode(b).decode('ascii')}"


@dataclass
class _CsvSummary:
    path: str
    fmt: str  # wide | long | unknown
    n_rows: int = 0
    n_cols: int = 0
    n_frames: int = 0
    n_freqs: int = 0
    time_min: float = math.nan
    time_max: float = math.nan
    freq_min: float = math.nan
    freq_max: float = math.nan
    power_min: float = math.nan
    power_max: float = math.nan
    power_mean: float = math.nan
    truncated: bool = False
    error: str = ""


def _update_minmax(cur_min: float, cur_max: float, v: float) -> Tuple[float, float]:
    if math.isnan(v):
        return cur_min, cur_max
    if math.isnan(cur_min) or v < cur_min:
        cur_min = v
    if math.isnan(cur_max) or v > cur_max:
        cur_max = v
    return cur_min, cur_max


def _summarize_csv(path: str, *, max_rows: int = 2_000_000) -> _CsvSummary:
    """Stream a spectrogram CSV and compute lightweight summary stats.

    The CLI can emit either:
      - wide format: time_sec,<freq1>,<freq2>,...
      - long format: time_sec,freq_hz,power_db

    We do not load the full matrix into memory.
    """
    out = _CsvSummary(path=path, fmt="unknown")
    try:
        with open(path, "r", encoding="utf-8-sig", newline="") as f:
            # Peek the header with a basic reader.
            r = csv.reader(f)
            header = next(r, [])
            if not header:
                out.error = "missing header row"
                return out

            h0 = str(header[0]).strip().lower()
            h1 = str(header[1]).strip().lower() if len(header) > 1 else ""

            # Detect format.
            if h0 == "time_sec" and h1 == "freq_hz":
                out.fmt = "long"
            elif h0 == "time_sec":
                out.fmt = "wide"
            else:
                # Unknown / user-modified
                out.fmt = "unknown"

            if out.fmt == "wide":
                # Frequencies are in the header after time_sec.
                freqs: List[float] = []
                for cell in header[1:]:
                    freqs.append(try_float(cell))
                out.n_cols = len(header)
                out.n_freqs = max(0, out.n_cols - 1)
                if freqs:
                    out.freq_min = min([v for v in freqs if not math.isnan(v)], default=math.nan)
                    out.freq_max = max([v for v in freqs if not math.isnan(v)], default=math.nan)

                pmin = math.nan
                pmax = math.nan
                psum = 0.0
                pcount = 0

                tmin = math.nan
                tmax = math.nan

                nframes = 0
                nrows = 1  # header

                for row in r:
                    if not row:
                        continue
                    nrows += 1
                    nframes += 1

                    t = try_float(row[0]) if row else math.nan
                    tmin, tmax = _update_minmax(tmin, tmax, t)

                    for cell in row[1:]:
                        v = try_float(cell)
                        if math.isnan(v):
                            continue
                        pmin, pmax = _update_minmax(pmin, pmax, v)
                        psum += v
                        pcount += 1

                    if nrows >= max_rows:
                        out.truncated = True
                        break

                out.n_rows = nrows
                out.n_frames = nframes
                out.time_min, out.time_max = tmin, tmax
                out.power_min, out.power_max = pmin, pmax
                if pcount:
                    out.power_mean = psum / float(pcount)
                return out

            # Rewind and use DictReader for long/unknown (handles re-ordered columns).
            f.seek(0)
            dr = csv.DictReader(f)
            if not dr.fieldnames:
                out.error = "missing header row (DictReader)"
                return out

            fields = [str(x).strip().lower() for x in (dr.fieldnames or [])]
            out.n_cols = len(fields)
            out.n_rows = 1  # header row

            # Try to locate columns.
            def _pick(*names: str) -> Optional[str]:
                for nm in names:
                    if nm.lower() in fields:
                        # Return the original fieldname with correct case.
                        for orig in (dr.fieldnames or []):
                            if str(orig).strip().lower() == nm.lower():
                                return str(orig)
                return None

            c_time = _pick("time_sec", "time", "t")
            c_freq = _pick("freq_hz", "freq", "f")
            c_pow = _pick("power_db", "power", "db")

            tmin = math.nan
            tmax = math.nan
            fmin = math.nan
            fmax = math.nan
            pmin = math.nan
            pmax = math.nan
            psum = 0.0
            pcount = 0

            # Best-effort unique counts (bounded).
            uniq_times: set[float] = set()
            uniq_freqs: set[float] = set()
            uniq_limit = 5000

            for row in dr:
                out.n_rows += 1
                if out.n_rows >= max_rows:
                    out.truncated = True
                    break

                t = try_float(row.get(c_time, "")) if c_time else math.nan
                fr = try_float(row.get(c_freq, "")) if c_freq else math.nan
                pw = try_float(row.get(c_pow, "")) if c_pow else math.nan

                tmin, tmax = _update_minmax(tmin, tmax, t)
                fmin, fmax = _update_minmax(fmin, fmax, fr)
                pmin, pmax = _update_minmax(pmin, pmax, pw)

                if not math.isnan(pw):
                    psum += pw
                    pcount += 1

                if (len(uniq_times) < uniq_limit) and (not math.isnan(t)):
                    uniq_times.add(round(t, 6))
                if (len(uniq_freqs) < uniq_limit) and (not math.isnan(fr)):
                    uniq_freqs.add(round(fr, 6))

            out.time_min, out.time_max = tmin, tmax
            out.freq_min, out.freq_max = fmin, fmax
            out.power_min, out.power_max = pmin, pmax
            if pcount:
                out.power_mean = psum / float(pcount)

            # Expose best-effort counts.
            if uniq_times:
                out.n_frames = len(uniq_times)
            if uniq_freqs:
                out.n_freqs = len(uniq_freqs)

            if out.fmt == "unknown":
                # If it wasn't detected as wide, treat as long-ish for display.
                out.fmt = "long"

            return out

    except FileNotFoundError:
        out.error = "missing"
        return out
    except Exception as ex:
        out.error = str(ex)
        return out


def _fmt_float(x: float, *, digits: int = 3) -> str:
    if x is None or math.isnan(x) or math.isinf(x):
        return ""
    try:
        return f"{x:.{digits}f}"
    except Exception:
        return str(x)


def _render_kv_table(d: Dict[str, Any], keys: Sequence[str]) -> str:
    rows: List[str] = []
    for k in keys:
        if k not in d:
            continue
        v = d.get(k)
        if isinstance(v, (dict, list)):
            # Keep it lightweight; show a compact JSON snippet.
            try:
                import json as _json

                vs = _json.dumps(v, ensure_ascii=False)
            except Exception:
                vs = str(v)
        else:
            vs = str(v)
        rows.append(f"<tr><th>{e(k)}</th><td>{e(vs)}</td></tr>")
    if not rows:
        return '<div class="muted">No metadata found.</div>'
    return (
        '<div class="table-wrap"><table class="kv"><tbody>'
        + "".join(rows)
        + "</tbody></table></div>"
    )


def _render_csv_summary(s: _CsvSummary, *, rel: str) -> str:
    if s.error:
        return f'<div class="muted">CSV: <code>{e(rel)}</code> ({e(s.error)})</div>'

    fmt = s.fmt
    parts: List[str] = []
    parts.append(f"<b>CSV</b>: <code>{e(rel)}</code> <span class='muted'>({e(fmt)} format)</span>")

    if s.fmt == "wide":
        parts.append(f"<span class='muted'>frames</span>: {s.n_frames}")
        parts.append(f"<span class='muted'>freq bins</span>: {s.n_freqs}")
    else:
        if s.n_frames:
            parts.append(f"<span class='muted'>unique times</span>: {s.n_frames}")
        if s.n_freqs:
            parts.append(f"<span class='muted'>unique freqs</span>: {s.n_freqs}")

    if not math.isnan(s.time_min) and not math.isnan(s.time_max):
        parts.append(f"<span class='muted'>t</span>: {_fmt_float(s.time_min)}–{_fmt_float(s.time_max)} s")
    if not math.isnan(s.freq_min) and not math.isnan(s.freq_max):
        parts.append(f"<span class='muted'>f</span>: {_fmt_float(s.freq_min)}–{_fmt_float(s.freq_max)} Hz")
    if not math.isnan(s.power_min) and not math.isnan(s.power_max):
        pm = _fmt_float(s.power_mean) if not math.isnan(s.power_mean) else ""
        extra = f" (mean {pm} dB)" if pm else ""
        parts.append(f"<span class='muted'>power</span>: {_fmt_float(s.power_min)}–{_fmt_float(s.power_max)} dB{extra}")
    if s.truncated:
        parts.append("<span class='badge warn'>truncated stats</span>")

    return "<div>" + " · ".join(parts) + "</div>"


def _find_input_dir(path: str) -> str:
    p = os.path.abspath(path)
    if os.path.isdir(p):
        return p
    return os.path.dirname(p) or "."


def _list_spectrogram_images(indir: str) -> List[Tuple[str, str]]:
    """Return list of (channel, image_path)."""
    out: List[Tuple[str, str]] = []
    try:
        for fn in os.listdir(indir):
            m = _IMG_RE.match(fn or "")
            if not m:
                continue
            ch = m.group("ch") or ""
            out.append((ch, os.path.join(indir, fn)))
    except Exception:
        pass
    out.sort(key=lambda x: (x[0].lower(), x[1].lower()))
    return out


def _relpath_under(base: str, p: str) -> str:
    try:
        rel = os.path.relpath(os.path.abspath(p), os.path.abspath(base))
        return rel.replace(os.sep, "/")
    except Exception:
        return p


def _build_html(indir: str, out_path: str) -> str:
    now = utc_now_iso()
    imgs = _list_spectrogram_images(indir)

    run_meta_path = os.path.join(indir, "spectrogram_run_meta.json")
    meta = read_json_if_exists(run_meta_path) or {}

    # Per-channel CSV summaries (optional)
    channel_blocks: List[str] = []
    if not imgs:
        channel_blocks.append(
            '<div class="note">No <code>spectrogram_*.bmp</code> images were found in this folder. '
            "If you only exported CSVs, re-run <code>qeeg_spectrogram_cli</code> without disabling BMP output.</div>"
        )
    else:
        for ch, img_path in imgs:
            rel_img = _relpath_under(indir, img_path)
            data_uri = _file_to_data_uri(img_path)

            # Optional CSV next to the image
            csv_path = os.path.splitext(img_path)[0] + ".csv"
            csv_rel = _relpath_under(indir, csv_path)
            csv_sum: Optional[_CsvSummary] = None
            csv_preview: Optional[str] = None
            if os.path.exists(csv_path):
                csv_sum = _summarize_csv(csv_path)
                csv_preview = read_text_if_exists(csv_path, max_bytes=80_000)

            # Optional per-channel parameter sidecar (qeeg_spectrogram_cli writes *_meta.txt).
            meta_txt_path = os.path.splitext(img_path)[0] + "_meta.txt"
            meta_txt_rel = _relpath_under(indir, meta_txt_path)
            meta_txt_preview: Optional[str] = None
            if os.path.exists(meta_txt_path):
                meta_txt_preview = read_text_if_exists(meta_txt_path, max_bytes=80_000)

            # Title / subtitle
            title = f"Channel {ch}" if ch else os.path.basename(img_path)

            # Render.
            img_html = (
                f"<div class='img-wrap'>"
                f"<div class='muted' style='font-size:12px;margin-bottom:6px;'>Image: <code>{e(rel_img)}</code></div>"
                f"<img class='spec-img' src='{e(data_uri)}' alt='{e(title)}'/>"
                f"</div>"
                if data_uri
                else f"<div class='note'>Could not read/encode image: <code>{e(rel_img)}</code></div>"
            )

            csv_html = ""
            if csv_sum:
                csv_html += _render_csv_summary(csv_sum, rel=csv_rel)
            if csv_preview:
                csv_html += (
                    "<details style='margin-top:8px;'>"
                    "<summary class='muted'>CSV preview (truncated)</summary>"
                    f"<pre style='margin-top:8px;'>{e(csv_preview)}</pre>"
                    "</details>"
                )

            if meta_txt_preview:
                csv_html += (
                    "<details style='margin-top:8px;'>"
                    f"<summary class='muted'>Meta sidecar: <code>{e(meta_txt_rel)}</code> (truncated)</summary>"
                    f"<pre style='margin-top:8px;'>{e(meta_txt_preview)}</pre>"
                    "</details>"
                )

            channel_blocks.append(
                "<div class='card'>"
                f"<h3>{e(title)}</h3>"
                f"{img_html}"
                f"{csv_html}"
                "</div>"
            )

    # Run meta summary
    meta_keys = [
        "Tool",
        "Version",
        "GitDescribe",
        "BuildType",
        "Compiler",
        "CppStandard",
        "TimestampUTC",
        "input_path",
        "fs_hz",
        "channel",
        "channels",
        "outdir",
        "OutputDir",
        "Outputs",
    ]

    # Optional full JSON dump
    meta_dump = ""
    if meta:
        try:
            import json as _json

            meta_dump = _json.dumps(meta, indent=2, ensure_ascii=False)
        except Exception:
            meta_dump = str(meta)

    meta_html = _render_kv_table(meta, meta_keys)
    if meta_dump:
        meta_html += (
            "<details style='margin-top:10px;'>"
            "<summary class='muted'>Full spectrogram_run_meta.json</summary>"
            f"<pre style='margin-top:8px;'>{e(meta_dump)}</pre>"
            "</details>"
        )
    else:
        if os.path.exists(run_meta_path):
            # read_json_if_exists returned None because parse failed
            bad_txt = read_text_if_exists(run_meta_path, max_bytes=80_000) or ""
            meta_html += (
                "<details style='margin-top:10px;'>"
                "<summary class='muted'>spectrogram_run_meta.json (unparsed)</summary>"
                f"<pre style='margin-top:8px;'>{e(bad_txt)}</pre>"
                "</details>"
            )

    css_extra = r"""
.kv th { text-align:left; color: var(--muted); font-weight: 600; padding: 6px 10px; border-bottom: 1px solid rgba(255,255,255,0.06); vertical-align: top; width: 220px; }
.kv td { padding: 6px 10px; border-bottom: 1px solid rgba(255,255,255,0.06); }
.table-wrap { overflow-x: auto; }
.img-wrap { overflow: auto; }
.spec-img {
  display: block;
  max-width: 100%;
  height: auto;
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 10px;
  background: #0b0f14;
  image-rendering: pixelated;
}
.badge { display:inline-block; padding:2px 8px; border-radius: 999px; border:1px solid rgba(255,255,255,0.12); font-size: 11px; }
.badge.warn { color: var(--warn); border-color: rgba(255,184,107,0.35); }
"""

    html_doc = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Spectrogram report</title>
<style>
{BASE_CSS}
{css_extra}
</style>
</head>
<body>
<header>
  <h1>Spectrogram report</h1>
  <div class="meta">
    <div><span class="muted">Input:</span> <code>{e(indir)}</code></div>
    <div><span class="muted">Generated (UTC):</span> {e(now)} · <span class="muted">Output:</span> <code>{e(out_path)}</code></div>
  </div>
</header>
<main>
  <div class="card">
    <h2>Run metadata</h2>
    {meta_html}
  </div>

  <div class="card">
    <h2>Spectrogram images</h2>
    <div class="note">
      The embedded images are the <code>spectrogram_*.bmp</code> outputs from <code>qeeg_spectrogram_cli</code>.
      Time is on the x-axis; low frequency is at the bottom of the plot.
    </div>
  </div>

  {''.join(channel_blocks)}

  <div class="footer">
    <div><b>Disclaimer:</b> This is research/educational software. It is not a medical device and is not intended for clinical decision-making.</div>
  </div>
</main>

<script>
{JS_SORT_TABLE}
</script>
</body>
</html>
"""
    return html_doc


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_spectrogram_cli outputs into a self-contained HTML report.")
    ap.add_argument("--input", required=True, help="Input directory or spectrogram_* artifact path.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <input-dir>/spectrogram_report.html)")
    ap.add_argument("--open", action="store_true", help="Open the generated report in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    indir = _find_input_dir(args.input)
    out_path = args.out or os.path.join(indir, "spectrogram_report.html")
    out_path = os.path.abspath(out_path)

    html_doc = _build_html(indir, out_path)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {out_path}")

    if args.open:
        try:
            webbrowser.open_new_tab("file://" + os.path.abspath(out_path))
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
