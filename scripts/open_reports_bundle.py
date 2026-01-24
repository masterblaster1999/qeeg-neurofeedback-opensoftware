#!/usr/bin/env python3
"""Verify, safely extract, and open a QEEG reports dashboard bundle.

This repository can produce a portable ZIP bundle that contains:
  - a dashboard HTML
  - a JSON index (qeeg_reports_dashboard_index.json)
  - per-run report HTML files
  - optional local assets referenced by the HTML
  - optional SHA-256 manifest (qeeg_reports_bundle_manifest.json)

This script is a convenience wrapper around scripts/verify_reports_bundle.py.
It verifies the bundle (hashes + index references by default), extracts it
safely, and then either:
  - serves it over a local HTTP server (recommended), or
  - opens the extracted dashboard as a file:// URL.

Serving is recommended because some browsers restrict features like
localStorage and fetch/XHR for local file:// pages.

Usage:
  # Verify + extract + serve (opens browser; Ctrl-C to stop)
  python3 scripts/open_reports_bundle.py --bundle qeeg_reports_bundle.zip

  # Extract into an explicit directory
  python3 scripts/open_reports_bundle.py --bundle qeeg_reports_bundle.zip --extract ./bundle_out

  # Extract + open file:// (no server)
  python3 scripts/open_reports_bundle.py --bundle qeeg_reports_bundle.zip --no-serve

  # For automation / tests
  python3 scripts/open_reports_bundle.py --bundle qeeg_reports_bundle.zip --no-serve --no-open
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import re
import shutil
import sys
import threading
import time
import webbrowser
import zipfile
from functools import partial
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional, Sequence, Tuple


# Best-effort: avoid polluting the source tree with __pycache__ when invoked
# from CI/CTest.
sys.dont_write_bytecode = True


def _cleanup_stale_pycache() -> None:
    """Remove bytecode cache files for this script (best-effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            if fn.startswith("open_reports_bundle.") and fn.endswith(".pyc"):
                try:
                    os.remove(os.path.join(pycache, fn))
                    removed_any = True
                except Exception:
                    pass

        if removed_any:
            try:
                if not os.listdir(pycache):
                    os.rmdir(pycache)
            except Exception:
                pass
    except Exception:
        pass


_cleanup_stale_pycache()


def _safe_zip_name(name: str) -> str:
    """Normalize and sanity-check a zip entry name (Zip Slip hardening)."""

    n = str(name).replace("\\", "/")
    is_dir = n.endswith("/")
    n = n.lstrip("/")
    if not n:
        raise RuntimeError("Invalid empty ZIP entry name")
    parts = [p for p in PurePosixPath(n).parts if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise RuntimeError(f"Unsafe ZIP entry name containing '..': {name}")
    if parts and re.match(r"^[A-Za-z]:", parts[0]):
        raise RuntimeError(f"Unsafe ZIP entry name with drive prefix: {name}")
    out = "/".join(parts)
    if is_dir and not out.endswith("/"):
        out += "/"
    return out


def _read_json_member(zf: zipfile.ZipFile, name: str) -> Any:
    with zf.open(name, "r") as f:
        return json.loads(f.read().decode("utf-8"))


def _find_default_index_name(names: List[str]) -> Optional[str]:
    # Common convention in this repo, but allow custom names.
    for cand in names:
        if cand.endswith("qeeg_reports_dashboard_index.json"):
            return cand
    # If there's exactly one .json at the top level, treat it as the index.
    top_json = [n for n in names if "/" not in n and n.lower().endswith(".json")]
    if len(top_json) == 1:
        return top_json[0]
    return None


def _detect_index_name_from_zip(
    bundle_zip: Path,
    *,
    manifest_name: str,
    index_name: Optional[str],
) -> Optional[str]:
    """Best-effort locate the index JSON name inside the ZIP."""

    if index_name:
        return _safe_zip_name(index_name)

    with zipfile.ZipFile(bundle_zip, "r") as zf:
        raw_names = zf.namelist()
        names = [_safe_zip_name(n) for n in raw_names]
        name_set = set(names)

        # Try manifest hint first.
        mname = _safe_zip_name(manifest_name)
        if mname in name_set:
            try:
                mobj = _read_json_member(zf, mname)
                if isinstance(mobj, dict) and isinstance(mobj.get("index_path"), str):
                    return _safe_zip_name(str(mobj.get("index_path")))
            except Exception:
                pass

        return _find_default_index_name(names)


def _resolve_rel_posix(base_dir: Path, posix_path: str) -> Path:
    """Resolve a POSIX-style relative path to a file under base_dir."""

    s = str(posix_path or "")
    if not s:
        return base_dir

    if os.path.isabs(s) or re.match(r"^[A-Za-z]:[\\/]", s):
        return Path(s).resolve()

    pp = PurePosixPath(s)
    return (base_dir.joinpath(*pp.parts)).resolve()


def _rel_posix(path: Path, base: Path) -> str:
    try:
        return path.relative_to(base).as_posix()
    except Exception:
        return path.as_posix()


class _QuietHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    """Silence request logs unless verbose is enabled."""

    def __init__(self, *args: object, directory: Optional[str] = None, verbose: bool = False, **kwargs: object):
        self._verbose = bool(verbose)
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, fmt: str, *args: object) -> None:
        if self._verbose:
            super().log_message(fmt, *args)


def _serve_and_maybe_open(
    *,
    root_dir: Path,
    dash_rel: str,
    host: str,
    port: int,
    open_browser: bool,
    serve_seconds: float,
    verbose: bool,
) -> int:
    """Serve root_dir over HTTP and open dash_rel."""

    root_dir = root_dir.resolve()
    handler = partial(_QuietHTTPRequestHandler, directory=str(root_dir), verbose=bool(verbose))

    # ThreadingHTTPServer is available in Python 3.7+.
    httpd = http.server.ThreadingHTTPServer((host, int(port)), handler)  # type: ignore[attr-defined]
    actual_port = int(httpd.server_address[1])

    url_host = host
    if url_host in ("0.0.0.0", "::"):
        url_host = "127.0.0.1"

    dash_rel = dash_rel.lstrip("/")
    url = f"http://{url_host}:{actual_port}/{dash_rel}"

    print(f"Serving extracted bundle at: http://{url_host}:{actual_port}/")
    print(f"Dashboard URL: {url}")

    if open_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass

    t = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.5}, daemon=True)
    t.start()

    try:
        if serve_seconds and serve_seconds > 0:
            time.sleep(float(serve_seconds))
        else:
            print("Press Ctrl-C to stop the server.")
            while True:
                time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            httpd.shutdown()
        except Exception:
            pass
        try:
            httpd.server_close()
        except Exception:
            pass

    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Verify, safely extract, and open a QEEG reports dashboard bundle ZIP.")
    ap.add_argument("--bundle", required=True, help="Path to qeeg_reports_bundle.zip")
    ap.add_argument(
        "--extract",
        default=None,
        help=(
            "Extraction directory (default: <bundle-dir>/<bundle-stem>_extracted). "
            "Will refuse to overwrite unless --force is provided."
        ),
    )
    ap.add_argument("--force", action="store_true", help="If extraction dir exists, delete it first.")

    ap.add_argument(
        "--manifest-name",
        default="qeeg_reports_bundle_manifest.json",
        help="Manifest filename inside the ZIP (default: qeeg_reports_bundle_manifest.json).",
    )
    ap.add_argument(
        "--index-name",
        default=None,
        help="Index JSON filename inside the ZIP (optional; auto-detected or read from manifest).",
    )
    ap.add_argument("--no-hashes", action="store_true", help="Skip SHA-256 manifest verification.")
    ap.add_argument("--no-index", action="store_true", help="Skip index reference verification.")

    ap.add_argument("--no-serve", action="store_true", help="Do not start a local HTTP server (open file:// instead).")
    ap.add_argument("--no-open", action="store_true", help="Do not open a browser window/tab.")

    ap.add_argument(
        "--host",
        default="127.0.0.1",
        help="Host/interface to bind the local server to (default: 127.0.0.1).",
    )
    ap.add_argument(
        "--port",
        type=int,
        default=0,
        help="Port to bind the local server to (default: 0 = auto).",
    )
    ap.add_argument(
        "--serve-seconds",
        type=float,
        default=0.0,
        help="If >0, run the local server for this many seconds then exit (useful for CI smoke tests).",
    )
    ap.add_argument("--verbose", action="store_true", help="Print verbose progress details.")

    args = ap.parse_args(list(argv) if argv is not None else None)

    bundle_zip = Path(str(args.bundle)).resolve()
    if not bundle_zip.is_file():
        print(f"ERROR: bundle does not exist: {str(bundle_zip)}", file=sys.stderr)
        return 1

    out_dir: Path
    if args.extract:
        out_dir = Path(str(args.extract)).resolve()
    else:
        out_dir = bundle_zip.parent / (bundle_zip.stem + "_extracted")
        out_dir = out_dir.resolve()

    if out_dir.exists():
        if not bool(args.force):
            print(f"ERROR: extraction dir already exists: {str(out_dir)} (use --force to overwrite)", file=sys.stderr)
            return 1
        try:
            shutil.rmtree(out_dir)
        except Exception as e:
            print(f"ERROR: failed to remove existing extraction dir: {e}", file=sys.stderr)
            return 1

    # Import verifier/extractor from sibling script.
    try:
        import verify_reports_bundle as _vb  # type: ignore
    except Exception as e:
        print(f"ERROR: Could not import verify_reports_bundle.py: {e}", file=sys.stderr)
        return 1

    ok, vlog = _vb.verify_bundle(
        bundle_zip,
        manifest_name=str(args.manifest_name or _vb.DEFAULT_MANIFEST_NAME),
        index_name=str(args.index_name) if args.index_name else None,
        check_hashes=not bool(args.no_hashes),
        check_index=not bool(args.no_index),
        verbose=bool(args.verbose),
    )
    for ln in vlog:
        if str(ln).startswith("ERROR"):
            print(str(ln), file=sys.stderr)
        else:
            print(str(ln))
    if not ok:
        return 1

    try:
        _vb.safe_extract_bundle(bundle_zip, out_dir, verbose=bool(args.verbose))
    except Exception as e:
        print(f"ERROR: extraction failed: {e}", file=sys.stderr)
        return 1

    # Locate the index in the extracted folder.
    idx_name = _detect_index_name_from_zip(
        bundle_zip,
        manifest_name=str(args.manifest_name or _vb.DEFAULT_MANIFEST_NAME),
        index_name=str(args.index_name) if args.index_name else None,
    )
    if not idx_name:
        print("ERROR: could not locate index JSON inside bundle", file=sys.stderr)
        return 1

    idx_path = out_dir.joinpath(*PurePosixPath(idx_name).parts)
    if not idx_path.is_file():
        print(f"ERROR: extracted index not found: {str(idx_path)}", file=sys.stderr)
        return 1

    try:
        idx_obj = json.loads(idx_path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"ERROR: failed to parse extracted index JSON: {e}", file=sys.stderr)
        return 1

    if not isinstance(idx_obj, dict):
        print("ERROR: index JSON must be an object", file=sys.stderr)
        return 1

    dash_rel = str(idx_obj.get("dashboard_html") or "")
    dash_path = _resolve_rel_posix(idx_path.parent, dash_rel)
    # Enforce that the resolved dashboard lives under the extracted folder.
    try:
        dash_path.relative_to(out_dir)
    except Exception:
        print(
            "ERROR: dashboard_html resolved outside extraction dir; refusing to open.\n"
            f"  dashboard_html: {dash_rel}\n"
            f"  resolved      : {str(dash_path)}\n"
            f"  extract dir   : {str(out_dir)}",
            file=sys.stderr,
        )
        return 1

    if not dash_path.is_file():
        print(f"ERROR: dashboard file missing after extraction: {str(dash_path)}", file=sys.stderr)
        return 1

    print(f"Extracted to: {str(out_dir)}")
    print(f"Dashboard file: {str(dash_path)}")

    open_browser = not bool(args.no_open)

    if args.no_serve:
        if open_browser:
            try:
                webbrowser.open(dash_path.resolve().as_uri())
            except Exception:
                pass
        return 0

    # Serve from the extraction root so relative links work.
    dash_rel_to_root = _rel_posix(dash_path, out_dir)
    return _serve_and_maybe_open(
        root_dir=out_dir,
        dash_rel=dash_rel_to_root,
        host=str(args.host or "127.0.0.1"),
        port=int(args.port or 0),
        open_browser=open_browser,
        serve_seconds=float(args.serve_seconds or 0.0),
        verbose=bool(args.verbose),
    )


if __name__ == "__main__":
    raise SystemExit(main())
