#!/usr/bin/env python3
"""rt_dashboard_simulate_output.py

Generate synthetic real-time CSV outputs compatible with scripts/rt_qeeg_dashboard.py.

This is a convenience tool for:
- Demoing the real-time dashboard without building/running qeeg_nf_cli
- UI development/testing on machines that don't have EEG hardware

It writes (as appending CSV streams):
- nf_feedback.csv (always)
- bandpower_timeseries.csv (optional)
- artifact_gate_timeseries.csv (optional)

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
- Set --overwrite to replace existing CSVs instead of appending.

Stdlib only. Tested with Python 3.8+.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO, List, Optional, Sequence, Tuple


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
    ap.add_argument("--overwrite", action="store_true", help="Overwrite existing CSVs instead of appending")
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

    # Streams
    nf_path = outdir / "nf_feedback.csv"
    nf_header = ["t_end_sec", "metric", "threshold", "reward", "reward_rate"]
    nf = _open_stream(nf_path, nf_header, overwrite=bool(ns.overwrite))

    bp: Optional[CsvStream] = None
    art: Optional[CsvStream] = None

    channels = _split_csv_list(ns.channels)
    bands = _split_csv_list(ns.bands)
    if ns.with_bandpower:
        if not channels:
            channels = ["Pz"]
        if not bands:
            bands = ["alpha", "beta"]
        bp_path = outdir / "bandpower_timeseries.csv"
        cols = [f"{b}_{ch}" for b in bands for ch in channels]
        bp_header = ["t_end_sec"] + cols
        bp = _open_stream(bp_path, bp_header, overwrite=bool(ns.overwrite))

    if ns.with_artifact:
        art_path = outdir / "artifact_gate_timeseries.csv"
        art_header = ["t_end_sec", "ready", "bad", "bad_channels"]
        art = _open_stream(art_path, art_header, overwrite=bool(ns.overwrite))

    # Simple synthetic dynamics.
    base_thr = float(ns.threshold)
    noise = float(ns.noise)
    baseline_sec = float(ns.baseline)

    rr = 0.0  # exponentially smoothed reward rate

    t0_wall = time.time()
    next_wall = t0_wall
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

            nf.write_row([f"{t_end_sec:.3f}", f"{metric:.6f}", f"{thr:.6f}", int(reward), f"{rr:.6f}"], fsync=bool(ns.fsync))

            if art is not None:
                ready = 1 if t_end_sec >= baseline_sec else 0
                # Artifact bursts become rarer once ready.
                p_bad = 0.10 if ready == 0 else 0.03
                bad = 1 if rng.random() < p_bad else 0
                bad_channels = int(rng.randint(0, 2) if bad else 0)
                art.write_row([f"{t_end_sec:.3f}", int(ready), int(bad), int(bad_channels)], fsync=bool(ns.fsync))

            if bp is not None:
                # Generate plausible bandpowers: positive, band-dependent scaling.
                # Tie alpha to the metric a bit, and keep others semi-independent.
                vals: List[str] = []
                for b in bands:
                    b_low = b.lower()
                    for ch_i, ch in enumerate(channels):
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

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
