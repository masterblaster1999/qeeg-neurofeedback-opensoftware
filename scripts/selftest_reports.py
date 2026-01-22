#!/usr/bin/env python3
"""Self-test harness for the HTML report renderers (stdlib only).

This script generates small synthetic "out_*" directories resembling CLI outputs and
runs the renderers to ensure they produce HTML without errors.

It is useful for:
  - quick sanity checks after refactors
  - CI smoke tests (no dependencies beyond Python 3)

Usage:
  python3 scripts/selftest_reports.py
  python3 scripts/selftest_reports.py --keep --outdir /tmp/qeeg_report_selftest

Notes:
- This is a smoke test; it does not validate scientific correctness.
- Generated data are synthetic and not representative of real EEG.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Sequence


def _write_csv(path: Path, headers: Sequence[str], rows: Iterable[Dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(headers))
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in headers})


def _write_json(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=True)


def _make_bandpowers(outdir: Path) -> None:
    headers = ["channel", "delta", "theta", "alpha", "beta", "gamma"]
    rows = [
        {"channel": "Fz", "delta": 1.1, "theta": 2.2, "alpha": 3.3, "beta": 4.4, "gamma": 5.5},
        {"channel": "Cz", "delta": 1.2, "theta": 2.1, "alpha": 3.0, "beta": 4.0, "gamma": 5.0},
        {"channel": "Pz", "delta": 0.9, "theta": 2.5, "alpha": 3.8, "beta": 4.1, "gamma": 5.2},
    ]
    _write_csv(outdir / "bandpowers.csv", headers, rows)
    _write_json(
        outdir / "bandpowers.json",
        {
            "channel": {
                "LongName": "Channel label",
                "Description": "EEG channel label (one row per channel).",
            },
            "delta": {
                "LongName": "delta band power",
                "Description": "Bandpower integrated from 1 to 4 Hz.",
                "Units": "a.u.",
            },
            "theta": {
                "LongName": "theta band power",
                "Description": "Bandpower integrated from 4 to 8 Hz.",
                "Units": "a.u.",
            },
            "alpha": {
                "LongName": "alpha band power",
                "Description": "Bandpower integrated from 8 to 12 Hz.",
                "Units": "a.u.",
            },
            "beta": {
                "LongName": "beta band power",
                "Description": "Bandpower integrated from 12 to 30 Hz.",
                "Units": "a.u.",
            },
            "gamma": {
                "LongName": "gamma band power",
                "Description": "Bandpower integrated from 30 to 45 Hz.",
                "Units": "a.u.",
            },
        },
    )

    _write_json(
        outdir / "bandpower_run_meta.json",
        {
            "Tool": "qeeg_bandpower_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "bandpowers.csv",
                "bandpowers.json",
                "bandpower_run_meta.json",
            ],
        },
    )



def _make_bandratios(outdir: Path) -> None:
    headers = ["channel", "theta_beta", "alpha_theta"]
    rows = [
        {"channel": "Fz", "theta_beta": 0.52, "alpha_theta": 1.50},
        {"channel": "Cz", "theta_beta": 0.71, "alpha_theta": 1.12},
        # Include a blank / missing value to ensure renderers handle NaNs.
        {"channel": "Pz", "theta_beta": "", "alpha_theta": 0.92},
    ]
    _write_csv(outdir / "bandratios.csv", headers, rows)

    _write_json(
        outdir / "bandratios.json",
        {
            "channel": {
                "LongName": "Channel label",
                "Description": "EEG channel label (one row per channel).",
            },
            "theta_beta": {
                "LongName": "theta/beta band ratio",
                "Description": "Ratio computed from bandpowers.csv columns: (theta) / (beta).",
                "Units": "n/a",
            },
            "alpha_theta": {
                "LongName": "alpha/theta band ratio",
                "Description": "Ratio computed from bandpowers.csv columns: (alpha) / (theta).",
                "Units": "n/a",
            },
        },
    )

    _write_json(
        outdir / "bandratios_run_meta.json",
        {
            "Tool": "qeeg_bandratios_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "bandratios.csv",
                "bandratios.json",
                "bandratios_run_meta.json",
            ],
        },
    )


def _write_dummy_bmp(path: Path, *, w: int = 1, h: int = 1, bgr: tuple[int, int, int] = (30, 144, 255)) -> None:
    """Write a tiny valid 24-bit BMP.

    This is used to exercise report image embedding in self-tests without
    pulling in non-stdlib dependencies.
    """

    # Each row is padded to a multiple of 4 bytes.
    row_size = (w * 3 + 3) & ~3
    pixel_array_size = row_size * h
    file_size = 14 + 40 + pixel_array_size

    # BMP file header (14 bytes)
    bfType = b"BM"
    bfSize = file_size.to_bytes(4, "little")
    bfReserved1 = (0).to_bytes(2, "little")
    bfReserved2 = (0).to_bytes(2, "little")
    bfOffBits = (14 + 40).to_bytes(4, "little")

    # DIB header: BITMAPINFOHEADER (40 bytes)
    biSize = (40).to_bytes(4, "little")
    biWidth = int(w).to_bytes(4, "little", signed=True)
    biHeight = int(h).to_bytes(4, "little", signed=True)
    biPlanes = (1).to_bytes(2, "little")
    biBitCount = (24).to_bytes(2, "little")
    biCompression = (0).to_bytes(4, "little")
    biSizeImage = int(pixel_array_size).to_bytes(4, "little")
    biXPelsPerMeter = (0).to_bytes(4, "little", signed=True)
    biYPelsPerMeter = (0).to_bytes(4, "little", signed=True)
    biClrUsed = (0).to_bytes(4, "little")
    biClrImportant = (0).to_bytes(4, "little")

    header = (
        bfType
        + bfSize
        + bfReserved1
        + bfReserved2
        + bfOffBits
        + biSize
        + biWidth
        + biHeight
        + biPlanes
        + biBitCount
        + biCompression
        + biSizeImage
        + biXPelsPerMeter
        + biYPelsPerMeter
        + biClrUsed
        + biClrImportant
    )
    assert len(header) == 54

    # Pixel data: bottom-up rows. Each pixel is B,G,R.
    pad = b"\x00" * (row_size - w * 3)
    px = bytes([bgr[0] & 0xFF, bgr[1] & 0xFF, bgr[2] & 0xFF])
    rows = (px * w + pad) * h

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + rows)


def _make_iaf(outdir: Path) -> None:
    headers = ["channel", "iaf_hz", "found", "peak_value_db", "prominence_db", "cog_hz"]
    rows = [
        {"channel": "O1", "iaf_hz": 10.2, "found": 1, "peak_value_db": 8.1, "prominence_db": 1.2, "cog_hz": 10.0},
        {"channel": "O2", "iaf_hz": 10.6, "found": 1, "peak_value_db": 7.7, "prominence_db": 0.9, "cog_hz": 10.3},
        {"channel": "Pz", "iaf_hz": 9.8, "found": 1, "peak_value_db": 6.9, "prominence_db": 0.7, "cog_hz": 9.7},
        # Include a channel with no robust peak.
        {"channel": "Fz", "iaf_hz": "", "found": 0, "peak_value_db": "", "prominence_db": "", "cog_hz": 10.1},
    ]
    _write_csv(outdir / "iaf_by_channel.csv", headers, rows)

    (outdir / "iaf_summary.txt").write_text(
        "\n".join(
            [
                "input=synthetic",
                "fs_hz=250",
                "n_channels=4",
                "n_samples=10000",
                "",
                "welch_nperseg=1024",
                "welch_overlap=0.5",
                "",
                "alpha_min_hz=7",
                "alpha_max_hz=13",
                "detrend_1_f=1",
                "smooth_hz=1.0",
                "min_prom_db=0.5",
                "require_local_max=1",
                "",
                "aggregate_mode=median",
                "metric=peak",
                "aggregate_paf_hz=10.2",
                "aggregate_cog_hz=10.05",
                "aggregate_iaf_hz=10.2",
                "",
            ]
        ),
        encoding="utf-8",
    )

    (outdir / "iaf_band_spec.txt").write_text(
        "delta:[1.0,4.0] theta:[4.0,8.0] alpha:[8.0,12.0] beta:[12.0,30.0] gamma:[30.0,45.0]\n",
        encoding="utf-8",
    )

    # Exercise image embedding in the report.
    _write_dummy_bmp(outdir / "topomap_iaf.bmp")

    _write_json(
        outdir / "iaf_run_meta.json",
        {
            "Tool": "qeeg_iaf_cli",
            "TimestampLocal": "selftest",
            "Input": {"Path": "synthetic", "FsCsvHz": 250},
            "OutputDir": str(outdir),
            "SamplingFrequencyHz": 250,
            "ChannelCount": 4,
            "MontageSpec": "builtin:standard_1020_19",
            "AggregateMode": "median",
            "Metric": "peak",
            "AggregateIAFHz": 10.2,
            "AggregatePAFHz": 10.2,
            "AggregateCoGHz": 10.05,
            "Outputs": [
                "iaf_by_channel.csv",
                "iaf_summary.txt",
                "iaf_band_spec.txt",
                "topomap_iaf.bmp",
                "iaf_run_meta.json",
            ],
        },
    )



def _make_spectral_features(outdir: Path) -> None:
    # Mirrors qeeg_spectral_features_cli output names and a subset of its sidecar metadata.
    edge_col = "sef95_hz"
    headers = ["channel", "total_power", "entropy", "mean_hz", "peak_hz", "median_hz", edge_col]
    rows = [
        {
            "channel": "Fz",
            "total_power": 12.3,
            "entropy": 0.62,
            "mean_hz": 10.4,
            "peak_hz": 10.0,
            "median_hz": 9.2,
            edge_col: 18.0,
        },
        {
            "channel": "Cz",
            "total_power": 10.1,
            "entropy": 0.58,
            "mean_hz": 11.1,
            "peak_hz": 11.5,
            "median_hz": 10.2,
            edge_col: 20.5,
        },
        {
            "channel": "Pz",
            "total_power": 14.7,
            "entropy": 0.66,
            "mean_hz": 9.7,
            "peak_hz": 9.5,
            "median_hz": 8.9,
            edge_col: 17.2,
        },
    ]
    _write_csv(outdir / "spectral_features.csv", headers, rows)

    rng = "[1.0000,45.0000] Hz"
    _write_json(
        outdir / "spectral_features.json",
        {
            "channel": {
                "LongName": "Channel label",
                "Description": "EEG channel label (one row per channel).",
            },
            "total_power": {
                "LongName": "Total power",
                "Description": f"Total power (integral of PSD) within {rng}.",
                "Units": "a.u.",
            },
            "entropy": {
                "LongName": "Spectral entropy (normalized)",
                "Description": f"Normalized spectral entropy within {rng}. Values are in [0,1].",
                "Units": "n/a",
            },
            "mean_hz": {
                "LongName": "Mean frequency (spectral centroid)",
                "Description": f"Power-weighted mean frequency within {rng}.",
                "Units": "Hz",
            },
            "peak_hz": {
                "LongName": "Peak frequency",
                "Description": f"Frequency of maximum PSD within {rng} (simple argmax).",
                "Units": "Hz",
            },
            "median_hz": {
                "LongName": "Median frequency (SEF50)",
                "Description": f"Spectral edge frequency at 50% cumulative power within {rng}.",
                "Units": "Hz",
            },
            edge_col: {
                "LongName": "Spectral edge frequency (SEF95)",
                "Description": f"Spectral edge frequency at 95% cumulative power within {rng}.",
                "Units": "Hz",
            },
        },
    )

    _write_json(
        outdir / "spectral_features_run_meta.json",
        {
            "Tool": "qeeg_spectral_features_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "spectral_features.csv",
                "spectral_features.json",
                "spectral_features_run_meta.json",
            ],
        },
    )
def _make_connectivity(outdir: Path) -> None:
    # pairs
    pairs_headers = ["channel_a", "channel_b", "coherence"]
    pairs_rows = [
        {"channel_a": "F3", "channel_b": "F4", "coherence": 0.42},
        {"channel_a": "F3", "channel_b": "Pz", "coherence": 0.31},
        {"channel_a": "F4", "channel_b": "Pz", "coherence": 0.28},
    ]
    _write_csv(outdir / "coherence_pairs.csv", pairs_headers, pairs_rows)

    # matrix (alpha band)
    chans = ["F3", "F4", "Pz"]
    mat_headers = [""] + chans
    mat_rows: List[Dict[str, object]] = []
    vals = {
        ("F3", "F3"): 1.0,
        ("F3", "F4"): 0.42,
        ("F3", "Pz"): 0.31,
        ("F4", "F3"): 0.42,
        ("F4", "F4"): 1.0,
        ("F4", "Pz"): 0.28,
        ("Pz", "F3"): 0.31,
        ("Pz", "F4"): 0.28,
        ("Pz", "Pz"): 1.0,
    }
    for rch in chans:
        row: Dict[str, object] = {"": rch}
        for cch in chans:
            row[cch] = vals.get((rch, cch), 0.0)
        mat_rows.append(row)
    _write_csv(outdir / "coherence_matrix_alpha.csv", mat_headers, mat_rows)


def _make_channel_qc(outdir: Path) -> None:
    headers = [
        "channel",
        "bad",
        "flatline",
        "noisy",
        "artifact_often_bad",
        "corr_low",
        "robust_scale",
        "artifact_bad_window_fraction",
        "abs_corr_with_mean",
    ]
    rows = [
        {
            "channel": "Fz",
            "bad": 0,
            "flatline": 0,
            "noisy": 0,
            "artifact_often_bad": 0,
            "corr_low": 0,
            "robust_scale": 1.0,
            "artifact_bad_window_fraction": 0.02,
            "abs_corr_with_mean": 0.88,
        },
        {
            "channel": "Cz",
            "bad": 1,
            "flatline": 0,
            "noisy": 1,
            "artifact_often_bad": 0,
            "corr_low": 1,
            "robust_scale": 2.4,
            "artifact_bad_window_fraction": 0.17,
            "abs_corr_with_mean": 0.42,
        },
        {
            "channel": "Pz",
            "bad": 0,
            "flatline": 0,
            "noisy": 0,
            "artifact_often_bad": 0,
            "corr_low": 0,
            "robust_scale": 1.2,
            "artifact_bad_window_fraction": 0.04,
            "abs_corr_with_mean": 0.80,
        },
    ]
    _write_csv(outdir / "channel_qc.csv", headers, rows)
    (outdir / "qc_summary.txt").write_text("Synthetic QC summary.\n", encoding="utf-8")


def _make_artifacts(outdir: Path) -> None:
    headers = ["t_start_sec", "t_end_sec", "bad"]
    rows: List[Dict[str, object]] = []
    t = 0.0
    step = 0.5
    for i in range(120):
        bad = 1 if (20 <= t <= 25 or 48 <= t <= 49.5) else 0
        rows.append({"t_start_sec": t, "t_end_sec": t + step, "bad": bad})
        t += step
    _write_csv(outdir / "artifact_windows.csv", headers, rows)

    ch_headers = ["channel", "bad_window_fraction"]
    ch_rows = [
        {"channel": "Fz", "bad_window_fraction": 0.03},
        {"channel": "Cz", "bad_window_fraction": 0.12},
        {"channel": "Pz", "bad_window_fraction": 0.05},
    ]
    _write_csv(outdir / "artifact_channel_summary.csv", ch_headers, ch_rows)

    (outdir / "artifact_summary.txt").write_text("Synthetic artifact summary.\n", encoding="utf-8")


def _make_quality(outdir: Path) -> None:
    """Synthetic outputs for qeeg_quality_cli (line-noise report)."""

    # Per-channel CSV (mirrors qeeg_quality_cli)
    headers = [
        "channel",
        "ratio_50",
        "peak_mean_50",
        "baseline_mean_50",
        "ratio_60",
        "peak_mean_60",
        "baseline_mean_60",
    ]
    rows = [
        {"channel": "Fz", "ratio_50": 1.25, "peak_mean_50": 4.2, "baseline_mean_50": 3.36, "ratio_60": 1.05, "peak_mean_60": 3.1, "baseline_mean_60": 2.95},
        {"channel": "Cz", "ratio_50": 1.40, "peak_mean_50": 4.6, "baseline_mean_50": 3.28, "ratio_60": 1.10, "peak_mean_60": 3.3, "baseline_mean_60": 3.0},
        {"channel": "Pz", "ratio_50": 1.10, "peak_mean_50": 3.9, "baseline_mean_50": 3.55, "ratio_60": 1.55, "peak_mean_60": 5.0, "baseline_mean_60": 3.23},
        # Include a blank to exercise NaN handling.
        {"channel": "Oz", "ratio_50": "", "peak_mean_50": "", "baseline_mean_50": "", "ratio_60": 1.20, "peak_mean_60": 3.9, "baseline_mean_60": 3.25},
    ]
    _write_csv(outdir / "line_noise_per_channel.csv", headers, rows)

    # JSON report (mirrors qeeg_quality_cli keys used by the renderer)
    _write_json(
        outdir / "quality_report.json",
        {
            "fs_hz": 250.0,
            "n_channels": 4,
            "n_samples": 250 * 20,
            "duration_sec": 20.0,
            "params": {"max_channels": 32, "nperseg": 1024, "overlap": 0.5, "min_ratio": 1.2},
            "line_noise": {
                "median_ratio_50": 1.25,
                "median_ratio_60": 1.20,
                "recommended_notch_hz": 50.0,
                "strength_ratio": 1.25,
                "channels_used": 4,
                "median_peak_mean_50": 4.2,
                "median_baseline_mean_50": 3.3,
                "median_peak_mean_60": 3.5,
                "median_baseline_mean_60": 3.0,
            },
            "per_channel": [
                {
                    "channel": "Fz",
                    "cand50": {"ratio": 1.25, "peak_mean": 4.2, "baseline_mean": 3.36},
                    "cand60": {"ratio": 1.05, "peak_mean": 3.1, "baseline_mean": 2.95},
                },
                {
                    "channel": "Cz",
                    "cand50": {"ratio": 1.40, "peak_mean": 4.6, "baseline_mean": 3.28},
                    "cand60": {"ratio": 1.10, "peak_mean": 3.3, "baseline_mean": 3.0},
                },
                {
                    "channel": "Pz",
                    "cand50": {"ratio": 1.10, "peak_mean": 3.9, "baseline_mean": 3.55},
                    "cand60": {"ratio": 1.55, "peak_mean": 5.0, "baseline_mean": 3.23},
                },
                {
                    "channel": "Oz",
                    "cand50": {"ratio": 0.0, "peak_mean": 0.0, "baseline_mean": 0.0},
                    "cand60": {"ratio": 1.20, "peak_mean": 3.9, "baseline_mean": 3.25},
                },
            ],
        },
    )

    (outdir / "quality_summary.txt").write_text(
        "\n".join(
            [
                "qeeg_quality_cli",
                "",
                "Input: synthetic",
                "Sampling rate (Hz): 250",
                "Channels: 4",
                "Samples: 5000",
                "Duration (sec): 20.000",
                "",
                "Line noise (median peak/baseline ratio across up to 4 channels):",
                "  50 Hz ratio: 1.250",
                "  60 Hz ratio: 1.200",
                "  Recommended notch: 50 Hz (ratio=1.25)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    _write_json(
        outdir / "quality_run_meta.json",
        {
            "Tool": "qeeg_quality_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "quality_report.json",
                "quality_summary.txt",
                "line_noise_per_channel.csv",
                "quality_run_meta.json",
            ],
        },
    )


def _make_bids_scan(outdir: Path) -> None:
    """Synthetic outputs for qeeg_bids_scan_cli."""

    outdir.mkdir(parents=True, exist_ok=True)

    # CSV header matches qeeg_bids_scan_cli output.
    (outdir / "bids_index.csv").write_text(
        "\n".join(
            [
                "path,format,sub,ses,task,acq,run,eeg_json,channels_tsv,events_tsv,events_json,electrodes_tsv,coordsystem_json,issues",
                '"sub-01/ses-01/eeg/sub-01_ses-01_task-rest_eeg.edf",EDF,01,01,rest,,1,1,1,1,0,0,0,"[WARN] Missing electrodes.tsv | [WARN] Missing coordsystem.json"',
                '"sub-01/ses-01/eeg/sub-01_ses-01_task-rest_run-02_eeg.EDF",EDF,01,01,rest,,02,1,1,1,0,0,0,0,"[WARN] Uppercase extension used: .EDF"',
                '"sub-02/eeg/sub-02_task-oddball_eeg.vhdr",BrainVision,02,,oddball,,1,1,1,0,0,0,0,"[ERROR] BrainVision triplet missing: .eeg/.vmrk"',
                '"sub-03/eeg/broken_filename.edf",EDF,,,,,1,0,0,0,0,0,0,"[ERROR] Could not parse required BIDS entities (sub/task) from filename"',
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    _write_json(
        outdir / "bids_index.json",
        {
            "DatasetRoot": "/path/to/synthetic_bids",
            "GeneratedAtUTC": "selftest",
            "Warnings": [
                "dataset_description.json is missing required key: Name",
                "dataset_description.json is missing required key: BIDSVersion",
            ],
            "Errors": ["Found files with invalid BIDS filenames"],
            "Recordings": [
                {
                    "Path": "sub-01/ses-01/eeg/sub-01_ses-01_task-rest_eeg.edf",
                    "Format": "EDF",
                    "Entities": {"sub": "01", "ses": "01", "task": "rest", "acq": "", "run": "1"},
                    "Sidecars": {
                        "eeg_json": True,
                        "channels_tsv": True,
                        "events_tsv": True,
                        "events_json": False,
                        "electrodes_tsv": False,
                        "coordsystem_json": False,
                    },
                    "BrainVisionTripletOK": True,
                    "Issues": [
                        "[WARN] Missing electrodes.tsv",
                        "[WARN] Missing coordsystem.json",
                    ],
                },
            ],
        },
    )

    (outdir / "bids_scan_report.txt").write_text(
        "\n".join(
            [
                "qeeg_bids_scan_cli report",
                "Generated (UTC): selftest",
                "Dataset root: /path/to/synthetic_bids",
                "",
                "Found recordings: 4",
                "Warnings: 3",
                "Errors: 2",
                "",
                "Errors:",
                "  - Found files with invalid BIDS filenames",
                "",
                "Warnings:",
                "  - dataset_description.json is missing required key: Name",
                "  - dataset_description.json is missing required key: BIDSVersion",
                "",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    _write_json(
        outdir / "bids_scan_run_meta.json",
        {
            "Tool": "qeeg_bids_scan_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "bids_index.json",
                "bids_index.csv",
                "bids_scan_report.txt",
                "bids_scan_run_meta.json",
            ],
        },
    )



def _make_trace_plot(outdir: Path) -> None:
    """Synthetic outputs for qeeg_trace_plot_cli (SVG trace plot + meta)."""

    outdir.mkdir(parents=True, exist_ok=True)

    # A tiny but valid SVG.
    svg = """<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"""
    svg += """<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"180\" viewBox=\"0 0 640 180\">\n"""
    svg += """  <rect x=\"0\" y=\"0\" width=\"640\" height=\"180\" fill=\"#ffffff\"/>\n"""
    svg += """  <text x=\"10\" y=\"18\" font-family=\"sans-serif\" font-size=\"14\" fill=\"#111\">Trace plot (synthetic)</text>\n"""
    svg += """  <polyline fill=\"none\" stroke=\"#1f77b4\" stroke-width=\"2\" points=\"10,80 80,60 140,90 200,70 260,100 320,65 380,95 440,75 500,105 630,85\"/>\n"""
    svg += """  <polyline fill=\"none\" stroke=\"#d62728\" stroke-width=\"2\" points=\"10,140 80,120 140,150 200,130 260,160 320,125 380,155 440,135 500,165 630,145\"/>\n"""
    svg += """</svg>\n"""
    (outdir / "traces.svg").write_text(svg, encoding="utf-8")

    (outdir / "trace_plot_meta.txt").write_text(
        "\n".join(
            [
                "input=synthetic",
                "fs_hz=250",
                "start_sec=0",
                "end_sec=10",
                "duration_sec=10",
                "channels=(first N)",
                "autoscale=0",
                "uv_per_row=100",
                "max_points=2000",
                "events_drawn=0",
                "average_reference=0",
                "notch_hz=0",
                "notch_q=30",
                "bandpass_low_hz=0",
                "bandpass_high_hz=0",
                "zero_phase=0",
                "",
            ]
        ),
        encoding="utf-8",
    )

    _write_json(
        outdir / "trace_plot_run_meta.json",
        {
            "Tool": "qeeg_trace_plot_cli",
            "TimestampLocal": "selftest",
            "Input": {"Path": "synthetic"},
            "OutputDir": str(outdir),
            "Outputs": [
                "traces.svg",
                "trace_plot_meta.txt",
                "trace_plot_run_meta.json",
            ],
        },
    )



