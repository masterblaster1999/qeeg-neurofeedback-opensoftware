#!/usr/bin/env python3
"""Render qeeg_trace_plot_cli outputs into a self-contained HTML report.

The qeeg_trace_plot_cli tool writes an SVG trace plot plus small metadata sidecars.
This script bundles those artifacts into a single, easy-to-share HTML file.

This script is intentionally dependency-free (Python stdlib only).

Inputs:
  - traces.svg (default name; required unless another SVG is present)
  - trace_plot_meta.txt (optional)
  - trace_plot_run_meta.json (optional)

Typical usage:

  # Pass an output directory produced by qeeg_trace_plot_cli
  python3 scripts/render_trace_plot_report.py --input out_traces

  # Or pass the SVG directly
  python3 scripts/render_trace_plot_report.py --input out_traces/traces.svg --out trace_plot_report.html

The generated HTML is self-contained (inline CSS; SVG embedded as a data URI) and
is safe to open locally.
"""

from __future__ import annotations

import argparse
import base64
import os
import pathlib
import webbrowser
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    e as _e,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_json_if_exists as _read_json_if_exists,
    read_text_if_exists as _read_text_if_exists,
    utc_now_iso,
)


def _pick_svg_from_run_meta(outdir: str, run_meta: Optional[Dict[str, Any]]) -> Optional[str]:
    if not isinstance(run_meta, dict):
        return None
    outs = run_meta.get("Outputs")
    if not isinstance(outs, list):
        return None
    for v in outs:
        if not isinstance(v, str):
            continue
        if v.lower().endswith(".svg"):
            cand = os.path.join(outdir, v)
            if os.path.exists(cand) and os.path.isfile(cand):
                return cand
    return None


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, str, str, Optional[Dict[str, Any]]]:
    """Return (outdir, svg_path, html_path, run_meta_dict_or_None)."""

    run_meta: Optional[Dict[str, Any]] = None

    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        run_meta = _read_json_if_exists(os.path.join(outdir, "trace_plot_run_meta.json"))
        svg_path = _pick_svg_from_run_meta(outdir, run_meta)
        if svg_path is None:
            # Default name
            cand = os.path.join(outdir, "traces.svg")
            if os.path.exists(cand) and os.path.isfile(cand):
                svg_path = cand
        if svg_path is None:
            # Fallback: first .svg in the directory.
            try:
                svgs = sorted([n for n in os.listdir(outdir) if n.lower().endswith(".svg")])
            except Exception:
                svgs = []
            if svgs:
                svg_path = os.path.join(outdir, svgs[0])
        if svg_path is None:
            raise SystemExit(f"Could not find an SVG trace plot under: {outdir}")

        if out is None:
            out = os.path.join(outdir, "trace_plot_report.html")
        return outdir, svg_path, os.path.abspath(out), run_meta

    # File path passed.
    p = os.path.abspath(inp)
    outdir = os.path.dirname(p) or "."
    if p.lower().endswith(".svg") and os.path.exists(p):
        svg_path = p
    else:
        # Allow passing meta/run_meta; still locate an SVG in the same folder.
        run_meta = _read_json_if_exists(os.path.join(outdir, "trace_plot_run_meta.json"))
        svg_path = _pick_svg_from_run_meta(outdir, run_meta) or os.path.join(outdir, "traces.svg")
        if not (os.path.exists(svg_path) and os.path.isfile(svg_path)):
            # Final fallback.
            try:
                svgs = sorted([n for n in os.listdir(outdir) if n.lower().endswith(".svg")])
            except Exception:
                svgs = []
            if svgs:
                svg_path = os.path.join(outdir, svgs[0])

    if out is None:
        out = os.path.join(outdir, "trace_plot_report.html")
    return outdir, svg_path, os.path.abspath(out), run_meta


