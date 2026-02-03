#!/usr/bin/env python3
"""Render qEEG topomap outputs into a self-contained HTML report.

This repo contains CLI tools that render scalp topographic maps ("topomaps") as
image files, for example:

  - qeeg_map_cli (when invoked with --annotate / topomap output enabled)
  - qeeg_topomap_cli (renders topomaps from a per-channel CSV table)

Typical artifacts include:

  - topomap_<metric>.bmp
  - topomap_<metric>_z.bmp            (optional; when a reference / z-scoring is used)
  - topomap_run_meta.json             (optional; tool run metadata)
  - topomap_index.json                (optional; machine-readable index from newer qeeg_topomap_cli)

This script turns those artifacts into a single HTML file with inline CSS and
embedded images (data URIs). The resulting HTML makes **no network requests**
and is safe to open locally.

Usage:

  # From an output directory containing topomap_*.bmp
  python3 scripts/render_topomap_report.py --input out_topomap

  # Or point at a single image (the report will include all topomap_*.* in the same folder)
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
    JS_THEME_TOGGLE,
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


def _file_to_data_uri(path: str, *, max_bytes: int) -> str:
    try:
        size = int(os.path.getsize(path))
    except Exception:
        size = 0
    if size <= 0:
        return ""
    if size > max_bytes:
        return ""
    mime = _guess_mime(path)
    with open(path, "rb") as f:
        data = f.read()
    b64 = base64.b64encode(data).decode("ascii")
    return f"data:{mime};base64,{b64}"


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
            p = os.path.join(outdir, v.replace("/", os.sep))
            if os.path.exists(p) and os.path.isfile(p):
                files.append(p)
    # stable unique
    seen: set[str] = set()
    out: List[str] = []
    for p in files:
        ap = os.path.abspath(p)
        if ap in seen:
            continue
        seen.add(ap)
        out.append(ap)
    return out


def _resolve_index_file(index_path: str, file_field: str) -> str:
    """Resolve an index map file entry to an absolute path."""

    if not file_field:
        return ""
    f = str(file_field).strip()
    if not f:
        return ""
    # Index uses POSIX separators; make it OS-friendly.
    f_os = f.replace("/", os.sep).replace("\\", os.sep)
    if os.path.isabs(f_os):
        return os.path.abspath(f_os)
    base = os.path.dirname(os.path.abspath(index_path)) or "."
    return os.path.abspath(os.path.join(base, f_os))


def _read_topomap_index(outdir: str, index_override: Optional[str]) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    """Return (index_dict_or_None, index_path_or_None)."""

    candidates: List[str] = []
    if index_override:
        candidates.append(index_override)
    candidates.append(os.path.join(outdir, "topomap_index.json"))

    for p in candidates:
        try:
            idx = _read_json_if_exists(p)
        except Exception:
            idx = None
        if isinstance(idx, dict) and isinstance(idx.get("maps"), list):
            return idx, os.path.abspath(p)
    return None, None


def _collect_topomap_files(
    outdir: str,
    run_meta: Optional[Dict[str, Any]],
    index: Optional[Dict[str, Any]],
    index_path: Optional[str],
) -> List[str]:
    """Collect topomap image files, preferring index/run_meta ordering when present."""

    outdir = os.path.abspath(outdir)

    # Discover all topomap_* images in the directory (for completeness).
    try:
        names = os.listdir(outdir)
    except Exception:
        names = []
    discovered: List[str] = []
    for n in sorted(names, key=lambda s: (str(s).lower(), str(s))):
        if _TOPOMAP_IMG_RE.match(n or ""):
            p = os.path.join(outdir, n)
            if os.path.isfile(p):
                discovered.append(os.path.abspath(p))
    discovered_set = set(discovered)

    ordered: List[str] = []

    # 1) Prefer index ordering.
    if isinstance(index, dict) and isinstance(index.get("maps"), list) and index_path:
        for m in index.get("maps", []):
            if not isinstance(m, dict):
                continue
            f = m.get("file")
            if not isinstance(f, str):
                continue
            p = _resolve_index_file(index_path, f)
            ap = os.path.abspath(p)
            if ap in discovered_set:
                ordered.append(ap)

    # 2) Next, prefer run_meta ordering.
    for p in _pick_topomap_files_from_run_meta(outdir, run_meta):
        ap = os.path.abspath(p)
        if ap in discovered_set:
            ordered.append(ap)

    # De-duplicate ordered list.
    out: List[str] = []
    seen: set[str] = set()
    for p in ordered:
        ap = os.path.abspath(p)
        if ap in seen:
            continue
        seen.add(ap)
        out.append(ap)

    # Append remaining discovered files not in the ordered set.
    for p in discovered:
        if p not in seen:
            out.append(p)
            seen.add(p)
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
    s = str(metric or "metric")
    return s or "metric"


def _rm_get(run_meta: Optional[Dict[str, Any]], *keys: str) -> str:
    if not isinstance(run_meta, dict):
        return ""
    for k in keys:
        if not k:
            continue
        v = run_meta.get(k)
        if v is None:
            continue
        if isinstance(v, (str, int, float)):
            t = str(v)
            if t:
                return t
    # legacy nested: Input.Path
    if "Input" in keys and isinstance(run_meta.get("Input"), dict):
        v = run_meta["Input"].get("Path")
        if isinstance(v, str) and v:
            return v
    return ""


@dataclass
class _MapImg:
    metric: str
    variant: str  # raw | z
    path: str
    rel_for_link: str
    data_uri: str
    size_bytes: int
    # Index metadata (optional)
    vmin: Optional[float] = None
    vmax: Optional[float] = None
    n_channels: Optional[int] = None


def _guess_paths(inp: str, out: Optional[str], index_override: Optional[str]) -> Tuple[str, List[str], str, Optional[Dict[str, Any]], Optional[Dict[str, Any]], Optional[str]]:
    """Return (outdir, img_paths, html_path, run_meta_dict_or_None, index_dict_or_None, index_path_or_None)."""

    run_meta: Optional[Dict[str, Any]] = None
    index: Optional[Dict[str, Any]] = None
    index_path: Optional[str] = None

    if _is_dir(inp):
        outdir = os.path.abspath(inp)
        run_meta = _read_json_if_exists(os.path.join(outdir, "topomap_run_meta.json"))
        index, index_path = _read_topomap_index(outdir, index_override)
        imgs = _collect_topomap_files(outdir, run_meta, index, index_path)
        if not imgs:
            raise SystemExit(f"Could not find any topomap_* images under: {outdir}")
        if out is None:
            out = os.path.join(outdir, "topomap_report.html")
        return outdir, imgs, os.path.abspath(out), run_meta, index, index_path

    # File path passed.
    p = os.path.abspath(inp)
    outdir = os.path.dirname(p) or "."
    run_meta = _read_json_if_exists(os.path.join(outdir, "topomap_run_meta.json"))
    index, index_path = _read_topomap_index(outdir, index_override)
    imgs = _collect_topomap_files(outdir, run_meta, index, index_path)
    if not imgs and os.path.exists(p) and os.path.isfile(p) and _TOPOMAP_IMG_RE.match(os.path.basename(p)):
        imgs = [p]
    if not imgs:
        raise SystemExit(f"Could not find any topomap_* images in: {outdir}")
    if out is None:
        out = os.path.join(outdir, "topomap_report.html")
    return outdir, imgs, os.path.abspath(out), run_meta, index, index_path


def _render(
    outdir: str,
    img_paths: Sequence[str],
    run_meta: Optional[Dict[str, Any]],
    index: Optional[Dict[str, Any]],
    index_path: Optional[str],
    *,
    out_html: str,
    embed: bool,
    max_embed_bytes: int,
) -> str:
    # Optional per-file metadata from the index
    idx_meta_by_abs: Dict[str, Dict[str, Any]] = {}
    if isinstance(index, dict) and isinstance(index.get("maps"), list) and index_path:
        for m in index.get("maps", []):
            if not isinstance(m, dict):
                continue
            f = m.get("file")
            if not isinstance(f, str):
                continue
            ap = os.path.abspath(_resolve_index_file(index_path, f))
            idx_meta_by_abs[ap] = m

    # Group by metric -> {raw,z}
    groups: Dict[str, Dict[str, _MapImg]] = {}

    for p in img_paths:
        metric, variant = _parse_metric_and_variant(p)
        rel = _posix_relpath(p, os.path.dirname(out_html) or ".")
        try:
            size = int(os.path.getsize(p))
        except Exception:
            size = 0

        data = ""
        if embed:
            data = _file_to_data_uri(p, max_bytes=max_embed_bytes)

        meta = idx_meta_by_abs.get(os.path.abspath(p))
        vmin: Optional[float] = None
        vmax: Optional[float] = None
        n_ch: Optional[int] = None
        if isinstance(meta, dict):
            try:
                vmin = float(meta.get("vmin")) if meta.get("vmin") is not None else None
            except Exception:
                vmin = None
            try:
                vmax = float(meta.get("vmax")) if meta.get("vmax") is not None else None
            except Exception:
                vmax = None
            try:
                n_ch = int(meta.get("n_channels")) if meta.get("n_channels") is not None else None
            except Exception:
                n_ch = None

        img = _MapImg(
            metric=metric,
            variant=variant,
            path=p,
            rel_for_link=rel,
            data_uri=data,
            size_bytes=size,
            vmin=vmin,
            vmax=vmax,
            n_channels=n_ch,
        )
        groups.setdefault(metric, {})[variant] = img

    # HTML header metadata
    tool = _rm_get(run_meta, "Tool")
    ver = _rm_get(run_meta, "QeegVersion", "Version")
    inp_path = _rm_get(run_meta, "InputPath", "input_path", "Input")

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

    # Index-derived metadata (if present)
    if isinstance(index, dict):
        mnt = index.get("montage")
        if isinstance(mnt, dict):
            spec = mnt.get("spec")
            if isinstance(spec, str) and spec:
                header_meta_rows.append(f"<tr><th>Montage</th><td><code>{_e(spec)}</code></td></tr>")
            n = mnt.get("n_channels")
            if isinstance(n, int):
                header_meta_rows.append(f"<tr><th>Montage channels</th><td><code>{n}</code></td></tr>")
        interp = index.get("interpolation")
        if isinstance(interp, dict):
            method = interp.get("method")
            grid = interp.get("grid")
            if isinstance(method, str) and method:
                header_meta_rows.append(f"<tr><th>Interpolation</th><td><code>{_e(method)}</code></td></tr>")
            if isinstance(grid, int):
                header_meta_rows.append(f"<tr><th>Grid</th><td><code>{grid}</code></td></tr>")
        scaling = index.get("scaling")
        if isinstance(scaling, dict):
            mode = scaling.get("mode")
            if isinstance(mode, str) and mode:
                header_meta_rows.append(f"<tr><th>Scaling</th><td><code>{_e(mode)}</code></td></tr>")
        if index_path and os.path.isfile(index_path):
            rel = _posix_relpath(index_path, os.path.dirname(out_html) or ".")
            header_meta_rows.append(
                f"<tr><th>Index</th><td><a href=\"{_e(rel)}\"><code>{_e(os.path.basename(index_path))}</code></a></td></tr>"
            )

    meta_table = "<table class=\"kv\">" + "".join(header_meta_rows) + "</table>"

    # Metric ordering: prefer index order, then alphabetical.
    metric_keys: List[str] = []
    if isinstance(index, dict) and isinstance(index.get("maps"), list):
        seen_m: set[str] = set()
        for m in index.get("maps", []):
            if not isinstance(m, dict):
                continue
            metric = m.get("metric")
            if not isinstance(metric, str) or not metric:
                continue
            if metric in groups and metric not in seen_m:
                metric_keys.append(metric)
                seen_m.add(metric)
    for m in sorted(groups.keys(), key=lambda s: (s.lower(), s)):
        if m not in metric_keys:
            metric_keys.append(m)

    cards: List[str] = []
    for metric in metric_keys:
        variants = groups.get(metric, {})
        pretty = _pretty_metric(metric)

        figs: List[str] = []
        for variant, label in (("raw", "raw"), ("z", "z")):
            img = variants.get(variant)
            if not img:
                continue
            src = img.data_uri if (embed and img.data_uri) else img.rel_for_link

            bits: List[str] = []
            if img.n_channels is not None:
                bits.append(f"n={img.n_channels}")
            if img.vmin is not None and img.vmax is not None:
                bits.append(f"vmin={img.vmin:.6g}, vmax={img.vmax:.6g}")
            if img.size_bytes:
                bits.append(f"{img.size_bytes/1024.0:.1f} KiB")
            info = f"<div class=\"img-meta\">{_e(' · '.join(bits))}</div>" if bits else ""

            figs.append(
                f"""<figure class=\"map-fig\">
  <div class=\"fig-h\"><span>{_e(label)}</span><a class=\"raw-link\" href=\"{_e(img.rel_for_link)}\">file</a></div>
  <img class=\"map-img\" alt=\"topomap {_e(metric)} {_e(label)}\" src=\"{_e(src)}\">
  {info}