def _make_nf(outdir: Path) -> None:
    headers = ["t_end_sec", "metric", "threshold", "reward", "reward_rate", "artifact_ready", "artifact", "bad_channels", "phase", "raw_reward", "band", "channel"]
    rows: List[Dict[str, object]] = []
    dt = 0.25
    thr = 0.15
    rr = 0.0
    alpha = 0.02  # smoothing for reward_rate
    for i in range(600):
        t = (i + 1) * dt
        metric = 0.2 * math.sin(2 * math.pi * 0.07 * t) + 0.05 * random.uniform(-1.0, 1.0)
        raw_reward = 1 if metric > thr else 0
        # Simple dwell/refractory not modeled; use raw_reward as reward for the smoke test.
        reward = raw_reward
        rr = (1.0 - alpha) * rr + alpha * float(reward)

        # Artifact gate "ready" after 5s, occasional artifacts.
        artifact_ready = 1 if t >= 5.0 else 0
        artifact = 1 if (artifact_ready and (int(t) % 17 == 0) and (0.0 < (t % 1.0) < 0.5)) else 0
        bad_channels = 0

        if t < 10.0:
            phase = "baseline"
        else:
            # alternate 5s train/rest blocks
            block = int((t - 10.0) // 5.0)
            phase = "train" if (block % 2 == 0) else "rest"

        rows.append(
            {
                "t_end_sec": t,
                "metric": metric,
                "threshold": thr,
                "reward": reward,
                "reward_rate": rr,
                "artifact_ready": artifact_ready,
                "artifact": artifact,
                "bad_channels": bad_channels,
                "phase": phase,
                "raw_reward": raw_reward,
                "band": "alpha",
                "channel": "Pz",
            }
        )

    _write_csv(outdir / "nf_feedback.csv", headers, rows)
    _write_json(
        outdir / "nf_summary.json",
        {
            "protocol": "alpha_up_pz",
            "metric_spec": {"type": "band", "band": "alpha", "channel": "Pz"},
            "fs_hz": 250,
            "baseline_seconds": 10.0,
            "train_block_seconds": 5.0,
            "rest_block_seconds": 5.0,
            "dwell_seconds": 0.0,
            "refractory_seconds": 0.0,
        },
    )
    _write_json(
        outdir / "nf_run_meta.json",
        {
            "Tool": "qeeg_nf_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": ["nf_feedback.csv", "nf_summary.json", "nf_run_meta.json"],
        },
    )


def _make_pac(outdir: Path) -> None:
    headers = ["t_end_sec", "pac"]
    rows: List[Dict[str, object]] = []

    dt = 0.5
    t = 0.0
    # Deterministic synthetic PAC signal (bounded and finite).
    for i in range(600):
        t = (i + 1) * dt
        pac = 0.25 + 0.08 * math.sin(2 * math.pi * 0.03 * t) + 0.02 * random.uniform(-1.0, 1.0)
        # Clamp to a sensible range for the smoke test.
        if pac < 0.0:
            pac = 0.0
        rows.append({"t_end_sec": t, "pac": pac})

    _write_csv(outdir / "pac_timeseries.csv", headers, rows)

    # Optional MI-style phase distribution (18 bins).
    bins = 18
    dist_rows: List[Dict[str, object]] = []
    weights = []
    for i in range(bins):
        # Smooth unimodal distribution with slight noise.
        x = (i - (bins - 1) / 2.0) / (bins / 6.0)
        w = math.exp(-0.5 * x * x) + 0.02 * random.uniform(0.0, 1.0)
        weights.append(w)
    s = sum(weights) if weights else 1.0
    for i, w in enumerate(weights):
        dist_rows.append({"bin_index": i, "prob": (w / s) if s > 0 else 0.0})
    _write_csv(outdir / "pac_phase_distribution.csv", ["bin_index", "prob"], dist_rows)

    (outdir / "pac_summary.txt").write_text(
        "Synthetic PAC summary\n"
        "Channel: Cz\n"
        "Phase band: 4-8 Hz\n"
        "Amplitude band: 30-45 Hz\n"
        "Method: MI\n"
        "Window: 2 s\n"
        "Update: 0.5 s\n",
        encoding="utf-8",
    )

    _write_json(
        outdir / "pac_run_meta.json",
        {
            "Tool": "qeeg_pac_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "pac_timeseries.csv",
                "pac_phase_distribution.csv",
                "pac_summary.txt",
                "pac_run_meta.json",
            ],
        },
    )



