# qeeg-map (first pass)

A small, dependency-light **C++17** project that:
- loads EEG recordings from **EDF** (16‑bit EDF/EDF+), **BDF** (24‑bit) or **CSV**
- computes per‑channel **PSD via Welch's method**
- integrates **band power** (delta/theta/alpha/beta/gamma)
- estimates **magnitude-squared coherence** (basic connectivity)
- renders **2D topographic scalp maps (topomaps)** to **BMP** images
- parses **EDF+/BDF+ annotations/events** (TAL) and supports basic **epoch/segment feature extraction**

> ⚠️ **Research / educational use only.** This is a first-pass implementation and **not** a medical device. Do not use it to diagnose, treat, or make clinical decisions.

## What this first pass includes

- **Readers**
  - `EDFReader`: EDF/EDF+ (16-bit) parser (EDF+ annotations are parsed into `EEGRecording::events`)
  - `BDFReader`: BDF/BDF+ (24-bit) parser (BDF+ annotations are parsed into `EEGRecording::events`)
  - `CSVReader`: simple numeric CSV reader (fs provided via `--fs`)

- **Signal processing**
  - Welch PSD (Hann window, overlap, mean detrend)
  - Bandpower via trapezoidal integration over PSD bins
  - Welch-style magnitude-squared coherence for channel pairs
  - Optional preprocessing: common average reference (CAR), IIR notch, simple bandpass; offline CLIs can run forward-backward (zero-phase) filtering

- **Topomap**
  - Built-in approximate **10–20 19-channel** 2D layout
  - Optional montage CSV file: `name,x,y` (unit circle coordinates)
  - Inverse-distance weighting (IDW) **or** Perrin-style **spherical spline** interpolation onto a grid

- **Outputs**
  - `bandpowers.csv`
  - `topomap_<band>.bmp`
  - Optional: `topomap_<band>_z.bmp` if `--reference` is provided
  - Coherence CLI outputs:
    - `coherence_matrix_<band>.csv` (matrix)
    - `coherence_pairs.csv` (edge list)

## Build

### Linux/macOS

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows (Visual Studio)

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Usage

### 1) Run a demo (synthetic data)

```bash
./build/qeeg_map_cli --demo --outdir out_demo --fs 250
```

### 2) Run on EDF

```bash
./build/qeeg_map_cli --input path/to/recording.edf --outdir out_edf
```

### 3) Run on CSV

CSV format:
- first row: channel names
- each following row: one sample per column

```bash
./build/qeeg_map_cli --input examples/sample_data_sine.csv --fs 250 --outdir out_csv
```

### 4) Use a custom montage

Montage CSV format: `name,x,y` where `x,y` are within the unit circle.

```bash
./build/qeeg_map_cli --input path/to/recording.edf --montage examples/sample_montage_1020_19.csv --outdir out_custom
```

### 5) Provide a reference (optional) to compute z-scores

Reference CSV format (one row per channel-band):
`channel,band,mean,std`

Example row:
`Fz,alpha,3.21,0.84`

```bash
./build/qeeg_map_cli --input path/to/recording.edf --reference my_reference.csv --outdir out_z
```

### Optional preprocessing (CAR + filters)

All CLIs support `--average-reference`. In addition:

- `--notch HZ` applies a simple IIR notch (e.g., `--notch 50` or `--notch 60`). Use `--notch-q Q` to adjust the width (default Q=30).
- `--bandpass LO HI` applies a simple bandpass by chaining a highpass at `LO` and a lowpass at `HI`.
- Offline CLIs (`qeeg_map_cli`, `qeeg_coherence_cli`) can add `--zero-phase` to do forward-backward filtering (less phase distortion).

Examples:

```bash
# Offline map with CAR + notch + bandpass + zero-phase filtering
./build/qeeg_map_cli --input path/to/recording.edf --outdir out_filtered \
  --average-reference --notch 50 --bandpass 1 45 --zero-phase

# Neurofeedback playback (causal filtering)
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_filtered --metric alpha:Pz \
  --average-reference --notch 50 --bandpass 1 45
```

### 6) Neurofeedback-style online metrics + adaptive threshold (offline playback)

This CLI simulates a real-time neurofeedback loop by:
- computing a metric on a sliding window (Welch PSD)
- setting an initial threshold from a baseline period
- optionally adapting the threshold to maintain a target reward rate

Supported metrics:
- bandpower: `alpha:Pz`
- ratio: `alpha/beta:Pz`
- coherence (magnitude-squared): `coh:alpha:F3:F4`
- phase-amplitude coupling (PAC): `pac:theta:gamma:Cz` (Tort modulation index)
- phase-amplitude coupling (PAC): `mvl:theta:gamma:Cz` (mean vector length)

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6
```

Coherence neurofeedback example:

```bash
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_coh --metric coh:alpha:F3:F4 \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 --export-coherence
```

PAC neurofeedback examples:

```bash
# Tort Modulation Index: theta phase -> gamma amplitude
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_pac --metric pac:theta:gamma:Cz \
  --window 4.0 --update 0.25 --baseline 10 --target-rate 0.6 --pac-bins 18 --pac-trim 0.10

