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
import zipfile
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
    periodic_edge_col = "periodic_" + edge_col
    headers = [
        "channel",
        "total_power",
        "entropy",
        "mean_hz",
        "bandwidth_hz",
        "skewness",
        "kurtosis_excess",
        "flatness",
        "peak_hz",
        "peak_hz_refined",
        "peak_value_db",
        "peak_fwhm_hz",
        "peak_q",
        "peak_prominence_db",
        "prominent_peak_hz",
        "prominent_peak_hz_refined",
        "prominent_peak_value_db",
        "prominent_peak_fwhm_hz",
        "prominent_peak_q",
        "prominent_peak_prominence_db",
        "alpha_peak_hz",
        "alpha_peak_hz_refined",
        "alpha_peak_value_db",
        "alpha_fwhm_hz",
        "alpha_q",
        "alpha_prominence_db",
        "median_hz",
        edge_col,
        "periodic_median_hz",
        periodic_edge_col,
        "aperiodic_offset",
        "aperiodic_exponent",
        "aperiodic_r2",
        "periodic_power",
        "periodic_rel",
        "aperiodic_rmse",
        "aperiodic_n_points",
        "aperiodic_slope",
        "aperiodic_offset_db",
        "aperiodic_aic",
        "aperiodic_bic",
        "aperiodic_knee_hz",
        "aperiodic_slope_low",
        "aperiodic_slope_high",
        "aperiodic_exponent_low",
        "aperiodic_exponent_high",
        "aperiodic_r2_two_slope",
        "aperiodic_rmse_two_slope",
        "aperiodic_aic_two_slope",
        "aperiodic_bic_two_slope",
        "aperiodic_offset_knee",
        "aperiodic_exponent_knee",
        "aperiodic_knee_param",
        "aperiodic_knee_freq_hz",
        "aperiodic_r2_knee",
        "aperiodic_rmse_knee",
        "aperiodic_n_points_knee",
        "aperiodic_aic_knee",
        "aperiodic_bic_knee",
        "aperiodic_best_model_aic",
        "aperiodic_best_model_bic",
        "aperiodic_delta_aic_loglog",
        "aperiodic_aic_weight_loglog",
        "aperiodic_delta_aic_two_slope",
        "aperiodic_aic_weight_two_slope",
        "aperiodic_delta_aic_knee",
        "aperiodic_aic_weight_knee",
        "aperiodic_delta_bic_loglog",
        "aperiodic_bic_weight_loglog",
        "aperiodic_delta_bic_two_slope",
        "aperiodic_bic_weight_two_slope",
        "aperiodic_delta_bic_knee",
        "aperiodic_bic_weight_knee",
        "aperiodic_background_used",
        "delta_power",
        "delta_rel",
        "delta_periodic_power",
        "delta_periodic_rel",
        "delta_periodic_frac",
        "delta_prominent_peak_hz",
        "delta_prominent_peak_hz_refined",
        "delta_prominent_peak_value_db",
        "delta_prominent_peak_fwhm_hz",
        "delta_prominent_peak_q",
        "delta_prominent_peak_prominence_db",
        "theta_power",
        "theta_rel",
        "theta_periodic_power",
        "theta_periodic_rel",
        "theta_periodic_frac",
        "theta_prominent_peak_hz",
        "theta_prominent_peak_hz_refined",
        "theta_prominent_peak_value_db",
        "theta_prominent_peak_fwhm_hz",
        "theta_prominent_peak_q",
        "theta_prominent_peak_prominence_db",
        "alpha_power",
        "alpha_rel",
        "alpha_periodic_power",
        "alpha_periodic_rel",
        "alpha_periodic_frac",
        "alpha_prominent_peak_hz",
        "alpha_prominent_peak_hz_refined",
        "alpha_prominent_peak_value_db",
        "alpha_prominent_peak_fwhm_hz",
        "alpha_prominent_peak_q",
        "alpha_prominent_peak_prominence_db",
        "beta_power",
        "beta_rel",
        "beta_periodic_power",
        "beta_periodic_rel",
        "beta_periodic_frac",
        "beta_prominent_peak_hz",
        "beta_prominent_peak_hz_refined",
        "beta_prominent_peak_value_db",
        "beta_prominent_peak_fwhm_hz",
        "beta_prominent_peak_q",
        "beta_prominent_peak_prominence_db",
        "gamma_power",
        "gamma_rel",
        "gamma_periodic_power",
        "gamma_periodic_rel",
        "gamma_periodic_frac",
        "gamma_prominent_peak_hz",
        "gamma_prominent_peak_hz_refined",
        "gamma_prominent_peak_value_db",
        "gamma_prominent_peak_fwhm_hz",
        "gamma_prominent_peak_q",
        "gamma_prominent_peak_prominence_db",
        "theta_beta",
        "alpha_theta",
    ]
    rows = [
        {
            "channel": "Fz",
            "total_power": 12.3,
            "entropy": 0.62,
            "mean_hz": 10.4,
            "bandwidth_hz": 8.1,
            "skewness": -0.15,
            "kurtosis_excess": -0.75,
            "flatness": 0.42,
            "peak_hz": 10.0,
            "peak_hz_refined": 10.2,
            "peak_value_db": 8.1,
            "peak_fwhm_hz": 2.4,
            "peak_q": 4.17,
            "peak_prominence_db": 6.4,
            "prominent_peak_hz": 10.0,
            "prominent_peak_hz_refined": 10.2,
            "prominent_peak_value_db": 8.1,
            "prominent_peak_fwhm_hz": 2.4,
            "prominent_peak_q": 4.17,
            "prominent_peak_prominence_db": 6.4,
            "alpha_peak_hz": 10.0,
            "alpha_peak_hz_refined": 10.1,
            "alpha_peak_value_db": 7.7,
            "alpha_fwhm_hz": 1.8,
            "alpha_q": 5.56,
            "alpha_prominence_db": 5.1,
            "median_hz": 9.2,
            edge_col: 18.0,
            "periodic_median_hz": 10.0,
            periodic_edge_col: 14.5,
            "aperiodic_offset": -1.23,
            "aperiodic_exponent": 1.80,
            "aperiodic_r2": 0.92,
            "periodic_power": 3.1,
            "periodic_rel": 0.25,
            "aperiodic_rmse": 0.06,
            "aperiodic_n_points": 200,
            "aperiodic_slope": -1.80,
            "aperiodic_offset_db": -12.3,
            "aperiodic_aic": -1234.5,
            "aperiodic_bic": -1220.1,
            "aperiodic_knee_hz": 12.0,
            "aperiodic_slope_low": -1.60,
            "aperiodic_slope_high": -2.10,
            "aperiodic_exponent_low": 1.60,
            "aperiodic_exponent_high": 2.10,
            "aperiodic_r2_two_slope": 0.94,
            "aperiodic_rmse_two_slope": 0.05,
            "aperiodic_aic_two_slope": -1240.2,
            "aperiodic_bic_two_slope": -1218.7,
            "aperiodic_offset_knee": -1.22,
            "aperiodic_exponent_knee": 1.85,
            "aperiodic_knee_param": 78.0,
            "aperiodic_knee_freq_hz": 10.5,
            "aperiodic_r2_knee": 0.95,
            "aperiodic_rmse_knee": 0.045,
            "aperiodic_n_points_knee": 200,
            "aperiodic_aic_knee": -1255.8,
            "aperiodic_bic_knee": -1225.4,
            "aperiodic_best_model_aic": "knee",
            "aperiodic_best_model_bic": "knee",
            "aperiodic_delta_aic_loglog": 21.3,
            "aperiodic_aic_weight_loglog": 2.36e-05,
            "aperiodic_delta_aic_two_slope": 15.6,
            "aperiodic_aic_weight_two_slope": 4.06e-04,
            "aperiodic_delta_aic_knee": 0.0,
            "aperiodic_aic_weight_knee": 0.99957,
            "aperiodic_delta_bic_loglog": 5.3,
            "aperiodic_bic_weight_loglog": 0.0641,
            "aperiodic_delta_bic_two_slope": 6.7,
            "aperiodic_bic_weight_two_slope": 0.0319,
            "aperiodic_delta_bic_knee": 0.0,
            "aperiodic_bic_weight_knee": 0.9040,
            "aperiodic_background_used": "knee",
            "delta_power": 1.1,
            "delta_rel": 0.09,
            "delta_periodic_power": 0.40,
            "delta_periodic_rel": 0.033,
            "delta_periodic_frac": 0.129,
            "delta_prominent_peak_hz": 2.0,
            "delta_prominent_peak_hz_refined": 2.1,
            "delta_prominent_peak_value_db": 3.2,
            "delta_prominent_peak_fwhm_hz": 1.2,
            "delta_prominent_peak_q": 1.67,
            "delta_prominent_peak_prominence_db": 1.8,
            "theta_power": 2.2,
            "theta_rel": 0.18,
            "theta_periodic_power": 0.60,
            "theta_periodic_rel": 0.049,
            "theta_periodic_frac": 0.194,
            "theta_prominent_peak_hz": 6.0,
            "theta_prominent_peak_hz_refined": 6.1,
            "theta_prominent_peak_value_db": 4.5,
            "theta_prominent_peak_fwhm_hz": 2.0,
            "theta_prominent_peak_q": 3.0,
            "theta_prominent_peak_prominence_db": 2.2,
            "alpha_power": 3.3,
            "alpha_rel": 0.27,
            "alpha_periodic_power": 1.40,
            "alpha_periodic_rel": 0.114,
            "alpha_periodic_frac": 0.452,
            "alpha_prominent_peak_hz": 10.0,
            "alpha_prominent_peak_hz_refined": 10.1,
            "alpha_prominent_peak_value_db": 7.7,
            "alpha_prominent_peak_fwhm_hz": 1.8,
            "alpha_prominent_peak_q": 5.56,
            "alpha_prominent_peak_prominence_db": 5.1,
            "beta_power": 4.4,
            "beta_rel": 0.36,
            "beta_periodic_power": 0.50,
            "beta_periodic_rel": 0.041,
            "beta_periodic_frac": 0.161,
            "beta_prominent_peak_hz": 20.0,
            "beta_prominent_peak_hz_refined": 19.8,
            "beta_prominent_peak_value_db": 3.0,
            "beta_prominent_peak_fwhm_hz": 6.0,
            "beta_prominent_peak_q": 3.33,
            "beta_prominent_peak_prominence_db": 1.0,
            "gamma_power": 1.3,
            "gamma_rel": 0.11,
            "gamma_periodic_power": 0.20,
            "gamma_periodic_rel": 0.016,
            "gamma_periodic_frac": 0.065,
            "gamma_prominent_peak_hz": 35.0,
            "gamma_prominent_peak_hz_refined": 35.5,
            "gamma_prominent_peak_value_db": 1.2,
            "gamma_prominent_peak_fwhm_hz": 8.0,
            "gamma_prominent_peak_q": 4.38,
            "gamma_prominent_peak_prominence_db": 0.5,
            "theta_beta": 0.50,
            "alpha_theta": 1.50,
        },
        {
            "channel": "Cz",
            "total_power": 10.1,
            "entropy": 0.58,
            "mean_hz": 11.1,
            "bandwidth_hz": 7.6,
            "skewness": -0.05,
            "kurtosis_excess": -0.62,
            "flatness": 0.38,
            "peak_hz": 11.5,
            "peak_hz_refined": 11.6,
            "peak_value_db": 8.4,
            "peak_fwhm_hz": 2.1,
            "peak_q": 5.48,
            "peak_prominence_db": 7.0,
            "prominent_peak_hz": 11.5,
            "prominent_peak_hz_refined": 11.6,
            "prominent_peak_value_db": 8.4,
            "prominent_peak_fwhm_hz": 2.1,
            "prominent_peak_q": 5.48,
            "prominent_peak_prominence_db": 7.0,
            "alpha_peak_hz": 11.5,
            "alpha_peak_hz_refined": 11.6,
            "alpha_peak_value_db": 8.0,
            "alpha_fwhm_hz": 1.7,
            "alpha_q": 6.76,
            "alpha_prominence_db": 5.8,
            "median_hz": 10.2,
            edge_col: 20.5,
            "periodic_median_hz": 11.2,
            periodic_edge_col: 16.8,
            "aperiodic_offset": -1.10,
            "aperiodic_exponent": 1.95,
            "aperiodic_r2": 0.90,
            "periodic_power": 2.6,
            "periodic_rel": 0.26,
            "aperiodic_rmse": 0.07,
            "aperiodic_n_points": 200,
            "aperiodic_slope": -1.95,
            "aperiodic_offset_db": -11.0,
            "aperiodic_aic": -1199.9,
            "aperiodic_bic": -1182.0,
            "aperiodic_knee_hz": 13.0,
            "aperiodic_slope_low": -1.70,
            "aperiodic_slope_high": -2.20,
            "aperiodic_exponent_low": 1.70,
            "aperiodic_exponent_high": 2.20,
            "aperiodic_r2_two_slope": 0.92,
            "aperiodic_rmse_two_slope": 0.06,
            "aperiodic_aic_two_slope": -1205.1,
            "aperiodic_bic_two_slope": -1180.5,
            "aperiodic_offset_knee": -1.30,
            "aperiodic_exponent_knee": 1.90,
            "aperiodic_knee_param": 60.0,
            "aperiodic_knee_freq_hz": 9.0,
            "aperiodic_r2_knee": 0.93,
            "aperiodic_rmse_knee": 0.050,
            "aperiodic_n_points_knee": 200,
            "aperiodic_aic_knee": -1218.3,
            "aperiodic_bic_knee": -1188.1,
            "aperiodic_best_model_aic": "knee",
            "aperiodic_best_model_bic": "knee",
            "aperiodic_delta_aic_loglog": 18.4,
            "aperiodic_aic_weight_loglog": 0.000100,
            "aperiodic_delta_aic_two_slope": 13.2,
            "aperiodic_aic_weight_two_slope": 0.00136,
            "aperiodic_delta_aic_knee": 0.0,
            "aperiodic_aic_weight_knee": 0.99854,
            "aperiodic_delta_bic_loglog": 6.1,
            "aperiodic_bic_weight_loglog": 0.0442,
            "aperiodic_delta_bic_two_slope": 7.6,
            "aperiodic_bic_weight_two_slope": 0.0209,
            "aperiodic_delta_bic_knee": 0.0,
            "aperiodic_bic_weight_knee": 0.9349,
            "aperiodic_background_used": "knee",
            "delta_power": 1.0,
            "delta_rel": 0.10,
            "delta_periodic_power": 0.30,
            "delta_periodic_rel": 0.030,
            "delta_periodic_frac": 0.115,
            "delta_prominent_peak_hz": 2.5,
            "delta_prominent_peak_hz_refined": 2.6,
            "delta_prominent_peak_value_db": 2.8,
            "delta_prominent_peak_fwhm_hz": 1.4,
            "delta_prominent_peak_q": 1.79,
            "delta_prominent_peak_prominence_db": 1.2,
            "theta_power": 1.8,
            "theta_rel": 0.18,
            "theta_periodic_power": 0.60,
            "theta_periodic_rel": 0.049,
            "theta_periodic_frac": 0.194,
            "theta_prominent_peak_hz": 6.5,
            "theta_prominent_peak_hz_refined": 6.4,
            "theta_prominent_peak_value_db": 4.0,
            "theta_prominent_peak_fwhm_hz": 1.9,
            "theta_prominent_peak_q": 3.42,
            "theta_prominent_peak_prominence_db": 1.9,
            "alpha_power": 2.6,
            "alpha_rel": 0.26,
            "alpha_periodic_power": 1.20,
            "alpha_periodic_rel": 0.119,
            "alpha_periodic_frac": 0.462,
            "alpha_prominent_peak_hz": 11.5,
            "alpha_prominent_peak_hz_refined": 11.6,
            "alpha_prominent_peak_value_db": 8.2,
            "alpha_prominent_peak_fwhm_hz": 1.5,
            "alpha_prominent_peak_q": 7.67,
            "alpha_prominent_peak_prominence_db": 5.8,
            "beta_power": 4.0,
            "beta_rel": 0.40,
            "beta_periodic_power": 0.40,
            "beta_periodic_rel": 0.040,
            "beta_periodic_frac": 0.154,
            "beta_prominent_peak_hz": 18.0,
            "beta_prominent_peak_hz_refined": 18.2,
            "beta_prominent_peak_value_db": 2.7,
            "beta_prominent_peak_fwhm_hz": 5.5,
            "beta_prominent_peak_q": 3.27,
            "beta_prominent_peak_prominence_db": 0.9,
            "gamma_power": 0.7,
            "gamma_rel": 0.07,
            "gamma_periodic_power": 0.15,
            "gamma_periodic_rel": 0.015,
            "gamma_periodic_frac": 0.058,
            "gamma_prominent_peak_hz": 34.0,
            "gamma_prominent_peak_hz_refined": 34.5,
            "gamma_prominent_peak_value_db": 0.9,
            "gamma_prominent_peak_fwhm_hz": 7.5,
            "gamma_prominent_peak_q": 4.53,
            "gamma_prominent_peak_prominence_db": 0.4,
            "theta_beta": 0.45,
            "alpha_theta": 1.44,
        },
        {
            "channel": "Pz",
            "total_power": 14.7,
            "entropy": 0.66,
            "mean_hz": 9.7,
            "bandwidth_hz": 8.5,
            "skewness": -0.22,
            "kurtosis_excess": -0.88,
            "flatness": 0.45,
            "peak_hz": 9.5,
            "peak_hz_refined": 9.6,
            "peak_value_db": 7.9,
            "peak_fwhm_hz": 2.6,
            "peak_q": 3.65,
            "peak_prominence_db": 6.1,
            "prominent_peak_hz": 9.5,
            "prominent_peak_hz_refined": 9.6,
            "prominent_peak_value_db": 7.9,
            "prominent_peak_fwhm_hz": 2.6,
            "prominent_peak_q": 3.65,
            "prominent_peak_prominence_db": 6.1,
            "alpha_peak_hz": 9.5,
            "alpha_peak_hz_refined": 9.55,
            "alpha_peak_value_db": 7.4,
            "alpha_fwhm_hz": 2.0,
            "alpha_q": 4.75,
            "alpha_prominence_db": 4.9,
            "median_hz": 8.9,
            edge_col: 17.2,
            "periodic_median_hz": 9.4,
            periodic_edge_col: 13.9,
            "aperiodic_offset": -1.35,
            "aperiodic_exponent": 1.70,
            "aperiodic_r2": 0.93,
            "periodic_power": 3.8,
            "periodic_rel": 0.26,
            "aperiodic_rmse": 0.07,
            "aperiodic_n_points": 200,
            "aperiodic_slope": -1.95,
            "aperiodic_offset_db": -11.0,
            "aperiodic_knee_hz": 11.0,
            "aperiodic_slope_low": -1.40,
            "aperiodic_slope_high": -2.00,
            "aperiodic_exponent_low": 1.40,
            "aperiodic_exponent_high": 2.00,
            "aperiodic_r2_two_slope": 0.95,
            "aperiodic_rmse_two_slope": 0.06,
            "aperiodic_aic_two_slope": -1205.1,
            "aperiodic_bic_two_slope": -1180.5,
            "aperiodic_offset_knee": -1.30,
            "aperiodic_exponent_knee": 1.90,
            "aperiodic_knee_param": 60.0,
            "aperiodic_knee_freq_hz": 9.0,
            "aperiodic_r2_knee": 0.93,
            "aperiodic_rmse_knee": 0.050,
            "aperiodic_n_points_knee": 200,
            "delta_power": 1.2,
            "delta_rel": 0.08,
            "delta_periodic_power": 0.50,
            "delta_periodic_rel": 0.034,
            "delta_periodic_frac": 0.132,
            "delta_prominent_peak_hz": 2.2,
            "delta_prominent_peak_hz_refined": 2.3,
            "delta_prominent_peak_value_db": 3.0,
            "delta_prominent_peak_fwhm_hz": 1.3,
            "delta_prominent_peak_q": 1.69,
            "delta_prominent_peak_prominence_db": 1.5,
            "theta_power": 2.6,
            "theta_rel": 0.18,
            "theta_periodic_power": 0.60,
            "theta_periodic_rel": 0.049,
            "theta_periodic_frac": 0.194,
            "theta_prominent_peak_hz": 5.5,
            "theta_prominent_peak_hz_refined": 5.6,
            "theta_prominent_peak_value_db": 4.2,
            "theta_prominent_peak_fwhm_hz": 2.2,
            "theta_prominent_peak_q": 2.5,
            "theta_prominent_peak_prominence_db": 2.0,
            "alpha_power": 4.2,
            "alpha_rel": 0.29,
            "alpha_periodic_power": 1.80,
            "alpha_periodic_rel": 0.122,
            "alpha_periodic_frac": 0.474,
            "alpha_prominent_peak_hz": 9.5,
            "alpha_prominent_peak_hz_refined": 9.6,
            "alpha_prominent_peak_value_db": 7.6,
            "alpha_prominent_peak_fwhm_hz": 2.1,
            "alpha_prominent_peak_q": 4.52,
            "alpha_prominent_peak_prominence_db": 4.9,
            "beta_power": 4.8,
            "beta_rel": 0.33,
            "beta_periodic_power": 0.60,
            "beta_periodic_rel": 0.041,
            "beta_periodic_frac": 0.158,
            "beta_prominent_peak_hz": 19.0,
            "beta_prominent_peak_hz_refined": 19.1,
            "beta_prominent_peak_value_db": 3.1,
            "beta_prominent_peak_fwhm_hz": 6.2,
            "beta_prominent_peak_q": 3.06,
            "beta_prominent_peak_prominence_db": 1.1,
            "gamma_power": 1.9,
            "gamma_rel": 0.13,
            "gamma_periodic_power": 0.20,
            "gamma_periodic_rel": 0.014,
            "gamma_periodic_frac": 0.053,
            "gamma_prominent_peak_hz": 33.0,
            "gamma_prominent_peak_hz_refined": 33.2,
            "gamma_prominent_peak_value_db": 1.0,
            "gamma_prominent_peak_fwhm_hz": 8.2,
            "gamma_prominent_peak_q": 4.02,
            "gamma_prominent_peak_prominence_db": 0.6,
            "theta_beta": 0.54,
            "alpha_theta": 1.62,
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
            "bandwidth_hz": {
                "LongName": "Spectral bandwidth",
                "Description": f"Power-weighted standard deviation of frequency within {rng}.",
                "Units": "Hz",
            },
            "skewness": {
                "LongName": "Spectral skewness",
                "Description": f"Skewness of the power-weighted frequency distribution within {rng} (dimensionless).",
                "Units": "n/a",
            },
            "kurtosis_excess": {
                "LongName": "Spectral excess kurtosis",
                "Description": f"Excess kurtosis (kurtosis-3) of the power-weighted frequency distribution within {rng} (dimensionless).",
                "Units": "n/a",
            },
            "flatness": {
                "LongName": "Spectral flatness",
                "Description": f"Spectral flatness within {rng} (geometric_mean/arith_mean of PSD; values in [0,1]).",
                "Units": "n/a",
            },
            "peak_hz": {
                "LongName": "Peak frequency",
                "Description": f"Frequency of maximum PSD within {rng} (simple argmax).",
                "Units": "Hz",
            },
            "peak_hz_refined": {
                "LongName": "Peak frequency (refined)",
                "Description": f"Peak frequency refined by quadratic (parabolic) interpolation around peak_hz within {rng}.",
                "Units": "Hz",
            },
            "peak_value_db": {
                "LongName": "Peak PSD value (dB)",
                "Description": f"PSD value at peak_hz expressed in dB (10*log10) within {rng}.",
                "Units": "dB",
            },
            "peak_fwhm_hz": {
                "LongName": "Peak bandwidth (FWHM)",
                "Description": f"Full-width at half-maximum (FWHM) around peak_hz within {rng}.",
                "Units": "Hz",
            },
            "peak_q": {
                "LongName": "Peak Q factor",
                "Description": f"Q factor computed as peak_hz / peak_fwhm_hz within {rng}.",
                "Units": "n/a",
            },
            "peak_prominence_db": {
                "LongName": "Peak prominence (dB)",
                "Description": f"Peak prominence in dB at peak_hz relative to the selected aperiodic background model within {rng}.",
                "Units": "dB",
            },
            "prominent_peak_hz": {
                "LongName": "Most prominent peak frequency",
                "Description": f"Frequency of the most prominent oscillatory peak (maximum prominence above the selected aperiodic background model) within {rng}.",
                "Units": "Hz",
            },
            "prominent_peak_hz_refined": {
                "LongName": "Most prominent peak frequency (refined)",
                "Description": f"Most prominent peak frequency refined by quadratic (parabolic) interpolation around prominent_peak_hz within {rng}.",
                "Units": "Hz",
            },
            "prominent_peak_value_db": {
                "LongName": "Most prominent peak PSD value (dB)",
                "Description": f"PSD value at prominent_peak_hz expressed in dB (10*log10) within {rng}.",
                "Units": "dB",
            },
            "prominent_peak_fwhm_hz": {
                "LongName": "Most prominent peak bandwidth (FWHM)",
                "Description": f"Full-width at half-maximum (FWHM) around prominent_peak_hz within {rng}.",
                "Units": "Hz",
            },
            "prominent_peak_q": {
                "LongName": "Most prominent peak Q factor",
                "Description": f"Q factor computed as prominent_peak_hz / prominent_peak_fwhm_hz within {rng}.",
                "Units": "n/a",
            },
            "prominent_peak_prominence_db": {
                "LongName": "Most prominent peak prominence (dB)",
                "Description": f"Maximum peak prominence in dB relative to the robust aperiodic log-log fit within {rng}.",
                "Units": "dB",
            },
            "alpha_peak_hz": {
                "LongName": "Alpha peak frequency",
                "Description": f"Peak frequency within alpha range [8.0000,12.0000] Hz (intersected with {rng}).",
                "Units": "Hz",
            },
            "alpha_peak_hz_refined": {
                "LongName": "Alpha peak frequency (refined)",
                "Description": f"Alpha peak frequency refined by quadratic (parabolic) interpolation around alpha_peak_hz within alpha range [8.0000,12.0000] Hz.",
                "Units": "Hz",
            },
            "alpha_peak_value_db": {
                "LongName": "Alpha PSD value (dB)",
                "Description": f"PSD value at alpha_peak_hz expressed in dB (10*log10) within alpha range [8.0000,12.0000] Hz.",
                "Units": "dB",
            },
            "alpha_fwhm_hz": {
                "LongName": "Alpha peak bandwidth (FWHM)",
                "Description": f"Full-width at half-maximum (FWHM) around alpha_peak_hz within alpha range [8.0000,12.0000] Hz.",
                "Units": "Hz",
            },
            "alpha_q": {
                "LongName": "Alpha peak Q factor",
                "Description": f"Q factor computed as alpha_peak_hz / alpha_fwhm_hz within alpha range [8.0000,12.0000] Hz.",
                "Units": "n/a",
            },
            "alpha_prominence_db": {
                "LongName": "Alpha peak prominence (dB)",
                "Description": f"Alpha peak prominence in dB at alpha_peak_hz relative to the robust aperiodic log-log fit within {rng}.",
                "Units": "dB",
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
            "periodic_median_hz": {
                "LongName": "Periodic median frequency (periodic SEF50)",
                "Description": f"Spectral edge frequency at 50% cumulative periodic power within {rng} (aperiodic-adjusted).",
                "Units": "Hz",
            },
            periodic_edge_col: {
                "LongName": "Periodic spectral edge frequency (periodic SEF95)",
                "Description": f"Spectral edge frequency at 95% cumulative periodic power within {rng} (aperiodic-adjusted).",
                "Units": "Hz",
            },
            "aperiodic_offset": {
                "LongName": "Aperiodic offset (log-log intercept)",
                "Description": f"Intercept of a robust linear fit of log10(PSD) vs log10(frequency) within {rng}.",
                "Units": "log10(a.u.)",
            },
            "aperiodic_exponent": {
                "LongName": "Aperiodic exponent (1/f^k)",
                "Description": f"Exponent k from a robust 1/f^k fit within {rng} (computed as -slope in log-log space).",
                "Units": "n/a",
            },
            "aperiodic_r2": {
                "LongName": "Aperiodic fit R^2",
                "Description": f"R^2 goodness-of-fit for the robust log-log linear fit within {rng}.",
                "Units": "n/a",
            },
            "aperiodic_rmse": {
                "LongName": "Aperiodic fit RMSE",
                "Description": f"Root-mean-square error of the robust log-log linear fit within {rng} (in log10(PSD) units).",
                "Units": "log10(a.u.)",
            },
            "aperiodic_n_points": {
                "LongName": "Aperiodic fit N points",
                "Description": f"Number of sample points used in the aperiodic (log-log) fit within {rng}.",
                "Units": "count",
            },
            "aperiodic_slope": {
                "LongName": "Aperiodic slope (log-log)",
                "Description": f"Slope of a robust linear fit of log10(PSD) vs log10(frequency) within {rng}.",
                "Units": "n/a",
            },
            "aperiodic_offset_db": {
                "LongName": "Aperiodic offset (dB)",
                "Description": "Aperiodic offset in dB: 10*aperiodic_offset.",
                "Units": "dB",
            },
            "aperiodic_background_used": {
                "LongName": "Aperiodic background model used",
                "Description": "Aperiodic background model actually used for prominence and periodic residual metrics for this channel (after fallbacks).",
                "Units": "n/a",
            },
            "aperiodic_knee_hz": {
                "LongName": "Aperiodic knee frequency (two-slope)",
                "Description": f"Estimated knee frequency for a continuous two-slope fit of log10(PSD) vs log10(frequency) within {rng}.",
                "Units": "Hz",
            },
            "aperiodic_slope_low": {
                "LongName": "Aperiodic slope (low frequencies, two-slope)",
                "Description": f"Low-frequency slope of the two-slope log-log aperiodic fit within {rng}.",
                "Units": "n/a",
            },
            "aperiodic_slope_high": {
                "LongName": "Aperiodic slope (high frequencies, two-slope)",
                "Description": f"High-frequency slope of the two-slope log-log aperiodic fit within {rng}.",
                "Units": "n/a",
            },
            "aperiodic_exponent_low": {
                "LongName": "Aperiodic exponent (low frequencies, two-slope)",
                "Description": f"Low-frequency exponent k from the two-slope fit within {rng} (computed as -aperiodic_slope_low).",
                "Units": "n/a",
            },
            "aperiodic_exponent_high": {
                "LongName": "Aperiodic exponent (high frequencies, two-slope)",
                "Description": f"High-frequency exponent k from the two-slope fit within {rng} (computed as -aperiodic_slope_high).",
                "Units": "n/a",
            },
            "aperiodic_r2_two_slope": {
                "LongName": "Aperiodic fit R^2 (two-slope)",
                "Description": f"R^2 goodness-of-fit for the two-slope aperiodic fit within {rng} (log10 domain).",
                "Units": "n/a",
            },
            "aperiodic_rmse_two_slope": {
                "LongName": "Aperiodic fit RMSE (two-slope)",
                "Description": f"Root-mean-square error of the two-slope aperiodic fit within {rng} (in log10(PSD) units).",
                "Units": "log10(a.u.)",
            },
            "aperiodic_offset_knee": {
                "LongName": "Aperiodic offset (knee model)",
                "Description": f"Offset b of a curved knee aperiodic model within {rng}: log10(PSD)=b-log10(knee+f^exponent).",
                "Units": "log10(a.u.)",
            },
            "aperiodic_exponent_knee": {
                "LongName": "Aperiodic exponent (knee model)",
                "Description": f"Exponent of the curved knee aperiodic model within {rng}.",
                "Units": "n/a",
            },
            "aperiodic_knee_param": {
                "LongName": "Aperiodic knee parameter (knee model)",
                "Description": f"Knee parameter of the curved knee aperiodic model within {rng} (same units as f^exponent).",
                "Units": "Hz^exponent",
            },
            "aperiodic_knee_freq_hz": {
                "LongName": "Aperiodic knee frequency (knee model)",
                "Description": f"Derived knee frequency (Hz) from the knee model within {rng}: knee_freq = knee_param^(1/exponent).",
                "Units": "Hz",
            },
            "aperiodic_r2_knee": {
                "LongName": "Aperiodic fit R^2 (knee model)",
                "Description": f"R^2 goodness-of-fit for the curved knee aperiodic model within {rng} (log10 domain).",
                "Units": "n/a",
            },
            "aperiodic_rmse_knee": {
                "LongName": "Aperiodic fit RMSE (knee model)",
                "Description": f"Root-mean-square error of the curved knee aperiodic model within {rng} (in log10(PSD) units).",
                "Units": "log10(a.u.)",
            },
            "aperiodic_n_points_knee": {
                "LongName": "Aperiodic fit N points (knee model)",
                "Description": f"Number of sample points used in the aperiodic knee model fit within {rng}.",
                "Units": "count",
            },
            "periodic_power": {
                "LongName": "Periodic power above aperiodic",
                "Description": f"Integrated power above the fitted aperiodic background within {rng} (∫ max(0, PSD-PSD_aperiodic) df).",
                "Units": "a.u.",
            },
            "periodic_rel": {
                "LongName": "Periodic power fraction",
                "Description": f"Periodic power fraction within {rng}: periodic_power / total_power.",
                "Units": "n/a",
            },
            "delta_power": {
                "LongName": "delta band power",
                "Description": "Bandpower integrated over [1.0000,4.0000] Hz (intersected with the analysis range).",
                "Units": "a.u.",
            },
            "delta_rel": {
                "LongName": "delta relative band power",
                "Description": f"Relative delta bandpower: (delta_power) / (total_power) within {rng}.",
                "Units": "n/a",
            },
            "delta_periodic_power": {
                "LongName": "delta periodic band power",
                "Description": "Periodic power above the fitted aperiodic background integrated over [1.0000,4.0000] Hz.",
                "Units": "a.u.",
            },
            "delta_periodic_rel": {
                "LongName": "delta periodic relative band power",
                "Description": f"Relative periodic delta bandpower: delta_periodic_power / total_power within {rng}.",
                "Units": "n/a",
            },
            "delta_periodic_frac": {
                "LongName": "delta periodic band fraction",
                "Description": f"Fraction of periodic power in delta band: delta_periodic_power / periodic_power within {rng}.",
                "Units": "n/a",
            },
            "theta_power": {
                "LongName": "theta band power",
                "Description": "Bandpower integrated over [4.0000,8.0000] Hz (intersected with the analysis range).",
                "Units": "a.u.",
            },
            "theta_rel": {
                "LongName": "theta relative band power",
                "Description": f"Relative theta bandpower: (theta_power) / (total_power) within {rng}.",
                "Units": "n/a",
            },
            "theta_periodic_power": {
                "LongName": "theta periodic band power",
                "Description": "Periodic power above the fitted aperiodic background integrated over [4.0000,8.0000] Hz.",
                "Units": "a.u.",
            },
            "theta_periodic_rel": {
                "LongName": "theta periodic relative band power",
                "Description": f"Relative periodic theta bandpower: theta_periodic_power / total_power within {rng}.",
                "Units": "n/a",
            },
            "theta_periodic_frac": {
                "LongName": "theta periodic band fraction",
                "Description": f"Fraction of periodic power in theta band: theta_periodic_power / periodic_power within {rng}.",
                "Units": "n/a",
            },
            "alpha_power": {
                "LongName": "alpha band power",
                "Description": "Bandpower integrated over [8.0000,12.0000] Hz (intersected with the analysis range).",
                "Units": "a.u.",
            },
            "alpha_rel": {
                "LongName": "alpha relative band power",
                "Description": f"Relative alpha bandpower: (alpha_power) / (total_power) within {rng}.",
                "Units": "n/a",
            },
            "alpha_periodic_power": {
                "LongName": "alpha periodic band power",
                "Description": "Periodic power above the fitted aperiodic background integrated over [8.0000,12.0000] Hz.",
                "Units": "a.u.",
            },
            "alpha_periodic_rel": {
                "LongName": "alpha periodic relative band power",
                "Description": f"Relative periodic alpha bandpower: alpha_periodic_power / total_power within {rng}.",
                "Units": "n/a",
            },
            "alpha_periodic_frac": {
                "LongName": "alpha periodic band fraction",
                "Description": f"Fraction of periodic power in alpha band: alpha_periodic_power / periodic_power within {rng}.",
                "Units": "n/a",
            },
            "beta_power": {
                "LongName": "beta band power",
                "Description": "Bandpower integrated over [12.0000,30.0000] Hz (intersected with the analysis range).",
                "Units": "a.u.",
            },
            "beta_rel": {
                "LongName": "beta relative band power",
                "Description": f"Relative beta bandpower: (beta_power) / (total_power) within {rng}.",
                "Units": "n/a",
            },
            "beta_periodic_power": {
                "LongName": "beta periodic band power",
                "Description": "Periodic power above the fitted aperiodic background integrated over [12.0000,30.0000] Hz.",
                "Units": "a.u.",
            },
            "beta_periodic_rel": {
                "LongName": "beta periodic relative band power",
                "Description": f"Relative periodic beta bandpower: beta_periodic_power / total_power within {rng}.",
                "Units": "n/a",
            },
            "beta_periodic_frac": {
                "LongName": "beta periodic band fraction",
                "Description": f"Fraction of periodic power in beta band: beta_periodic_power / periodic_power within {rng}.",
                "Units": "n/a",
            },
            "gamma_power": {
                "LongName": "gamma band power",
                "Description": "Bandpower integrated over [30.0000,45.0000] Hz (intersected with the analysis range).",
                "Units": "a.u.",
            },
            "gamma_rel": {
                "LongName": "gamma relative band power",
                "Description": f"Relative gamma bandpower: (gamma_power) / (total_power) within {rng}.",
                "Units": "n/a",
            },
            "gamma_periodic_power": {
                "LongName": "gamma periodic band power",
                "Description": "Periodic power above the fitted aperiodic background integrated over [30.0000,45.0000] Hz.",
                "Units": "a.u.",
            },
            "gamma_periodic_rel": {
                "LongName": "gamma periodic relative band power",
                "Description": f"Relative periodic gamma bandpower: gamma_periodic_power / total_power within {rng}.",
                "Units": "n/a",
            },
            "gamma_periodic_frac": {
                "LongName": "gamma periodic band fraction",
                "Description": f"Fraction of periodic power in gamma band: gamma_periodic_power / periodic_power within {rng}.",
                "Units": "n/a",
            },
            "theta_beta": {
                "LongName": "theta/beta band ratio",
                "Description": "Ratio computed as (theta_power) / (beta_power).",
                "Units": "n/a",
            },
            "alpha_theta": {
                "LongName": "alpha/theta band ratio",
                "Description": "Ratio computed as (alpha_power) / (theta_power).",
                "Units": "n/a",
            },
        },
    )


    _write_json(
        outdir / "spectral_features_params.json",
        {
            "Tool": "qeeg_spectral_features_cli",
            "TimestampUTC": "selftest",
            "analysis_range_hz": [1.0, 45.0],
            "alpha_range_hz": [8.0, 12.0],
            "aperiodic_fit_range_hz": [1.0, 45.0],
            "aperiodic_exclude_ranges_hz": [],
            "aperiodic_two_slope": {"enabled": True, "min_points_per_side": 6},
            "aperiodic_knee_model": {"enabled": True, "robust": True, "max_iter": 4},
            "edges": [0.95],
            "welch": {"nperseg": 1024, "overlap_fraction": 0.5},
            "preprocess": {
                "average_reference": False,
                "notch_hz": 0.0,
                "notch_q": 30.0,
                "bandpass_low_hz": 0.0,
                "bandpass_high_hz": 0.0,
                "zero_phase": False,
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
                "spectral_features_params.json",
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
    import validate_reports_dashboard_index
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

    # Dashboard (no-render; use the reports we generated above) + bundle.
    # Exercise the convenience flags:
    #  - --bundle (with no explicit path): defaults to <out-dir>/qeeg_reports_bundle.zip
    #  - implicit JSON index creation when --bundle is present (even if --json-index is omitted)
    dash_out = root / "qeeg_reports_dashboard.html"
    dash_json = root / "qeeg_reports_dashboard_index.json"
    bundle_zip = root / "qeeg_reports_bundle.zip"
    assert (
        render_reports_dashboard.main(
            [
                str(root),
                "--out",
                str(dash_out),
                "--no-render",
                "--bundle",
                "--verify-bundle",
            ]
        )
        == 0
    )
    _assert_file(dash_out, min_bytes=500)
    _assert_file(dash_json, min_bytes=200)
    _assert_file(bundle_zip, min_bytes=600)
    dash_idx = json.loads(dash_json.read_text(encoding="utf-8"))
    assert dash_idx.get("$schema")
    assert dash_idx.get("schema_version") == 1
    assert dash_idx.get("dashboard_html")
    assert dash_idx.get("dashboard_exists") is True
    assert isinstance(dash_idx.get("dashboard_mtime_utc"), str) and dash_idx["dashboard_mtime_utc"].endswith("Z")
    assert isinstance(dash_idx.get("dashboard_size_bytes"), int) and int(dash_idx["dashboard_size_bytes"]) > 0

    assert isinstance(dash_idx.get("reports"), list)
    assert isinstance(dash_idx.get("reports_summary"), dict)
    assert any((r.get("kind") == "quality") for r in dash_idx.get("reports", []))

    # Per-report metadata should be present for existing reports.
    q = [r for r in dash_idx.get("reports", []) if r.get("kind") == "quality"]
    assert q, "Expected at least one quality report entry"
    assert q[0].get("report_exists") is True
    assert isinstance(q[0].get("report_mtime_utc"), str) and str(q[0]["report_mtime_utc"]).endswith("Z")
    assert isinstance(q[0].get("report_size_bytes"), int) and int(q[0]["report_size_bytes"]) > 0

    # Validate the JSON index against the repo schema (prefers python-jsonschema
    # when available, but is dependency-free in minimal mode).
    assert validate_reports_dashboard_index.main([str(dash_json), "--check-files"]) == 0

    # Verify bundle integrity (manifest + index references) and safely extract it.
    import verify_reports_bundle
    extract_dir = root / "bundle_extract"
    extract_dir.mkdir(parents=True, exist_ok=True)
    assert verify_reports_bundle.main(["--bundle", str(bundle_zip), "--extract", str(extract_dir)]) == 0

    extracted_index = extract_dir / dash_json.name
    _assert_file(extracted_index, min_bytes=200)
    assert validate_reports_dashboard_index.main([str(extracted_index), "--check-files"]) == 0

    # Convenience wrapper: verify+extract+open script (run in no-serve/no-open mode for tests).
    import open_reports_bundle
    extract_dir2 = root / "bundle_open_extract"
    assert open_reports_bundle.main(["--bundle", str(bundle_zip), "--extract", str(extract_dir2), "--force", "--no-serve", "--no-open"]) == 0
    _assert_file(extract_dir2 / dash_out.name, min_bytes=500)
    _assert_file(extract_dir2 / dash_json.name, min_bytes=200)
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