def _make_epoch(outdir: Path) -> None:
    """Synthetic qeeg_epoch_cli outputs."""
    outdir.mkdir(parents=True, exist_ok=True)

    # ---- Events -------------------------------------------------------------
    ev_headers = ["event_id", "onset_sec", "duration_sec", "text"]
    events: List[Dict[str, object]] = []
    base_onsets = [5.0, 15.0, 28.0, 42.0, 60.0]
    for i, onset in enumerate(base_onsets, 1):
        dur = 3.0 + 0.5 * (i % 3)
        events.append(
            {
                "event_id": i,
                "onset_sec": onset,
                "duration_sec": dur,
                "text": f"stim_{i}",
            }
        )
    _write_csv(outdir / "events.csv", ev_headers, events)

    # A "events_table.csv" form (qeeg-style) and a BIDS-like TSV form.
    _write_csv(outdir / "events_table.csv", ["onset_sec", "duration_sec", "text"], events)
    bids_rows = [{"onset": r["onset_sec"], "duration": r["duration_sec"], "trial_type": r["text"]} for r in events]
    (outdir / "events_table.tsv").parent.mkdir(parents=True, exist_ok=True)
    with (outdir / "events_table.tsv").open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["onset", "duration", "trial_type"], delimiter="\t")
        w.writeheader()
        for r in bids_rows:
            w.writerow(r)

    # ---- Epoch bandpowers (long) -------------------------------------------
    channels = ["Fz", "Cz", "Pz"]
    bands = ["delta", "theta", "alpha", "beta", "gamma"]
    band_base = {"delta": 1.0, "theta": 1.5, "alpha": 2.2, "beta": 1.8, "gamma": 0.9}
    chan_gain = {"Fz": 1.05, "Cz": 1.00, "Pz": 0.95}

    long_headers = [
        "event_id",
        "onset_sec",
        "duration_sec",
        "epoch_start_sec",
        "epoch_end_sec",
        "text",
        "channel",
        "band",
        "power",
    ]
    long_rows: List[Dict[str, object]] = []
    for ev in events:
        onset = float(ev["onset_sec"])
        dur = float(ev["duration_sec"])
        for ch in channels:
            for band in bands:
                # Synthetic but deterministic (seed set in _run_renderers).
                base = band_base[band] * chan_gain[ch]
                power = base * (1.0 + 0.10 * math.sin(0.12 * onset) + 0.05 * random.uniform(-1.0, 1.0))
                if power < 1e-6:
                    power = 1e-6
                long_rows.append(
                    {
                        "event_id": ev["event_id"],
                        "onset_sec": onset,
                        "duration_sec": dur,
                        "epoch_start_sec": onset,
                        "epoch_end_sec": onset + dur,
                        "text": ev["text"],
                        "channel": ch,
                        "band": band,
                        "power": power,
                    }
                )

    _write_csv(outdir / "epoch_bandpowers.csv", long_headers, long_rows)

    # ---- Summary ------------------------------------------------------------
    sum_headers = ["channel", "band", "mean_power", "n_epochs"]
    sum_rows: List[Dict[str, object]] = []
    for ch in channels:
        for band in bands:
            vals = [float(r["power"]) for r in long_rows if r["channel"] == ch and r["band"] == band]
            mean_p = sum(vals) / len(vals) if vals else 0.0
            sum_rows.append({"channel": ch, "band": band, "mean_power": mean_p, "n_epochs": len(events)})
    _write_csv(outdir / "epoch_bandpowers_summary.csv", sum_headers, sum_rows)

    # ---- Baseline-normalized outputs ---------------------------------------
    nlong_headers = [
        "event_id",
        "onset_sec",
        "duration_sec",
        "epoch_start_sec",
        "epoch_end_sec",
        "baseline_start_sec",
        "baseline_end_sec",
        "text",
        "channel",
        "band",
        "baseline_power",
        "epoch_power",
        "value",
    ]
    nlong_rows: List[Dict[str, object]] = []
    for r in long_rows:
        onset = float(r["onset_sec"])
        dur = float(r["duration_sec"])
        epoch_p = float(r["power"])
        # baseline: small window immediately preceding event
        b0 = onset - 2.0
        b1 = onset - 0.5
        baseline_p = epoch_p * (0.85 + 0.08 * random.uniform(-1.0, 1.0))
        if baseline_p < 1e-6:
            baseline_p = 1e-6
        # Use a dB-like normalization value.
        value_db = 10.0 * math.log10(epoch_p / baseline_p) if baseline_p > 0 else 0.0
        nlong_rows.append(
            {
                "event_id": r["event_id"],
                "onset_sec": onset,
                "duration_sec": dur,
                "epoch_start_sec": onset,
                "epoch_end_sec": onset + dur,
                "baseline_start_sec": b0,
                "baseline_end_sec": b1,
                "text": r["text"],
                "channel": r["channel"],
                "band": r["band"],
                "baseline_power": baseline_p,
                "epoch_power": epoch_p,
                "value": value_db,
            }
        )
    _write_csv(outdir / "epoch_bandpowers_norm.csv", nlong_headers, nlong_rows)

    nsum_headers = ["channel", "band", "mode", "mean_value", "n_epochs"]
    nsum_rows: List[Dict[str, object]] = []
    for ch in channels:
        for band in bands:
            vals = [float(r["value"]) for r in nlong_rows if r["channel"] == ch and r["band"] == band]
            mean_v = sum(vals) / len(vals) if vals else 0.0
            nsum_rows.append({"channel": ch, "band": band, "mode": "db", "mean_value": mean_v, "n_epochs": len(events)})
    _write_csv(outdir / "epoch_bandpowers_norm_summary.csv", nsum_headers, nsum_rows)

    # ---- Run metadata -------------------------------------------------------
    _write_json(
        outdir / "epoch_run_meta.json",
        {
            "Tool": "qeeg_epoch_cli",
            "Version": "selftest",
            "GitDescribe": "selftest",
            "BuildType": "unknown",
            "Compiler": "unknown",
            "CppStandard": "c++17",
            "TimestampUTC": "selftest",
            "input_path": "synthetic",
            "OutputDir": str(outdir),
            "Outputs": [
                "events.csv",
                "events_table.csv",
                "events_table.tsv",
                "epoch_bandpowers.csv",
                "epoch_bandpowers_summary.csv",
                "epoch_bandpowers_norm.csv",
                "epoch_bandpowers_norm_summary.csv",
                "epoch_run_meta.json",
            ],
        },
    )

