#!/usr/bin/env python3
"""selftest_rt_dashboard.py

Smoke-test for the real-time qEEG dashboard server.

This test:
  - creates a temporary outdir with small CSV fixtures
  - starts scripts/rt_qeeg_dashboard.py on an ephemeral port
  - fetches /api/meta and checks basic structure
  - opens SSE endpoints and reads at least one event

No third-party dependencies.
"""

from __future__ import annotations

import csv
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Dict, List, Tuple


def _write_csv(path: Path, header, rows) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for r in rows:
            w.writerow(r)
        f.flush()
        try:
            os.fsync(f.fileno())
        except Exception:
            pass


def _read_json(url: str, timeout: float = 5.0) -> Dict:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        data = resp.read()
    return json.loads(data.decode("utf-8"))


def _read_one_sse_event(url: str, timeout: float = 5.0) -> Dict:
    # Read until we see a "data:" line.
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        t0 = time.time()
        while True:
            if (time.time() - t0) > timeout:
                raise TimeoutError(f"Timed out waiting for SSE data from {url}")
            line = resp.readline()
            if not line:
                time.sleep(0.05)
                continue
            if line.startswith(b"data:"):
                payload = line[len(b"data:") :].strip()
                return json.loads(payload.decode("utf-8"))


def _unwrap_batch(evt: Dict) -> Dict:
    """rt_qeeg_dashboard.py may emit SSE batches (frames...)."""
    if isinstance(evt, dict) and evt.get("type") == "batch" and isinstance(evt.get("frames"), list):
        frames = evt.get("frames") or []
        if not frames:
            raise AssertionError("Empty SSE batch")
        if not isinstance(frames[0], dict):
            raise AssertionError("Non-dict frame in SSE batch")
        return frames[0]
    return evt


def _read_sse_events(url: str, n: int, *, timeout: float = 5.0, after_first=None) -> List[Dict]:
    """Read n SSE "data:" events from a single connection.

    If after_first is provided, it is called after the first event is received.
    """
    evts: List[Dict] = []
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        t0 = time.time()
        while len(evts) < n:
            if (time.time() - t0) > timeout:
                raise TimeoutError(f"Timed out waiting for SSE data from {url}")
            line = resp.readline()
            if not line:
                time.sleep(0.05)
                continue
            if line.startswith(b"data:"):
                payload = line[len(b"data:") :].strip()
                evts.append(json.loads(payload.decode("utf-8")))
                if len(evts) == 1 and after_first is not None:
                    after_first()
    return evts


def _read_sse_named_events(url: str, want: List[str], timeout: float = 8.0) -> Dict[str, Dict]:
    """Read named SSE events (using the `event:` field).

    Returns a mapping from event name -> decoded JSON object for the first
    occurrence of each requested event.
    """
    want_set = set(want)
    got: Dict[str, Dict] = {}
    cur_event = "message"
    cur_data: List[str] = []
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        t0 = time.time()
        while want_set.difference(got.keys()):
            if (time.time() - t0) > timeout:
                missing = ",".join(sorted(want_set.difference(got.keys())))
                raise TimeoutError(f"Timed out waiting for SSE named events: {missing}")
            line = resp.readline()
            if not line:
                time.sleep(0.05)
                continue
            s = line.decode("utf-8", errors="replace").rstrip("\r\n")
            if not s:
                if cur_data:
                    try:
                        obj = json.loads("\n".join(cur_data))
                        if cur_event not in got:
                            got[cur_event] = obj
                    except Exception:
                        pass
                cur_event = "message"
                cur_data = []
                continue
            if s.startswith(":"):
                # comment / keepalive
                continue
            if s.startswith("event:"):
                cur_event = s[len("event:") :].strip() or "message"
                continue
            if s.startswith("data:"):
                cur_data.append(s[len("data:") :].lstrip())
                continue
            # ignore other SSE fields (retry/id/etc.)
    return got


