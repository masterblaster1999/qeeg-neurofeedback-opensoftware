#!/usr/bin/env python3
"""selftest_rt_dashboard.py

Dependency-free smoke tests for the real-time qEEG dashboard server.

This test intentionally uses ONLY the Python standard library so it can run
in minimal CI environments.

It validates that:
- The dashboard server can start on an ephemeral port.
- Token redirects/authorization work.
- Key JSON endpoints respond and are parseable.
- Legacy per-topic SSE endpoints emit a well-formed preamble, include SSE id fields, and support resume via Last-Event-ID.
- The multiplexed SSE endpoint (/api/sse/stream) emits named events with JSON payloads and supports resume via Last-Event-ID.

This is not a performance test and it does not validate numerical correctness
of qEEG metrics.
"""

from __future__ import annotations

import http.client
import io
import json
import zipfile
import os
import sys
import threading
import time
from dataclasses import dataclass
from email.utils import formatdate
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Dict, List, Optional, Tuple


# Avoid writing .pyc files when running locally.
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")


@dataclass
class HttpResp:
    status: int
    headers: Dict[str, str]
    body: bytes


def _http_request(
    host: str,
    port: int,
    method: str,
    path: str,
    *,
    body: Optional[bytes] = None,
    headers: Optional[Dict[str, str]] = None,
    timeout: float = 5.0,
) -> HttpResp:
    h = headers or {}
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=h)
        resp = conn.getresponse()
        data = resp.read()
        hdrs = {k.lower(): v for (k, v) in resp.getheaders()}
        return HttpResp(status=int(resp.status), headers=hdrs, body=data)
    finally:
        try:
            conn.close()
        except Exception:
            pass





def _assert_sse_headers(hdrs: Dict[str, str]) -> None:
    ct = (hdrs.get("content-type", "") or "").lower()
    if "text/event-stream" not in ct:
        raise AssertionError(f"expected text/event-stream Content-Type (got {ct!r})")
    # When running behind nginx/reverse proxies, buffering can break SSE; the server should disable it.
    xab = (hdrs.get("x-accel-buffering", "") or "").strip().lower()
    if xab != "no":
        raise AssertionError(f"expected X-Accel-Buffering: no (got {xab!r})")


def _sse_read_first_data(
    host: str,
    port: int,
    path: str,
    *,
    request_headers: Optional[Dict[str, str]] = None,
    timeout: float = 5.0,
    max_lines: int = 200,
) -> Tuple[int, Dict[str, str], Optional[str], object]:
    """Connect to an SSE endpoint and return the first JSON object seen in a data: line.

    Returns: (status, response_headers, last_id, json_obj)
    where last_id is the most recent `id:` field seen before the data payload.
    """
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        h = {"Accept": "text/event-stream"}
        if request_headers:
            h.update(request_headers)
        conn.request("GET", path, headers=h)
        resp = conn.getresponse()
        status = int(resp.status)
        hdrs = {k.lower(): v for (k, v) in resp.getheaders()}

        # First line should be the SSE retry hint.
        line1 = resp.fp.readline().decode("utf-8", errors="replace")
        if not line1.startswith("retry:"):
            raise AssertionError(f"SSE preamble missing retry: line (got {line1!r})")

        # Then a blank line.
        _ = resp.fp.readline()

        cur_id: Optional[str] = None

        # Then either an id/event/data line or (rarely) comments/keepalives.
        for _i in range(max_lines):
            line = resp.fp.readline().decode("utf-8", errors="replace")
            if not line:
                break
            if line.startswith("id:"):
                cur_id = line[len("id:") :].strip() or None
                continue
            if line.startswith("data:"):
                payload = line[len("data:") :].strip()
                obj = json.loads(payload)
                return status, hdrs, cur_id, obj
            # Skip other SSE fields (event:, comments).

        raise AssertionError("SSE stream did not yield a data: JSON line")
    finally:
        try:
            conn.close()
        except Exception:
            pass