</figure>"""
            )

        if not figs:
            continue

        cards.append(
            f"""<section class=\"map-card\">
  <h3><code>{_e(pretty)}</code></h3>
  <div class=\"map-row\">
    {''.join(figs)}
  </div>
</section>"""
        )

    cards_html = "\n".join(cards)

    css = BASE_CSS + "\n" + r""".muted { color: var(--muted); }
.wrap { max-width: 1200px; margin: 0 auto; padding: 24px 16px 60px 16px; }
.kv { width: 100%; border-collapse: collapse; margin: 14px 0 10px 0; }
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
"""  # noqa: E501

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
<script>
{JS_THEME_TOGGLE}
</script>
</body>
</html>
"""
    with open(out_html, "w", encoding="utf-8") as f:
        f.write(html)
    return out_html


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render qEEG topomap image outputs into a self-contained HTML report.")
    ap.add_argument("--input", required=True, help="Input directory (preferred) or one topomap_* image file.")
    ap.add_argument("--out", default=None, help="Output HTML file (default: <outdir>/topomap_report.html)")
    ap.add_argument("--index", default=None, help="Optional explicit topomap_index.json (preserves render order + adds metadata)")
    ap.add_argument("--no-embed", action="store_true", help="Do not embed images as data URIs (link to files instead).")

    ap.add_argument(
        "--max-embed-mb",
        type=float,
        default=25.0,
        help="Maximum size per image to embed (MiB). Larger images require --no-embed or a higher limit.",
    )
    ap.add_argument("--open", action="store_true", help="Open the report in your default browser.")
    args = ap.parse_args(list(argv) if argv is not None else None)

    outdir, img_paths, out_html, run_meta, index, index_path = _guess_paths(args.input, args.out, args.index)
    max_bytes = int(max(0.1, float(args.max_embed_mb)) * 1024 * 1024)

    _render(outdir, img_paths, run_meta, index, index_path, out_html=out_html, embed=not args.no_embed, max_embed_bytes=max_bytes)

    if args.open:
        try:
            webbrowser.open("file://" + os.path.abspath(out_html))
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
