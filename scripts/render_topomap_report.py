#!/usr/bin/env python3
"""Render qEEG topomap outputs into a self-contained HTML report.

This repo contains CLI tools that render scalp topographic maps ("topomaps") as
BMP images, for example:

  - qeeg_map_cli (when invoked with --annotate / topomap output enabled)
  - qeeg_topomap_cli (renders topomaps from a per-channel CSV table)

Typical artifacts include:

  - topomap_<metric>.bmp
  - topomap_<metric>_z.bmp            (optional; when a reference / z-scoring is used)
  - topomap_run_meta.json             (optional; tool run metadata)

This script turns those artifacts into a single HTML file with inline CSS and
embedded images (data URIs). The resulting HTML makes **no network requests**
and is safe to open locally.

Usage:

  # From an output directory containing topomap_*.bmp
  python3 scripts/render_topomap_report.py --input out_topomap

  # Or point at a single BMP (the report will include all topomap_*.bmp in the same folder)
  python3 scripts/render_topomap_report.py --input out_topomap/topomap_alpha.bmp

The default output is: <outdir>/topomap_report.html
"""

from __future__ import annotations

import argparse
import base64
import os
import re
import webbrowser
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

from report_common import (
    BASE_CSS,
    e as _e,
    is_dir as _is_dir,
    posix_relpath as _posix_relpath,
    read_json_if_exists as _read_json_if_exists,
    utc_now_iso,
)

_TOPOMAP_IMG_RE = re.compile(r"^topomap_.+?\.(bmp|png|jpe?g|gif|svg)$", re.IGNORECASE)