def _start_server(outdir: Path) -> Tuple[subprocess.Popen, str, str]:
    proc = subprocess.Popen(
        [
            sys.executable,
            "-u",
            "scripts/rt_qeeg_dashboard.py",
            "--outdir",
            str(outdir),
            "--host",
            "127.0.0.1",
            "--port",
            "0",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        cwd=str(Path(__file__).resolve().parents[1]),
    )

    assert proc.stdout is not None
    url = None
    t0 = time.time()
    while (time.time() - t0) < 8.0:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        m = re.search(r"Dashboard:\s+(http://[^\s]+)", line)
        if m:
            url = m.group(1).strip()
            break
    if not url:
        proc.terminate()
        raise RuntimeError("Did not get dashboard URL from server")

    m = re.search(r"token=([^&]+)", url)
    if not m:
        proc.terminate()
        raise RuntimeError("Could not parse token from server URL")
    token = m.group(1)
    base = url.split("/?", 1)[0]
    return proc, base, token


def _stop_server(proc: subprocess.Popen) -> None:
    try:
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGINT)
            except Exception:
                proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="qeeg_rt_dash_") as td:
        outdir = Path(td)

        # Minimal NF feedback fixture.
        _write_csv(
            outdir / "nf_feedback.csv",
            ["t_end_sec", "metric", "threshold", "reward", "reward_rate", "artifact_ready", "artifact", "bad_channels"],
            [
                [0.25, 1.0, 0.9, 1, 1.0, 0, 0, 0],
                [0.50, 0.8, 0.9, 0, 0.5, 1, 1, 2],
            ],
        )

        # Minimal bandpower fixture: alpha/beta for two channels.
        _write_csv(
            outdir / "bandpower_timeseries.csv",
            ["t_end_sec", "alpha_ExG1", "alpha_ExG2", "beta_ExG1", "beta_ExG2"],
            [
                [0.25, 10.0, 12.0, 7.0, 8.0],
                [0.50, 11.0, 11.5, 7.2, 7.9],
            ],
        )

        # Minimal artifact gate fixture.
        _write_csv(
            outdir / "artifact_gate_timeseries.csv",
            ["t_end_sec", "ready", "bad", "bad_channels"],
            [
                [0.25, 0, 0, 0],
                [0.50, 1, 1, 2],
            ],
        )

        # Optional montage override fixture (tests custom montage + fallback positions).
        (outdir / "montage.csv").write_text(
            "# name,x,y\n"
            "ExG1,0.0,0.5\n",
            encoding="utf-8",
        )

        proc, base, token = _start_server(outdir)

        # Kiosk page should render (lightweight tablet view)
        with urllib.request.urlopen(f"{base}/kiosk?token={token}") as resp:
            html = resp.read().decode("utf-8", errors="replace")
            assert "qEEG Kiosk" in html

        # Main dashboard should render and reference external assets.
        with urllib.request.urlopen(f"{base}/?token={token}") as resp:
            html = resp.read().decode("utf-8", errors="replace")
            assert "qEEG Real-time Dashboard" in html
            assert "/app.js" in html
            assert "/app_legacy.js" in html

        # Static assets should exist.
        with urllib.request.urlopen(f"{base}/app.js") as resp:
            js = resp.read().decode("utf-8", errors="replace")
            assert "EventSource" in js
        with urllib.request.urlopen(f"{base}/app_legacy.js") as resp:
            js2 = resp.read().decode("utf-8", errors="replace")
            assert "XMLHttpRequest" in js2 or "EventSource" in js2
        try:
            meta = _read_json(f"{base}/api/meta?token={token}")
            assert "outdir" in meta
            assert "files" in meta
            assert "files_stat" in meta

            bp = meta.get("bandpower")
            assert isinstance(bp, dict)
            assert bp.get("channels") == ["ExG1", "ExG2"]
            assert len(bp.get("positions") or []) == 2
            assert all(p is not None for p in (bp.get("positions") or []))
            assert (bp.get("positions_source") or [None, None])[0] == "custom"
            assert (bp.get("positions_source") or [None, None])[1] == "fallback"
            assert bp.get("fallback_positions_count") == 1

            nf_event = _read_one_sse_event(f"{base}/api/sse/nf?token={token}")
            nf_event = _unwrap_batch(nf_event)
            assert "t" in nf_event and "metric" in nf_event

            bp_event = _read_one_sse_event(f"{base}/api/sse/bandpower?token={token}")
            bp_event = _unwrap_batch(bp_event)
            assert "t" in bp_event and "values" in bp_event

            art_event = _read_one_sse_event(f"{base}/api/sse/artifact?token={token}")
            art_event = _unwrap_batch(art_event)
            assert "t" in art_event and "bad" in art_event

            meta_evt = _read_one_sse_event(f"{base}/api/sse/meta?token={token}")
            meta_evt = _unwrap_batch(meta_evt)
            assert "outdir" in meta_evt and "files" in meta_evt

            # Multiplexed SSE stream should emit named events for the unified frontend.
            want = ["config", "meta", "state", "nf", "artifact", "bandpower"]
            got = _read_sse_named_events(
                f"{base}/api/sse/stream?token={token}&topics=" + ",".join(want),
                want,
                timeout=8.0,
            )
            cfg_evt = got.get("config") or {}
            assert cfg_evt.get("supports", {}).get("sse_stream") is True
            assert "files" in _unwrap_batch(got.get("meta") or {})
            assert "schema_version" in _unwrap_batch(got.get("state") or {})
            assert "metric" in _unwrap_batch(got.get("nf") or {})
            assert "values" in _unwrap_batch(got.get("bandpower") or {})
            assert "bad" in _unwrap_batch(got.get("artifact") or {})

            cfg = _read_json(f"{base}/api/config?token={token}")
            assert cfg.get("supports", {}).get("ui_state") is True

            snap = _read_json(
                f"{base}/api/snapshot?token={token}&topics=nf,bandpower,artifact,meta,state&wait=0&limit=50"
            )
            assert snap.get("server_instance_id")
            assert "meta" in snap and "batch" in snap["meta"]
            assert "nf" in snap and "batch" in snap["nf"]

            st0 = _read_json(f"{base}/api/state?token={token}")
            assert int(st0.get("schema_version") or 0) >= 1

            def do_update():
                req = urllib.request.Request(
                    f"{base}/api/state?token={token}",
                    data=json.dumps({"paused": True, "win_sec": 30, "transform": "log10", "client_id": "selftest"}).encode("utf-8"),
                    method="PUT",
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=5.0) as r:
                    _ = r.read()

            evts = _read_sse_events(f"{base}/api/sse/state?token={token}", 2, timeout=8.0, after_first=do_update)
            # First is an initial snapshot, second should reflect the update.
            st1 = _unwrap_batch(evts[-1])
            assert st1.get("paused") is True
            assert int(st1.get("win_sec") or 0) == 30
            assert st1.get("transform") == "log10"
        finally:
            _stop_server(proc)

    print("selftest_rt_dashboard: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
