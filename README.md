# qeeg-map (first pass)

A small, dependency-light **C++17** project that:
- loads EEG recordings from **EDF** (16‑bit EDF/EDF+) or **CSV**
- computes per‑channel **PSD via Welch's method**
- integrates **band power** (delta/theta/alpha/beta/gamma)
- renders **2D topographic scalp maps (topomaps)** to **BMP** images

> ⚠️ **Research / educational use only.** This is a first-pass implementation and **not** a medical device. Do not use it to diagnose, treat, or make clinical decisions.

## What this first pass includes

- **Readers**
  - `EDFReader`: EDF/EDF+ (16-bit) parser (annotations are ignored)
  - `CSVReader`: simple numeric CSV reader (fs provided via `--fs`)

- **Signal processing**
  - Welch PSD (Hann window, overlap, mean detrend)
  - Bandpower via trapezoidal integration over PSD bins

- **Topomap**
  - Built-in approximate **10–20 19-channel** 2D layout
  - Optional montage CSV file: `name,x,y` (unit circle coordinates)
  - Inverse-distance weighting (IDW) interpolation onto a grid

- **Outputs**
  - `bandpowers.csv`
  - `topomap_<band>.bmp`
  - Optional: `topomap_<band>_z.bmp` if `--reference` is provided

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

## Notes / limitations

- EDF support is **16-bit EDF/EDF+** only (BDF 24-bit not yet).
- Montage positions included are **approximate** for quick visualization.
- For serious work, you should:
  - load digitized electrode positions or a vetted template montage
  - add artifact detection/rejection and proper filtering
  - validate PSD scaling and units against a reference toolchain

## Project layout

- `include/qeeg/` — library headers
- `src/` — implementation + CLI
- `examples/` — sample montage + sample data
- `tests/` — a couple of basic sanity tests

## License

MIT (see `LICENSE`).
