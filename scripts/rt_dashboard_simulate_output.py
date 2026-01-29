#!/usr/bin/env python3
"""rt_dashboard_simulate_output.py

Generate synthetic real-time outputs compatible with scripts/rt_qeeg_dashboard.py.

This is a convenience tool for:
- Demoing the real-time dashboard without building/running qeeg_nf_cli
- UI development/testing on machines that don't have EEG hardware

It writes (as appending CSV streams):
- nf_feedback.csv (always)
- bandpower_timeseries.csv (optional)
- artifact_gate_timeseries.csv (optional)

For a smoother dashboard demo, the simulator also writes a couple of small companion files
when they are missing (or when --overwrite is used):
- nf_run_meta.json (minimal session metadata for the dashboard "Session" panel)
- reference_used.csv (if --with-bandpower; enables "z (reference)" map mode in the UI)
- nf_summary.json (written when the simulator exits)

The generated files are *not* scientifically meaningful; they are plausible-looking
signals intended for exercising the visualization pipeline.

Example
-------
Terminal A (simulator):
  python3 scripts/rt_dashboard_simulate_output.py --outdir out_nf --with-bandpower --with-artifact --seconds 0

Terminal B (dashboard):
  python3 scripts/rt_qeeg_dashboard.py --outdir out_nf --open

Notes
-----
- Use --fsync to more closely mimic qeeg_nf_cli --flush-csv behavior (slower).
- Set --overwrite to replace existing outputs instead of appending.

Stdlib only. Tested with Python 3.8+.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Dict, List, Optional, Sequence, Tuple


# Avoid writing .pyc files when running locally.
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
sys.dont_write_bytecode = True


@dataclass
class CsvStream:
    fp: IO[str]
    writer: csv.writer

    def write_row(self, row: Sequence[object], *, fsync: bool = False) -> None:
        self.writer.writerow(list(row))
        try:
            self.fp.flush()
            if fsync:
                os.fsync(self.fp.fileno())
        except Exception:
            # Best effort; simulator should keep running.
            pass

    def close(self) -> None:
        try:
            self.fp.close()
        except Exception:
            pass


def _split_csv_list(s: str) -> List[str]:
    out: List[str] = []
    for part in (s or "").split(","):
        p = (part or "").strip()
        if p:
            out.append(p)
    return out


def _open_stream(path: Path, header: List[str], *, overwrite: bool) -> CsvStream:
    """Open a CSV stream for appending (and write header if needed)."""

    mode = "a"
    need_header = overwrite
    if not path.exists():
        need_header = True
        mode = "w"
    else:
        try:
            if path.stat().st_size == 0:
                need_header = True
                mode = "w"
        except Exception:
            # If stat fails, fall back to overwrite.
            need_header = True
            mode = "w"

    if overwrite:
        mode = "w"

    fp = path.open(mode, encoding="utf-8", newline="")
    w = csv.writer(fp)
    if need_header:
        w.writerow(header)
        try:
            fp.flush()
        except Exception:
            pass
    return CsvStream(fp=fp, writer=w)


def _write_bytes_atomic(path: Path, data: bytes) -> None:
    """Best-effort atomic write."""
    try:
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(data)
        os.replace(str(tmp), str(path))
    except Exception:
        # Fall back to best-effort direct write.
        try:
            path.write_bytes(data)
        except Exception:
            pass


def _maybe_write_json(path: Path, obj: Dict[str, object], *, overwrite: bool) -> None:
    if path.exists() and (not overwrite):
        return
    try:
        payload = (json.dumps(obj, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        _write_bytes_atomic(path, payload)
    except Exception:
        pass


def _reference_stats_for_band(band: str) -> Tuple[float, float]:
    b = (band or "").strip().lower()
    if b == "alpha":
        return 11.0, 1.6
    if b == "beta":
        return 8.0, 1.3
    if b == "theta":
        return 5.0, 0.9
    if b == "delta":
        return 5.5, 1.0
    # generic
    return 4.0, 1.0


def _maybe_write_reference_used(outdir: Path, *, channels: List[str], bands: List[str], seed: int, overwrite: bool) -> None:
    """Write a simple reference_used.csv compatible with /api/reference."""
    path = outdir / "reference_used.csv"
    if path.exists() and (not overwrite):
        return
    try:
        rng = random.Random(int(seed) ^ 0xA5A5A5A5)
        with path.open("w", encoding="utf-8", newline="") as fp:
            fp.write(f"# generator=rt_dashboard_simulate_output.py\n")
            fp.write(f"# seed={int(seed)}\n")
            fp.write(f"# created_utc={float(time.time())}\n")
            w = csv.writer(fp)
            w.writerow(["channel", "band", "mean", "stdev"])
            for bi, b in enumerate(bands):
                mu0, sd0 = _reference_stats_for_band(b)
                for ci, ch in enumerate(channels):
                    # small per-channel/per-band jitter to look realistic
                    mu = float(mu0) + rng.uniform(-0.25, 0.25) + 0.05 * float(ci) + 0.02 * float(bi)
                    sd = max(1e-6, float(sd0) + rng.uniform(-0.08, 0.08))
                    w.writerow([ch, b, f"{mu:.6f}", f"{sd:.6f}"])
    except Exception:
        pass


def _make_run_meta(
    outdir: Path,
    *,
    seed: int,
    update_sec: float,
    baseline_sec: float,
    base_threshold: float,
    noise: float,
    with_bandpower: bool,
    with_artifact: bool,
    channels: List[str],
    bands: List[str],
) -> Dict[str, object]:
    ts_local = time.strftime("%Y-%m-%d %H:%M:%S")
    return {
        "Tool": "rt_dashboard_simulate_output.py",
        "Version": "sim",
        "GitDescribe": "sim",
        "TimestampLocal": ts_local,
        "OutputDir": str(outdir),
        "demo": True,
        "input_path": None,
        "protocol": "synthetic",
        "fs_hz": 250.0,
        "metric_spec": "sim_metric",
        "band_spec": ",".join(bands) if bands else None,
        "reference_csv": None,
        "reference_csv_out": "reference_used.csv" if with_bandpower else None,
        "reward_direction": "up",
        "threshold_init": float(base_threshold),
        "baseline_seconds": float(baseline_sec),
        "baseline_quantile_used": 0.5,
        "target_reward_rate": 0.6,
        "adapt_mode": "none",
        "adapt_eta": 0.0,
        "window_seconds": 2.0,
        "update_seconds": float(update_sec),
        "metric_smooth_seconds": 0.0,
        # Live visualization hints (optional; the browser can render topomaps without these).
        "topomap_latest": False,
        "topomap_band": (bands[0] if bands else None),
        "topomap_mode": "raw",
        "topomap_every": 1,
        "topomap_grid": 64,
        "topomap_annotate": True,
        "topomap_vmin": None,
        "topomap_vmax": None,
        "artifact_gate": bool(with_artifact),
        "qc_bad_channel_count": 0,
        "qc_bad_channels": [],
        "biotrace_ui": False,
        "export_derived_events": False,
        "derived_events_written": False,
        "simulator": {
            "seed": int(seed),
            "update_sec": float(update_sec),
            "noise": float(noise),
            "with_bandpower": bool(with_bandpower),
            "with_artifact": bool(with_artifact),
            "channels": list(channels),
            "bands": list(bands),
        },
    }


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Generate synthetic real-time CSV outputs for rt_qeeg_dashboard.py",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--outdir", required=True, help="Output directory to write CSV streams into")
    ap.add_argument("--update", type=float, default=0.25, help="Seconds between appended rows")
    ap.add_argument(
        "--seconds",
        type=float,
        default=60.0,
        help="Total duration to run (0 = run until Ctrl+C)",
    )
    ap.add_argument("--seed", type=int, default=0, help="Random seed (0 = use time-based seed)")
    ap.add_argument("--overwrite", action="store_true", help="Overwrite existing outputs instead of appending")
    ap.add_argument("--fsync", action="store_true", help="Call os.fsync() after each row (slower)")

    ap.add_argument("--with-bandpower", action="store_true", help="Also write bandpower_timeseries.csv")
    ap.add_argument("--with-artifact", action="store_true", help="Also write artifact_gate_timeseries.csv")

    ap.add_argument(
        "--channels",
        default="Fz,Cz,Pz",
        help="Comma-separated channel list for bandpower_timeseries.csv",
    )
    ap.add_argument(
        "--bands",
        default="delta,theta,alpha,beta",
        help="Comma-separated band list for bandpower_timeseries.csv",
    )

    # Mildly configurable behavior.
    ap.add_argument("--baseline", type=float, default=10.0, help="Seconds until artifact gate becomes ready")
    ap.add_argument("--threshold", type=float, default=0.5, help="Base threshold used for nf_feedback.csv")
    ap.add_argument("--noise", type=float, default=0.05, help="Noise amplitude added to the synthetic metric")

    return ap.parse_args(argv)


def _smoothstep(x: float) -> float:
    # Clamp 0..1
    x = 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)
    return x * x * (3.0 - 2.0 * x)


def _mean_std(xs: List[float]) -> Tuple[Optional[float], Optional[float]]:
    if not xs:
        return None, None
    n = float(len(xs))
    mu = float(sum(xs)) / n
    var = 0.0
    for x in xs:
        dx = float(x) - mu
        var += dx * dx
    var = var / n
    sd = math.sqrt(var) if var > 0.0 else 0.0
    if not (sd > 1e-9):
        return mu, None
    return mu, sd


def main(argv: Optional[List[str]] = None) -> int:
    ns = parse_args(argv)

    outdir = Path(ns.outdir).expanduser()
    outdir.mkdir(parents=True, exist_ok=True)

    update = float(ns.update)
    if not (update > 0.0):
        print("Error: --update must be > 0", file=sys.stderr)
        return 2

    seconds = float(ns.seconds)
    if seconds < 0.0:
        print("Error: --seconds must be >= 0", file=sys.stderr)
        return 2

    seed = int(ns.seed)
    if seed == 0:
        seed = int(time.time() * 1000) & 0xFFFFFFFF
    rng = random.Random(seed)

    channels = _split_csv_list(ns.channels)
    bands = _split_csv_list(ns.bands)

    # Simple synthetic dynamics.
    base_thr = float(ns.threshold)
    noise = float(ns.noise)
    baseline_sec = float(ns.baseline)

    # Ensure a small session metadata file exists so the dashboard can show context.
    try:
        rm = _make_run_meta(
            outdir,
            seed=seed,
            update_sec=update,
            baseline_sec=baseline_sec,
            base_threshold=base_thr,
            noise=noise,
            with_bandpower=bool(ns.with_bandpower),
            with_artifact=bool(ns.with_artifact),
            channels=channels or ["Pz"],
            bands=bands or ["alpha", "beta"],
        )
        _maybe_write_json(outdir / "nf_run_meta.json", rm, overwrite=bool(ns.overwrite))
    except Exception:
        pass

    # Write a simple reference_used.csv so the dashboard "z (reference)" view works in demos.
    if ns.with_bandpower:
        if not channels:
            channels = ["Pz"]
        if not bands:
            bands = ["alpha", "beta"]
        _maybe_write_reference_used(outdir, channels=channels, bands=bands, seed=seed, overwrite=bool(ns.overwrite))

    # Streams
    nf_path = outdir / "nf_feedback.csv"
    nf_header: List[str] = ["t_end_sec", "metric", "threshold", "reward", "reward_rate"]
    if ns.with_artifact:
        # qeeg_nf_cli includes artifact columns in nf_feedback.csv when gating is enabled.
        nf_header += ["artifact_ready", "artifact", "bad_channels"]
    # qeeg_nf_cli always appends z-score columns (they may be empty/NaN if not available).
    nf_header += ["metric_z", "threshold_z", "metric_z_ref", "threshold_z_ref"]
    nf = _open_stream(nf_path, nf_header, overwrite=bool(ns.overwrite))

    bp: Optional[CsvStream] = None
    art: Optional[CsvStream] = None

    if ns.with_bandpower:
        bp_path = outdir / "bandpower_timeseries.csv"
        cols = [f"{b}_{ch}" for b in bands for ch in channels]
        bp_header = ["t_end_sec"] + cols
        bp = _open_stream(bp_path, bp_header, overwrite=bool(ns.overwrite))

    if ns.with_artifact:
        art_path = outdir / "artifact_gate_timeseries.csv"
        art_header = ["t_end_sec", "ready", "bad", "bad_channels"]
        art = _open_stream(art_path, art_header, overwrite=bool(ns.overwrite))

    rr = 0.0  # exponentially smoothed reward rate

    # Baseline z-scores: estimate mean/std from the initial baseline window.
    baseline_metrics: List[float] = []
    base_mu: Optional[float] = None
    base_sd: Optional[float] = None

    # Reference z-scores: use a fixed synthetic distribution (roughly centered on the threshold).
    ref_mu = float(base_thr)
    ref_sd = 0.15

    t0_wall = time.time()
    step = 0

    print(f"Writing synthetic outputs to: {outdir}")
    print(f"Seed: {seed}")
    print("Press Ctrl+C to stop.")

    try:
        while True:
            t_end_sec = step * update
            if seconds > 0.0 and t_end_sec >= seconds:
                break

            # Metric: smooth oscillation + mild trend + noise.
            # Scale to ~[0,1] for convenient thresholding.
            osc = 0.5 + 0.25 * math.sin(2.0 * math.pi * 0.08 * t_end_sec)
            trend = 0.1 * math.sin(2.0 * math.pi * 0.01 * t_end_sec)
            metric = osc + trend + (rng.uniform(-1.0, 1.0) * noise)
            metric = 0.0 if metric < 0.0 else (1.0 if metric > 1.0 else metric)

            # Threshold: slowly varying around base threshold.
            thr = base_thr + 0.05 * math.sin(2.0 * math.pi * 0.02 * t_end_sec)
            thr = 0.0 if thr < 0.0 else (1.0 if thr > 1.0 else thr)

            reward = 1 if metric > thr else 0
            rr = (0.98 * rr) + (0.02 * float(reward))

            # Artifact dynamics (optional).
            a_ready: Optional[int] = None
            a_bad: Optional[int] = None
            a_bad_channels: Optional[int] = None
            if art is not None:
                a_ready = 1 if t_end_sec >= baseline_sec else 0
                # Artifact bursts become rarer once ready.
                p_bad = 0.10 if a_ready == 0 else 0.03
                a_bad = 1 if rng.random() < p_bad else 0
                a_bad_channels = int(rng.randint(0, 2) if a_bad else 0)
                art.write_row([f"{t_end_sec:.3f}", int(a_ready), int(a_bad), int(a_bad_channels)], fsync=bool(ns.fsync))

            # Baseline stats accumulation.
            if t_end_sec < baseline_sec:
                baseline_metrics.append(float(metric))
            elif base_mu is None and base_sd is None:
                mu, sd = _mean_std(baseline_metrics)
                base_mu, base_sd = mu, sd

            # z-scores (strings for CSV).
            mz: str = ""
            tz: str = ""
            if base_mu is not None and base_sd is not None and base_sd > 0:
                mz = f"{(float(metric) - base_mu) / base_sd:.6f}"
                tz = f"{(float(thr) - base_mu) / base_sd:.6f}"

            mzr: str = ""
            tzr: str = ""
            if ref_sd > 1e-9:
                mzr = f"{(float(metric) - ref_mu) / ref_sd:.6f}"
                tzr = f"{(float(thr) - ref_mu) / ref_sd:.6f}"

            nf_row: List[object] = [f"{t_end_sec:.3f}", f"{metric:.6f}", f"{thr:.6f}", int(reward), f"{rr:.6f}"]
            if ns.with_artifact:
                nf_row += [int(a_ready or 0), int(a_bad or 0), int(a_bad_channels or 0)]
            nf_row += [mz, tz, mzr, tzr]
            nf.write_row(nf_row, fsync=bool(ns.fsync))

            if bp is not None:
                # Generate plausible bandpowers: positive, band-dependent scaling.
                # Tie alpha to the metric a bit, and keep others semi-independent.
                vals: List[str] = []
                for b in bands:
                    b_low = b.lower()
                    for ch_i, _ch in enumerate(channels):
                        # channel-specific wobble
                        wob = 1.0 + 0.05 * math.sin(2.0 * math.pi * 0.03 * t_end_sec + (ch_i * 0.7))
                        if b_low == "alpha":
                            v = 8.0 + 6.0 * metric
                        elif b_low == "beta":
                            v = 6.0 + 4.0 * (1.0 - metric)
                        elif b_low == "theta":
                            v = 5.0 + 2.0 * math.sin(2.0 * math.pi * 0.015 * t_end_sec)
                        elif b_low == "delta":
                            # fade in over baseline period
                            frac = _smoothstep(t_end_sec / max(1e-6, baseline_sec))
                            v = 4.0 + 3.0 * frac
                        else:
                            v = 3.0 + 1.0 * math.sin(2.0 * math.pi * 0.02 * t_end_sec)

                        v = max(0.0, float(v) * wob + rng.uniform(-0.2, 0.2))
                        vals.append(f"{v:.6f}")

                bp.write_row([f"{t_end_sec:.3f}"] + vals, fsync=bool(ns.fsync))

            # Sleep to maintain a stable wall-clock pacing.
            step += 1
            next_wall = t0_wall + (step * update)
            now = time.time()
            if next_wall > now:
                time.sleep(next_wall - now)
            else:
                # If we're behind (slow disk / fsync), yield briefly.
                time.sleep(0.001)

    except KeyboardInterrupt:
        pass
    finally:
        try:
            nf.close()
        except Exception:
            pass
        if bp is not None:
            bp.close()
        if art is not None:
            art.close()

        # Write a tiny summary at the end of the run (best effort).
        summary = {
            "Tool": "rt_dashboard_simulate_output.py",
            "Version": "sim",
            "TimestampLocal": time.strftime("%Y-%m-%d %H:%M:%S"),
            "OutputDir": str(outdir),
            "seed": int(seed),
            "seconds_requested": float(seconds),
            "seconds_written": float(step * update),
            "update_sec": float(update),
            "frames_written": int(step),
            "reward_rate_ewma": float(rr),
            "with_bandpower": bool(ns.with_bandpower),
            "with_artifact": bool(ns.with_artifact),
        }
        _maybe_write_json(outdir / "nf_summary.json", summary, overwrite=bool(ns.overwrite))

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