def _make_microstates(outdir: Path) -> None:
    stats_headers = ["microstate", "coverage", "mean_duration_sec", "occurrence_per_sec", "gev_contrib", "gev_frac"]
    rows = [
        {"microstate": "A", "coverage": 0.32, "mean_duration_sec": 0.12, "occurrence_per_sec": 2.6, "gev_contrib": 0.18, "gev_frac": 0.22},
        {"microstate": "B", "coverage": 0.27, "mean_duration_sec": 0.10, "occurrence_per_sec": 2.9, "gev_contrib": 0.16, "gev_frac": 0.19},
        {"microstate": "C", "coverage": 0.22, "mean_duration_sec": 0.08, "occurrence_per_sec": 3.2, "gev_contrib": 0.14, "gev_frac": 0.17},
        {"microstate": "D", "coverage": 0.19, "mean_duration_sec": 0.11, "occurrence_per_sec": 2.1, "gev_contrib": 0.12, "gev_frac": 0.15},
    ]
    _write_csv(outdir / "microstate_state_stats.csv", stats_headers, rows)

    # transition probs matrix
    states = ["A", "B", "C", "D"]
    mat_headers = [""] + states
    mat_rows: List[Dict[str, object]] = []
    for s in states:
        row: Dict[str, object] = {"": s}
        # simple ring transitions
        for t in states:
            if t == s:
                row[t] = 0.1
            elif states[(states.index(s) + 1) % len(states)] == t:
                row[t] = 0.6
            else:
                row[t] = 0.3 / (len(states) - 2)
        mat_rows.append(row)
    _write_csv(outdir / "microstate_transition_probs.csv", mat_headers, mat_rows)

    # timeseries
    ts_headers = ["time_sec", "label", "gfp", "corr"]
    ts_rows: List[Dict[str, object]] = []
    dt = 0.02
    labels = ["A", "B", "C", "D"]
    cur = 0
    t = 0.0
    for i in range(3000):
        if i % 250 == 0 and i > 0:
            cur = (cur + 1) % len(labels)
        lab = labels[cur]
        gfp = 1.0 + 0.2 * math.sin(2 * math.pi * 0.5 * t) + 0.05 * random.uniform(-1.0, 1.0)
        corr = 0.6 + 0.2 * math.sin(2 * math.pi * 0.2 * t + 0.5) + 0.05 * random.uniform(-1.0, 1.0)
        ts_rows.append({"time_sec": t, "label": lab, "gfp": gfp, "corr": corr})
        t += dt
    _write_csv(outdir / "microstate_timeseries.csv", ts_headers, ts_rows)

    # segments
    seg_headers = ["label", "start_sec", "end_sec"]
    seg_rows = [
        {"label": "A", "start_sec": 0.0, "end_sec": 5.0},
        {"label": "B", "start_sec": 5.0, "end_sec": 10.0},
        {"label": "C", "start_sec": 10.0, "end_sec": 15.0},
        {"label": "D", "start_sec": 15.0, "end_sec": 20.0},
    ]
    _write_csv(outdir / "microstate_segments.csv", seg_headers, seg_rows)

    (outdir / "microstate_summary.txt").write_text("Synthetic microstate summary.\n", encoding="utf-8")


