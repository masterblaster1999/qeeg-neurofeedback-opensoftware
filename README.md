# qeeg-map (first pass)

A small, dependency-light **C++17** project that:
- loads EEG recordings from **EDF** (16‑bit EDF/EDF+), **BDF** (24‑bit) or **CSV**
- computes per‑channel **PSD via Welch's method**
- integrates **band power** (delta/theta/alpha/beta/gamma)
- estimates **magnitude-squared coherence** (basic connectivity)
- performs a first-pass **EEG microstate analysis** (GFP peak clustering + template maps)
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
  - Tip: add `--annotate` in `qeeg_map_cli` to draw a head outline, electrode markers, and an embedded vmin/vmax colorbar in the BMPs
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

#### Build a reference CSV from a small dataset

If you don't already have reference stats, you can build a simple reference file from
one or more recordings using `qeeg_reference_cli`. It computes per-channel Welch PSD,
integrates bandpower, and aggregates a **mean** and **standard deviation** per
channel-band across files.

```bash
# Build reference from multiple files (repeat --input)
./build/qeeg_reference_cli --input subj01.edf --input subj02.edf --input subj03.edf \
  --outdir out_ref

# Or pass a text file listing recordings (one path per line; "#" comments allowed)
./build/qeeg_reference_cli --list recordings.txt --outdir out_ref
```

By default the output is `out_ref/reference.csv` and is compatible with
`qeeg_map_cli --reference ...`.

Optional: compute stats over `log10(power)` (often closer to a normal distribution
than raw power):

```bash
./build/qeeg_reference_cli --list recordings.txt --outdir out_ref --log10
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

Optional: write a simple reward-tone WAV you can play back or feed into another system:

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
  --audio-wav nf_reward.wav --audio-tone 440 --audio-gain 0.2
```

Optional: stream neurofeedback state over OSC/UDP

If you want to drive external software (e.g., Max/MSP, Pure Data, TouchOSC), you can stream each NF update over UDP using **Open Sound Control (OSC)**:

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
  --osc-host 127.0.0.1 --osc-port 9000 --osc-prefix /qeeg --osc-mode state
```

Messages sent:
- `${prefix}/metric_spec` (string) — once at startup
- `${prefix}/fs` (float32) — once at startup
- `${prefix}/state` (float32 t_end_sec, float32 metric, float32 threshold, int32 reward, float32 reward_rate, int32 have_threshold) — every update

If you use `--osc-mode split`, `qeeg_nf_cli` sends multiple addresses per update:
`${prefix}/time`, `${prefix}/metric`, `${prefix}/threshold`, `${prefix}/reward`, `${prefix}/reward_rate`, `${prefix}/have_threshold`.

Note: UDP delivery is **best-effort**; packets may be dropped or reordered.

Optional: artifact gating (recommended for real data)

You can suppress rewards and adaptive threshold updates during gross artifacts
(e.g., big blinks / movement) using a simple, robust time-domain detector.
This uses sliding-window peak-to-peak, RMS and excess kurtosis outliers
relative to the baseline period.

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
  --artifact-gate --artifact-ptp-z 6 --artifact-rms-z 6 --artifact-kurtosis-z 6 \
  --export-artifacts
```

When OSC is enabled, the CLI also emits:
- `${prefix}/artifact_ready` (int32 0/1)
- `${prefix}/artifact` (int32 0/1)
- `${prefix}/artifact_bad_channels` (int32)

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
- (optional) `artifact_gate_timeseries.csv` — artifact gate aligned to NF updates (when `--export-artifacts`)
- (optional) `nf_reward.wav` — a simple reward-tone audio track (mono PCM16). Use `--audio-wav nf_reward.wav`.

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
- Tip: add `--colorbar` to embed a vertical vmin/vmax colorbar into the BMP
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

### 11) Artifact window detection (first pass)

Flag likely artifact-contaminated windows using simple **robust** time-domain features
(peak-to-peak amplitude, RMS, and excess kurtosis) computed on sliding windows.

This produces:
- `artifact_windows.csv` — one row per time window (including list of bad channels)
- `artifact_channels.csv` — long-format per-window × per-channel feature table
- `artifact_summary.txt` — parameters + summary stats

Example:

```bash
./build/qeeg_artifacts_cli --input path/to/recording.edf --outdir out_artifacts \
  --window 1.0 --step 0.5 --baseline 10 --ptp-z 6 --rms-z 6 --kurtosis-z 6

# Optional light preprocessing before scoring
./build/qeeg_artifacts_cli --input path/to/recording.edf --outdir out_artifacts_filtered \
  --window 1.0 --step 0.5 --baseline 10 --average-reference --notch 50 --bandpass 1 45 --zero-phase
```

### 12) Individual Alpha Frequency (IAF) / alpha peak estimation

Estimate an **alpha peak frequency** (IAF) per channel from a Welch PSD.

By default this CLI:
- computes a Welch PSD per channel
- optionally removes a simple **1/f trend** in log-frequency space (enabled by default)
- smooths the spectrum and finds the dominant peak in an alpha search band (default **7–13 Hz**)
- writes a per-channel table and renders an optional IAF topomap

```bash
./build/qeeg_iaf_cli --input path/to/recording.edf --outdir out_iaf \
  --average-reference --notch 50 --bandpass 1 45 --zero-phase
```

Outputs:
- `iaf_by_channel.csv` — per-channel IAF estimates
- `iaf_summary.txt` — parameters + aggregate IAF
- `topomap_iaf.bmp` — topomap of per-channel IAF (when enabled)

The CLI also writes a small helper file `iaf_band_spec.txt` containing a
simple **IAF-relative band spec** you can pass into other CLIs that accept
`--bands` (e.g., `qeeg_nf_cli`, `qeeg_map_cli`).

```bash
# Example: compute IAF, then run NF using IAF-relative alpha band
./build/qeeg_iaf_cli --input recording.edf --outdir out_iaf

./build/qeeg_nf_cli --input recording.edf --outdir out_nf \
  --bands "$(cat out_iaf/iaf_band_spec.txt)" --metric alpha:Pz
```

### 13) EEG microstates (first pass)

Estimate EEG **microstates** by:
- computing Global Field Power (GFP) over time
- taking local maxima of GFP (optionally thinned by a minimum distance)
- clustering peak topographies with k-means (polarity-invariant by default)
- assigning every sample to the closest template and exporting basic microstate statistics

```bash
./build/qeeg_microstates_cli --input path/to/recording.edf --outdir out_ms \
  --average-reference --notch 50 --bandpass 1 45 --zero-phase \
  --k 4 --peak-fraction 0.10 --min-duration-ms 30
```

Outputs:
- `microstate_templates.csv` — one row per microstate (A,B,C,...) containing the template topography values
- `topomap_microstate_<state>.bmp` — template topomap per microstate
- `microstate_timeseries.csv` — per-sample label + GFP + correlation to template
- `microstate_transition_counts.csv` — segment-to-segment transition count matrix
- `microstate_summary.txt` — parameters + Global Explained Variance (GEV) and per-state coverage/duration/occurrence

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
