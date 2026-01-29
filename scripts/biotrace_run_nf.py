#!/usr/bin/env python3
"""biotrace_run_nf.py

One-step helper for Mind Media BioTrace+ / NeXus session containers (ZIP-like .bcd/.mbd/.m2k/.zip).

Background
----------
BioTrace+ can export recordings to open formats like EDF/BDF or ASCII. Those exported
files are supported directly by this repository.

However, some exported/backup `.bcd`/`.mbd`/`.m2k` files are *ZIP containers* that already
include an embedded EDF/BDF/ASCII export. For those cases, this script can:

  1) Extract the best embedded export to a temp folder (or user-specified folder)
  2) Launch `qeeg_nf_cli` with `--input <extracted>` and `--outdir <outdir>`
  3) Pass through the remaining CLI args you provide (metric, baseline, realtime, etc.)

This does **not** implement the proprietary BioTrace container format; it only works
for the ZIP-like containers (best effort).

Usage
-----
List embedded items:

  python3 scripts/biotrace_run_nf.py --container session.m2k --list

Run neurofeedback from a container:

  python3 scripts/biotrace_run_nf.py --container session.m2k --outdir out_nf \
    --metric alpha/beta:Pz --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
    --realtime --export-bandpowers --flush-csv

If the container has multiple embedded recordings, you can pick one explicitly:

  # Use the 2nd candidate shown by --list
  python3 scripts/biotrace_run_nf.py --container session.m2k --outdir out_nf --select 2 \
    --metric alpha:Pz --window 2.0

Tips
----
- You can view the outputs live with:
    python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --open
- Or start the dashboard automatically while qeeg_nf_cli runs:
    python3 scripts/biotrace_run_nf.py --container session.m2k --outdir out_nf --dashboard --open-dashboard \
      --metric alpha/beta:Pz --window 2.0 --realtime
- For tablets on the same LAN:
    python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --host 0.0.0.0 --allow-remote
    (open the printed Kiosk (LAN) URL)
- For programmatic integrations, use --run-json to emit a machine-readable manifest of what was run.

"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from urllib.parse import urlparse
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# Best-effort: avoid polluting the source tree with __pycache__ when this script
# is invoked from CI/CTest.
sys.dont_write_bytecode = True


# JSON Schema identifier for --list-json output (matches schemas/biotrace_run_nf_list.schema.json).
SCHEMA_LIST_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_run_nf_list.schema.json"
)

# JSON Schema identifier for --run-json output.
SCHEMA_RUN_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_run_nf_run.schema.json"
)



def _cleanup_stale_pycache() -> None:
    """Remove bytecode cache files for this script (best effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            # Typical filename:
            #   biotrace_run_nf.cpython-311.pyc
            if fn.startswith("biotrace_run_nf.") and fn.endswith(".pyc"):
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


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _scripts_dir() -> Path:
    return _repo_root() / "scripts"


def _import_biotrace_extract():
    # Import scripts/biotrace_extract_container.py as a normal module
    scripts = _scripts_dir()
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    import biotrace_extract_container  # type: ignore

    return biotrace_extract_container


def _default_nf_cli_path() -> Optional[str]:
    """Try to locate a built qeeg_nf_cli binary (best effort).

    Priority:
      1) QEEG_NF_CLI env var (if set to an existing file)
      2) Common build folders in this repo (including CMake preset build dirs)
      3) PATH lookup (shutil.which)

    Note: CMakePresets.json in this repo uses build/* subdirectories (e.g. build/release),
    so we check those explicitly in addition to legacy build/ locations.
    """

    # Allow callers / CI wrappers to override the lookup in a stable way.
    env_path = os.environ.get("QEEG_NF_CLI", "").strip()
    if env_path:
        p = Path(env_path)
        if p.exists() and p.is_file():
            return str(p)

    root = _repo_root()

    # Executable name varies by platform.
    exe = "qeeg_nf_cli.exe" if os.name == "nt" else "qeeg_nf_cli"

    # Common build locations in this repo (ordered by usefulness / typical presets).
    build_root = root / "build"
    build_dirs = [
        build_root,  # legacy / manual builds
        build_root / "release",
        build_root / "lto-release",
        build_root / "debug",
        build_root / "asan",
        build_root / "shared-release",
        build_root / "shared-debug",
        build_root / "package-release",
    ]

    # Multi-config generators (MSVC) may place binaries under <dir>/<Config>/.
    configs = ["Release", "Debug", "RelWithDebInfo", "MinSizeRel"]

    candidates: List[Path] = []
    for d in build_dirs:
        candidates.append(d / exe)
        for cfg in configs:
            candidates.append(d / cfg / exe)

    # Legacy capitalized dirs sometimes used by IDE builds.
    for cfg in configs:
        candidates.append(build_root / cfg / exe)

    # Windows convenience: if we didn't find an .exe, allow a .cmd/.bat wrapper with the same base name.
    if os.name == "nt":
        for d in build_dirs + [build_root]:
            for ext in (".cmd", ".bat"):
                candidates.append(d / ("qeeg_nf_cli" + ext))
                for cfg in configs:
                    candidates.append(d / cfg / ("qeeg_nf_cli" + ext))

    for p in candidates:
        try:
            if p.exists() and p.is_file():
                return str(p)
        except Exception:
            # Defensive: ignore permission/path errors and keep searching.
            pass

    # Last resort: scan build/ for the binary name (can help when the build dir is custom).
    try:
        if build_root.exists():
            # Prefer deterministic ordering (string sort).
            matches = sorted([p for p in build_root.rglob(exe) if p.is_file()], key=lambda x: str(x))
            if matches:
                return str(matches[0])
    except Exception:
        pass

    # Fall back to PATH.
    return shutil.which("qeeg_nf_cli")


def _strip_overrides(args: List[str]) -> List[str]:
    """Remove any user-provided --input/--outdir from passthrough args.

    This script owns those flags, so they cannot accidentally point somewhere else.
    """
    out: List[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--":
            i += 1
            continue
        if a.startswith("--input=") or a.startswith("--outdir="):
            i += 1
            continue
        if a in ("--input", "--outdir"):
            # Skip flag + value if present.
            i += 2 if (i + 1) < len(args) else 1
            continue
        out.append(a)
        i += 1
    return out


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Extract a ZIP-like BioTrace+/NeXus container (.bcd/.mbd/.m2k/.zip) and run qeeg_nf_cli in one step.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        allow_abbrev=False,
        epilog=(
            "Any additional arguments not recognized by this wrapper are forwarded to qeeg_nf_cli. "
            "You may optionally insert a `--` separator before the forwarded arguments."
        ),
    )
    ap.add_argument(
        "--container",
        required=True,
        help="Path to a ZIP-like BioTrace+/NeXus session container (.bcd/.mbd/.m2k/.zip) or a directory",
    )
    ap.add_argument(
        "--outdir",
        default="",
        help="Output directory for qeeg_nf_cli (required unless --list/--list-json)",
    )
    ap.add_argument(
        "--extract-dir",
        default="",
        help="Directory to place extracted export (default: a temporary folder that is deleted after qeeg_nf_cli exits)",
    )
    ap.add_argument(
        "--keep-extracted",
        action="store_true",
        help="Do not delete temporary extracted export folder",
    )
    ap.add_argument(
        "--prefer",
        default="",
        help=(
            "Comma-separated extension preference order used to pick the embedded recording "
            "(e.g. .edf,.bdf,.csv). Default: extractor defaults."
        ),
    )
    ap.add_argument(
        "--select",
        default="",
        help=(
            "Select which embedded recording to run on when the container has multiple candidates. "
            "Accepts 1-based index from --list, exact name, substring, or glob pattern (case-insensitive)."
        ),
    )
    ap.add_argument(
        "--nf-cli",
        default="",
        help="Path to qeeg_nf_cli (default: auto-detect in ./build/* preset dirs, env QEEG_NF_CLI, then PATH)",
    )

    dash = ap.add_argument_group("dashboard (optional)")
    dash.add_argument(
        "--dashboard",
        action="store_true",
        help="Start the real-time dashboard (scripts/rt_qeeg_dashboard.py) pointed at --outdir while qeeg_nf_cli runs.",
    )
    dash.add_argument(
        "--dashboard-host",
        default="",
        help="Dashboard bind host (default: rt_qeeg_dashboard.py default, typically 127.0.0.1).",
    )
    dash.add_argument(
        "--dashboard-port",
        type=int,
        default=0,
        help="Dashboard bind port (0 = ephemeral).",
    )
    dash.add_argument(
        "--dashboard-allow-remote",
        action="store_true",
        help="Allow binding the dashboard to non-loopback hosts (use only on trusted networks).",
    )
    dash.add_argument(
        "--dashboard-frontend-dir",
        default="",
        help="Optional dashboard frontend dir (index.html/app.js/style.css).",
    )
    dash.add_argument(
        "--dashboard-token",
        default="",
        help="Optional fixed dashboard token (default: dashboard generates a random token).",
    )
    dash.add_argument(
        "--open-dashboard",
        action="store_true",
        help="Open the dashboard URL in your default browser (uses the dashboard server's --open flag).",
    )
    dash.add_argument(
        "--open-dashboard-kiosk",
        action="store_true",
        help="Open the lightweight kiosk view (implies --open-dashboard).",
    )
    dash.add_argument(
        "--keep-dashboard",
        action="store_true",
        help="Do not terminate the dashboard process when qeeg_nf_cli exits.",
    )

    mode_group = ap.add_mutually_exclusive_group()
    mode_group.add_argument("--list", action="store_true", help="List container contents and exit")
    mode_group.add_argument("--list-json", action="store_true", help="List container contents as JSON and exit")
    mode_group.add_argument(
        "--run-json",
        action="store_true",
        help="Run qeeg_nf_cli and emit a machine-readable JSON run manifest to stdout (logs to stderr).",
    )

    # parse_known_args() lets callers pass qeeg_nf_cli flags directly without requiring a '--' separator.
    ns, unknown = ap.parse_known_args(argv)
    setattr(ns, "nf_args", list(unknown))
    return ns


def _dashboard_script_path() -> Path:
    return _scripts_dir() / "rt_qeeg_dashboard.py"


def _read_line_with_timeout(stream, timeout_sec: float) -> Optional[str]:
    """Read one line from a text stream with a timeout (portable).

    On Windows, select() does not work on pipes; using a tiny thread works everywhere.
    """
    out: List[str] = []

    def _reader() -> None:
        try:
            line = stream.readline()
            out.append(line)
        except Exception:
            pass

    th = threading.Thread(target=_reader, daemon=True)
    th.start()
    th.join(timeout_sec)
    if not out:
        return None
    return out[0]


def _http_get_json(url: str, *, timeout_sec: float = 1.5) -> Tuple[int, Optional[object], str]:
    """GET a URL and attempt to parse JSON (best effort, stdlib only)."""
    try:
        u = urlparse(url)
        scheme = (u.scheme or "http").lower()
        host = u.hostname or ""
        if not host:
            return 0, None, "missing host"
        port = int(u.port or (443 if scheme == "https" else 80))
        path = u.path or "/"
        if u.query:
            path = path + "?" + u.query

        conn_cls = http.client.HTTPSConnection if scheme == "https" else http.client.HTTPConnection
        conn = conn_cls(host, port, timeout=float(timeout_sec))
        try:
            conn.request("GET", path, headers={"Accept": "application/json"})
            resp = conn.getresponse()
            raw = resp.read() or b""
        finally:
            try:
                conn.close()
            except Exception:
                pass

        status = int(getattr(resp, "status", 0) or 0)
        if not raw:
            return status, None, ""
        try:
            obj = json.loads(raw.decode("utf-8", "replace"))
            return status, obj, ""
        except Exception as e:
            return status, None, f"json parse failed: {e}"
    except Exception as e:
        return 0, None, str(e)


def _wait_dashboard_health(health_url: str, *, timeout_sec: float = 3.0) -> Tuple[bool, str]:
    """Wait until the dashboard health endpoint answers (best effort)."""
    if not health_url:
        return False, "missing health_url"
    deadline = float(time.time()) + float(max(0.1, timeout_sec))
    last_err = ""
    while float(time.time()) < deadline:
        status, obj, err = _http_get_json(health_url, timeout_sec=1.2)
        if status == 200 and isinstance(obj, dict) and str(obj.get("status", "")).lower() == "ok":
            return True, ""
        if err:
            last_err = err
        elif status:
            last_err = f"status={status}"
        time.sleep(0.10)
    return False, last_err or "timeout"


def _terminate_proc(proc: subprocess.Popen, timeout_sec: float = 2.0) -> bool:
    """Terminate a subprocess (best effort)."""
    try:
        if proc.poll() is not None:
            return True
    except Exception:
        pass

    try:
        proc.terminate()
    except Exception:
        return False

    try:
        proc.wait(timeout=timeout_sec)
        return True
    except Exception:
        # TimeoutExpired or other
        pass

    try:
        proc.kill()
    except Exception:
        return False

    try:
        proc.wait(timeout=timeout_sec)
    except Exception:
        pass
    return True


def _start_dashboard(
    outdir: Path,
    *,
    host: str = "",
    port: int = 0,
    allow_remote: bool = False,
    frontend_dir: str = "",
    token: str = "",
    open_dashboard: bool = False,
    open_kiosk: bool = False,
    timeout_sec: float = 6.0,
) -> Tuple[Optional[subprocess.Popen], Dict[str, Any]]:
    """Start rt_qeeg_dashboard.py in a background process and capture its startup JSON."""

    rec: Dict[str, Any] = {
        "enabled": True,
        "cmd": [],
        "pid": None,
        "info": None,
        "error": None,
        "raw_line": None,
        "terminated": False,
    }

    script = _dashboard_script_path()
    if not script.exists():
        rec["error"] = f"dashboard script not found: {script}"
        return None, rec

    cmd: List[str] = [sys.executable, "-B", str(script), "--outdir", str(outdir), "--print-json"]
    if host:
        cmd += ["--host", str(host)]
    if int(port or 0) != 0:
        cmd += ["--port", str(int(port))]
    if allow_remote:
        cmd.append("--allow-remote")
    if frontend_dir:
        cmd += ["--frontend-dir", str(frontend_dir)]
    if token:
        cmd += ["--token", str(token)]
    if open_kiosk:
        cmd.append("--open-kiosk")
    elif open_dashboard:
        cmd.append("--open")

    rec["cmd"] = list(cmd)

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            text=True,
        )
        rec["pid"] = int(proc.pid) if proc.pid is not None else None
    except Exception as e:
        rec["error"] = f"failed to start dashboard: {e}"
        return None, rec

    # Read the first line (startup JSON) with a timeout.
    line: Optional[str] = None
    if proc.stdout is not None:
        line = _read_line_with_timeout(proc.stdout, timeout_sec)

    if not line:
        rc = None
        try:
            rc = proc.poll()
        except Exception:
            rc = None

        if rc is not None:
            rec["error"] = f"dashboard exited early (rc={rc})"
        else:
            rec["error"] = f"dashboard did not emit startup JSON within {timeout_sec:.1f}s"
        return proc, rec

    rec["raw_line"] = line.strip()

    try:
        rec["info"] = json.loads(line)
    except Exception as e:
        rec["error"] = f"failed to parse dashboard startup JSON: {e}"
        return proc, rec

    # Best-effort readiness check (avoids a race where the wrapper prints a URL before
    # the HTTP server is actually accepting connections).
    try:
        info = rec.get("info") if isinstance(rec.get("info"), dict) else None
        urls = info.get("urls") if isinstance(info, dict) else None
        health_url = ""
        if isinstance(urls, dict):
            health_url = str(urls.get("health_url") or "")
        hc_timeout = max(0.5, min(2.5, float(timeout_sec) * 0.5))
        ok, err = _wait_dashboard_health(health_url, timeout_sec=hc_timeout) if health_url else (False, "missing health_url")
        if isinstance(info, dict):
            info["health_check"] = {"url": health_url or None, "ok": bool(ok), "error": (err or None)}
    except Exception:
        pass

    return proc, rec