def _assert_file(path: Path, *, min_bytes: int = 300) -> None:
    if not path.exists():
        raise RuntimeError(f"Expected file not created: {path}")
    if path.stat().st_size < min_bytes:
        raise RuntimeError(f"File too small (unexpected): {path} ({path.stat().st_size} bytes)")

def _assert_contains(path: Path, needle: str) -> None:
    txt = path.read_text(encoding="utf-8", errors="replace")
    if needle not in txt:
        raise RuntimeError(f"Expected to find {needle!r} in {path}")

def _assert_table_enhancements(path: Path) -> None:
    """If the shared table helpers are present, ensure the newer UX helpers are too."""
    txt = path.read_text(encoding="utf-8", errors="replace")
    # Only run this check on pages that include the shared table helper JS (filterTable).
    if "filterTable" not in txt:
        return
    if "qeeg_init_table_enhancements" not in txt:
        raise RuntimeError(f"Expected table UX helpers missing in {path}")
    # Sanity: look for the inserted UI element classes in the JS payload.
    if "col-chooser" not in txt or "filter-help" not in txt:
        raise RuntimeError(f"Expected column chooser / filter help hooks missing in {path}")




def _assert_report_common_js_sanity() -> None:
    """Validate the shared report JS doesn't regress.

    This catches issues like:
      - accidentally nesting DOMContentLoaded handlers so late-added listeners never run
      - breaking hidden-column CSV export
    """

    import report_common

    js = report_common.JS_SORT_TABLE

    # DOMContentLoaded handler should be registered at most once (we use qeeg_onReady).
    if js.count("document.addEventListener('DOMContentLoaded'") > 1:
        raise RuntimeError('JS_SORT_TABLE unexpectedly registers DOMContentLoaded more than once')

    # Column chooser persistence (best-effort) relies on storage helpers.
    for s in ("_qeegStorageGet", "_qeegStorageSet", "_qeegLoadHiddenColumns", "_qeegSaveHiddenColumns"):
        if s not in js:
            raise RuntimeError(f"Expected {s} in JS_SORT_TABLE")

    # Ensure CSV export respects hidden columns.
    if "function tableToCSV" not in js or "_qeegIsHiddenCell" not in js:
        raise RuntimeError('Expected tableToCSV/_qeegIsHiddenCell in JS_SORT_TABLE')
    if "if (_qeegIsHiddenCell(td)) return;" not in js:
        raise RuntimeError('Expected hidden-column skip in tableToCSV')