def _read_file_bytes(path: str, *, max_bytes: int = 25 * 1024 * 1024) -> bytes:
    with open(path, "rb") as f:
        b = f.read(max_bytes + 1)
    if len(b) > max_bytes:
        raise RuntimeError(f"File too large to embed ({len(b)} bytes): {path}")
    return b


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
        '<div class="note">Build/run metadata written by <code>qeeg_trace_plot_cli</code> (if available).</div>'
        '<table class="kv">'
        + "".join(rows)
        + "</table>"
        + "</div>"
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Render qeeg_trace_plot_cli outputs to a self-contained HTML report (stdlib only).")
    ap.add_argument("--input", required=True, help="Path to outdir containing traces.svg, or the SVG itself.")
    ap.add_argument("--out", default=None, help="Output HTML path (default: <outdir>/trace_plot_report.html).")
    ap.add_argument("--title", default="Trace plot report", help="Report title.")
    ap.add_argument(
        "--link-svg",
        action="store_true",
        help="Do not embed the SVG; link to the local .svg file instead (keeps HTML smaller).",
    )
    ap.add_argument("--open", action="store_true", help="Open the generated HTML in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, svg_path, html_path, run_meta = _guess_paths(args.input, args.out)

    if not (os.path.exists(svg_path) and os.path.isfile(svg_path)):
        raise SystemExit(f"Could not find SVG at: {svg_path}")

    meta_txt = _read_text_if_exists(os.path.join(outdir, "trace_plot_meta.txt"))
    if run_meta is None:
        run_meta = _read_json_if_exists(os.path.join(outdir, "trace_plot_run_meta.json"))

    now = utc_now_iso()
    out_dir = os.path.dirname(os.path.abspath(html_path)) or "."

    # The trace SVG uses light theme colors (dark text); wrap on white background.
    if args.link_svg:
        rel_svg = _posix_relpath(svg_path, out_dir)
        svg_html = f'<div class="svg-frame"><object type="image/svg+xml" data="{_e(rel_svg)}" class="svg-obj"></object></div>'
    else:
        b = _read_file_bytes(svg_path)
        data = base64.b64encode(b).decode("ascii")
        svg_html = (
            '<div class="svg-frame">'
            f'<img class="svg-img" alt="Trace plot" src="data:image/svg+xml;base64,{data}">' 
            "</div>"
        )

    # Always provide a link to the raw SVG if it exists.
    rel_svg_for_link = _posix_relpath(svg_path, out_dir)
    open_svg_link = f'<a href="{_e(rel_svg_for_link)}">open raw SVG</a>'

    run_meta_card = _render_run_meta_card(run_meta)

    meta_block = ""
    if meta_txt:
        meta_block = (
            '<div class="card">'
            '<h2>trace_plot_meta.txt</h2>'
            '<div class="note">Parameters recorded by <code>qeeg_trace_plot_cli</code> for reproducibility.</div>'
            f'<pre>{_e(meta_txt)}</pre>'
            "</div>"
        )

    css = BASE_CSS + r"""
.kv { width: 100%; border-collapse: collapse; font-size: 13px; }
.kv th, .kv td { border-bottom: 1px solid rgba(255,255,255,0.08); padding: 8px; text-align: left; vertical-align: top; }
.kv th { width: 240px; color: #d7e4ff; background: #0f1725; }

.svg-frame {
  background: #ffffff;
  border-radius: 12px;
  border: 1px solid var(--grid);
  padding: 10px;
  overflow: auto;
}

.svg-img { width: 100%; height: auto; display: block; }
.svg-obj { width: 100%; height: 720px; display: block; }

@media (max-width: 900px) {
  .svg-obj { height: 520px; }
}
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
      This report bundles the SVG trace plot written by <code>qeeg_trace_plot_cli</code> into a single HTML.
      It is for research/educational inspection only and is not a medical device.
    </div>
    <div class=\"note\">Tip: {open_svg_link}</div>
  </div>

  {run_meta_card}

  <div class=\"card\">
    <h2>Trace plot</h2>
    <div class=\"note\">If the plot looks small, use your browser zoom. The plot is wrapped on a white background for readability.</div>
    {svg_html}
  </div>

  {meta_block}

  <div class=\"footer\">This HTML makes no network requests.</div>
</main>
</body>
</html>
"""

    os.makedirs(os.path.dirname(os.path.abspath(html_path)) or ".", exist_ok=True)
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_doc)

    print(f"Wrote: {html_path}")

    if args.open:
        try:
            webbrowser.open(pathlib.Path(os.path.abspath(html_path)).as_uri())
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