# Mean Vector Length variant (often used in CFC literature)
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_mvl --metric mvl:theta:gamma:Cz \
  --window 4.0 --update 0.25 --baseline 10 --target-rate 0.6
```

Outputs:
- `nf_feedback.csv` — time series of metric, threshold and reward
- (optional) `bandpower_timeseries.csv` — all bands/channels per update (bandpower mode)
- (optional) `coherence_timeseries.csv` — all bands for the chosen pair per update (coherence mode)

### 7) Connectivity: coherence matrix / pair report

Compute an **alpha-band** coherence matrix for all channel pairs:

```bash
./build/qeeg_coherence_cli --input path/to/recording.edf --outdir out_coh --band alpha
```

Compute **one pair** (and optionally export the full spectrum):

```bash
./build/qeeg_coherence_cli --input path/to/recording.edf --outdir out_pair --band alpha --pair F3:F4 --export-spectrum
```

### 8) Time-frequency: STFT spectrogram (BMP + CSV)

Generate a **spectrogram** (time-frequency power) for a single channel.

```bash
./build/qeeg_spectrogram_cli --input path/to/recording.edf --outdir out_spec --channel Cz
```

Common tweaks:

```bash
# Higher time resolution, show up to 50 Hz, and apply light preprocessing
./build/qeeg_spectrogram_cli --input path/to/recording.edf --outdir out_spec --channel Cz \
  --window 1.0 --step 0.1 --maxfreq 50 --average-reference --notch 50 --bandpass 1 45 --zero-phase
```

Outputs:
- `spectrogram_<channel>.bmp` — heatmap (time on x, low freq at bottom)
- `spectrogram_<channel>.csv` — dB matrix (wide by default; use `--csv-long` for long format)
- `spectrogram_<channel>_meta.txt` — parameters used (for reproducibility)

### 9) Cross-frequency coupling: phase-amplitude coupling (PAC)

Estimate **phase-amplitude coupling** within a single channel using a sliding window.

Two estimators are available:
- **MI**: Tort modulation index (`--method mi`, default)
- **MVL**: mean vector length (`--method mvl`)

```bash
./build/qeeg_pac_cli --input path/to/recording.edf --outdir out_pac --channel Cz \
  --phase-band theta --amp-band gamma --window 4.0 --update 0.25

# Explicit bands (Hz) + zero-phase bandpass inside the PAC estimator
./build/qeeg_pac_cli --input path/to/recording.edf --outdir out_pac --channel Cz \
  --phase 4 8 --amp 70 90 --method mi --bins 18 --trim 0.10 --pac-zero-phase
```

Outputs:
- `pac_timeseries.csv` — time series (t_end_sec,pac)
- `pac_summary.txt` — parameters + summary stats
- (MI only) `pac_phase_distribution.csv` — average phase-binned amplitude distribution

### 10) Epoch/segment bandpower from annotations

If your EDF/BDF file is **EDF+/BDF+** and contains an **Annotations** signal, you can extract
bandpower features per event/epoch:

```bash
# Use the event duration (if present in the annotation) and keep only events containing "Stim"
./build/qeeg_epoch_cli --input path/to/recording.edf --outdir out_epochs --event-contains Stim

# Use a fixed 1.0s window starting 0.2s after each matching event
./build/qeeg_epoch_cli --input path/to/recording.edf --outdir out_epochs \
  --event-glob "*Stim*" --offset 0.2 --window 1.0
```

Outputs:
- `events.csv` — exported event list
- `epoch_bandpowers.csv` — long-format table of (event × channel × band)
- `epoch_bandpowers_summary.csv` — mean power per channel/band across processed epochs

### Topomap interpolation

By default, maps are rendered using **IDW** (fast, simple). You can enable **spherical spline** interpolation (smoother, common in EEG topography) via:

```bash
./build/qeeg_map_cli --input file.edf --outdir out --interp spline --spline-terms 60 --spline-m 4 --spline-lambda 1e-6
```

## Notes / limitations

- EDF support is **16-bit EDF/EDF+**.
- BDF support is **24-bit BDF/BDF+**.
- EDF+/BDF+ annotation parsing is **best-effort**. The code extracts TAL entries from "EDF Annotations" / "BDF Annotations" signals and ignores empty per-record timestamp markers.
- Montage positions included are **approximate** for quick visualization.
- Coherence here is **magnitude-squared coherence** (linear coupling) and is sensitive to volume conduction / common reference. Treat it as a rough first-pass connectivity feature.
- For serious work, you should:
  - load digitized electrode positions or a vetted template montage
  - add artifact detection/rejection and validate filtering choices for your protocol (this repo now includes a basic notch/bandpass, but it's still a first pass)
  - validate PSD scaling and units against a reference toolchain

## Project layout

- `include/qeeg/` — library headers
- `src/` — implementation + CLI
- `examples/` — sample montage + sample data
- `tests/` — a couple of basic sanity tests

## License

MIT (see `LICENSE`).