def _sse_read_n_events(
    host: str,
    port: int,
    path: str,
    *,
    n: int = 5,
    request_headers: Optional[Dict[str, str]] = None,
    timeout: float = 5.0,
    max_lines: int = 2000,
) -> Tuple[int, Dict[str, str], List[Tuple[str, Optional[str], object]]]:
    """Read the first N SSE messages (as named events) and parse their JSON payloads.

    This is used to validate /api/sse/stream, which sends named events like:
      id: <cursor>\n
      event: config\n
      data: {...}\n\n

    Notes:
    - We implement a minimal SSE parser sufficient for this test.
    - Multi-line data fields are supported (joined with \n).
    """
    if n <= 0:
        return 0, {}, []

    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        h = {"Accept": "text/event-stream"}
        if request_headers:
            h.update(request_headers)
        conn.request("GET", path, headers=h)
        resp = conn.getresponse()
        status = int(resp.status)
        hdrs = {k.lower(): v for (k, v) in resp.getheaders()}

        # Preamble: retry + blank line.
        line1 = resp.fp.readline().decode("utf-8", errors="replace")
        if not line1.startswith("retry:"):
            raise AssertionError(f"SSE preamble missing retry: line (got {line1!r})")
        _ = resp.fp.readline()

        events: List[Tuple[str, Optional[str], object]] = []
        cur_event: Optional[str] = None
        cur_id: Optional[str] = None
        data_lines: List[str] = []

        t_end = time.time() + float(timeout)
        for _i in range(max_lines):
            if len(events) >= n:
                break
            if time.time() > t_end:
                break

            raw = resp.fp.readline()
            if not raw:
                break

            s = raw.decode("utf-8", errors="replace").rstrip("\r\n")

            if s == "":
                # End of message.
                if data_lines:
                    payload = "\n".join(data_lines)
                    try:
                        obj = json.loads(payload)
                    except Exception as e:
                        raise AssertionError(f"Failed to parse SSE JSON payload: {e}: {payload!r}")
                    events.append((cur_event or "", cur_id, obj))
                cur_event = None
                cur_id = None
                data_lines = []
                continue

            if s.startswith(":"):
                # Comment / keepalive.
                continue

            if s.startswith("id:"):
                cur_id = s[len("id:") :].strip() or None
                continue

            if s.startswith("event:"):
                cur_event = s[len("event:") :].strip() or None
                continue

            if s.startswith("data:"):
                data_lines.append(s[len("data:") :].lstrip())
                continue

            # Ignore other fields (retry:, etc).

        if len(events) < n:
            raise AssertionError(f"Expected {n} SSE events but only read {len(events)}")

        return status, hdrs, events
    finally:
        try:
            conn.close()
        except Exception:
            pass


def _wait_until(pred, *, timeout_sec: float = 5.0, step_sec: float = 0.05) -> bool:
    t_end = time.time() + timeout_sec
    while time.time() < t_end:
        if pred():
            return True
        time.sleep(step_sec)
    return bool(pred())


def _write_min_nf_csv(outdir: Path) -> None:
    """Write a tiny nf_feedback.csv so the nf SSE stream has at least one frame."""
    p = outdir / "nf_feedback.csv"
    # Minimal columns used by rt_qeeg_dashboard LiveHub._run_nf.
    p.write_text(
        "t_end_sec,metric,threshold,reward,reward_rate\n"
        "1.0,0.5,0.3,1,0.2\n",
        encoding="utf-8",
    )




def _replace_nf_csv_atomic(outdir: Path, *, rows: str) -> None:
    """Atomically replace nf_feedback.csv (simulates temp-write + rename log rotation)."""
    p = outdir / "nf_feedback.csv"
    tmp = outdir / "nf_feedback.csv.tmp"
    tmp.write_text(rows, encoding="utf-8")
    try:
        os.replace(tmp, p)
    except Exception:
        # Fallback: overwrite in-place.
        p.write_text(rows, encoding="utf-8")

def _write_min_bandpower_csv(outdir: Path) -> None:
    """Write a tiny bandpower_timeseries.csv so the bandpower SSE stream has data."""
    p = outdir / "bandpower_timeseries.csv"
    # Format expected by parse_bandpower_timeseries_header(): <band>_<channel> columns.
    p.write_text(
        "t_end_sec,alpha_Pz,beta_Pz\n"
        "1.0,10.0,5.0\n",
        encoding="utf-8",
    )


def _write_min_artifact_csv(outdir: Path) -> None:
    """Write a tiny artifact_gate_timeseries.csv so the artifact SSE stream has data."""
    p = outdir / "artifact_gate_timeseries.csv"
    p.write_text(
        "t_end_sec,ready,bad,bad_channels\n"
        "1.0,1,0,0\n",
        encoding="utf-8",
    )


