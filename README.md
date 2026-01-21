# qeeg-map (first pass)

![CI](https://github.com/masterblaster1999/qeeg-neurofeedback-opensoftware/actions/workflows/ci.yml/badge.svg)
![CodeQL](https://github.com/masterblaster1999/qeeg-neurofeedback-opensoftware/actions/workflows/codeql.yml/badge.svg)

A small, dependency-light **C++17** project that:
- loads EEG recordings from **EDF** (16‑bit EDF/EDF+), **BDF** (24‑bit) or **CSV**
- computes per‑channel **PSD via Welch's method**
- integrates **band power** (delta/theta/alpha/beta/gamma)
- estimates **magnitude-squared coherence** and **phase locking value (PLV)** (basic connectivity)
- performs a first-pass **EEG microstate analysis** (GFP peak clustering + template maps)
- renders **2D topographic scalp maps (topomaps)** to **BMP** images
- parses **EDF+/BDF+ annotations/events** (TAL) and supports basic **epoch/segment feature extraction**

> ⚠️ **Research / educational use only.** This is a first-pass implementation and **not** a medical device. Do not use it to diagnose, treat, or make clinical decisions.

## What this first pass includes

- **Readers**
  - `EDFReader`: EDF/EDF+ (16-bit) parser (EDF+ annotations are parsed into `EEGRecording::events`)
  - `BDFReader`: BDF/BDF+ (24-bit) parser (BDF+ annotations are parsed into `EEGRecording::events`)
  - `CSVReader`: simple numeric CSV reader (fs provided via `--fs` or inferred from a time column)

- **Signal processing**
  - Welch PSD (Hann window, overlap, mean detrend)
  - Bandpower via trapezoidal integration over PSD bins
  - Welch-style magnitude-squared coherence for channel pairs
  - Optional preprocessing: common average reference (CAR), IIR notch, simple bandpass; offline CLIs can run forward-backward (zero-phase) filtering

- **Topomap**
  - Built-in approximate **10–20 19-channel** 2D layout (supports both modern **T7/T8/P7/P8** and legacy **T3/T4/T5/T6** labels)
  - Optional montage CSV file: `name,x,y` (unit circle coordinates)
  - Inverse-distance weighting (IDW) **or** Perrin-style **spherical spline** interpolation onto a grid

- **Outputs**
  - `bandpowers.csv`
  - `topomap_<band>.bmp`
  - Tip: add `--annotate` in `qeeg_map_cli` to draw a head outline, electrode markers, and an embedded vmin/vmax colorbar in the BMPs
  - Optional: `topomap_<band>_z.bmp` if `--reference` is provided
  - Coherence CLI outputs:
    - Pair mode (`--pair F3:F4`):
      - `coherence_band.csv` (or `imcoh_band.csv`)
      - optional: `coherence_spectrum.csv` (with `--export-spectrum`)
      - optional: `coherence_timeseries.csv` / `imcoh_timeseries.csv` (+ matching `.json` sidecar) (with `--timeseries`)
    - Matrix mode (no `--pair`):
      - `coherence_matrix_<band>.csv` (matrix)
      - `coherence_pairs.csv` (edge list)
  - PLV CLI outputs:
    - `plv_matrix_<band>.csv` (matrix)
    - `plv_pairs.csv` (edge list)


## Neurofeedback (qeeg_nf_cli)

This repository includes a first-pass, dependency-light **offline neurofeedback loop** tool: `qeeg_nf_cli`.

It runs a "real-time-ish" feedback loop over an EDF/BDF/CSV recording (or a synthetic `--demo` stream), computing a metric per update window, estimating an initial threshold from a baseline period (unless overridden), then emitting reward state and optional continuous feedback.

Quick start:

```bash
# List built-in protocol presets
qeeg_nf_cli --list-protocols

# Inspect channels in a recording (useful for choosing protocol channels)
qeeg_nf_cli --input recording.edf --list-channels
qeeg_nf_cli --input recording.edf --list-channels-json > channels.json

# Optional: enrich the JSON channel listing with channel-QC labels (bad channels + reasons)
qeeg_nf_cli --input recording.edf --channel-qc qc_outdir --list-channels-json > channels_qc.json

# Inspect the fully resolved configuration (after protocol preset defaults and overrides)
qeeg_nf_cli --demo --fs 250 --seconds 5 --protocol alpha_up_pz --print-config-json > config.json

# Machine-friendly listings (for scripts / GUIs)
qeeg_nf_cli --list-bands
qeeg_nf_cli --list-bands-json > bands.json
qeeg_nf_cli --list-metrics
qeeg_nf_cli --list-metrics-json > metrics.json

qeeg_nf_cli --list-protocols-names
qeeg_nf_cli --list-protocols-json > protocols.json
qeeg_nf_cli --protocol-help-json alpha_up_pz > alpha_up_pz.json

# Version/build metadata (useful for bug reports)
qeeg_nf_cli --version
qeeg_nf_cli --version-json

# Run a preset protocol and write a self-contained HTML UI export
qeeg_nf_cli --input recording.edf --outdir out_nf --protocol alpha_up_pz --biotrace-ui

# Expert mode: specify an explicit metric
qeeg_nf_cli --input recording.edf --outdir out_nf --metric alpha:Pz --reward-direction above
```

Common outputs (written under `--outdir`): `nf_feedback.csv`, `nf_run_meta.json`, `nf_summary.json`.
Optionally: `biotrace_ui.html`, `nf_derived_events.*`, and timeseries exports (bandpowers/coherence/artifacts).

### JSON Schemas for qeeg_nf_cli JSON outputs

To help build GUIs/analysis pipelines, this repo includes JSON Schema (Draft 2020-12) documents under `schemas/` that describe the structure of key machine-readable outputs from `qeeg_nf_cli`.

STDOUT JSON (printed by flags):

- `--list-channels-json`
- `--print-config-json`
- `--list-bands-json`
- `--list-metrics-json`
- `--list-protocols-json`
- `--version-json`

On-disk JSON (written under `--outdir`):

- `nf_run_meta.json`
- `nf_summary.json`
- `nf_derived_events.json` (when exporting derived events; BIDS-style sidecar describing the `onset`/`duration` (seconds) and `trial_type` columns in `nf_derived_events.tsv`)

For the object-shaped outputs above, `qeeg_nf_cli` also emits a top-level `$schema` field pointing at the schema document’s `$id`.
This makes it easier for some editors/validators to automatically associate a JSON document with the right schema.

These schema files are installed alongside the other documentation when `QEEG_INSTALL_DOCS=ON` (for example: `<prefix>/share/doc/qeeg/schemas/`).

To validate the current `qeeg_nf_cli` JSON outputs against these schemas, you can run:

```bash
python3 scripts/validate_nf_cli_json_outputs.py \
  --nf-cli ./build/qeeg_nf_cli \
  --schemas-dir ./schemas

# Validate an existing neurofeedback output directory (no demo run, no STDOUT checks)
python3 scripts/validate_nf_cli_json_outputs.py \
  --schemas-dir ./schemas \
  --validate-outdir ./out_nf \
  --skip-stdout \
  --skip-demo
```

If you are building with CMake and a Python interpreter is available, the build
also provides a convenience target that runs the same validation against the
in-tree schemas and the built `qeeg_nf_cli`:

```bash
cmake --build ./build --target qeeg_validate_nf_cli_json
```

The validator will:

- validate the STDOUT JSON outputs directly
- run a short `--demo` session into a temporary `--outdir` and validate `nf_run_meta.json` / `nf_summary.json` / `nf_derived_events.json`
- when exporting derived events, also sanity-check `nf_derived_events.tsv` and `nf_derived_events.csv` (required columns, numeric onset/duration, and basic consistency with the JSON sidecar)

If `--validate-outdir` is provided, the validator will also validate any matching
JSON outputs found there (derived-events outputs are treated as optional unless present).

The validator will use full Draft 2020-12 JSON Schema validation when the `jsonschema` Python package is available:

```bash
python3 -m pip install --user jsonschema
```

When `jsonschema` is available, the validator also checks that the schema documents
themselves are valid Draft 2020-12 schemas.

If the dependency is not installed, it falls back to a lightweight, best-effort structural check
(including arrays and nested required keys) for the subset of schema keywords used in this repo.

The fallback validator also resolves local and cross-document `$ref` (by schema `$id` or filename),
so it stays useful even if the JSON Schemas are refactored into multiple documents.

> ⚠️ Neurofeedback is included here for **research/educational prototyping**.
>
> - This software is **not** a medical device and this documentation is **not** medical advice.
> - Neurofeedback protocols can have **unintended effects** (for example, changes in arousal/sleep, headache, irritability, anxiety, or fatigue).
> - If you have a neurological/psychiatric condition, are on medication, have an implanted medical device, or are unsure, consult a qualified clinician and stop if symptoms worsen.
>
> When in doubt, start with conservative parameters, short sessions, and careful monitoring/logging.

Further reading / practice standards (background only):

- Hammond & Kirk (2008) *First, do no harm: Adverse effects and the need for practice standards in neurofeedback* (Journal of Neurotherapy / ISNR-JNT open-access): https://www.isnr-jnt.org/article/view/16682
- Rogel et al. (2015) *Transient adverse side effects during neurofeedback training: a randomized, sham-controlled, double blind study* (PubMed): https://pubmed.ncbi.nlm.nih.gov/26008757/
- BCIA Professional Standards/Ethical Principles: https://www.bcia.org/bcia-professional-standards-ethical-principles
- ISNR Neurofeedback FAQ (includes discussion of possible side effects): https://isnr.org/neurofeedback-faq

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

### CMake presets (optional)

If you have **CMake >= 3.19**, you can use the included `CMakePresets.json`:

```bash
# Standard release build
cmake --preset release
cmake --build --preset release
ctest --preset release

# Release build with LTO/IPO (best-effort; toolchain-dependent)
cmake --preset lto-release
cmake --build --preset lto-release
ctest --preset lto-release

# Generate local HTML docs with Doxygen (requires doxygen)
cmake --preset docs
cmake --build --preset docs

# clang-tidy analysis build (requires clang-tidy; can be slow)
cmake --preset tidy
cmake --build --preset tidy
```

### Useful build options

```bash
# Build only the qeeg library (skip all CLI tools)
cmake -S . -B build -DQEEG_BUILD_CLI=OFF

# Enable AddressSanitizer + UndefinedBehaviorSanitizer (best-effort; non-MSVC)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DQEEG_ENABLE_SANITIZERS=ON

# Enable ccache (if installed) for faster rebuilds (developer convenience)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DQEEG_ENABLE_CCACHE=ON

# Enable clang-tidy during compilation (requires clang-tidy; can slow builds)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DQEEG_ENABLE_CLANG_TIDY=ON

# Build static libqeeg with position-independent code (-fPIC where relevant)
# (useful if you link libqeeg.a into your own shared library)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQEEG_ENABLE_PIC=ON

# Enable interprocedural optimization (LTO/IPO) in Release-ish builds (best-effort)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQEEG_ENABLE_IPO=ON

# Build qeeg as a shared library (.so/.dylib) instead of a static library (.a)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON

# Generate local HTML docs with Doxygen (requires doxygen)
cmake -S . -B build-docs -DCMAKE_BUILD_TYPE=Release -DQEEG_BUILD_DOCS=ON -DQEEG_BUILD_TESTS=OFF -DQEEG_BUILD_CLI=OFF
cmake --build build-docs --target docs

# For shared builds, you can disable the (relative) install RPATH if you are
# packaging for a distribution that prefers to manage runtime search paths
# externally.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DQEEG_INSTALL_RPATH=OFF
```


### Formatting (clang-format)

The repo includes a `.clang-format` file, and CI checks formatting on changed C/C++ files.

* Check formatting (no changes):

```bash
clang-format --style=file --dry-run --Werror path/to/file.cpp
```

* Apply formatting in-place:

```bash
clang-format --style=file -i path/to/file.cpp
```

## Install (optional)

You can optionally install the **qeeg** library and the CLI tools:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --prefix <install-prefix>
```

To uninstall (from the same build directory you installed from):

```bash
cmake --build build --target uninstall
```

Note: the uninstall target uses the build directory's `install_manifest.txt`, which is written by the most recent `cmake --install` run from that build tree.


### Shell completions (optional)

If you install the CLI tools, the build can also install shell completion scripts for:

- `qeeg_offline_app_cli` (the single-binary toolbox)
  - completes offline-app flags (`--list-tools`, `--install-shims`, ...)
  - completes tool names for the first positional argument
  - completes tool names after `--tool`
- `qeeg_version_cli`
  - completes `--full`, `--json`, and `--help`
- `qeeg_nf_cli`
  - completes common neurofeedback flags
  - completes protocol preset names for `--protocol` / `--protocol-help`
  - completes enumerated values for select flags (for example `above|below`, `exp|quantile`, `state|split|bundle`)

On Unix-y platforms this is enabled by default and can be toggled with:

```bash
cmake -S . -B build -DQEEG_INSTALL_COMPLETIONS=ON   # enable
cmake -S . -B build -DQEEG_INSTALL_COMPLETIONS=OFF  # disable
```

Installed locations (relative to `<prefix>`):

- **Bash** (bash-completion):
  - `<prefix>/share/bash-completion/completions/qeeg_offline_app_cli`
  - `<prefix>/share/bash-completion/completions/qeeg_version_cli`
  - `<prefix>/share/bash-completion/completions/qeeg_nf_cli`
  - Note: the installed file name must match the command for bash-completion's on-demand loader.
- **Zsh**:
  - `<prefix>/share/zsh/site-functions/_qeeg_offline_app_cli`
  - `<prefix>/share/zsh/site-functions/_qeeg_version_cli`
  - `<prefix>/share/zsh/site-functions/_qeeg_nf_cli`
- **Fish**:
  - `<prefix>/share/fish/vendor_completions.d/qeeg_offline_app_cli.fish`
  - `<prefix>/share/fish/vendor_completions.d/qeeg_version_cli.fish`
  - `<prefix>/share/fish/vendor_completions.d/qeeg_nf_cli.fish`

Per-user install (no root) examples:

- Bash: copy the completion file(s) to:
  - `~/.local/share/bash-completion/completions/qeeg_offline_app_cli`
  - `~/.local/share/bash-completion/completions/qeeg_version_cli`
  - `~/.local/share/bash-completion/completions/qeeg_nf_cli`
  (or `$XDG_DATA_HOME/bash-completion/completions/<command>`)
- Zsh: copy `_qeeg_offline_app_cli`, `_qeeg_version_cli`, and `_qeeg_nf_cli` into a directory on your `$fpath` (for example `~/.zsh/completions/`) and ensure `compinit` is enabled.
- Fish:
  - `~/.config/fish/completions/qeeg_offline_app_cli.fish`
  - `~/.config/fish/completions/qeeg_version_cli.fish`
  - `~/.config/fish/completions/qeeg_nf_cli.fish`

Then restart your shell.

### Man pages (optional)

If you install the CLI tools, the build can also install **man pages** for:

- `qeeg_offline_app_cli`
- `qeeg_version_cli`
- `qeeg_nf_cli`

On Unix-y platforms this is enabled by default and can be toggled with:

```bash
cmake -S . -B build -DQEEG_INSTALL_MANPAGES=ON   # enable
cmake -S . -B build -DQEEG_INSTALL_MANPAGES=OFF  # disable
```

Installed locations (relative to `<prefix>`):

- `<prefix>/share/man/man1/qeeg_offline_app_cli.1`
- `<prefix>/share/man/man1/qeeg_version_cli.1`
- `<prefix>/share/man/man1/qeeg_nf_cli.1`

Usage:

```bash
man qeeg_offline_app_cli
man qeeg_version_cli
man qeeg_nf_cli
```

If you installed into a non-system prefix, you may need to extend `MANPATH`:

```bash
export MANPATH="<prefix>/share/man:${MANPATH}"
man qeeg_offline_app_cli
```


To consume the library from another CMake project:

```cmake
find_package(qeeg CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE qeeg::qeeg)
```

To consume via **pkg-config** (non-Windows):

```bash
# libdir is usually 'lib' or 'lib64' depending on your platform/toolchain
export PKG_CONFIG_PATH="<install-prefix>/<libdir>/pkgconfig:${PKG_CONFIG_PATH}"
pkg-config --cflags --libs qeeg

# Example: compile+link a tiny consumer (replace main.cpp with your source file):
#   c++ $(pkg-config --cflags qeeg) main.cpp -o main $(pkg-config --libs qeeg)
```


## Package (optional)

If you want a simple portable archive of the install tree (useful for sharing binaries without running an installer), you can use **CPack**:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQEEG_BUILD_TESTS=OFF -DQEEG_BUILD_CLI=ON
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release
```

The resulting `.zip` / `.tar.gz` files will be written to the build directory and contain the files from the project's `install()` rules (so the contents depend on options like `QEEG_BUILD_CLI`, `QEEG_INSTALL_DOCS`, etc.).

## Docs (optional)

If you have **Doxygen** installed, you can generate local HTML documentation:

```bash
cmake -S . -B build-docs -DCMAKE_BUILD_TYPE=Release -DQEEG_BUILD_DOCS=ON -DQEEG_BUILD_TESTS=OFF -DQEEG_BUILD_CLI=OFF
cmake --build build-docs --target docs

# Open build-docs/docs/html/index.html in your browser
```

## Usage

### Version

```bash
# From your build directory (where the executables live):
./build/qeeg_version_cli
./build/qeeg_version_cli --full
./build/qeeg_version_cli --json  # includes version_major/minor/patch for scripts
```

### QEEG Tools UI (dashboard)

If you want a single "home page" that lists all of the project's executables and links to any
discovered outputs, you can generate a self-contained HTML dashboard:

```bash
# From your build directory (where the executables live):
./build/qeeg_ui_cli --root . --bin-dir ./build --open

# Or point it at a parent folder that contains multiple tool outdirs (out_map/out_nf/...):
./build/qeeg_ui_cli --root /path/to/runs --bin-dir ./build --output /path/to/runs/qeeg_ui.html
```

The dashboard can (optionally) embed each tool's `--help` output and will surface run artifacts via
the `*_run_meta.json` manifests emitted by some tools.

If you provide `--bin-dir`, the dashboard will also **auto-discover any additional**
`qeeg_*_cli` executables present in that folder and include them in the UI (so new tools show up
without manually updating a list). You can disable this with `--no-bin-scan`.

#### Optional: run tools from the browser (local-only)

Browsers can't directly execute local binaries for security reasons, so the "Run" buttons in the
dashboard are enabled only when you serve the UI with the local server:

```bash
./build/qeeg_ui_server_cli --root /path/to/runs --bin-dir ./build --port 8765 --max-parallel 2 --open
```

Tip: add `--max-parallel N` to limit how many tools run at once; additional runs are kept in a **queued** state until a slot is free.


#### Optional: single-binary offline toolbox

If you prefer to distribute **one executable** instead of many `qeeg_*_cli` binaries, build/use the
multicall wrapper:

```bash
./build/qeeg_offline_app_cli --list-tools
./build/qeeg_offline_app_cli qeeg_version_cli --help
./build/qeeg_offline_app_cli qeeg_map_cli --help
```

Optionally, you can create per-tool *shim* command names next to the toolbox binary (implemented
as hardlinks/symlinks where possible):

```bash
# Create qeeg_map_cli, qeeg_ui_server_cli, ... next to qeeg_offline_app_cli
./build/qeeg_offline_app_cli --install-shims ./build

# Now you can call tools directly
./build/qeeg_map_cli --help
```


The UI server can also use it as a fallback runner (so the dashboard can still execute tools even if
the per-tool executables aren't present in `--bin-dir`):

```bash
./build/qeeg_ui_server_cli --root /path/to/runs --bin-dir ./build --toolbox qeeg_offline_app_cli --open
```

#### Optional: create a portable offline app folder

To ship a **single folder** (e.g., zip it and share), you can build a bundle that contains:
- `bin/` (executables)
- `runs/` (a writable root for UI runs + outputs)
- `start_qeeg_ui.sh` / `start_qeeg_ui.bat` launch scripts

```bash
# Create the bundle folder
./build/qeeg_bundle_cli --bin-dir ./build --outdir ./qeeg_offline_bundle

# Run it (Linux/macOS)
cd qeeg_offline_bundle
./start_qeeg_ui.sh

# Run it (Windows)
start_qeeg_ui.bat
```

By default the bundle is **minimal**: it copies `qeeg_offline_app_cli` and then generates per-tool
shims (hardlinks/symlinks where possible) inside `bin/`. This lets you run `qeeg_map_cli`,
`qeeg_ui_server_cli`, etc. directly while still shipping a single binary. Disable shim creation with
`--no-tool-shims`.

The launcher scripts start the dashboard via the embedded `qeeg_ui_server_cli` entrypoint (inside the
toolbox) and serve the pre-generated `runs/qeeg_ui.html` without regenerating it on startup.

This serves the dashboard at `http://127.0.0.1:8765/` and exposes a tiny local-only API that lets the
UI launch `qeeg_*_cli` executables and write per-run logs under `.../ui_runs/`.

The hero **Recent UI runs** card has two views:
- **Session**: jobs launched since the current server started (from the live `/api/runs` list)
- **History**: scans `ui_runs/` so you can find runs from previous sessions and quickly **Load args** back into a tool card (handy after restarting the server)

Browsing outputs: the server also renders a simple directory index for folders under `--root` when no
`index.html` is present. This makes the UI's **Run dir** links (e.g., `ui_runs/<timestamp>_<tool>_idX/`)
clickable so you can quickly browse outputs (CSV/JSON/SVG/HTML reports) in your browser.

Security headers: when serving the built-in dashboard (\`/\` or \`/index.html\`) and the auto-generated directory index pages, the server also sets a Content-Security-Policy (CSP) and a few common hardening headers (\`X-Content-Type-Options\`, \`X-Frame-Options\`, \`Referrer-Policy\`, \`Cross-Origin-Resource-Policy\`). This helps reduce the risk of accidental script execution when browsing or previewing arbitrary outputs under the same origin.


The per-tool **Run** panel also has an **Outputs** toggle that reads the job's `ui_server_run_meta.json`
and lists artifacts with quick inline previews (text + images). For CSV/TSV outputs, the preview opens a
small **table viewer** (with row filtering) and will show a **matrix heatmap** when the file looks like a
numeric matrix (handy for PLV/coherence matrices).

New: for CSV/TSV outputs that look like **time series** tables (a numeric time column like `t_end_sec` plus numeric value columns), the preview adds a **Series** tab with a lightweight line-chart overlay for selected columns (useful for `bandpower_timeseries.csv`).

New: for CSV/TSV outputs that look like per-channel QEEG tables (e.g., `bandpowers.csv` with `channel,delta,theta,alpha,beta,gamma` and optional `*_z` columns, or `bandratios.csv` / other channel-metric tables), the preview adds a **QEEG** tab with:
- quick bar charts (band profile or metric-by-channel)
- a per-band **scalp topomap** view (approximate 10–20 / 10–10 layout)
- a few common ratios (theta/beta, alpha/theta, etc.)
- in Z-score view, an optional **|z| threshold** list of outlier channels

Tip: in the topomap, you can click an electrode to jump the channel picker (bandpowers) or filter the table/plots to that channel (metrics).

Channels that aren’t recognized in the built-in layout are simply skipped for the topomap (they’ll still appear in the table).

Chaining tools: in the Outputs table, use **Use run dir** (for the run folder) or **Use** (for an output file) to set the dashboard’s
global Workspace selection, then click **Inject path** on another tool card.

New: the hero **Selected path** bar includes quick actions (**Browse**, **Preview**, **Copy**, **Clear**) and a small **Suggested / Quick run** row.
- **Suggested** buttons jump to a common next-step tool for the selected input and inject the path into that tool’s args.
- **Quick run** buttons do the same, and also launch the tool when the local server is connected.


The **Outputs** panel also includes a **Download zip** button, which bundles the job’s run folder into a `.../run.zip` download for easy sharing.

New: the Outputs panel also includes a **Notes** button. It opens an in-browser editor for a per-run `note.md` file saved inside the run folder (under `ui_runs/<run>/note.md`). You can use it to annotate runs (QC decisions, parameter tweaks, observations) and flip to a lightweight Markdown **Preview**. Notes are capped at **128KB** and writing is restricted to `ui_runs/` for safety.

Cleanup: the Outputs panel also has a **Delete run** button and the **History** table includes a **Delete** action. These remove a single run folder under `ui_runs/` (helpful when runs accumulate on disk). For safety, deletion is restricted to directories under `ui_runs/` and is blocked while a job is still **running/queued**.

For safety, the server enforces size limits (currently: **25 MiB per file** and **80 MiB total**); if anything is skipped, the zip includes a `_ZIP_NOTICE.txt` file listing what was omitted.

The per-tool **Tail log** panel will **live-tail while open** (incremental polling) so you can watch
stdout/stderr update during long-running runs. The **Refresh** button hard-resets the tail view.

Windows note: the server launches tools via the Win32 process APIs and redirects stdout/stderr into the per-run `run.log`. The UI 'Kill' button force-terminates the process (best-effort).

The server prints a random API token on startup. The dashboard fetches it automatically, but if you
call the API yourself (e.g. with `curl`), you must include it in the `X-QEEG-Token` header.

Convenience: any arguments you type into the UI can use `{{RUN_DIR}}` (relative) or `{{RUN_DIR_ABS}}`
(absolute) placeholders, which expand to the per-job run folder created by the server. This makes it
easy to route `--outdir` under the run folder.

The dashboard also includes a small **Workspace browser** (when served via `qeeg_ui_server_cli`) so you can
browse files *and folders* under `--root` and quickly select a path for tool runs. The browser supports
breadcrumb navigation, optional **hidden files** (dotfiles) visibility, server-side sorting (name / modified / size),
and quick inline **Preview** for common text/image file types.

New: the Workspace browser also includes a few basic **file actions** (when served via `qeeg_ui_server_cli`):
- **New folder**: creates a directory in the current folder.
- **Rename**: renames a file/folder within its parent directory.
- **Trash**: moves a file/folder into `--root/.qeeg_trash/` with a timestamp prefix (a safer alternative to permanent delete).

New: the Workspace browser also supports **Upload** (and drag-and-drop):
- Click **Upload** to select one or more files to upload into the current directory.
- Or drag files from your file explorer onto the Workspace panel and drop to upload.

If a destination file already exists, the UI will prompt to **Overwrite** (otherwise it skips that file).

Uploads are streamed to disk by the local server via `POST /api/fs_upload` (no multipart parsing) and are
currently limited to **1 GiB per file** by default (see `kMaxUploadBytes` in `src/ui_server_cli.cpp`).

All operations are restricted to paths under `--root` (absolute paths and `..` traversal are rejected).

New: the Workspace browser includes **Find** (recursive search):
- Enter a substring or glob pattern (e.g., `*.edf`, `run_meta.json`).
- Choose **type** (files / dirs / any) and a max **depth** to keep searches bounded.
- Results show quick actions (**Select**, **Preview**, **Open**, and **Reveal** to jump to the containing folder).

This is powered by `POST /api/find` on the local server (still restricted to `--root`).

New: Workspace **Back / Forward** navigation.
- Use the **← / →** buttons next to **Up** to jump between recently visited folders.
- The command palette also shows **Workspace: Back/Forward** entries when available.

New: **Reveal highlight**.
- Actions like **Selection → Browse** and **Find → Reveal** open the containing folder and momentarily highlight the target row so you can spot it immediately.

New: **Drag a path into tool args** (better integration between Workspace ↔ Tools).
- Drag any row from the Workspace (or Find results) and drop it onto a tool’s **args** field.
- The UI will set that path as your current **Selection** and inject it into the args using the tool’s chosen **Inject flag**.

Quality-of-life: the UI now shows small **toast** confirmations for common copy/inject actions (e.g., Copy path / Copy full command).

New: **Command palette**. Press **Ctrl+K** (or click **Palette (Ctrl+K)** in the hero card) to open a quick-search palette. It helps you jump to tools and trigger common UI actions (Selection copy/preview/browse/clear, Workspace root/ui_runs/trash, upload/new folder/refresh, Runs Session/History, toggle animations, toggle dotfiles) without scrolling.

New: **Mobile navigation drawer**. On narrow screens, the left tool list collapses into an off-canvas sidebar. Use **Menu** in the hero card to open it; tap outside (or press **Esc**) to close. Selecting a tool auto-closes the drawer.

New: **Live run badges**. When served via `qeeg_ui_server_cli`, tools with active jobs (running / queued / stopping) show a small live status badge in:
- the tool card header
- the left navigation (including the mobile drawer)
- the command palette (as compact `R#/Q#/S#` hints)

New: **Browse run folders in Workspace**. In the Runs (Session/History) tables and in the per-run Outputs panel, use **Browse** to open the run directory inside the Workspace browser, so you can preview/copy outputs without leaving the page.


On each tool card, use the **Inject flag** dropdown next to **Inject path** to choose *which* argument flag should
receive the currently-selected file/folder (defaults try to pick a sensible input flag like `--input` or a dataset/root
flag like `--dataset`/`--bids-root`). This makes it easier to drive tools with multiple path-style arguments (for example,
`qeeg_export_derivatives_cli --map-outdir ... --qc-outdir ...`).

New: the Run panel includes a **Flags** button (when help embedding is enabled). It parses the tool’s embedded `--help` output into a searchable table so you can click **Insert** to add a flag to the args field, or **Use selection** (for path-style flags) to immediately set the flag value to the Workspace-selected file/folder.



New: **Presets** (args templates). Each tool card has a Presets dropdown with **Save / Delete / Reset**, plus **Export / Import** buttons:
- When the UI is served via `qeeg_ui_server_cli`, presets are persisted under `--root` in a `qeeg_ui_presets.json` file, so they survive refreshes and work across browsers/machines.
- If the server isn't running, presets fall back to browser `localStorage` (and will auto-migrate into the shared store once the server is available).

New: **Batch runner** (multi-input queue helper). When the dashboard is served via `qeeg_ui_server_cli`, each tool card includes a **Batch** button that lets you queue up many runs at once.

- The batch modal lists **files in the directory** derived from the current Workspace selection (if you selected a file, it uses that file’s parent directory; if you selected a folder, it uses that folder).
- Your tool’s args field is used as the **args template**. You can use placeholders:
  - `{input}` → the selected file path (auto-quoted if needed)
  - `{name}` → file name
  - `{stem}` → file name without extension
  - `{index}` → 1-based index in the batch
- If you *don’t* include `{input}` in the template, the runner will inject the file using the chosen **Inject flag** (e.g., `--input`).
- Jobs are submitted one-by-one and respect the server’s `--max-parallel` setting (extra jobs will sit in the **queued** state).

This is handy for processing a folder of EDF/BDF/CSV files with consistent parameters without copy/pasting commands.


#### Procedural animations (optional)

The dashboard includes lightweight procedural canvas visuals:
- a subtle particle "neural field" background
- a selectable synthetic preview panel in the hero card:
  - EEG (multi-band waves)
  - Topomap (synthetic scalp field)
  - Spectrogram (synthetic waterfall)
  - Flow field (synthetic curl / vector field)

These are **decorative only** (they do not use recording data). Use the **Animations** toggle in the hero panel to stop motion; the canvases will still render a static frame.

You can export the synthetic panel for quick sharing/debugging:
- **Snapshot PNG** downloads the currently selected synthetic canvas as a PNG
- **Record 5s** records a short WebM clip (via the browser's MediaRecorder / canvas capture APIs)


Accessibility: the UI respects your OS/browser `prefers-reduced-motion` preference and defaults animations **off** when that setting is enabled.

For convenience, the "Run" panel defaults rewrite common output flags (like `--outdir`, `--out-dir`, `--out`, `--output`,
and `--events-out`) to land under `{{RUN_DIR}}` so each UI-launched run stays self-contained. You can also save/load/delete
per-tool argument presets; when served via `qeeg_ui_server_cli` they persist under `--root` in `qeeg_ui_presets.json`, otherwise
they are stored in the browser's `localStorage`.

### 0) Inspect a recording (quick summary)

```bash
# Print sampling rate / sizes + a basic data scan (non-finite counts + global min/max)
./build/qeeg_info_cli --input path/to/recording.edf

# Print channel list + first few EDF+/BDF+ annotation events
./build/qeeg_info_cli --input path/to/recording.edf --channels --events

# Machine-readable JSON (useful for scripts)
./build/qeeg_info_cli --input path/to/recording.edf --json --channels --events
```

Tip: for very large files, you can skip scanning samples with `--no-scan`, or request
per-channel stats with `--per-channel` (limited by `--max-channels`).

### 0c) Quick trace plot (SVG)

If you want a fast, shareable view of raw (or lightly filtered) EEG, you can render a
stacked time-series trace plot to a single SVG file:

```bash
# Plot a 10s window (default) from the first 8 channels
./build/qeeg_trace_plot_cli --input path/to/recording.edf --outdir out_traces

# Pick channels explicitly (names are case-insensitive; indices also accepted)
./build/qeeg_trace_plot_cli --input path/to/recording.edf --outdir out_traces --channels Cz,Fz,Pz

# Plot a specific time window and add basic filtering
./build/qeeg_trace_plot_cli --input path/to/recording.edf --outdir out_traces \
  --start 30 --duration 15 --notch 50 --bandpass 1 40 --zero-phase

# Auto-scale each channel row (useful when channels have very different amplitudes)
./build/qeeg_trace_plot_cli --input path/to/recording.edf --outdir out_traces --autoscale
```

The SVG includes optional vertical markers for EDF+/BDF+ annotations/events (disable with
`--no-events`).

Outputs (under `--outdir`):
- `traces.svg` (or the filename passed via `--output`)
- `trace_plot_meta.txt` — parameters used (for reproducibility)
- `trace_plot_run_meta.json` — manifest used by `qeeg_ui_cli` to auto-link run artifacts

### 0b) Check 50/60 Hz line-noise (choose a notch frequency)

If you don't know whether your recording is contaminated by **50 Hz** or **60 Hz** power-line
interference (common when receiving exports from other labs/regions), you can run:

```bash
# Human-readable summary to stdout
./build/qeeg_quality_cli --input path/to/recording.edf

# Write a small report bundle under --outdir (useful for qeeg_ui_cli auto-linking)
./build/qeeg_quality_cli --input path/to/recording.edf --outdir out_quality

# JSON to stdout (script-friendly)
./build/qeeg_quality_cli --input path/to/recording.edf --json

# JSON to stdout + also write the report bundle
./build/qeeg_quality_cli --input path/to/recording.edf --json --outdir out_quality
```

Outputs (when `--outdir` is set):
- `quality_report.json` — machine-readable report (includes per-channel 50/60 ratios)
- `quality_summary.txt` — human summary
- `line_noise_per_channel.csv` — per-channel candidate ratios + PSD density means
- `quality_run_meta.json` — manifest used by `qeeg_ui_cli` to auto-link run artifacts

Then pass `--notch 50` or `--notch 60` to the analysis CLIs as needed.

### 0d) Spectral summary features (entropy, SEF95, peak frequency)

If you want quick per-channel **spectral summary** features derived from a Welch PSD
(useful for simple QC, vigilance proxies, or lightweight feature extraction), run:

```bash
./build/qeeg_spectral_features_cli --input path/to/recording.edf --outdir out_spec

# Restrict the analysis range and choose a different spectral edge fraction
./build/qeeg_spectral_features_cli --input path/to/recording.edf --outdir out_spec \
  --range 1 40 --edge 0.95
```

Outputs (under `--outdir`):
- `spectral_features.csv`
- `spectral_features.json` (column descriptions)
- `spectral_features_run_meta.json` (manifest used by `qeeg_ui_cli`)

### 1) Run a demo (synthetic data)

```bash
./build/qeeg_map_cli --demo --outdir out_demo --fs 250
```

### 2) Run on EDF

```bash
./build/qeeg_map_cli --input path/to/recording.edf --outdir out_edf
```

### 2a) Bandpower table only (no topomaps)

If you only want a **tabular bandpower export** (and optional z-scores) without
rendering any images, you can run:

```bash
./build/qeeg_bandpower_cli --input path/to/recording.edf --outdir out_bp

# Relative + log10 (common for simple normalization)
./build/qeeg_bandpower_cli --input path/to/recording.edf --outdir out_bp_rel --relative --log10

# Append per-band z-score columns from a reference CSV built by qeeg_reference_cli
./build/qeeg_bandpower_cli --input path/to/recording.edf --outdir out_bp_z --reference out_ref/reference.csv

# Also write a sliding-window bandpower time series (one row per update)
./build/qeeg_bandpower_cli --input path/to/recording.edf --outdir out_bp_ts --timeseries --window 2.0 --update 0.25
```

This writes:
- `bandpowers.csv` (wide format: one row per channel, one column per band)
- `bandpowers.json` (column descriptions)
- `bandpower_run_meta.json` (a small manifest consumed by `qeeg_ui_cli`)

Optional (when `--timeseries` is used):
- `bandpower_timeseries.csv` (wide format: one row per update; columns are `<band>_<channel>`)
- `bandpower_timeseries.json` (column descriptions)

### 2a1) Derive neurofeedback band ratios from `bandpowers.csv`

If you want common **band ratio** features (e.g., **theta/beta**), you can derive them
directly from an existing `bandpowers.csv` (from either `qeeg_map_cli` or `qeeg_bandpower_cli`):

```bash
./build/qeeg_bandratios_cli --bandpowers out_bp/bandpowers.csv --outdir out_ratios --ratio theta/beta

# Name the output column explicitly and also export a BIDS-friendly TSV copy
./build/qeeg_bandratios_cli --bandpowers out_bp/bandpowers.csv --outdir out_ratios \
  --ratio tbr=theta/beta --log10 --tsv
```

Outputs (under `--outdir`):
- `bandratios.csv` (one row per channel)
- `bandratios.json` (column descriptions)
- `bandratios.tsv` (optional; written when `--tsv` is used)
- `bandratios_run_meta.json` (manifest used by `qeeg_ui_cli` to auto-link run artifacts)


### 2b) BioTrace+ / NeXus 10 MKII

If you record with Mind Media BioTrace+ (e.g., a NeXus-10 MKII), export sessions to **EDF/EDF+** (recommended) or **BDF/BDF+** and use the exported file as input here.

This project reads EDF/EDF+ and BDF/BDF+. If the export includes peripheral channels at lower sampling rates (skin conductance, respiration, temperature, etc.), the reader will keep all non-annotation channels and resample them to the highest EEG/ExG-like sampling rate (best effort). Voltage-like channels exported in mV or V are converted to microvolts. If you only want EEG/ExG channels, drop peripherals with a channel-map (set `new=DROP`; leaving `new` blank keeps the channel unchanged).

**Notes on BioTrace+ export formats**
- The `.bcd` / `.mbd` formats are BioTrace+/NeXus session containers/backups and are **not supported** here.
  Export to EDF/BDF or ASCII from BioTrace+ first.
- BioTrace+ ASCII exports are often saved as `.txt` / `.tsv` / `.asc`; these are treated like CSV inputs in this project.

**Trigger/event channels (important for some BDF recordings)**

Some recordings store event codes as a dedicated numeric channel (for example `TRIG`, `STI 014`, or a BioSemi-style `Status` channel) rather than an EDF+/BDF+ annotations signal. If no annotations are present, this project will now attempt a conservative auto-detection and convert trigger transitions into `events`.

That means tools like `qeeg_convert_cli --events-out ...` and `qeeg_export_bids_cli` can still emit an events file even when the original recording had no EDF+/BDF+ annotations.

```bash
./build/qeeg_info_cli --input path/to/biotrace_export.edf
./build/qeeg_map_cli  --input path/to/biotrace_export.edf --outdir out_biotrace
```

**Optional: normalize/export to a simple CSV (and rename channels)**

```bash
# Convert an ASCII export (e.g. .txt) to a clean CSV and generate a channel-map template
./build/qeeg_convert_cli --input path/to/session.txt --output session.csv --channel-map-template channel_map.csv

# Edit channel_map.csv (e.g., map ExG1->C3, ExG2->C4; leave `new` blank to keep a channel; set `new=DROP` to remove channels),
# then apply it during conversion (works for EDF/BDF too)
./build/qeeg_convert_cli --input path/to/session.edf --output session_mapped.csv --channel-map channel_map.csv --events-out events.csv
```

**Optional: export to EDF (round-trip friendly)**

```bash
# Export a cleaned-up EDF (e.g., after channel remapping).
# For CSV/ASCII inputs, pass --fs unless there is a time column that can be inferred.
./build/qeeg_export_edf_cli --input session.csv --output session.edf --fs 256 --channel-map channel_map.csv --events-out events.csv

# If you want to avoid EDF padding, you can write a single datarecord:
./build/qeeg_export_edf_cli --input session.edf --output session_one_record.edf --record-duration 0
```

**Optional: export to BDF (.bdf, 24-bit) / BDF+ (with annotations)**

If you want to preserve 24-bit dynamic range (BioSemi-style BDF), you can export any supported input to
`.bdf`. When the input contains events and you do not pass `--plain-bdf`, the tool emits a BDF+ annotation
channel (`BDF Annotations`).

```bash
# Export to BDF (24-bit). Events are embedded as BDF+ annotations by default.
./build/qeeg_export_bdf_cli --input session.edf --output session.bdf --events-out events.csv

# Force classic BDF (no embedded annotations channel):
./build/qeeg_export_bdf_cli --input session.edf --output session_plain.bdf --plain-bdf
```

**Optional: export to BrainVision (.vhdr/.vmrk/.eeg)**

Some EEG tools prefer the BrainVision Core Data Format (a 3-file set: header `.vhdr`, marker `.vmrk`, and binary `.eeg`).
You can export any supported input to BrainVision:

```bash
# Write a BrainVision set: session.vhdr, session.vmrk, session.eeg
./build/qeeg_export_brainvision_cli --input session.edf --output session.vhdr

# INT_16 output (smaller files) with fixed 0.1 uV resolution
./build/qeeg_export_brainvision_cli --input session.edf --output session_int16.vhdr --int16 --int16-resolution 0.1
```

You can also **use a BrainVision header (`.vhdr`) as an input** to any CLI (the reader will automatically load the referenced `.eeg` and `.vmrk` files):

```bash
./build/qeeg_info_cli --input session.vhdr --channels --events
./build/qeeg_map_cli  --input session.vhdr --outdir out_bv
```



**Optional: export to a BIDS EEG folder layout**

If you want to organize exports for other tools that expect the
[Brain Imaging Data Structure (BIDS)](https://bids-specification.readthedocs.io/) EEG layout, you can use:

```bash
# Writes: <out-dir>/sub-01/[ses-01/]eeg/sub-01[_ses-01]_task-rest[_acq-...][_run-...]_eeg.edf
# plus: *_eeg.json, *_channels.tsv, and (if events exist) *_events.tsv/json.
# Also creates <out-dir>/dataset_description.json if missing.
./build/qeeg_export_bids_cli --input session.edf --out-dir my_bids --sub 01 --task rest --ses 01   --format edf --eeg-reference Cz

# BrainVision output variant (.vhdr/.vmrk/.eeg)
./build/qeeg_export_bids_cli --input session.edf --out-dir my_bids --sub 01 --task rest --ses 01   --format brainvision --eeg-reference Cz
```

If your recording has trigger/annotation codes, you can optionally add extra columns to `*_events.tsv`:

```bash
# Add:
#  - sample: event onset in samples (derived from onset * SamplingFrequency)
#  - value: integer event code when the annotation text is numeric
./build/qeeg_export_bids_cli --input session.edf --out-dir my_bids --sub 01 --task rest --ses 01 \
  --format edf --eeg-reference Cz --events-sample --events-sample-base 0 --events-value
```

(When you add extra columns, they should be described in the accompanying `*_events.json`; this tool writes that description automatically.)

**Optional: scan / sanity-check a BIDS EEG dataset**

If you're handed a BIDS dataset (or you've just exported one) and want a quick, dependency-light
"what's in here?" summary, you can run:

```bash
./build/qeeg_bids_scan_cli --dataset my_bids --outdir out_bids_scan
```

This writes:
- `bids_index.json` — machine-readable list of discovered recordings + sidecar presence
- `bids_index.csv` — quick spreadsheet-friendly version
- `bids_scan_report.txt` — human-readable warnings/errors

It is **not** a full validator, but it does a few practical checks:
- finds the dataset root by locating `dataset_description.json`
- indexes EEG recordings in EDF/BDF/BrainVision format
- checks for the presence of `*_eeg.json` (required) and `*_channels.tsv` (recommended)
- best-effort checks that `*_eeg.json` contains required EEG keys (string search)
- warns if a BrainVision `.vhdr` file is missing its companion `.vmrk`/`.eeg`
- flags the common "electrodes.tsv present but coordsystem.json missing" issue

For CI or scripting, you can treat warnings as failures:

```bash
./build/qeeg_bids_scan_cli --dataset my_bids --strict
```

### 2c) Preprocess (CAR / notch / bandpass) and export cleaned EDF/BDF/BrainVision or CSV

If you want to apply quick, dependency-light preprocessing before running qEEG features (or before
sharing data with other tools), use `qeeg_preprocess_cli`.

It supports writing:
- **EDF/EDF+** (`.edf`)
- **BDF/BDF+** (`.bdf`) — 24-bit samples (often a better "lossless" export target than EDF)
- **BrainVision** (`.vhdr/.vmrk/.eeg`) — widely supported by EEG toolchains
- **CSV** (`.csv`)

It can also **auto-detect 50/60 Hz line-noise** and choose a notch frequency when you don’t know
the export settings.

```bash
# Auto-notch + simple EEG bandpass, then write EDF+ (events preserved)
./build/qeeg_preprocess_cli --input session.edf --output session_clean.edf \
  --auto-notch --bandpass 1 40 --average-reference --zero-phase \
  --events-out session_clean_events.csv

# Or write a cleaned CSV instead
./build/qeeg_preprocess_cli --input session.edf --output session_clean.csv --auto-notch --bandpass 1 40

# Or write a cleaned BDF+ (24-bit)
./build/qeeg_preprocess_cli --input session.edf --output session_clean.bdf --auto-notch --bandpass 1 40

# Or write a cleaned BrainVision set (.vhdr/.vmrk/.eeg)
./build/qeeg_preprocess_cli --input session.edf --output session_clean.vhdr --auto-notch --bandpass 1 40
```

### 2d) Channel QC: find bad channels and optionally drop/interpolate

If your exports include disconnected electrodes (flat lines) or very noisy channels, you can run:

```bash
./build/qeeg_channel_qc_cli --input session.edf --outdir out_qc
```

This writes:
- `channel_qc.csv` — per-channel summary metrics + flags
- `bad_channels.txt` — one channel name per line
- `qc_summary.txt` — parameters + actions taken

**Drop** bad channels (no montage required):

```bash
./build/qeeg_channel_qc_cli --input session.edf --outdir out_qc_drop \
  --drop-bad --output session_dropbad.edf
```

**Interpolate** bad channels using Perrin-style **spherical spline** interpolation.
This requires electrode positions (a montage). If your channels are standard 10-20 names
(or you've remapped them using `--channel-map`), the builtin 19ch montage is used by default.

```bash
./build/qeeg_channel_qc_cli --input session.edf --outdir out_qc_interp \
  --interpolate --output session_interpolated.edf

# Or provide your own montage CSV (name,x,y):
./build/qeeg_channel_qc_cli --input session.edf --outdir out_qc_interp \
  --montage my_montage.csv --interpolate --output session_interpolated.edf
```

Tip: If you want this to behave well for BioTrace+/NeXus exports, first drop non-EEG/ExG
channels with a channel-map (set new=DROP). Then run interpolation only on your EEG montage.

### 3) Run on CSV

CSV format:
- first row: channel names (or: `time,<ch1>,<ch2>,...`)
- each following row: one sample per column
- if the first column is `time`/`time_ms` (seconds or milliseconds), `--fs` can be omitted
- if a column is named like `marker`, `event`, or `trigger`, it is treated as an **event/marker stream**
  and converted into `EEGRecording::events` (useful for `qeeg_info_cli --events`, `qeeg_epoch_cli`,
  and `qeeg_convert_cli --events-out`)
- commas inside quoted fields are supported (e.g., `time_ms;"Ch,1,2";"Ch,3,4"`)
- comma/semicolon/tab delimiters are auto-detected from the header row
- for semicolon/tab-delimited exports, European numeric formats like `1,23` and `1.234,56` are supported

```bash
./build/qeeg_map_cli --input examples/sample_data_sine.csv --fs 250 --outdir out_csv
```

### 4) Use a custom montage

Montage CSV format: `name,x,y` where `x,y` are within the unit circle.

Notes:
- comma/semicolon/tab delimiters are auto-detected
- quoted fields are supported (e.g. `"Ch,1";0.1;0.2`)

```bash
./build/qeeg_map_cli --input path/to/recording.edf --montage examples/sample_montage_1020_19.csv --outdir out_custom
```

### 5) Provide a reference (optional) to compute z-scores

Reference CSV format (one row per channel-band):
`channel,band,mean,std`

Notes:
- comma/semicolon/tab delimiters are supported
- quoted fields are supported
- UTF-8 BOM at the start of the file is tolerated

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

When using a reference built with `--log10`, compute maps on the same scale.
You can either pass `--log10` to `qeeg_map_cli`, or rely on the reference
metadata (if present) to auto-enable log10 mode when `--reference` is used.

Optional: compute stats over **relative bandpower** (band / total power within
a frequency range). This yields dimensionless fractions and can help reduce
between-subject amplitude differences:

```bash
./build/qeeg_reference_cli --list recordings.txt --outdir out_ref --relative --relative-range 1 45
```

When using a reference built with `--relative`, compute maps on the same scale.
You can either pass `--relative` (and the same `--relative-range`) to
`qeeg_map_cli`, or rely on the reference metadata (if present) to auto-enable
relative mode when `--reference` is used.

You can combine `--relative` and `--log10` (log10 is applied *after* the
relative normalization).

Optional: build a more **robust** reference (median + MAD-derived scale) instead
of mean/std (still written as `mean,std` columns for compatibility with
`--reference`):

```bash
./build/qeeg_reference_cli --list recordings.txt --outdir out_ref --robust
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
- setting an initial threshold from a baseline period (or explicitly via `--threshold`)
  - by default, the baseline threshold is an empirical quantile chosen to approximately
    match `--target-rate` at initialization (above => q=1-R, below => q=R). Use
    `--baseline-quantile 0.5` to force median behavior.
- optionally rewarding when the metric is **above** or **below** the threshold (`--reward-direction`)
- optionally adapting the threshold to maintain a target reward rate


Adaptive threshold modes:
- `--adapt-mode exp` (default): multiplicative update driven by reward-rate error.
- `--adapt-mode quantile`: tracks a running empirical quantile of recent metric values so the reward fraction approaches `--target-rate`.

Useful controls:
- `--adapt-window S` (quantile mode): rolling window length in seconds (0 => full history).
- `--adapt-min-samples N` (quantile mode): minimum metric samples before adapting.
- `--adapt-interval S`: only update threshold every S seconds (0 => every update frame).

Example (quantile mode):

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf_q --metric alpha:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
  --adapt-mode quantile --adapt-window 30 --adapt-min-samples 20
```

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

Optional: pace **offline** playback so updates arrive in real time (useful when driving an
external UI via OSC, or when watching the built-in BioTrace-style UI):

```bash
# Real-time pacing (1x)
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 --realtime

# Faster-than-real-time pacing (e.g., 2x)
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 --speed 2.0
```

Optional: smooth the metric with an **exponential moving average** before thresholding/feedback:

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha/beta:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 --metric-smooth 0.5
```

Optional: scale bandpower values (bandpower/ratio metrics only)

- `--relative` uses relative bandpower: `band_power / total_power`
- `--relative-range LO HI` sets the total-power integration range for `--relative`
- `--log10` applies `log10(power)` after any optional relative normalization

Example:

```bash
./build/qeeg_nf_cli --input path/to/recording.bdf --outdir out_nf --metric alpha:Pz \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 \
  --relative --relative-range 1 45 --log10
```

Note: when `--log10` is used with a ratio metric like `alpha/beta:Pz`, the CLI reports
`log10(alpha/beta)` (difference of log10 bandpowers).

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
- `${prefix}/reward_direction` (string) — once at startup (`above` or `below`)
- `${prefix}/threshold_init` (float32) — once at startup if `--threshold` is provided
- `${prefix}/state` (float32 t_end_sec, float32 metric, float32 threshold, int32 reward, float32 reward_rate, int32 have_threshold) — every update

If you use `--osc-mode split`, `qeeg_nf_cli` sends multiple addresses per update:
`${prefix}/time`, `${prefix}/metric`, `${prefix}/threshold`, `${prefix}/reward`, `${prefix}/reward_rate`, `${prefix}/have_threshold`.

If you use `--osc-mode bundle`, the split-style messages are sent inside **one OSC bundle** per update
(still over UDP).

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

Coherence neurofeedback examples:

```bash
# Magnitude-squared coherence (MSC)
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_coh --metric coh:alpha:F3:F4 \
  --window 2.0 --update 0.25 --baseline 10 --target-rate 0.6 --export-coherence

# Imaginary coherency (abs(imag(coherency)))
./build/qeeg_nf_cli --input path/to/recording.edf --outdir out_nf_imcoh --metric imcoh:alpha:F3:F4 \
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
- `nf_summary.json` — small summary of the run (thresholds, reward stats, wall time)
- (optional) `bandpower_timeseries.csv` — all bands/channels per update (bandpower mode)
- (optional) `coherence_timeseries.csv` or `imcoh_timeseries.csv` — all bands for the chosen pair per update (coherence/imcoh mode)
- (optional) `artifact_gate_timeseries.csv` — artifact gate aligned to NF updates (when `--export-artifacts`)
- (optional) `nf_reward.wav` — a simple reward-tone audio track (mono PCM16). Use `--audio-wav nf_reward.wav`.

Note: when `--metric-smooth` is enabled, `nf_feedback.csv` appends a `metric_raw` column for debugging. When `--adapt-mode quantile` is used, it also appends a `threshold_desired` column (the running quantile target used for threshold tracking).

### 7) Connectivity: coherence matrix / pair report

Compute an **alpha-band** coherence matrix for all channel pairs:

```bash
./build/qeeg_coherence_cli --input path/to/recording.edf --outdir out_coh --band alpha
```

Compute the **absolute imaginary part of coherency** ("imcoh") to reduce
spurious zero-lag coupling (often attributed to volume conduction / field
spread):

```bash
./build/qeeg_coherence_cli --input path/to/recording.edf --outdir out_imcoh --band alpha --measure imcoh
```

Compute **one pair** (and optionally export the full spectrum):

```bash
./build/qeeg_coherence_cli --input path/to/recording.edf --outdir out_pair --band alpha --pair F3:F4 --export-spectrum
```

### 7b) Connectivity: phase-based (PLV / PLI / wPLI)

Compute phase-based connectivity for all channel pairs.

Available measures:
- `plv` (default): Phase Locking Value (sensitive to zero-lag coupling)
- `pli`: Phase Lag Index (counts consistent non-zero lag)
- `wpli`: Weighted Phase Lag Index (PLI weighted by the magnitude of the imaginary component; often more robust to noise)
- `wpli2_debiased`: Debiased estimator of **squared** wPLI (wPLI^2), which can reduce small-sample bias

Compute a **PLV** matrix for all channel pairs:

```bash
./build/qeeg_plv_cli --input path/to/recording.edf --outdir out_plv --band alpha
```

Compute a **wPLI** matrix (often used to reduce spurious zero-lag coupling):

```bash
./build/qeeg_plv_cli --input path/to/recording.edf --outdir out_wpli --band alpha --measure wpli
```

Compute a **debiased wPLI^2** matrix:

```bash
./build/qeeg_plv_cli --input path/to/recording.edf --outdir out_wpli2_debiased --band alpha --measure wpli2_debiased
```

Compute **one pair**:

```bash
./build/qeeg_plv_cli --input path/to/recording.edf --outdir out_plv_pair --band alpha --pair F3:F4
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
- `spectrogram_<channel>.csv` — dB matrix (wide by default; use `--csv-long` for long format; omitted with `--no-csv`)
- `spectrogram_<channel>_meta.txt` — parameters used (for reproducibility)
- `spectrogram_run_meta.json` — manifest used by `qeeg_ui_cli` to auto-link run artifacts

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

# Or use a real regular expression (ECMAScript):
./build/qeeg_epoch_cli --input path/to/recording.edf --outdir out_epochs --event-regex "Stim.*"


# Use a fixed 1.0s window starting 0.2s after each matching event
./build/qeeg_epoch_cli --input path/to/recording.edf --outdir out_epochs \
  --event-glob "*Stim*" --offset 0.2 --window 1.0

# Optional: baseline-normalize epoch bandpowers relative to a pre-epoch baseline.
# The baseline window ends at the epoch start; add --baseline-gap to leave a small gap.
# Available modes:
#  - ratio    : epoch / baseline
#  - rel      : (epoch - baseline) / baseline
#  - logratio : log10(epoch / baseline)
#  - db       : 10*log10(epoch / baseline)
./build/qeeg_epoch_cli --input path/to/recording.edf --outdir out_epochs_norm \
  --event-glob "*Stim*" --offset 0.2 --window 1.0 --baseline 1.0 --baseline-gap 0.1 --baseline-mode db
```

Outputs:
- `events.csv` — exported event list (includes a stable `event_id` column)
- `events_table.csv` — qeeg event table (onset_sec,duration_sec,text)
- `events_table.tsv` — BIDS-style events table (onset,duration,trial_type)
- `epoch_bandpowers.csv` — long-format table of (event × channel × band)
- `epoch_bandpowers_summary.csv` — mean power per channel/band across processed epochs
- (optional) `epoch_bandpowers_norm.csv` — baseline-normalized values (when `--baseline` is used)
- (optional) `epoch_bandpowers_norm_summary.csv` — mean baseline-normalized values (when `--baseline` is used)
- `epoch_run_meta.json` — manifest used by `qeeg_ui_cli` to auto-link run artifacts

### 11) Artifact window detection (first pass)

Flag likely artifact-contaminated windows using simple **robust** time-domain features
(peak-to-peak amplitude, RMS, and excess kurtosis) computed on sliding windows.

This produces:
- `artifact_windows.csv` — one row per time window (including list of bad channels)
- `artifact_channels.csv` — long-format per-window × per-channel feature table
- `artifact_channel_summary.csv` — per-channel fraction of windows flagged
- `artifact_segments.csv` — merged contiguous bad-window segments
- `artifact_summary.txt` — parameters + summary stats
- (optional) `artifact_events.tsv` / `artifact_events.json` — BIDS-style events export for artifact segments (when `--export-bids-events`)
- `artifact_run_meta.json` — manifest used by `qeeg_ui_cli` to auto-link run artifacts

Example:

```bash
./build/qeeg_artifacts_cli --input path/to/recording.edf --outdir out_artifacts \
  --window 1.0 --step 0.5 --baseline 10 --ptp-z 6 --rms-z 6 --kurtosis-z 6

# Also write BIDS-style events.tsv + events.json describing merged artifact segments
./build/qeeg_artifacts_cli --input path/to/recording.edf --outdir out_artifacts \
  --window 1.0 --step 0.5 --baseline 10 --export-bids-events

# Optional light preprocessing before scoring
./build/qeeg_artifacts_cli --input path/to/recording.edf --outdir out_artifacts_filtered \
  --window 1.0 --step 0.5 --baseline 10 --average-reference --notch 50 --bandpass 1 45 --zero-phase
```

### 11b) Extract clean segments from artifact detection

If you want to **extract contiguous “good” segments** (the complement of the detected bad windows), use:

```bash
# Generate bad_segments.csv + good_segments.csv + clean_summary.txt
./build/qeeg_clean_cli --input path/to/recording.edf --outdir out_clean \
  --window 1.0 --step 0.5 --baseline 10 --ptp-z 6 --rms-z 6 --kurtosis-z 6 \
  --merge-gap 0.0 --pad 0.25 --min-good 2.0

# Optional: export each good segment to its own CSV file
./build/qeeg_clean_cli --input path/to/recording.edf --outdir out_clean \
  --pad 0.25 --min-good 2.0 --export-csv

# Optional: export each good segment to EDF (still writes events sidecar CSV per segment)
./build/qeeg_clean_cli --input path/to/recording.edf --outdir out_clean \
  --pad 0.25 --min-good 2.0 --export-edf --record-duration 1.0

# You can also apply light preprocessing before scoring
./build/qeeg_clean_cli --input path/to/recording.edf --outdir out_clean_filtered \
  --average-reference --notch 50 --bandpass 1 45 --zero-phase \
  --pad 0.25 --min-good 2.0 --export-csv
```

Notes:
- `--pad` expands bad segments on each side (useful to avoid edge contamination).
- When exporting segments, events are filtered to those overlapping the segment and their onset times
  are shifted so each segment starts at `t=0`.

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

Tip: you can also point `--bands` to a text file by prefixing the path with `@`
(one band per line or comma-separated), e.g. `--bands @out_iaf/iaf_band_spec.txt`.

```bash
# Example: compute IAF, then run NF using IAF-relative alpha band
./build/qeeg_iaf_cli --input recording.edf --outdir out_iaf

./build/qeeg_nf_cli --input recording.edf --outdir out_nf \
  --bands @out_iaf/iaf_band_spec.txt --metric alpha:Pz
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
- `microstate_transition_probs.csv` — segment-to-segment transition **probabilities** (row-normalized from the count matrix)
- `microstate_state_stats.csv` — per-state coverage, mean duration, and occurrence rate (CSV)
- `microstate_segments.csv` — optional segment list (enabled with `--export-segments`)
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

## Citation

If you use this software in academic work, you can cite it using the metadata in `CITATION.cff` (supported by GitHub and many reference managers).

## License

MIT (see `LICENSE`).