def _run_renderers(root: Path) -> None:
    # Make the synthetic outputs deterministic.
    random.seed(0)
    _assert_report_common_js_sanity()


    # Import renderers from the scripts directory. When this file is run as
    # python3 scripts/selftest_reports.py, the scripts/ folder is on sys.path.
    import render_artifacts_report
    import render_bandpowers_report
    import render_bandratios_report
    import render_channel_qc_report
    import render_connectivity_report
    import render_iaf_report
    import render_bids_scan_report
    import render_microstates_report
    import render_nf_feedback_report
    import render_pac_report
    import render_epoch_report
    import render_reports_dashboard
    import render_quality_report
    import render_spectral_features_report
    import render_trace_plot_report

    out_bp = root / "out_bp"
    out_ratios = root / "out_ratios"
    out_conn = root / "out_conn"
    out_qc = root / "out_qc"
    out_art = root / "out_art"
    out_nf = root / "out_nf"
    out_pac = root / "out_pac"
    out_epoch = root / "out_epoch"
    out_ms = root / "out_ms"
    out_iaf = root / "out_iaf"
    out_sf = root / "out_sf"
    out_quality = root / "out_quality"
    out_traces = root / "out_traces"
    out_bids = root / "out_bids_scan"

    _make_bandpowers(out_bp)
    _make_bandratios(out_ratios)
    _make_connectivity(out_conn)
    _make_channel_qc(out_qc)
    _make_artifacts(out_art)
    _make_nf(out_nf)
    _make_pac(out_pac)
    _make_epoch(out_epoch)
    _make_microstates(out_ms)
    _make_iaf(out_iaf)
    _make_spectral_features(out_sf)
    _make_quality(out_quality)
    _make_trace_plot(out_traces)
    _make_bids_scan(out_bids)

    # Run individual reports
    assert render_bandpowers_report.main(["--input", str(out_bp)]) == 0
    _assert_file(out_bp / "bandpowers_report.html")
    _assert_contains(out_bp / "bandpowers_report.html", "downloadTableCSV")
    _assert_contains(out_bp / "bandpowers_report.html", "Download CSV")

    assert render_bandratios_report.main(["--input", str(out_ratios)]) == 0
    _assert_file(out_ratios / "bandratios_report.html")
    _assert_contains(out_ratios / "bandratios_report.html", "downloadTableCSV")
    _assert_contains(out_ratios / "bandratios_report.html", "Download CSV")

    assert render_connectivity_report.main(["--input", str(out_conn)]) == 0
    _assert_file(out_conn / "connectivity_report.html")
    _assert_contains(out_conn / "connectivity_report.html", "downloadTableCSV")
    _assert_contains(out_conn / "connectivity_report.html", "Download CSV")

    assert render_channel_qc_report.main(["--input", str(out_qc)]) == 0
    _assert_file(out_qc / "channel_qc_report.html")
    _assert_contains(out_qc / "channel_qc_report.html", "downloadTableCSV")
    _assert_contains(out_qc / "channel_qc_report.html", "Download CSV")

    assert render_artifacts_report.main(["--input", str(out_art)]) == 0
    _assert_file(out_art / "artifacts_report.html")
    _assert_contains(out_art / "artifacts_report.html", "downloadTableCSV")
    _assert_contains(out_art / "artifacts_report.html", "Download CSV")

    assert render_nf_feedback_report.main(["--input", str(out_nf)]) == 0
    _assert_file(out_nf / "nf_feedback_report.html")
    _assert_contains(out_nf / "nf_feedback_report.html", "downloadTableCSV")
    _assert_contains(out_nf / "nf_feedback_report.html", "Download CSV")

    assert render_pac_report.main(["--input", str(out_pac)]) == 0
    _assert_file(out_pac / "pac_report.html")
    _assert_contains(out_pac / "pac_report.html", "downloadTableCSV")
    _assert_contains(out_pac / "pac_report.html", "Download CSV")

    assert render_epoch_report.main(["--input", str(out_epoch)]) == 0
    _assert_file(out_epoch / "epoch_report.html")
    _assert_contains(out_epoch / "epoch_report.html", "downloadTableCSV")
    _assert_contains(out_epoch / "epoch_report.html", "Download CSV")

    assert render_microstates_report.main(["--input", str(out_ms)]) == 0
    _assert_file(out_ms / "microstates_report.html")
    _assert_contains(out_ms / "microstates_report.html", "downloadTableCSV")
    _assert_contains(out_ms / "microstates_report.html", "Download CSV")

    assert render_iaf_report.main(["--input", str(out_iaf)]) == 0
    _assert_file(out_iaf / "iaf_report.html")
    _assert_contains(out_iaf / "iaf_report.html", "downloadTableCSV")
    _assert_contains(out_iaf / "iaf_report.html", "Download CSV")

    assert render_spectral_features_report.main(["--input", str(out_sf)]) == 0
    _assert_file(out_sf / "spectral_features_report.html")
    _assert_contains(out_sf / "spectral_features_report.html", "downloadTableCSV")
    _assert_contains(out_sf / "spectral_features_report.html", "Download CSV")

    assert render_quality_report.main(["--input", str(out_quality)]) == 0
    _assert_file(out_quality / "quality_report.html")
    _assert_contains(out_quality / "quality_report.html", "Download CSV")

    assert render_trace_plot_report.main(["--input", str(out_traces)]) == 0
    _assert_file(out_traces / "trace_plot_report.html")
    _assert_contains(out_traces / "trace_plot_report.html", "data:image/svg+xml")

    assert render_bids_scan_report.main(["--input", str(out_bids)]) == 0
    _assert_file(out_bids / "bids_scan_report.html")
    _assert_contains(out_bids / "bids_scan_report.html", "downloadTableCSV")
    _assert_contains(out_bids / "bids_scan_report.html", "bids_index_filtered.csv")

    # Dashboard (no-render; use the reports we generated above)
    dash_out = root / "qeeg_reports_dashboard.html"
    assert (
        render_reports_dashboard.main(
            [
                str(root),
                "--out",
                str(dash_out),
                "--no-render",
            ]
        )
        == 0
    )
    _assert_file(dash_out, min_bytes=500)
    _assert_contains(dash_out, "Quality runs")
    _assert_contains(dash_out, "Trace plot runs")
    _assert_contains(dash_out, "BIDS scan runs")
    _assert_contains(dash_out, "Epoch runs")
    # Dashboard should include the shared table helpers for sorting/filtering/export.
    _assert_contains(dash_out, "sortTable")
    _assert_contains(dash_out, "filterTable")
    _assert_contains(dash_out, "downloadTableCSV")

    # Newer table UX helpers should be included anywhere the shared table JS is embedded.
    for p in [
        out_bp / "bandpowers_report.html",
        out_ratios / "bandratios_report.html",
        out_conn / "connectivity_report.html",
        out_qc / "channel_qc_report.html",
        out_art / "artifacts_report.html",
        out_nf / "nf_feedback_report.html",
        out_pac / "pac_report.html",
        out_epoch / "epoch_report.html",
        out_ms / "microstates_report.html",
        out_iaf / "iaf_report.html",
        out_sf / "spectral_features_report.html",
        out_quality / "quality_report.html",
        out_bids / "bids_scan_report.html",
        dash_out,
        out_traces / "trace_plot_report.html",
    ]:
        _assert_table_enhancements(p)