def main() -> int:
    # Import from the scripts/ directory (this file lives next to rt_qeeg_dashboard.py).
    try:
        import rt_qeeg_dashboard as dash  # type: ignore
    except Exception as e:
        print(f"ERROR: failed to import rt_qeeg_dashboard.py: {e}", file=sys.stderr)
        return 2

    with TemporaryDirectory(prefix="qeeg_rt_dash_") as td:
        outdir = Path(td)
        _write_min_nf_csv(outdir)
        _write_min_bandpower_csv(outdir)
        _write_min_artifact_csv(outdir)

        token = "selftest_token"
        frontend_dir = Path(__file__).resolve().parent / "rt_dashboard_frontend"

        cfg = dash.ServerConfig(
            outdir=outdir,
            host="127.0.0.1",
            port=0,
            token=token,
            max_hz=30.0,
            history_rows=50,
            meta_interval_sec=0.1,
            frontend_dir=frontend_dir,
        )

        httpd = dash.DashboardServer(cfg)
        host, port = str(httpd.server_address[0]), int(httpd.server_address[1])

        th = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        th.start()

        try:
            # Wait until /api/config responds.
            ok = _wait_until(
                lambda: _http_request(host, port, "GET", f"/api/config?token={token}").status == 200,
                timeout_sec=5.0,
            )
            if not ok:
                raise AssertionError("dashboard server did not become ready in time")

            # Authorization header token support (useful for non-SSE API clients).
            cfg_auth = _http_request(
                host,
                port,
                "GET",
                "/api/config",
                headers={"Authorization": f"Bearer {token}"},
            )
            if cfg_auth.status != 200:
                raise AssertionError(f"/api/config with Authorization header returned {cfg_auth.status}")
            cfg_auth_obj = json.loads(cfg_auth.body.decode("utf-8"))
            if not isinstance(cfg_auth_obj, dict) or int(cfg_auth_obj.get("schema_version", 0) or 0) < 1:
                raise AssertionError("/api/config (Authorization header) returned unexpected JSON")

            # Redirect behavior when token is missing.
            r = _http_request(host, port, "GET", "/")
            if r.status != 302:
                raise AssertionError(f"expected 302 redirect for / without token, got {r.status}")
            loc = r.headers.get("location", "")
            if f"token={token}" not in loc:
                raise AssertionError(f"redirect Location missing token (got {loc!r})")

            # HTML route with token.
            r = _http_request(host, port, "GET", f"/?token={token}")
            if r.status != 200:
                raise AssertionError(f"expected 200 for /?token=..., got {r.status}")
            if b"qEEG Real-time Dashboard" not in r.body and b"qEEG Neurofeedback" not in r.body:
                raise AssertionError("dashboard HTML did not contain expected marker text")

            # The HTML route should set a cookie so the token doesn't have to stay in the URL.
            set_cookie = str(r.headers.get("set-cookie", "") or "")
            if "qeeg_token=" not in set_cookie:
                raise AssertionError("expected Set-Cookie qeeg_token on HTML response")
            cookie_pair = set_cookie.split(";", 1)[0].strip()

            # With the cookie present, / should no longer redirect.
            r2 = _http_request(host, port, "GET", "/", headers={"Cookie": cookie_pair})
            if r2.status != 200:
                raise AssertionError(f"expected 200 for / with cookie, got {r2.status}")

            # With the cookie present, JSON endpoints should also authorize without ?token=...
            cfg_cookie = _http_request(host, port, "GET", "/api/config", headers={"Cookie": cookie_pair})
            if cfg_cookie.status != 200:
                raise AssertionError(f"expected 200 for /api/config with cookie, got {cfg_cookie.status}")

            meta_cookie = _http_request(host, port, "GET", "/api/meta", headers={"Cookie": cookie_pair})
            if meta_cookie.status != 200:
                raise AssertionError(f"expected 200 for /api/meta with cookie, got {meta_cookie.status}")

            # JSON endpoints.
            cfg_resp = _http_request(host, port, "GET", f"/api/config?token={token}")
            cfg_obj = json.loads(cfg_resp.body.decode("utf-8"))
            if not isinstance(cfg_obj, dict) or int(cfg_obj.get("schema_version", 0) or 0) < 1:
                raise AssertionError("/api/config returned unexpected JSON")
            supports = cfg_obj.get("supports")
            if not isinstance(supports, dict) or not supports.get("sse_stream"):
                raise AssertionError("/api/config missing supports.sse_stream")

            # Health endpoint should require auth, but accept both token query and cookie.
            health_forbidden = _http_request(host, port, "GET", "/api/health")
            if health_forbidden.status != 403:
                raise AssertionError(f"expected 403 for /api/health without auth, got {health_forbidden.status}")

            health_resp = _http_request(host, port, "GET", f"/api/health?token={token}")
            if health_resp.status != 200:
                raise AssertionError(f"expected 200 for /api/health, got {health_resp.status}")
            health_obj = json.loads(health_resp.body.decode("utf-8"))
            if not isinstance(health_obj, dict) or str(health_obj.get("status", "")).lower() != "ok":
                raise AssertionError("/api/health returned unexpected JSON")

            health_cookie = _http_request(host, port, "GET", "/api/health", headers={"Cookie": cookie_pair})
            if health_cookie.status != 200:
                raise AssertionError(f"expected 200 for /api/health with cookie, got {health_cookie.status}")

            # OpenAPI spec endpoint.
            oa_forbidden = _http_request(host, port, "GET", "/api/openapi.json")
            if oa_forbidden.status != 403:
                raise AssertionError(f"expected 403 for /api/openapi.json without auth, got {oa_forbidden.status}")
            oa = _http_request(host, port, "GET", f"/api/openapi.json?token={token}")
            if oa.status != 200:
                raise AssertionError(f"expected 200 for /api/openapi.json, got {oa.status}")
            spec = json.loads(oa.body.decode("utf-8"))
            if not isinstance(spec, dict) or spec.get("openapi") != "3.0.3":
                raise AssertionError("openapi spec missing openapi=3.0.3")
            paths = spec.get("paths", {})
            if not isinstance(paths, dict):
                raise AssertionError("openapi spec missing paths")
            for p in ("/api/meta", "/api/file", "/api/sse/stream", "/api/health"):
                if p not in paths:
                    raise AssertionError(f"openapi spec missing path {p}")
            comps = spec.get("components", {})
            schemes = comps.get("securitySchemes", {}) if isinstance(comps, dict) else {}
            if not isinstance(schemes, dict) or "tokenCookie" not in schemes:
                raise AssertionError("openapi spec missing tokenCookie security scheme")

            meta_resp = _http_request(host, port, "GET", f"/api/meta?token={token}")
            meta_obj = json.loads(meta_resp.body.decode("utf-8"))
            if not isinstance(meta_obj, dict):
                raise AssertionError("/api/meta returned non-object JSON")

            # State round-trip.
            state0 = _http_request(host, port, "GET", f"/api/state?token={token}")
            state0_obj = json.loads(state0.body.decode("utf-8"))
            if not isinstance(state0_obj, dict):
                raise AssertionError("/api/state returned non-object JSON")

            put_body = json.dumps({"paused": True}).encode("utf-8")
            put = _http_request(
                host,
                port,
                "PUT",
                f"/api/state?token={token}",
                body=put_body,
                headers={"Content-Type": "application/json", "Content-Length": str(len(put_body))},
            )
            put_obj = json.loads(put.body.decode("utf-8"))
            if not isinstance(put_obj, dict) or not bool(put_obj.get("paused")):
                raise AssertionError("PUT /api/state did not set paused=true")

            # Wait until the stream buffers have at least one frame (from the CSVs we wrote).
            ok = _wait_until(lambda: httpd.hub.nf.latest_seq() > 0, timeout_sec=5.0)
            if not ok:
                raise AssertionError("nf stream buffer did not receive any frames")

            ok = _wait_until(lambda: httpd.hub.bandpower.latest_seq() > 0, timeout_sec=5.0)
            if not ok:
                raise AssertionError("bandpower stream buffer did not receive any frames")

            ok = _wait_until(lambda: httpd.hub.artifact.latest_seq() > 0, timeout_sec=5.0)
            if not ok:
                raise AssertionError("artifact stream buffer did not receive any frames")

            
            # Legacy per-topic SSE endpoints (also validate SSE id + resume behavior).
            status, hdrs, meta_id, obj = _sse_read_first_data(host, port, f"/api/sse/meta?token={token}")
            if status != 200:
                raise AssertionError(f"/api/sse/meta returned {status}")
            _assert_sse_headers(hdrs)
            if meta_id is None:
                raise AssertionError("/api/sse/meta did not include an SSE id field")
            if not isinstance(obj, dict) or obj.get("type") != "batch":
                raise AssertionError("/api/sse/meta first data frame malformed")

            status, hdrs, nf_id1, obj = _sse_read_first_data(host, port, f"/api/sse/nf?token={token}")
            if status != 200:
                raise AssertionError(f"/api/sse/nf returned {status}")
            _assert_sse_headers(hdrs)
            if nf_id1 is None:
                raise AssertionError("/api/sse/nf did not include an SSE id field")
            if not isinstance(obj, dict) or obj.get("type") != "batch":
                raise AssertionError("/api/sse/nf first data frame malformed")
            frames = obj.get("frames")
            if not isinstance(frames, list) or not frames:
                raise AssertionError("/api/sse/nf did not include any frames")
            first = frames[0]
            if not isinstance(first, dict) or "metric" not in first or "threshold" not in first:
                raise AssertionError("/api/sse/nf frame missing expected keys")
            try:
                nf_seq1 = int(nf_id1)
            except Exception:
                raise AssertionError(f"/api/sse/nf returned a non-integer SSE id: {nf_id1!r}")
            if nf_seq1 <= 0:
                raise AssertionError(f"/api/sse/nf returned an unexpected SSE id: {nf_seq1}")

            status, hdrs, bp_id, obj = _sse_read_first_data(host, port, f"/api/sse/bandpower?token={token}")
            if status != 200:
                raise AssertionError(f"/api/sse/bandpower returned {status}")
            _assert_sse_headers(hdrs)
            if bp_id is None:
                raise AssertionError("/api/sse/bandpower did not include an SSE id field")
            if not isinstance(obj, dict) or obj.get("type") != "batch":
                raise AssertionError("/api/sse/bandpower first data frame malformed")
            frames = obj.get("frames")
            if not isinstance(frames, list) or not frames:
                raise AssertionError("/api/sse/bandpower did not include any frames")

            status, hdrs, art_id, obj = _sse_read_first_data(host, port, f"/api/sse/artifact?token={token}")
            if status != 200:
                raise AssertionError(f"/api/sse/artifact returned {status}")
            _assert_sse_headers(hdrs)
            if art_id is None:
                raise AssertionError("/api/sse/artifact did not include an SSE id field")
            if not isinstance(obj, dict) or obj.get("type") != "batch":
                raise AssertionError("/api/sse/artifact first data frame malformed")
            frames = obj.get("frames")
            if not isinstance(frames, list) or not frames:
                raise AssertionError("/api/sse/artifact did not include any frames")

            # Per-topic resume test (nf): reconnect with Last-Event-ID should not replay old frames.
            with (outdir / "nf_feedback.csv").open("a", encoding="utf-8") as f:
                f.write("2.0,0.6,0.4,0,0.1\n")
            ok = _wait_until(lambda: httpd.hub.nf.latest_seq() > nf_seq1, timeout_sec=5.0)
            if not ok:
                raise AssertionError("nf stream buffer did not receive appended frames")

            status, hdrs, nf_id2, obj2 = _sse_read_first_data(
                host,
                port,
                f"/api/sse/nf?token={token}",
                request_headers={"Last-Event-ID": str(nf_seq1)},
            )
            if status != 200:
                raise AssertionError(f"resume /api/sse/nf returned {status}")
            if nf_id2 is None:
                raise AssertionError("resume /api/sse/nf did not include an SSE id field")
            if not isinstance(obj2, dict) or obj2.get("type") != "batch":
                raise AssertionError("resume /api/sse/nf first data frame malformed")
            frames2 = obj2.get("frames")
            if not isinstance(frames2, list) or not frames2:
                raise AssertionError("resume /api/sse/nf did not include any frames")
            # The resumed batch should only contain the newly appended row(s).
            min_t = None
            for fr in frames2:
                if isinstance(fr, dict) and fr.get("t") is not None:
                    try:
                        tv = float(fr.get("t"))
                    except Exception:
                        continue
                    min_t = tv if (min_t is None or tv < min_t) else min_t
            if min_t is None or min_t < 2.0:
                raise AssertionError(f"resume /api/sse/nf replayed old frames (min_t={min_t})")

            # Simulate "log rotation"/atomic replace of nf_feedback.csv:
            # old readers that only watch file growth can miss this when the new file is larger.
            seq_before_rot = int(httpd.hub.nf.latest_seq())
            _replace_nf_csv_atomic(
                outdir,
                rows=(
                    "t_end_sec,metric,threshold,reward,reward_rate\n"
                    "10.0,0.9,0.4,1,0.9\n"
                    "11.0,0.8,0.4,0,0.1\n"
                ),
            )

            ok = _wait_until(lambda: httpd.hub.nf.latest_seq() > seq_before_rot, timeout_sec=5.0)
            if not ok:
                raise AssertionError("nf stream buffer did not detect rotated/replaced nf_feedback.csv")

            status, hdrs, nf_id_rot, rot_obj = _sse_read_first_data(
                host,
                port,
                f"/api/sse/nf?token={token}",
                request_headers={"Last-Event-ID": str(seq_before_rot)},
            )
            if status != 200:
                raise AssertionError(f"rotated /api/sse/nf returned {status}")
            if nf_id_rot is None:
                raise AssertionError("rotated /api/sse/nf did not include an SSE id field")
            if not isinstance(rot_obj, dict) or rot_obj.get("type") != "batch":
                raise AssertionError("rotated /api/sse/nf first data frame malformed")
            rot_frames = rot_obj.get("frames")
            if not isinstance(rot_frames, list) or not rot_frames:
                raise AssertionError("rotated /api/sse/nf did not include any frames")
            min_t = None
            for fr in rot_frames:
                if isinstance(fr, dict) and fr.get("t") is not None:
                    try:
                        tv = float(fr.get("t"))
                    except Exception:
                        continue
                    min_t = tv if (min_t is None or tv < min_t) else min_t
            if min_t is None or min_t < 10.0:
                raise AssertionError(f"rotated /api/sse/nf did not emit new file rows (min_t={min_t})")


            # Multiplexed SSE endpoint (recommended by README): expects named events.
            status, hdrs, evs = _sse_read_n_events(host, port, f"/api/sse/stream?token={token}", n=6, timeout=5.0)
            if status != 200:
                raise AssertionError(f"/api/sse/stream returned {status}")
            _assert_sse_headers(hdrs)

            # The first message should always be the config event.
            ev0, id0, obj0 = evs[0]
            if ev0 != "config" or not isinstance(obj0, dict):
                raise AssertionError(f"expected first SSE event to be config, got {ev0!r}")
            if id0 is None:
                raise AssertionError("/api/sse/stream config event missing SSE id field")
            if int(obj0.get("schema_version", 0) or 0) < 1:
                raise AssertionError("/api/sse/stream config event JSON malformed")

            names = [e for (e, _id, _o) in evs]
            for needed in ("meta", "state", "nf"):
                if needed not in names:
                    raise AssertionError(f"/api/sse/stream did not include expected event: {needed}")

            def _get(name: str) -> object:
                for (e, _id, o) in evs:
                    if e == name:
                        return o
                return {}

            for name in ("meta", "state", "nf", "artifact", "bandpower"):
                if name not in names:
                    continue
                o = _get(name)
                if not isinstance(o, dict) or o.get("type") != "batch":
                    raise AssertionError(f"/api/sse/stream event {name} did not use batch framing")

            nf_batch = _get("nf")
            if isinstance(nf_batch, dict):
                frames = nf_batch.get("frames")
                if not isinstance(frames, list) or not frames:
                    raise AssertionError("/api/sse/stream nf batch missing frames")

            # Multiplexed resume test (topics=nf): Last-Event-ID should resume without replay.
            status, hdrs, evs1 = _sse_read_n_events(
                host,
                port,
                f"/api/sse/stream?token={token}&topics=nf",
                n=1,
                timeout=5.0,
            )
            ev1, sid1, obj1 = evs1[0]
            if ev1 != "nf" or not isinstance(obj1, dict):
                raise AssertionError("expected first topics=nf SSE event to be nf")
            if sid1 is None:
                raise AssertionError("topics=nf stream missing SSE id field")
            frames1 = obj1.get("frames")
            if not isinstance(frames1, list) or not frames1:
                raise AssertionError("topics=nf stream did not include any frames")

            # Append another nf row and resume from the previous id.
            prev_seq = int(httpd.hub.nf.latest_seq())
            with (outdir / "nf_feedback.csv").open("a", encoding="utf-8") as f:
                f.write("3.0,0.7,0.4,1,0.3\n")
            ok = _wait_until(lambda: httpd.hub.nf.latest_seq() > prev_seq, timeout_sec=5.0)
            if not ok:
                raise AssertionError("nf stream buffer did not receive appended frames (for /api/sse/stream)")

            status, hdrs, evs2 = _sse_read_n_events(
                host,
                port,
                f"/api/sse/stream?token={token}&topics=nf",
                n=1,
                request_headers={"Last-Event-ID": sid1},
                timeout=5.0,
            )
            ev2, sid2, obj2 = evs2[0]
            if ev2 != "nf" or not isinstance(obj2, dict):
                raise AssertionError("resume topics=nf SSE did not return an nf event")
            if sid2 is None:
                raise AssertionError("resume topics=nf SSE missing SSE id field")
            frames2 = obj2.get("frames")
            if not isinstance(frames2, list) or not frames2:
                raise AssertionError("resume topics=nf SSE did not include any frames")
            min_t = None
            for fr in frames2:
                if isinstance(fr, dict) and fr.get("t") is not None:
                    try:
                        tv = float(fr.get("t"))
                    except Exception:
                        continue
                    min_t = tv if (min_t is None or tv < min_t) else min_t
            if min_t is None or min_t < 3.0:
                raise AssertionError(f"resume topics=nf SSE replayed old frames (min_t={min_t})")