def _guess_mime(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bmp":
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


def _read_file_bytes(path: str, *, max_bytes: int) -> bytes:
    with open(path, "rb") as f:
        b = f.read(max_bytes + 1)
    if len(b) > max_bytes:
        raise RuntimeError(f"File too large to embed ({len(b)} bytes): {path}\nTip: re-run with --no-embed or increase --max-embed-mb.")
    return b


def _file_to_data_uri(path: str, *, max_bytes: int) -> str:
    try:
        b = _read_file_bytes(path, max_bytes=max_bytes)
    except Exception:
        return ""
    mime = _guess_mime(path)
    return f"data:{mime};base64,{base64.b64encode(b).decode('ascii')}"


def _bmp_dimensions(path: str) -> Tuple[Optional[int], Optional[int]]:
    """Best-effort width/height for BMPs (returns (None, None) on failure)."""

    try:
        with open(path, "rb") as f:
            hdr = f.read(26)
        if len(hdr) < 26 or hdr[:2] != b"BM":
            return None, None
        w = int.from_bytes(hdr[18:22], "little", signed=True)
        h = int.from_bytes(hdr[22:26], "little", signed=True)
        if w <= 0:
            return None, None
        if h == 0:
            return w, None
        return w, abs(h)
    except Exception:
        return None, None


def _pick_topomap_files_from_run_meta(outdir: str, run_meta: Optional[Dict[str, Any]]) -> List[str]:
    if not isinstance(run_meta, dict):
        return []
    outs = run_meta.get("Outputs")
    if not isinstance(outs, list):
        return []
    files: List[str] = []
    for v in outs:
        if not isinstance(v, str):
            continue
        if _TOPOMAP_IMG_RE.match(v):
            p = os.path.join(outdir, v)
            if os.path.exists(p) and os.path.isfile(p):
                files.append(p)
    return sorted(set(files))


def _collect_topomap_files(outdir: str, run_meta: Optional[Dict[str, Any]]) -> List[str]:
    # Prefer run_meta ordering when available.
    files = _pick_topomap_files_from_run_meta(outdir, run_meta)
    if files:
        return files

    try:
        names = os.listdir(outdir)
    except Exception:
        names = []
    out: List[str] = []
    for n in sorted(names):
        if _TOPOMAP_IMG_RE.match(n or ""):
            p = os.path.join(outdir, n)
            if os.path.isfile(p):
                out.append(p)
    return out


def _parse_metric_and_variant(filename: str) -> Tuple[str, str]:
    """Return (metric_key, variant) where variant is 'raw' or 'z'."""

    base = os.path.basename(filename)
    stem, _ext = os.path.splitext(base)
    if not stem.lower().startswith("topomap_"):
        return "metric", "raw"
    metric_part = stem[len("topomap_") :]
    metric_part = metric_part.strip("_") or "metric"
    if metric_part.lower().endswith("_z"):
        m = metric_part[:-2].strip("_") or "metric"
        return m, "z"
    return metric_part, "raw"


def _pretty_metric(metric: str) -> str:
    # Keep underscores (common in ratio names), but also provide a nicer label.
    s = str(metric or "metric")
    if not s:
        s = "metric"
    return s


@dataclass
class _MapImg:
    metric: str
    variant: str  # raw | z
    path: str
    rel_for_link: str
    data_uri: str
    size_bytes: int
    width: Optional[int] = None
    height: Optional[int] = None


def _guess_paths(inp: str, out: Optional[str]) -> Tuple[str, List[str], str, Optional[Dict[str, Any]]]:
    """Return (outdir, img_paths, html_path, run_meta_dict_or_None)."""

    run_meta: Optional[Dict[str, Any]] = None

    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        run_meta = _read_json_if_exists(os.path.join(outdir, "topomap_run_meta.json"))
        imgs = _collect_topomap_files(outdir, run_meta)
        if not imgs:
            raise SystemExit(f"Could not find any topomap_*.bmp under: {outdir}")
        if out is None:
            out = os.path.join(outdir, "topomap_report.html")
        return outdir, imgs, os.path.abspath(out), run_meta

    # File path passed.
    p = os.path.abspath(inp)
    outdir = os.path.dirname(p) or "."
    run_meta = _read_json_if_exists(os.path.join(outdir, "topomap_run_meta.json"))
    imgs = _collect_topomap_files(outdir, run_meta)
    if not imgs and os.path.exists(p) and os.path.isfile(p) and _TOPOMAP_IMG_RE.match(os.path.basename(p)):
        imgs = [p]
    if not imgs:
        raise SystemExit(f"Could not find any topomap_*.bmp in: {outdir}")
    if out is None:
        out = os.path.join(outdir, "topomap_report.html")
    return outdir, imgs, os.path.abspath(out), run_meta


def _render(outdir: str, img_paths: Sequence[str], run_meta: Optional[Dict[str, Any]], *, out_html: str, embed: bool, max_embed_bytes: int) -> str:
    # Group by metric -> {raw,z}
    groups: Dict[str, Dict[str, _MapImg]] = {}

    for p in img_paths:
        metric, variant = _parse_metric_and_variant(p)
        rel = _posix_relpath(p, os.path.dirname(out_html) or ".")
        size = 0
        try:
            size = int(os.path.getsize(p))
        except Exception:
            size = 0
        w: Optional[int] = None
        h: Optional[int] = None
        if os.path.splitext(p)[1].lower() == ".bmp":
            w, h = _bmp_dimensions(p)
        data = ""
        if embed:
            data = _file_to_data_uri(p, max_bytes=max_embed_bytes)
        img = _MapImg(
            metric=metric,
            variant=variant,
            path=p,
            rel_for_link=rel,
            data_uri=data,
            size_bytes=size,
            width=w,
            height=h,
        )
        groups.setdefault(metric, {})[variant] = img

    # HTML sections
    tool = str(run_meta.get("Tool")) if isinstance(run_meta, dict) and run_meta.get("Tool") else ""
    ver = str(run_meta.get("Version")) if isinstance(run_meta, dict) and run_meta.get("Version") else ""
    inp_path = str(run_meta.get("input_path")) if isinstance(run_meta, dict) and run_meta.get("input_path") else ""

    header_meta_rows: List[str] = []
    if tool:
        header_meta_rows.append(f"<tr><th>Tool</th><td><code>{_e(tool)}</code></td></tr>")
    if ver:
        header_meta_rows.append(f"<tr><th>Version</th><td><code>{_e(ver)}</code></td></tr>")
    if inp_path:
        header_meta_rows.append(f"<tr><th>Input</th><td><code>{_e(inp_path)}</code></td></tr>")
    header_meta_rows.append(f"<tr><th>Output dir</th><td><code>{_e(outdir)}</code></td></tr>")
    header_meta_rows.append(f"<tr><th>Generated</th><td><code>{_e(utc_now_iso())}</code></td></tr>")
    header_meta_rows.append(f"<tr><th>Embedded images</th><td><code>{'yes' if embed else 'no'}</code></td></tr>")

    meta_table = (
        '<table class="kv">'
        + "".join(header_meta_rows)
        + "</table>"
    )

    # Gallery
    metric_keys = sorted(groups.keys(), key=lambda s: (s.lower(), s))
    cards: List[str] = []
    for metric in metric_keys:
        d = groups.get(metric, {})
        # Order: raw, z
        parts: List[str] = []
        for variant in ["raw", "z"]:
            if variant not in d:
                continue
            im = d[variant]
            label = "Raw" if variant == "raw" else "Z"
            # Choose src
            if embed and im.data_uri:
                src = im.data_uri
            else:
                src = im.rel_for_link
            dim = ""
            if im.width and im.height:
                dim = f"{im.width}×{im.height}"
            elif im.width and not im.height:
                dim = f"{im.width}×?"
            size_kb = ""
            if im.size_bytes > 0:
                size_kb = f"{im.size_bytes/1024.0:.1f} KB"
            info_bits = " · ".join([x for x in [dim, size_kb] if x])
            info = f"<div class=\"img-meta\">{_e(info_bits)}</div>" if info_bits else ""
            parts.append(
                '<figure class="map-fig">'
                f'<div class="fig-h">{_e(label)} <a class="raw-link" href="{_e(im.rel_for_link)}">open file</a></div>'
                f'<img class="map-img" alt="topomap { _e(metric) } { _e(label) }" src="{_e(src)}">'
                f'{info}'
                '</figure>'
            )

        pretty = _pretty_metric(metric)
        card = (
            '<div class="map-card">'
            f'<h3 class="metric">{_e(pretty)}</h3>'
            '<div class="map-row">'
            + "".join(parts)
            + '</div>'
            '</div>'
        )
        cards.append(card)

    cards_html = "".join(cards)

    css = BASE_CSS + r"""
.kv { width: 100%; border-collapse: collapse; margin-top: 10px; }
.kv th, .kv td { border-bottom: 1px solid var(--grid); padding: 8px 10px; vertical-align: top; text-align: left; }
.kv th { width: 140px; color: var(--muted); font-weight: 600; }

.map-card { border: 1px solid var(--grid); border-radius: 14px; padding: 12px 12px; margin: 12px 0; background: rgba(255,255,255,0.02); }
.map-card h3 { margin: 0 0 10px 0; font-size: 16px; }

.map-row { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px; align-items: start; }
.map-fig { margin: 0; padding: 10px; border: 1px solid var(--grid); border-radius: 12px; background: rgba(0,0,0,0.06); }
.fig-h { display: flex; justify-content: space-between; gap: 10px; font-size: 12px; color: var(--muted); margin-bottom: 6px; }
.raw-link { font-size: 12px; color: var(--link); text-decoration: none; }
.raw-link:hover { text-decoration: underline; }

.map-img { width: 100%; height: auto; image-rendering: auto; border-radius: 10px; border: 1px solid rgba(255,255,255,0.06); background: rgba(0,0,0,0.10); }
.img-meta { font-size: 12px; color: var(--muted); margin-top: 6px; }

.note { color: var(--muted); font-size: 12px; margin-top: 8px; }
"""

    title = "Topomap report"

    html = f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<title>{_e(title)}</title>
<style>
{css}
</style>
</head>
<body>
<main class=\"wrap\">
  <h1>{_e(title)}</h1>
  <p class=\"muted\">Self-contained HTML report (no network requests).</p>
  {meta_table}
  <p class=\"note\">Found <b>{len(metric_keys)}</b> metric(s) / <b>{len(img_paths)}</b> image file(s). If you prefer linking to image files instead of embedding, re-run with <code>--no-embed</code>.</p>
  {cards_html}
  <div class=\"footer\">Generated by scripts/render_topomap_report.py · { _e(utc_now_iso()) }</div>
</main>
</body>
</html>
"""
    with open(out_html, "w", encoding="utf-8") as f:
        f.write(html)
    return out_html


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render qEEG topomap BMP outputs into a self-contained HTML report.")
    ap.add_argument("--input", required=True, help="Input directory (preferred) or one topomap_*.bmp file.")
    ap.add_argument("--out", default=None, help="Output HTML file (default: <outdir>/topomap_report.html)")
    ap.add_argument("--no-embed", action="store_true", help="Do not embed images as data URIs (link to files instead).")

    ap.add_argument(
        "--max-embed-mb",
        type=float,
        default=25.0,
        help="Maximum size per image to embed (MiB). Larger images require --no-embed or a higher limit.",
    )
    ap.add_argument("--open", action="store_true", help="Open the report in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, img_paths, out_html, run_meta = _guess_paths(args.input, args.out)
    max_bytes = int(max(0.1, float(args.max_embed_mb)) * 1024 * 1024)

    _render(outdir, img_paths, run_meta, out_html=out_html, embed=not args.no_embed, max_embed_bytes=max_bytes)

    if args.open:
        try:
            webbrowser.open("file://" + os.path.abspath(out_html))
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