def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Self-test for HTML report renderers (stdlib only).")
    ap.add_argument(
        "--outdir",
        default=None,
        help="Optional output directory. If omitted, a temp dir is used.",
    )
    ap.add_argument(
        "--keep",
        action="store_true",
        help="Keep generated files (if using a temp dir).",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    if args.outdir:
        root = Path(args.outdir).expanduser().resolve()
        root.mkdir(parents=True, exist_ok=True)
        _run_renderers(root)
        print(f"OK: reports generated under {root}")
        return 0

    with tempfile.TemporaryDirectory(prefix="qeeg_report_selftest_") as td:
        root = Path(td).resolve()
        _run_renderers(root)
        if args.keep:
            # Re-run with a stable path under current working dir.
            keep_root = Path.cwd() / "qeeg_report_selftest_out"
            if keep_root.exists():
                # best-effort cleanup
                for p in sorted(keep_root.rglob("*"), reverse=True):
                    try:
                        if p.is_file():
                            p.unlink()
                        else:
                            p.rmdir()
                    except Exception:
                        pass
            keep_root.mkdir(parents=True, exist_ok=True)
            # Copy files
            for p in root.rglob("*"):
                rel = p.relative_to(root)
                dst = keep_root / rel
                if p.is_dir():
                    dst.mkdir(parents=True, exist_ok=True)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.write_bytes(p.read_bytes())
            print(f"OK: reports generated under {keep_root} (kept)")
        else:
            print("OK: reports generated (temp dir cleaned)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