def _json_dump_stdout(obj: object) -> None:
    json.dump(obj, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    sys.stdout.flush()


def main(argv: Optional[List[str]] = None) -> int:
    ns = parse_args(argv)

    want_list_json = bool(getattr(ns, "list_json", False))
    want_run_json = bool(getattr(ns, "run_json", False))

    # In --run-json mode, stdout must be pure JSON; log to stderr.
    log_fp = sys.stderr if want_run_json else sys.stdout

    def log(*args: object, **kwargs: object) -> None:
        print(*args, file=log_fp, **kwargs)

    started_utc = float(time.time())

    container = Path(ns.container)
    if not container.exists():
        msg = f"Error: container does not exist: {container}"
        if want_list_json:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_LIST_ID,
                    "input": str(container),
                    "prefer_exts": [],
                    "candidates": [],
                    "error": msg,
                }
            )
        elif want_run_json:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_RUN_ID,
                    "schema_version": 1,
                    "started_utc": started_utc,
                    "finished_utc": float(time.time()),
                    "input": str(container),
                    "outdir": str(getattr(ns, "outdir", "") or ""),
                    "extract_dir": str(getattr(ns, "extract_dir", "") or ""),
                    "kept_extracted": bool(getattr(ns, "keep_extracted", False) or getattr(ns, "extract_dir", "")),
                    "prefer_exts": [],
                    "select": str(getattr(ns, "select", "") or ""),
                    "extracted": None,
                    "qeeg_nf_cli": None,
                    "cmd": [],
                    "forwarded_args": list(getattr(ns, "nf_args", []) or []),
                    "returncode": 2,
                    "dashboard": {
                        "enabled": bool(getattr(ns, "dashboard", False)),
                        "cmd": [],
                        "pid": None,
                        "info": None,
                        "error": None,
                        "raw_line": None,
                        "terminated": False,
                        "kept": bool(getattr(ns, "keep_dashboard", False)),
                    },
                    "error": msg,
                }
            )
        else:
            print(msg, file=sys.stderr)
        return 2

    bt = _import_biotrace_extract()

    # Optional preference override.
    prefer_exts = getattr(bt, "DEFAULT_PREFER", None)
    if ns.prefer:
        parse_fn = getattr(bt, "_parse_prefer_list", None)
        if callable(parse_fn):
            prefer_exts = parse_fn(ns.prefer)
        else:
            prefer_exts = [p.strip().lower() for p in ns.prefer.split(",") if p.strip()]

    # Validate container type early for clearer errors.
    if not container.is_dir() and not zipfile.is_zipfile(str(container)):
        msg = (
            "Error: container is not a ZIP-like session container.\n"
            "Some BioTrace+/NeXus exports are proprietary (e.g., BCD/MBD/M2K session backups) and cannot be opened here.\n"
            "Please export an open format from BioTrace+ (EDF/BDF/ASCII) and run qeeg_nf_cli on that file.\n"
            f"Container: {container}"
        )
        if want_list_json:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_LIST_ID,
                    "input": str(container),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "candidates": [],
                    "error": msg,
                }
            )
        elif want_run_json:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_RUN_ID,
                    "schema_version": 1,
                    "started_utc": started_utc,
                    "finished_utc": float(time.time()),
                    "input": str(container),
                    "outdir": str(getattr(ns, "outdir", "") or ""),
                    "extract_dir": str(getattr(ns, "extract_dir", "") or ""),
                    "kept_extracted": bool(getattr(ns, "keep_extracted", False) or getattr(ns, "extract_dir", "")),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "select": str(getattr(ns, "select", "") or ""),
                    "extracted": None,
                    "qeeg_nf_cli": None,
                    "cmd": [],
                    "forwarded_args": list(getattr(ns, "nf_args", []) or []),
                    "returncode": 2,
                    "dashboard": {
                        "enabled": bool(getattr(ns, "dashboard", False)),
                        "cmd": [],
                        "pid": None,
                        "info": None,
                        "error": None,
                        "raw_line": None,
                        "terminated": False,
                        "kept": bool(getattr(ns, "keep_dashboard", False)),
                    },
                    "error": msg,
                }
            )
        else:
            print(msg, file=sys.stderr)
        return 2

    if want_list_json:
        # Machine-friendly listing (stable JSON).
        try:
            if hasattr(bt, "list_contents_jsonable") and callable(getattr(bt, "list_contents_jsonable")):
                cand = bt.list_contents_jsonable(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents_jsonable(container)
            else:
                raw = bt.list_contents(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents(container)
                conv = getattr(bt, "candidates_to_jsonable", None)
                cand = conv(raw) if callable(conv) else raw

            obj = {
                "$schema": SCHEMA_LIST_ID,
                "input": str(container),
                "prefer_exts": list(prefer_exts) if prefer_exts else [],
                "candidates": cand,
            }
            _json_dump_stdout(obj)
            return 0
        except Exception as e:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_LIST_ID,
                    "input": str(container),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "candidates": [],
                    "error": str(e),
                }
            )
            return 2

    if ns.list:
        # Print a friendly listing and exit.
        items = bt.list_contents(container, prefer_exts=prefer_exts) if prefer_exts else bt.list_contents(container)
        if not items:
            print("(no listable contents; file may not be ZIP-like)")
            return 0
        for i, it in enumerate(items):
            name = it.get("name", "")
            ext = str(it.get("ext", "") or "")
            disp_name = name
            if ext and (not name.lower().endswith(ext.lower())):
                disp_name = f"{name} [as {ext}]"
            size = it.get("size", None)
            mtime = it.get("mtime", None)
            extra = []
            if size is not None:
                extra.append(f"{size} bytes")
            if mtime:
                extra.append(str(mtime))
            extra_s = ("; " + ", ".join(extra)) if extra else ""
            print(f"{i+1:02d}. {disp_name}{extra_s}")
        return 0

    if not ns.outdir:
        msg = "Error: --outdir is required unless --list/--list-json is used"
        if want_run_json:
            _json_dump_stdout(
                {
                    "$schema": SCHEMA_RUN_ID,
                    "schema_version": 1,
                    "started_utc": started_utc,
                    "finished_utc": float(time.time()),
                    "input": str(container),
                    "outdir": "",
                    "extract_dir": str(getattr(ns, "extract_dir", "") or ""),
                    "kept_extracted": bool(getattr(ns, "keep_extracted", False) or getattr(ns, "extract_dir", "")),
                    "prefer_exts": list(prefer_exts) if prefer_exts else [],
                    "select": str(getattr(ns, "select", "") or ""),
                    "extracted": None,
                    "qeeg_nf_cli": None,
                    "cmd": [],
                    "forwarded_args": list(getattr(ns, "nf_args", []) or []),
                    "returncode": 2,
                    "dashboard": {
                        "enabled": bool(getattr(ns, "dashboard", False)),
                        "cmd": [],
                        "pid": None,
                        "info": None,
                        "error": None,
                        "raw_line": None,
                        "terminated": False,
                        "kept": bool(getattr(ns, "keep_dashboard", False)),
                    },
                    "error": msg,
                }
            )
            return 2
        print(msg, file=sys.stderr)
        return 2

    outdir = Path(ns.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    # Determine where to extract.
    tmp_obj = None
    if ns.extract_dir:
        extract_dir = Path(ns.extract_dir)
        extract_dir.mkdir(parents=True, exist_ok=True)
    else:
        tmp_obj = tempfile.TemporaryDirectory(prefix="qeeg_biotrace_extract_")
        extract_dir = Path(tmp_obj.name)

    dashboard_proc: Optional[subprocess.Popen] = None
    dashboard_rec: Dict[str, Any] = {
        "enabled": bool(getattr(ns, "dashboard", False)),
        "cmd": [],
        "pid": None,
        "info": None,
        "error": None,
        "raw_line": None,
        "terminated": False,
        "kept": bool(getattr(ns, "keep_dashboard", False)),
    }

    extracted: Optional[Path] = None
    nf_cli: Optional[str] = None
    cmd: List[str] = []
    rc: int = 0
    err_msg: Optional[str] = None

    try:
        # Extract embedded export.
        try:
            if ns.select:
                if prefer_exts:
                    extracted = bt.extract_selected_export(container, extract_dir, ns.select, prefer_exts=prefer_exts)
                else:
                    extracted = bt.extract_selected_export(container, extract_dir, ns.select)
            else:
                if prefer_exts:
                    extracted = bt.extract_best_export(container, extract_dir, prefer_exts=prefer_exts)
                else:
                    extracted = bt.extract_best_export(container, extract_dir)
        except Exception as e:
            mode = "select/extract" if ns.select else "extract"
            err_msg = f"Error: failed to {mode} embedded export from {container}: {e}"
            rc = 3
            raise RuntimeError(err_msg)

        nf_cli = ns.nf_cli.strip() or _default_nf_cli_path()
        if not nf_cli:
            err_msg = (
                "Error: could not find qeeg_nf_cli. Build it first (see README), or pass --nf-cli /path/to/qeeg_nf_cli."
            )
            rc = 4
            raise RuntimeError(err_msg)

        passthrough = _strip_overrides(list(ns.nf_args))
        cmd = [nf_cli, "--input", str(extracted), "--outdir", str(outdir)] + passthrough

        # Optional: start dashboard in the background while qeeg_nf_cli runs.
        if bool(getattr(ns, "dashboard", False)):
            if getattr(ns, "open_dashboard_kiosk", False):
                ns.open_dashboard = True

            dashboard_proc, dashboard_rec2 = _start_dashboard(
                outdir,
                host=str(getattr(ns, "dashboard_host", "") or ""),
                port=int(getattr(ns, "dashboard_port", 0) or 0),
                allow_remote=bool(getattr(ns, "dashboard_allow_remote", False)),
                frontend_dir=str(getattr(ns, "dashboard_frontend_dir", "") or ""),
                token=str(getattr(ns, "dashboard_token", "") or ""),
                open_dashboard=bool(getattr(ns, "open_dashboard", False)),
                open_kiosk=bool(getattr(ns, "open_dashboard_kiosk", False)),
            )
            dashboard_rec.update(dashboard_rec2)

            # If we captured URLs, print a friendly hint.
            try:
                info = dashboard_rec.get("info") or {}
                urls = info.get("urls") if isinstance(info, dict) else None
                if isinstance(urls, dict) and urls.get("dashboard_url"):
                    log(f"Dashboard: {urls.get('dashboard_url')}")
                    if urls.get("kiosk_url"):
                        log(f"Kiosk:     {urls.get('kiosk_url')}")
            except Exception:
                pass

        log("Running:")
        log("  " + " ".join([str(c) for c in cmd]))
        if not want_run_json:
            sys.stdout.flush()
        else:
            sys.stderr.flush()

        try:
            proc = subprocess.run(cmd)
            rc = int(proc.returncode)
        except FileNotFoundError:
            err_msg = f"Error: qeeg_nf_cli not found/executable: {nf_cli}"
            rc = 5
            raise RuntimeError(err_msg)

    except Exception as e:
        if err_msg is None:
            err_msg = str(e) if str(e) else "unknown error"
        # Non-run-json mode prints errors directly (run-json will embed it).
        if not want_run_json:
            print(err_msg, file=sys.stderr)
    finally:
        # Terminate dashboard unless asked to keep it running.
        if dashboard_proc is not None and (not bool(getattr(ns, "keep_dashboard", False))):
            try:
                _terminate_proc(dashboard_proc, timeout_sec=2.0)
                dashboard_rec["terminated"] = True
            except Exception:
                pass

        # Cleanup temp extraction.
        if tmp_obj is not None and (not ns.keep_extracted):
            tmp_obj.cleanup()
        elif tmp_obj is not None and ns.keep_extracted:
            log(f"Kept extracted files in: {extract_dir}")

    if want_run_json:
        finished_utc = float(time.time())
        run_obj: Dict[str, Any] = {
            "$schema": SCHEMA_RUN_ID,
            "schema_version": 1,
            "started_utc": started_utc,
            "finished_utc": finished_utc,
            "input": str(container),
            "outdir": str(outdir),
            "extract_dir": str(extract_dir),
            "kept_extracted": bool(ns.keep_extracted or bool(ns.extract_dir)),
            "prefer_exts": list(prefer_exts) if prefer_exts else [],
            "select": str(ns.select or ""),
            "extracted": str(extracted) if extracted is not None else None,
            "qeeg_nf_cli": str(nf_cli) if nf_cli is not None else None,
            "cmd": [str(x) for x in cmd],
            "forwarded_args": [str(x) for x in list(getattr(ns, "nf_args", []) or [])],
            "returncode": int(rc),
            "dashboard": dashboard_rec,
            "error": err_msg,
        }
        _json_dump_stdout(run_obj)

    return int(rc)


if __name__ == "__main__":
    raise SystemExit(main())