# Stats endpoint should be reachable.
            stats_resp = _http_request(host, port, "GET", f"/api/stats?token={token}")
            stats_obj = json.loads(stats_resp.body.decode("utf-8"))
            if not isinstance(stats_obj, dict) or "connections" not in stats_obj:
                raise AssertionError("/api/stats returned unexpected JSON")


            # Downloads endpoints
            files_resp = _http_request(host, port, "GET", f"/api/files?token={token}")
            files_obj = json.loads(files_resp.body.decode("utf-8"))
            if not isinstance(files_obj, dict) or "files" not in files_obj:
                raise AssertionError("/api/files returned unexpected JSON")
            names = []
            for f in files_obj.get("files", []) if isinstance(files_obj.get("files", []), list) else []:
                if isinstance(f, dict) and f.get("name"):
                    names.append(str(f.get("name")))
            if "nf_feedback.csv" not in names:
                raise AssertionError("/api/files did not include nf_feedback.csv")


            # Reports API (offline HTML/ZIP generation)
            reports_resp = _http_request(host, port, "GET", f"/api/reports?token={token}")
            reports_obj = json.loads(reports_resp.body.decode("utf-8"))
            if not isinstance(reports_obj, dict) or "kinds" not in reports_obj:
                raise AssertionError("/api/reports returned unexpected JSON")
            kind_names = []
            for k in reports_obj.get("kinds", []) if isinstance(reports_obj.get("kinds", []), list) else []:
                if isinstance(k, dict) and k.get("kind"):
                    kind_names.append(str(k.get("kind")))
            if "nf_feedback" not in kind_names:
                raise AssertionError("/api/reports missing nf_feedback kind")

            gen_body = json.dumps({"kinds": ["nf_feedback"]}).encode("utf-8")
            gen_resp = _http_request(
                host,
                port,
                "POST",
                f"/api/reports?token={token}",
                body=gen_body,
                headers={"Content-Type": "application/json"},
                timeout=30.0,
            )
            if gen_resp.status != 200:
                raise AssertionError(f"POST /api/reports returned status {gen_resp.status}")
            gen_obj = json.loads(gen_resp.body.decode("utf-8"))
            if not isinstance(gen_obj, dict) or "results" not in gen_obj:
                raise AssertionError("POST /api/reports returned unexpected JSON")
            ok_any = False
            for r in gen_obj.get("results", []) if isinstance(gen_obj.get("results", []), list) else []:
                if isinstance(r, dict) and r.get("kind") == "nf_feedback" and r.get("ok") is True:
                    ok_any = True
            if not ok_any:
                raise AssertionError("nf_feedback report generation did not succeed")

            rep_resp = _http_request(host, port, "GET", f"/api/file?token={token}&name=nf_feedback_report.html")
            if rep_resp.status != 200:
                raise AssertionError(f"nf_feedback_report.html fetch failed with {rep_resp.status}")
            if b"<html" not in rep_resp.body.lower():
                raise AssertionError("nf_feedback_report.html does not look like HTML")


            file_resp = _http_request(host, port, "GET", f"/api/file?token={token}&name=nf_feedback.csv")
            if file_resp.status != 200:
                raise AssertionError(f"/api/file returned status {file_resp.status}")
            if b"t_end_sec" not in file_resp.body:
                raise AssertionError("/api/file did not return expected nf_feedback.csv content")

            # File response should include caching + range support headers.
            if not file_resp.headers.get("etag"):
                raise AssertionError("/api/file missing ETag header")
            if not file_resp.headers.get("last-modified"):
                raise AssertionError("/api/file missing Last-Modified header")
            ar = (file_resp.headers.get("accept-ranges", "") or "").lower()
            if "bytes" not in ar:
                raise AssertionError(f"/api/file missing Accept-Ranges: bytes (got {ar!r})")

            # HEAD should return headers but no body.
            head_resp = _http_request(host, port, "HEAD", f"/api/file?token={token}&name=nf_feedback.csv")
            if head_resp.status != 200:
                raise AssertionError(f"HEAD /api/file returned status {head_resp.status}")
            if head_resp.body:
                raise AssertionError("HEAD /api/file should not include a body")
            if "content-length" not in head_resp.headers:
                raise AssertionError("HEAD /api/file missing Content-Length header")

            # Range: first 10 bytes should match.
            r10 = _http_request(
                host,
                port,
                "GET",
                f"/api/file?token={token}&name=nf_feedback.csv",
                headers={"Range": "bytes=0-9"},
            )
            if r10.status != 206:
                raise AssertionError(f"Range /api/file expected 206, got {r10.status}")
            if r10.body != file_resp.body[:10]:
                raise AssertionError("Range /api/file returned unexpected bytes")
            cr = (r10.headers.get("content-range") or "")
            if not cr.startswith("bytes 0-9/"):
                raise AssertionError(f"Range /api/file missing/invalid Content-Range (got {cr!r})")

            # If-Range mismatch should fall back to full body (200).
            ir = _http_request(
                host,
                port,
                "GET",
                f"/api/file?token={token}&name=nf_feedback.csv",
                headers={"Range": "bytes=0-4", "If-Range": '"nope"'},
            )
            if ir.status != 200:
                raise AssertionError(f"If-Range mismatch should return 200, got {ir.status}")
            if ir.body != file_resp.body:
                raise AssertionError("If-Range mismatch did not fall back to full body")

            # If-Modified-Since far in the future should return 304.
            ims = formatdate(time.time() + 86400, usegmt=True)
            ims_resp = _http_request(
                host,
                port,
                "GET",
                f"/api/file?token={token}&name=nf_feedback.csv",
                headers={"If-Modified-Since": ims},
            )
            if ims_resp.status != 304:
                raise AssertionError(f"If-Modified-Since expected 304, got {ims_resp.status}")

            # Path traversal should be blocked.
            bad = _http_request(host, port, "GET", f"/api/file?token={token}&name=../oops.txt")
            if bad.status != 404:
                raise AssertionError(f"path traversal name should return 404, got {bad.status}")

            bundle_resp = _http_request(host, port, "GET", f"/api/bundle?token={token}")
            if bundle_resp.status != 200:
                raise AssertionError(f"/api/bundle returned status {bundle_resp.status}")
            ct = (bundle_resp.headers.get("content-type", "") or "").lower()
            if "application/zip" not in ct:
                raise AssertionError(f"/api/bundle did not return application/zip (Content-Type={ct})")
            z = zipfile.ZipFile(io.BytesIO(bundle_resp.body))
            nms = z.namelist()
            if "bundle_manifest.json" not in nms:
                raise AssertionError("/api/bundle zip missing bundle_manifest.json")
            if "nf_feedback.csv" not in nms:
                raise AssertionError("/api/bundle zip missing nf_feedback.csv")

            # Bundle should include small generated dashboard context JSON files.
            for gen_name in ("dashboard_config.json", "dashboard_state.json", "dashboard_meta.json", "dashboard_openapi.json"):
                if gen_name not in nms:
                    raise AssertionError(f"/api/bundle zip missing {gen_name}")
                try:
                    _ = json.loads(z.read(gen_name).decode("utf-8"))
                except Exception as e:
                    raise AssertionError(f"/api/bundle {gen_name} is not valid JSON: {e}")


        finally:
            try:
                httpd.shutdown()
            except Exception:
                pass
            try:
                httpd.server_close()
            except Exception:
                pass
            th.join(timeout=2.0)

    print("rt_dashboard selftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
