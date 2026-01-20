#include "qeeg/bids.hpp"
#include "qeeg/channel_qc_io.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/run_meta.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string bids_root;
  // Optional convenience: parse entities and infer bids_root from an existing
  // BIDS filename (e.g. sub-01_task-rest_eeg.edf) that lives inside the dataset.
  std::string bids_file;
  std::string pipeline{"qeeg"};

  BidsEntities ent;

  std::string map_outdir;

  // Outputs from qeeg_topomap_cli (topomap_*.bmp, topomap_report.html, ...).
  std::string topomap_outdir;

  // Outputs from qeeg_region_summary_cli (region_summary.csv, region_report.html, ...).
  std::string region_summary_outdir;

  // Outputs from qeeg_connectivity_map_cli (connectivity_map.svg, connectivity_report.html, ...).
  std::string connectivity_map_outdir;

  // Outputs from qeeg_bandpower_cli (bandpowers.csv + JSON sidecar)
  std::string bandpower_outdir;

  // Outputs from qeeg_bandratios_cli (bandratios.csv + JSON sidecar)
  std::string bandratios_outdir;

  // Outputs from qeeg_spectral_features_cli (spectral_features.csv + JSON sidecar)
  std::string spectral_features_outdir;

  // Optional connectivity outputs.
  std::string coherence_outdir;
  std::string plv_outdir;
  std::string pac_outdir;

  std::string qc_outdir;

  // Optional: outputs from qeeg_artifacts_cli.
  std::string artifacts_outdir;

  std::string nf_outdir;
  // Outputs from qeeg_epoch_cli (epoch_bandpowers.csv, events_table.tsv, ...)
  std::string epoch_outdir;
  std::string iaf_outdir;

  // Optional: outputs from qeeg_microstates_cli.
  std::string microstates_outdir;


  std::string generated_by_version;
  std::string generated_by_code_url{"https://github.com/masterblaster1999/qeeg-neurofeedback-opensoftware"};
  std::string source_dataset_url;

  bool overwrite{false};
};

static void print_help() {
  std::cout
      << "qeeg_export_derivatives_cli\n\n"
      << "Copy qeeg tool outputs into a BIDS Derivatives folder layout.\n\n"
      << "This tool is designed to integrate outputs from:\n"
      << "  - qeeg_map_cli (bandpowers.csv, topomaps, report.html, ...)\n"
      << "  - qeeg_topomap_cli (topomap_*.bmp, topomap_report.html, ...)\n"
      << "  - qeeg_region_summary_cli (region_summary.csv, region_report.html, ...)\n"
      << "  - qeeg_connectivity_map_cli (connectivity_map.svg, connectivity_report.html, ...)\n"
      << "  - qeeg_bandpower_cli (bandpowers.csv, bandpowers.json, ...)\n"
      << "  - qeeg_bandratios_cli (bandratios.csv, bandratios.json, ...)\n"
      << "  - qeeg_spectral_features_cli (spectral_features.csv, spectral_features.json, ...)\n"
      << "  - qeeg_coherence_cli (coherence matrices / edge lists, ...)\n"
      << "  - qeeg_plv_cli (PLV/PLI/wPLI matrices / edge lists, ...)\n"
      << "  - qeeg_pac_cli (PAC time series / phase distributions, ...)\n"
      << "  - qeeg_channel_qc_cli (channel_qc.csv, bad_channels.txt, ...)\n"
      << "  - qeeg_artifacts_cli (artifact_windows.csv, artifact_segments.csv, artifact_events.tsv, ...)\n"
      << "  - qeeg_nf_cli (nf_run_meta.json, nf_derived_events.tsv, bandpower_timeseries.csv, ...)\n"
      << "  - qeeg_epoch_cli (epoch_bandpowers.csv, events_table.tsv, ...)\n\n"
      << "  - qeeg_iaf_cli (iaf_summary.txt, iaf_band_spec.txt, topomap_iaf.bmp, ...)\n"
      << "  - qeeg_microstates_cli (templates, time series, topomap_microstate_*.bmp, ...)\n\n"
      << "When channel QC outputs are provided, it also emits a BIDS-style\n"
      << "channels.tsv derivative (desc-qeegqc_channels.tsv) with QC status\n"
      << "labels (good/bad) and optional status_description.\n\n"
      << "It writes to: <bids-root>/derivatives/<pipeline>/sub-<sub>/[ses-<ses>/]eeg/\n"
      << "and ensures derivatives/<pipeline>/dataset_description.json exists with DatasetType=derivative and GeneratedBy.\n\n"
      << "Usage:\n"
      << "  qeeg_export_derivatives_cli --bids-root <dir> --sub <label> --task <label> [options]\n"
      << "  qeeg_export_derivatives_cli --bids-file <path> [--bids-root <dir>] [options]\n\n"
      << "Required (choose one path):\n"
      << "  --bids-root <dir>           Existing BIDS dataset root (folder containing dataset_description.json).\n"
      << "  --bids-file <path>          Existing BIDS filename used to infer entities and (if needed) bids_root.\n\n"
      << "Required entities (if --bids-file is NOT used):\n"
      << "  --sub <label>               Subject label (alphanumeric).\n"
      << "  --task <label>              Task label (alphanumeric).\n\n"
      << "Optional entities:\n"
      << "  --ses <label>               Session label (alphanumeric).\n"
      << "  --acq <label>               Acquisition label (alphanumeric).\n"
      << "  --run <index>               Run index label (alphanumeric; typically digits).\n\n"
      << "Inputs (tool output folders):\n"
      << "  --map-outdir <dir>          Output folder from qeeg_map_cli.\n"
      << "  --topomap-outdir <dir>      Output folder from qeeg_topomap_cli.\n"
      << "  --region-summary-outdir <dir> Output folder from qeeg_region_summary_cli.\n"
      << "  --connectivity-map-outdir <dir> Output folder from qeeg_connectivity_map_cli.\n"
      << "  --bandpower-outdir <dir>    Output folder from qeeg_bandpower_cli.\n"
      << "  --bandratios-outdir <dir>   Output folder from qeeg_bandratios_cli.\n"
      << "  --spectral-features-outdir <dir> Output folder from qeeg_spectral_features_cli.\n"
      << "  --coherence-outdir <dir>    Output folder from qeeg_coherence_cli.\n"
      << "  --plv-outdir <dir>          Output folder from qeeg_plv_cli.\n"
      << "  --pac-outdir <dir>          Output folder from qeeg_pac_cli.\n"
      << "  --qc-outdir <dir>           Output folder from qeeg_channel_qc_cli.\n"
      << "  --artifacts-outdir <dir>    Output folder from qeeg_artifacts_cli.\n"
      << "  --nf-outdir <dir>           Output folder from qeeg_nf_cli.\n"
      << "  --epoch-outdir <dir>        Output folder from qeeg_epoch_cli.\n\n"
      << "  --iaf-outdir <dir>          Output folder from qeeg_iaf_cli.\n\n"
      << "  --microstates-outdir <dir>  Output folder from qeeg_microstates_cli.\n\n"
      << "Derivatives metadata:\n"
      << "  --pipeline <name>           Derivatives pipeline folder name (default: qeeg).\n"
      << "  --generated-by-version <v>  Version string written into GeneratedBy[0].Version.\n"
      << "  --generated-by-code-url <u> CodeURL written into GeneratedBy[0].CodeURL.\n"
      << "  --source-dataset-url <u>    Optional SourceDatasets[0].URL value.\n\n"
      << "Other:\n"
      << "  --overwrite                 Overwrite outputs if they already exist.\n"
      << "  -h, --help                  Show this help.\n";
}

static bool is_flag(const std::string& a, const char* s1, const char* s2 = nullptr) {
  if (a == s1) return true;
  if (s2 && a == s2) return true;
  return false;
}

static std::string require_value(int& i, int argc, char** argv, const std::string& flag) {
  if (i + 1 >= argc) throw std::runtime_error("Missing value for " + flag);
  return std::string(argv[++i]);
}

static void ensure_writable(const std::filesystem::path& p, bool overwrite) {
  if (!overwrite && std::filesystem::exists(p)) {
    throw std::runtime_error("Output already exists: " + p.u8string() + " (use --overwrite)");
  }
}

static void copy_file_or_throw(const std::filesystem::path& src,
                               const std::filesystem::path& dst,
                               bool overwrite) {
  if (!std::filesystem::exists(src)) {
    throw std::runtime_error("Missing input file: " + src.u8string());
  }
  ensure_directory(dst.parent_path().u8string());
  ensure_writable(dst, overwrite);
  std::filesystem::copy_file(src, dst,
                             overwrite ? std::filesystem::copy_options::overwrite_existing
                                       : std::filesystem::copy_options::none);
}

static void copy_if_exists(const std::filesystem::path& src,
                           const std::filesystem::path& dst,
                           bool overwrite) {
  if (!std::filesystem::exists(src)) return;
  ensure_directory(dst.parent_path().u8string());
  ensure_writable(dst, overwrite);
  std::filesystem::copy_file(src, dst,
                             overwrite ? std::filesystem::copy_options::overwrite_existing
                                       : std::filesystem::copy_options::none);
}

static std::vector<std::filesystem::path> list_matching_files(const std::filesystem::path& dir,
                                                              const std::string& prefix,
                                                              const std::string& suffix) {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return out;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::string name = e.path().filename().u8string();
    if (!prefix.empty() && name.rfind(prefix, 0) != 0) continue;
    if (!suffix.empty() && (name.size() < suffix.size() || name.substr(name.size() - suffix.size()) != suffix)) continue;
    out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}


static std::string sanitize_rel_for_filename(std::string rel) {
  for (char& c : rel) {
    if (c == '/' || c == '\\' || c == ' ') c = '_';
  }
  return rel;
}

static bool copy_from_run_meta(const std::filesystem::path& tool_outdir,
                               const std::string& meta_filename,
                               const std::filesystem::path& eeg_dir,
                               const std::string& stem,
                               const std::string& desc,
                               bool overwrite) {
  const std::filesystem::path meta_path = tool_outdir / meta_filename;
  if (!std::filesystem::exists(meta_path) || !std::filesystem::is_regular_file(meta_path)) return false;

  const std::vector<std::string> outs = read_run_meta_outputs(meta_path.u8string());
  if (outs.empty()) return false;

  for (const auto& rel : outs) {
    if (rel.empty()) continue;
    const std::filesystem::path src = tool_outdir / std::filesystem::u8path(rel);
    if (!std::filesystem::exists(src) || !std::filesystem::is_regular_file(src)) {
      std::cerr << "Warning: run meta listed missing output: " << src.u8string() << "\n";
      continue;
    }
    const std::string safe_rel = sanitize_rel_for_filename(rel);
    const std::filesystem::path dst = eeg_dir / (stem + "_desc-" + desc + "_" + safe_rel);
    copy_if_exists(src, dst, overwrite);
  }

  return true;
}


static void write_readme_if_missing(const std::filesystem::path& dir,
                                    const std::string& pipeline,
                                    bool overwrite) {
  const auto readme = dir / "README.md";
  if (!overwrite && std::filesystem::exists(readme)) return;

  std::ofstream f(readme, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + readme.u8string());

  f << "# " << pipeline << " derivatives\n\n";
  f << "This folder contains derivative outputs produced by the qeeg-neurofeedback-opensoftware toolkit.\n\n";
  f << "Common contents (per recording):\n";
  f << "- qEEG brain mapping outputs (bandpowers, PSD exports, topomaps, connectivity maps, region summaries, HTML reports)\n";
  f << "- Spectral summary tables (entropy, edge frequency, peak frequency)\n";
  f << "- Channel quality control summaries\n";
  f << "- Individual Alpha Frequency (IAF) estimates and derived band specs\n";
  f << "- Neurofeedback session logs and derived events\n";
  f << "- Microstate templates, time series, and segment/event exports\n\n";
  f << "See dataset_description.json for pipeline provenance (GeneratedBy).\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc <= 1) {
      print_help();
      return 1;
    }

    Args args;

    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      if (is_flag(a, "-h", "--help")) {
        print_help();
        return 0;
      } else if (a == "--bids-root") {
        args.bids_root = require_value(i, argc, argv, a);
      } else if (a == "--bids-file") {
        args.bids_file = require_value(i, argc, argv, a);
      } else if (a == "--pipeline") {
        args.pipeline = require_value(i, argc, argv, a);
      } else if (a == "--sub") {
        args.ent.sub = require_value(i, argc, argv, a);
      } else if (a == "--task") {
        args.ent.task = require_value(i, argc, argv, a);
      } else if (a == "--ses") {
        args.ent.ses = require_value(i, argc, argv, a);
      } else if (a == "--acq") {
        args.ent.acq = require_value(i, argc, argv, a);
      } else if (a == "--run") {
        args.ent.run = require_value(i, argc, argv, a);
      } else if (a == "--map-outdir") {
        args.map_outdir = require_value(i, argc, argv, a);
      } else if (a == "--topomap-outdir") {
        args.topomap_outdir = require_value(i, argc, argv, a);
      } else if (a == "--region-summary-outdir") {
        args.region_summary_outdir = require_value(i, argc, argv, a);
      } else if (a == "--connectivity-map-outdir") {
        args.connectivity_map_outdir = require_value(i, argc, argv, a);
      } else if (a == "--bandpower-outdir") {
        args.bandpower_outdir = require_value(i, argc, argv, a);
      } else if (a == "--bandratios-outdir") {
        args.bandratios_outdir = require_value(i, argc, argv, a);
      } else if (a == "--spectral-features-outdir") {
        args.spectral_features_outdir = require_value(i, argc, argv, a);
      } else if (a == "--coherence-outdir") {
        args.coherence_outdir = require_value(i, argc, argv, a);
      } else if (a == "--plv-outdir") {
        args.plv_outdir = require_value(i, argc, argv, a);
      } else if (a == "--pac-outdir") {
        args.pac_outdir = require_value(i, argc, argv, a);
      } else if (a == "--qc-outdir") {
        args.qc_outdir = require_value(i, argc, argv, a);
      } else if (a == "--artifacts-outdir") {
        args.artifacts_outdir = require_value(i, argc, argv, a);
      } else if (a == "--nf-outdir") {
        args.nf_outdir = require_value(i, argc, argv, a);
      } else if (a == "--epoch-outdir") {
        args.epoch_outdir = require_value(i, argc, argv, a);
      } else if (a == "--iaf-outdir") {
        args.iaf_outdir = require_value(i, argc, argv, a);
      } else if (a == "--microstates-outdir") {
        args.microstates_outdir = require_value(i, argc, argv, a);
      } else if (a == "--generated-by-version") {
        args.generated_by_version = require_value(i, argc, argv, a);
      } else if (a == "--generated-by-code-url") {
        args.generated_by_code_url = require_value(i, argc, argv, a);
      } else if (a == "--source-dataset-url") {
        args.source_dataset_url = require_value(i, argc, argv, a);
      } else if (a == "--overwrite") {
        args.overwrite = true;
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    // Convenience: infer entities and/or bids_root from a BIDS file name.
    if (!args.bids_file.empty()) {
      const auto parsed = parse_bids_filename(args.bids_file);
      if (!parsed) {
        throw std::runtime_error("Failed to parse BIDS entities from --bids-file: " + args.bids_file);
      }

      auto check_or_set = [&](std::string* dst, const std::string& v, const char* what) {
        if (v.empty()) return;
        if (dst->empty()) {
          *dst = v;
          return;
        }
        if (*dst != v) {
          throw std::runtime_error(std::string("Conflict: --") + what + " '" + *dst +
                                   "' does not match value parsed from --bids-file ('" + v + "')");
        }
      };

      check_or_set(&args.ent.sub, parsed->ent.sub, "sub");
      check_or_set(&args.ent.task, parsed->ent.task, "task");
      check_or_set(&args.ent.ses, parsed->ent.ses, "ses");
      check_or_set(&args.ent.acq, parsed->ent.acq, "acq");
      check_or_set(&args.ent.run, parsed->ent.run, "run");

      if (args.bids_root.empty()) {
        const auto found = find_bids_dataset_root(args.bids_file);
        if (!found) {
          throw std::runtime_error(
              "--bids-file was provided, but a dataset_description.json could not be found in any parent folder. "
              "Either pass --bids-root explicitly or ensure the file is inside a valid BIDS dataset.");
        }
        args.bids_root = *found;
      } else {
        const auto found = find_bids_dataset_root(args.bids_file);
        if (found && trim(*found) != trim(args.bids_root)) {
          throw std::runtime_error(
              "--bids-root does not match the dataset root inferred from --bids-file.\n"
              "  --bids-root: " + args.bids_root + "\n" +
              "  inferred:    " + *found);
        }
      }
    }

    if (args.bids_root.empty()) {
      throw std::runtime_error("Missing required --bids-root (or provide --bids-file)");
    }
    if (args.ent.sub.empty() || args.ent.task.empty()) {
      throw std::runtime_error("Missing required --sub and/or --task (or provide --bids-file)");
    }

    // Validate entity chain early.
    const std::string stem = format_bids_entity_chain(args.ent);

    const std::filesystem::path bids_root = std::filesystem::u8path(args.bids_root);
    if (!std::filesystem::exists(bids_root)) {
      throw std::runtime_error("BIDS root does not exist: " + bids_root.u8string());
    }

    // Validate BIDS root contains dataset_description.json (raw dataset root).
    const auto raw_dd = bids_root / "dataset_description.json";
    if (!std::filesystem::exists(raw_dd) || !std::filesystem::is_regular_file(raw_dd)) {
      throw std::runtime_error(
          "BIDS root is missing dataset_description.json: " + raw_dd.u8string() + "\n" +
          "Hint: create a valid BIDS dataset first (e.g., run qeeg_export_bids_cli) before exporting derivatives.");
    }

    // Derivatives root: <bids-root>/derivatives/<pipeline>
    const std::filesystem::path deriv_root = bids_root / "derivatives" / args.pipeline;
    ensure_directory(deriv_root.u8string());

    // dataset_description.json (derivative) + README.
    {
      BidsDatasetDescription desc;
      desc.name = args.pipeline + " derivatives";
      desc.dataset_type = "derivative";

      BidsDatasetDescription::GeneratedByEntry g;
      g.name = args.pipeline;
      g.version = args.generated_by_version;
      g.code_url = args.generated_by_code_url;
      g.description = "Outputs exported/copied by qeeg_export_derivatives_cli";
      desc.generated_by.push_back(g);

      if (!trim(args.source_dataset_url).empty()) {
        BidsDatasetDescription::SourceDatasetEntry s;
        s.url = args.source_dataset_url;
        desc.source_datasets.push_back(s);
      }

      write_bids_dataset_description(deriv_root.u8string(), desc, /*overwrite=*/args.overwrite);
      write_readme_if_missing(deriv_root, args.pipeline, args.overwrite);
    }

    // Destination recording folder.
    std::filesystem::path eeg_dir = deriv_root / ("sub-" + args.ent.sub);
    if (!args.ent.ses.empty()) {
      eeg_dir /= ("ses-" + args.ent.ses);
    }
    eeg_dir /= "eeg";
    ensure_directory(eeg_dir.u8string());

    // --- Copy map outputs ---
    if (!args.map_outdir.empty()) {
      const std::filesystem::path map = std::filesystem::u8path(args.map_outdir);
      if (!std::filesystem::exists(map) || !std::filesystem::is_directory(map)) {
        throw std::runtime_error("--map-outdir is not a directory: " + map.u8string());
      }

      // Prefer a manifest-driven copy (map_run_meta.json -> Outputs array).
      const bool used_meta = copy_from_run_meta(map, "map_run_meta.json", eeg_dir, stem, "qeegmap", args.overwrite);
      if (!used_meta) {
        copy_if_exists(map / "bandpowers.csv", eeg_dir / (stem + "_desc-qeegmap_bandpowers.csv"), args.overwrite);
        copy_if_exists(map / "bandpowers.json", eeg_dir / (stem + "_desc-qeegmap_bandpowers.json"), args.overwrite);
        copy_if_exists(map / "psd.csv", eeg_dir / (stem + "_desc-qeegmap_psd.csv"), args.overwrite);
        copy_if_exists(map / "report.html", eeg_dir / (stem + "_desc-qeegmap_report.html"), args.overwrite);
        copy_if_exists(map / "bad_channels_used.txt", eeg_dir / (stem + "_desc-qeegmap_bad_channels_used.txt"), args.overwrite);
        copy_if_exists(map / "map_run_meta.json", eeg_dir / (stem + "_desc-qeegmap_map_run_meta.json"), args.overwrite);

        // Copy any topomap images.
        for (const auto& p : list_matching_files(map, "topomap", ".bmp")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegmap_" + fname), args.overwrite);
        }
      }

      // Also emit TSV aliases for key CSV tables (BIDS-friendly tabular format).
      // These are generated from the *source* map outputs to avoid depending on
      // whether a manifest-driven copy was used.
      if (std::filesystem::exists(map / "bandpowers.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegmap_bandpowers.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((map / "bandpowers.csv").u8string(), dst.u8string());
      }
      if (std::filesystem::exists(map / "psd.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegmap_psd.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((map / "psd.csv").u8string(), dst.u8string());
      }

    }

    // --- Copy standalone topomap outputs (qeeg_topomap_cli) ---
    if (!args.topomap_outdir.empty()) {
      const std::filesystem::path topo = std::filesystem::u8path(args.topomap_outdir);
      if (!std::filesystem::exists(topo) || !std::filesystem::is_directory(topo)) {
        throw std::runtime_error("--topomap-outdir is not a directory: " + topo.u8string());
      }

      const bool used_meta = copy_from_run_meta(topo, "topomap_run_meta.json", eeg_dir, stem, "qeegtopo", args.overwrite);
      if (!used_meta) {
        copy_if_exists(topo / "topomap_report.html", eeg_dir / (stem + "_desc-qeegtopo_topomap_report.html"), args.overwrite);
        copy_if_exists(topo / "topomap_run_meta.json", eeg_dir / (stem + "_desc-qeegtopo_topomap_run_meta.json"), args.overwrite);
        for (const auto& p : list_matching_files(topo, "topomap_", ".bmp")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegtopo_" + fname), args.overwrite);
        }
      }
    }

    // --- Copy region summaries (qeeg_region_summary_cli) ---
    if (!args.region_summary_outdir.empty()) {
      const std::filesystem::path reg = std::filesystem::u8path(args.region_summary_outdir);
      if (!std::filesystem::exists(reg) || !std::filesystem::is_directory(reg)) {
        throw std::runtime_error("--region-summary-outdir is not a directory: " + reg.u8string());
      }

      const bool used_meta = copy_from_run_meta(reg, "region_summary_run_meta.json", eeg_dir, stem, "qeegregion", args.overwrite);
      if (!used_meta) {
        copy_if_exists(reg / "region_summary.csv", eeg_dir / (stem + "_desc-qeegregion_region_summary.csv"), args.overwrite);
        copy_if_exists(reg / "region_summary_long.csv", eeg_dir / (stem + "_desc-qeegregion_region_summary_long.csv"), args.overwrite);
        copy_if_exists(reg / "region_report.html", eeg_dir / (stem + "_desc-qeegregion_region_report.html"), args.overwrite);
        copy_if_exists(reg / "region_summary_run_meta.json", eeg_dir / (stem + "_desc-qeegregion_region_summary_run_meta.json"), args.overwrite);
      }

      // BIDS-friendly TSV aliases.
      if (std::filesystem::exists(reg / "region_summary.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegregion_region_summary.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((reg / "region_summary.csv").u8string(), dst.u8string());
      }
      if (std::filesystem::exists(reg / "region_summary_long.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegregion_region_summary_long.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((reg / "region_summary_long.csv").u8string(), dst.u8string());
      }
    }

    // --- Copy connectivity map visuals (qeeg_connectivity_map_cli) ---
    if (!args.connectivity_map_outdir.empty()) {
      const std::filesystem::path conn = std::filesystem::u8path(args.connectivity_map_outdir);
      if (!std::filesystem::exists(conn) || !std::filesystem::is_directory(conn)) {
        throw std::runtime_error("--connectivity-map-outdir is not a directory: " + conn.u8string());
      }

      const bool used_meta = copy_from_run_meta(conn, "connectivity_run_meta.json", eeg_dir, stem, "qeegconnmap", args.overwrite);
      if (!used_meta) {
        copy_if_exists(conn / "connectivity_map.svg", eeg_dir / (stem + "_desc-qeegconnmap_connectivity_map.svg"), args.overwrite);
        copy_if_exists(conn / "connectivity_report.html", eeg_dir / (stem + "_desc-qeegconnmap_connectivity_report.html"), args.overwrite);
        copy_if_exists(conn / "connectivity_run_meta.json", eeg_dir / (stem + "_desc-qeegconnmap_connectivity_run_meta.json"), args.overwrite);
      }
    }

    // --- Copy bandpower-only outputs (qeeg_bandpower_cli) ---
    if (!args.bandpower_outdir.empty()) {
      const std::filesystem::path bp = std::filesystem::u8path(args.bandpower_outdir);
      if (!std::filesystem::exists(bp) || !std::filesystem::is_directory(bp)) {
        throw std::runtime_error("--bandpower-outdir is not a directory: " + bp.u8string());
      }

      const bool used_meta = copy_from_run_meta(bp, "bandpower_run_meta.json", eeg_dir, stem, "qeegbp", args.overwrite);
      if (!used_meta) {
        copy_if_exists(bp / "bandpowers.csv", eeg_dir / (stem + "_desc-qeegbp_bandpowers.csv"), args.overwrite);
        copy_if_exists(bp / "bandpowers.json", eeg_dir / (stem + "_desc-qeegbp_bandpowers.json"), args.overwrite);
        copy_if_exists(bp / "bandpower_timeseries.csv", eeg_dir / (stem + "_desc-qeegbp_bandpower_timeseries.csv"), args.overwrite);
        copy_if_exists(bp / "bandpower_timeseries.json", eeg_dir / (stem + "_desc-qeegbp_bandpower_timeseries.json"), args.overwrite);
        copy_if_exists(bp / "bandpower_run_meta.json", eeg_dir / (stem + "_desc-qeegbp_bandpower_run_meta.json"), args.overwrite);
      }

      // BIDS-friendly TSV alias.
      if (std::filesystem::exists(bp / "bandpowers.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegbp_bandpowers.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((bp / "bandpowers.csv").u8string(), dst.u8string());
      }

      if (std::filesystem::exists(bp / "bandpower_timeseries.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegbp_bandpower_timeseries.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((bp / "bandpower_timeseries.csv").u8string(), dst.u8string());
      }
    }

    // --- Copy band ratios outputs (qeeg_bandratios_cli) ---
    if (!args.bandratios_outdir.empty()) {
      const std::filesystem::path br = std::filesystem::u8path(args.bandratios_outdir);
      if (!std::filesystem::exists(br) || !std::filesystem::is_directory(br)) {
        throw std::runtime_error("--bandratios-outdir is not a directory: " + br.u8string());
      }

      const bool used_meta = copy_from_run_meta(br, "bandratios_run_meta.json", eeg_dir, stem, "qeegratio", args.overwrite);
      if (!used_meta) {
        copy_if_exists(br / "bandratios.csv", eeg_dir / (stem + "_desc-qeegratio_bandratios.csv"), args.overwrite);
        copy_if_exists(br / "bandratios.json", eeg_dir / (stem + "_desc-qeegratio_bandratios.json"), args.overwrite);
        copy_if_exists(br / "bandratios.tsv", eeg_dir / (stem + "_desc-qeegratio_bandratios.tsv"), args.overwrite);
        copy_if_exists(br / "bandratios_run_meta.json", eeg_dir / (stem + "_desc-qeegratio_bandratios_run_meta.json"), args.overwrite);
      }

      // BIDS-friendly TSV alias (generate if missing; skip if already present from manifest/tool).
      if (std::filesystem::exists(br / "bandratios.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegratio_bandratios.tsv");
        if (args.overwrite || !std::filesystem::exists(dst)) {
          ensure_directory(dst.parent_path().u8string());
          ensure_writable(dst, args.overwrite);
          convert_csv_file_to_tsv((br / "bandratios.csv").u8string(), dst.u8string());
        }
      }
    }

    // --- Copy spectral summary outputs (qeeg_spectral_features_cli) ---
    if (!args.spectral_features_outdir.empty()) {
      const std::filesystem::path sf = std::filesystem::u8path(args.spectral_features_outdir);
      if (!std::filesystem::exists(sf) || !std::filesystem::is_directory(sf)) {
        throw std::runtime_error("--spectral-features-outdir is not a directory: " + sf.u8string());
      }

      const bool used_meta = copy_from_run_meta(sf, "spectral_features_run_meta.json", eeg_dir, stem, "qeegspec", args.overwrite);
      if (!used_meta) {
        copy_if_exists(sf / "spectral_features.csv", eeg_dir / (stem + "_desc-qeegspec_spectral_features.csv"), args.overwrite);
        copy_if_exists(sf / "spectral_features.json", eeg_dir / (stem + "_desc-qeegspec_spectral_features.json"), args.overwrite);
        copy_if_exists(sf / "spectral_features_run_meta.json",
                       eeg_dir / (stem + "_desc-qeegspec_spectral_features_run_meta.json"),
                       args.overwrite);
      }

      // BIDS-friendly TSV alias.
      if (std::filesystem::exists(sf / "spectral_features.csv")) {
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegspec_spectral_features.tsv");
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv((sf / "spectral_features.csv").u8string(), dst.u8string());
      }
    }

    // --- Copy connectivity outputs ---
    // These tools emit lightweight *_run_meta.json manifests which we prefer.
    auto copy_connectivity_tool = [&](const std::string& flag,
                                      const std::string& outdir,
                                      const std::string& meta_name,
                                      const std::string& desc) {
      if (outdir.empty()) return;
      const std::filesystem::path d = std::filesystem::u8path(outdir);
      if (!std::filesystem::exists(d) || !std::filesystem::is_directory(d)) {
        throw std::runtime_error(flag + " is not a directory: " + d.u8string());
      }

      const bool used_meta = copy_from_run_meta(d, meta_name, eeg_dir, stem, desc, args.overwrite);
      if (!used_meta) {
        // Fallback: copy any CSV outputs and the run meta file if present.
        for (const auto& p : list_matching_files(d, "", ".csv")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-" + desc + "_" + fname), args.overwrite);
        }
        copy_if_exists(d / meta_name, eeg_dir / (stem + "_desc-" + desc + "_" + meta_name), args.overwrite);
      }

      // TSV aliases for any CSV tables in the source outdir.
      for (const auto& p : list_matching_files(d, "", ".csv")) {
        const std::string fname = p.filename().u8string();
        if (fname.size() < 4) continue;
        std::string tsv_name = fname.substr(0, fname.size() - 4) + ".tsv";
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-" + desc + "_" + tsv_name);
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv(p.u8string(), dst.u8string());
      }
    };

    copy_connectivity_tool("--coherence-outdir", args.coherence_outdir, "coherence_run_meta.json", "qeegcoh");
    copy_connectivity_tool("--plv-outdir", args.plv_outdir, "plv_run_meta.json", "qeegplv");
    copy_connectivity_tool("--pac-outdir", args.pac_outdir, "pac_run_meta.json", "qeegpac");

    // --- Copy channel QC outputs ---
    if (!args.qc_outdir.empty()) {
      const std::filesystem::path qc = std::filesystem::u8path(args.qc_outdir);
      if (!std::filesystem::exists(qc) || !std::filesystem::is_directory(qc)) {
        throw std::runtime_error("--qc-outdir is not a directory: " + qc.u8string());
      }

      const bool used_meta = copy_from_run_meta(qc, "qc_run_meta.json", eeg_dir, stem, "qeegqc", args.overwrite);

      if (!used_meta) {
        copy_if_exists(qc / "channel_qc.csv", eeg_dir / (stem + "_desc-qeegqc_channel_qc.csv"), args.overwrite);
        copy_if_exists(qc / "bad_channels.txt", eeg_dir / (stem + "_desc-qeegqc_bad_channels.txt"), args.overwrite);
        copy_if_exists(qc / "qc_summary.txt", eeg_dir / (stem + "_desc-qeegqc_summary.txt"), args.overwrite);
        copy_if_exists(qc / "qc_run_meta.json", eeg_dir / (stem + "_desc-qeegqc_qc_run_meta.json"), args.overwrite);

        // Optional: qc_output.edf (may be large) is not copied by default.
      }

      // Also emit a BIDS-style channels.tsv derivative that captures QC status.
      //
      // BIDS channels.tsv files should list channels in the same order as the
      // corresponding data file when possible. We therefore prefer the raw
      // dataset's channels.tsv as the ordering source.
      //
      // If no raw channels.tsv exists, we fall back to the channel list embedded
      // in channel_qc.csv (if available).
      const auto qc_csv = qc / "channel_qc.csv";
      const auto qc_txt = qc / "bad_channels.txt";
      if (std::filesystem::exists(qc_csv) || std::filesystem::exists(qc_txt)) {
        std::string qc_resolved;
        ChannelQcMap qc_map = load_channel_qc_any(qc.u8string(), &qc_resolved);

        // Load channel order from the raw dataset if present.
        std::vector<std::string> ch_names;
        {
          std::filesystem::path raw_eeg_dir = bids_root / ("sub-" + args.ent.sub);
          if (!args.ent.ses.empty()) raw_eeg_dir /= ("ses-" + args.ent.ses);
          raw_eeg_dir /= "eeg";
          const auto raw_channels_tsv = raw_eeg_dir / (stem + "_channels.tsv");

          if (std::filesystem::exists(raw_channels_tsv) && std::filesystem::is_regular_file(raw_channels_tsv)) {
            try {
              ch_names = load_bids_channels_tsv_names(raw_channels_tsv.u8string());
            } catch (const std::exception& e) {
              std::cerr << "Warning: failed to read raw channels.tsv for ordering: "
                        << raw_channels_tsv.u8string() << " (" << e.what() << ")\n";
            }
          }
        }

        // Fall back to QC file ordering if needed (requires channel_qc.csv).
        if (ch_names.empty() && std::filesystem::exists(qc_csv)) {
          try {
            ch_names = load_channel_qc_csv_channel_names(qc_csv.u8string());
          } catch (const std::exception& e) {
            std::cerr << "Warning: failed to read channel order from channel_qc.csv: "
                      << qc_csv.u8string() << " (" << e.what() << ")\n";
          }
        }

        if (!ch_names.empty()) {
          std::vector<std::string> status(ch_names.size(), "good");
          std::vector<std::string> status_desc(ch_names.size());

          size_t matched = 0;
          size_t bad = 0;
          for (size_t i = 0; i < ch_names.size(); ++i) {
            const std::string key = normalize_channel_name(ch_names[i]);
            if (key.empty()) continue;
            const auto it = qc_map.find(key);
            if (it == qc_map.end()) continue;
            ++matched;
            if (it->second.bad) {
              status[i] = "bad";
              ++bad;
              if (!it->second.reasons.empty()) {
                status_desc[i] = "qeeg_channel_qc:" + it->second.reasons;
              } else {
                status_desc[i] = "qeeg_channel_qc:bad";
              }
            }
          }

          if (matched == 0) {
            std::cerr << "Warning: QC loaded from '" << qc_resolved
                      << "', but no channels matched the derived channel list.\n";
          }

          const auto out_channels = eeg_dir / (stem + "_desc-qeegqc_channels.tsv");
          ensure_directory(out_channels.parent_path().u8string());
          ensure_writable(out_channels, args.overwrite);
          write_bids_channels_tsv(out_channels.u8string(), ch_names, status, status_desc);

          std::cout << "Derived channels.tsv: wrote " << out_channels.filename().u8string()
                    << " (matched=" << matched << ", bad=" << bad << ")\n";
        } else {
          std::cerr << "Warning: QC outputs present, but could not determine a full channel list. "
                    << "Skipping derived channels.tsv export.\n";
        }
      }
    }

    // --- Copy artifact detection outputs ---
    if (!args.artifacts_outdir.empty()) {
      const std::filesystem::path art = std::filesystem::u8path(args.artifacts_outdir);
      if (!std::filesystem::exists(art) || !std::filesystem::is_directory(art)) {
        throw std::runtime_error("--artifacts-outdir is not a directory: " + art.u8string());
      }

      const bool used_meta = copy_from_run_meta(art, "artifact_run_meta.json", eeg_dir, stem, "qeegart", args.overwrite);
      if (!used_meta) {
        // Fallback: copy any CSV/TXT/JSON outputs we recognize.
        for (const auto& p : list_matching_files(art, "", ".csv")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegart_" + fname), args.overwrite);
        }
        for (const auto& p : list_matching_files(art, "", ".txt")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegart_" + fname), args.overwrite);
        }
        for (const auto& p : list_matching_files(art, "", ".json")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegart_" + fname), args.overwrite);
        }
      }

      // TSV aliases for any CSV tables in the source outdir.
      for (const auto& p : list_matching_files(art, "", ".csv")) {
        const std::string fname = p.filename().u8string();
        if (fname.size() < 4) continue;
        std::string tsv_name = fname.substr(0, fname.size() - 4) + ".tsv";
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegart_" + tsv_name);
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv(p.u8string(), dst.u8string());
      }
    }

    // --- Copy neurofeedback outputs ---
    if (!args.nf_outdir.empty()) {
      const std::filesystem::path nf = std::filesystem::u8path(args.nf_outdir);
      if (!std::filesystem::exists(nf) || !std::filesystem::is_directory(nf)) {
        throw std::runtime_error("--nf-outdir is not a directory: " + nf.u8string());
      }

      // Prefer the outputs list in nf_run_meta.json if present, falling back to
      // legacy fixed-file copying.
      const std::filesystem::path meta_path = nf / "nf_run_meta.json";
      const std::vector<std::string> outs = read_run_meta_outputs(meta_path.u8string());
      std::unordered_set<std::string> listed;
      for (const auto& o : outs) {
        if (!o.empty()) listed.insert(o);
      }

      if (!outs.empty()) {
        copy_from_run_meta(nf, "nf_run_meta.json", eeg_dir, stem, "qeegnf", args.overwrite);
      } else {
        copy_if_exists(nf / "nf_run_meta.json", eeg_dir / (stem + "_desc-qeegnf_nf_run_meta.json"), args.overwrite);
        copy_if_exists(nf / "bad_channels_used.txt", eeg_dir / (stem + "_desc-qeegnf_bad_channels_used.txt"), args.overwrite);
        copy_if_exists(nf / "nf_derived_events.tsv", eeg_dir / (stem + "_desc-qeegnf_nf_derived_events.tsv"), args.overwrite);
        copy_if_exists(nf / "nf_derived_events.csv", eeg_dir / (stem + "_desc-qeegnf_nf_derived_events.csv"), args.overwrite);
        copy_if_exists(nf / "nf_derived_events.json", eeg_dir / (stem + "_desc-qeegnf_nf_derived_events.json"), args.overwrite);
        copy_if_exists(nf / "nf_feedback.csv", eeg_dir / (stem + "_desc-qeegnf_nf_feedback.csv"), args.overwrite);
        copy_if_exists(nf / "artifact_gate_timeseries.csv", eeg_dir / (stem + "_desc-qeegnf_artifact_gate_timeseries.csv"), args.overwrite);
        copy_if_exists(nf / "bandpower_timeseries.csv", eeg_dir / (stem + "_desc-qeegnf_bandpower_timeseries.csv"), args.overwrite);
        // Copy the optional BioTrace UI HTML if present.
        copy_if_exists(nf / "biotrace_ui.html", eeg_dir / (stem + "_desc-qeegnf_biotrace_ui.html"), args.overwrite);
      }

      
      // Emit a BIDS-style "events.tsv" alias for the neurofeedback derived events, if present.
      // nf_cli writes nf_derived_events.tsv with onset/duration/trial_type columns, matching
      // the BIDS events.tsv schema; in derivatives we prefer the standard suffix name.
      copy_if_exists(nf / "nf_derived_events.tsv", eeg_dir / (stem + "_desc-qeegnf_events.tsv"), args.overwrite);
      copy_if_exists(nf / "nf_derived_events.json", eeg_dir / (stem + "_desc-qeegnf_events.json"), args.overwrite);

      // Copy any additional *_timeseries.csv outputs (e.g., coherence modes) not already listed.
      for (const auto& p : list_matching_files(nf, "", "_timeseries.csv")) {
        const std::string fname = p.filename().u8string();
        if (listed.find(fname) != listed.end()) continue;
        // Avoid duplicating legacy explicit copies.
        if (fname == "artifact_gate_timeseries.csv" || fname == "bandpower_timeseries.csv") continue;
        copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegnf_" + fname), args.overwrite);
      }
    }



    // --- Copy epoch/segment feature outputs (qeeg_epoch_cli) ---
    if (!args.epoch_outdir.empty()) {
      const std::filesystem::path ep = std::filesystem::u8path(args.epoch_outdir);
      if (!std::filesystem::exists(ep) || !std::filesystem::is_directory(ep)) {
        throw std::runtime_error("--epoch-outdir is not a directory: " + ep.u8string());
      }

      const bool used_meta = copy_from_run_meta(ep, "epoch_run_meta.json", eeg_dir, stem, "qeegepoch", args.overwrite);
      if (!used_meta) {
        // Fallback: copy known outputs.
        copy_if_exists(ep / "events.csv", eeg_dir / (stem + "_desc-qeegepoch_events.csv"), args.overwrite);
        copy_if_exists(ep / "events_table.csv", eeg_dir / (stem + "_desc-qeegepoch_events_table.csv"), args.overwrite);
        copy_if_exists(ep / "events_table.tsv", eeg_dir / (stem + "_desc-qeegepoch_events_table.tsv"), args.overwrite);
        copy_if_exists(ep / "epoch_bandpowers.csv", eeg_dir / (stem + "_desc-qeegepoch_epoch_bandpowers.csv"), args.overwrite);
        copy_if_exists(ep / "epoch_bandpowers_summary.csv", eeg_dir / (stem + "_desc-qeegepoch_epoch_bandpowers_summary.csv"), args.overwrite);
        copy_if_exists(ep / "epoch_bandpowers_norm.csv", eeg_dir / (stem + "_desc-qeegepoch_epoch_bandpowers_norm.csv"), args.overwrite);
        copy_if_exists(ep / "epoch_bandpowers_norm_summary.csv", eeg_dir / (stem + "_desc-qeegepoch_epoch_bandpowers_norm_summary.csv"), args.overwrite);
        copy_if_exists(ep / "epoch_run_meta.json", eeg_dir / (stem + "_desc-qeegepoch_epoch_run_meta.json"), args.overwrite);
      }

      // Emit a BIDS-style events.tsv alias for the (BIDS-style) events table if present.
      // qeeg_epoch_cli writes events_table.tsv with onset/duration/trial_type columns.
      copy_if_exists(ep / "events_table.tsv", eeg_dir / (stem + "_desc-qeegepoch_events.tsv"), args.overwrite);

      // TSV aliases for any CSV tables in the source outdir.
      for (const auto& p : list_matching_files(ep, "", ".csv")) {
        const std::string fname = p.filename().u8string();
        if (fname.size() < 4) continue;
        std::string tsv_name = fname.substr(0, fname.size() - 4) + ".tsv";
        const std::filesystem::path dst = eeg_dir / (stem + "_desc-qeegepoch_" + tsv_name);
        ensure_writable(dst, args.overwrite);
        convert_csv_file_to_tsv(p.u8string(), dst.u8string());
      }
    }

    // --- Copy IAF outputs ---
    if (!args.iaf_outdir.empty()) {
      const std::filesystem::path iaf = std::filesystem::u8path(args.iaf_outdir);
      if (!std::filesystem::exists(iaf) || !std::filesystem::is_directory(iaf)) {
        throw std::runtime_error("--iaf-outdir is not a directory: " + iaf.u8string());
      }

      const bool used_meta = copy_from_run_meta(iaf, "iaf_run_meta.json", eeg_dir, stem, "qeegiaf", args.overwrite);
      if (!used_meta) {
        copy_if_exists(iaf / "iaf_by_channel.csv", eeg_dir / (stem + "_desc-qeegiaf_iaf_by_channel.csv"), args.overwrite);
        copy_if_exists(iaf / "iaf_summary.txt", eeg_dir / (stem + "_desc-qeegiaf_iaf_summary.txt"), args.overwrite);
        copy_if_exists(iaf / "iaf_band_spec.txt", eeg_dir / (stem + "_desc-qeegiaf_iaf_band_spec.txt"), args.overwrite);
        copy_if_exists(iaf / "topomap_iaf.bmp", eeg_dir / (stem + "_desc-qeegiaf_topomap_iaf.bmp"), args.overwrite);
        copy_if_exists(iaf / "iaf_run_meta.json", eeg_dir / (stem + "_desc-qeegiaf_iaf_run_meta.json"), args.overwrite);
      }
    }

    // --- Copy microstates outputs ---
    if (!args.microstates_outdir.empty()) {
      const std::filesystem::path ms = std::filesystem::u8path(args.microstates_outdir);
      if (!std::filesystem::exists(ms) || !std::filesystem::is_directory(ms)) {
        throw std::runtime_error("--microstates-outdir is not a directory: " + ms.u8string());
      }

      const bool used_meta = copy_from_run_meta(ms, "microstates_run_meta.json", eeg_dir, stem, "qeegms", args.overwrite);
      if (!used_meta) {
        copy_if_exists(ms / "microstate_templates.csv", eeg_dir / (stem + "_desc-qeegms_microstate_templates.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_timeseries.csv", eeg_dir / (stem + "_desc-qeegms_microstate_timeseries.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_segments.csv", eeg_dir / (stem + "_desc-qeegms_microstate_segments.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_transition_counts.csv", eeg_dir / (stem + "_desc-qeegms_microstate_transition_counts.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_transition_probs.csv", eeg_dir / (stem + "_desc-qeegms_microstate_transition_probs.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_state_stats.csv", eeg_dir / (stem + "_desc-qeegms_microstate_state_stats.csv"), args.overwrite);
        copy_if_exists(ms / "microstate_summary.txt", eeg_dir / (stem + "_desc-qeegms_microstate_summary.txt"), args.overwrite);
        copy_if_exists(ms / "microstate_events.tsv", eeg_dir / (stem + "_desc-qeegms_microstate_events.tsv"), args.overwrite);
        copy_if_exists(ms / "microstate_events.json", eeg_dir / (stem + "_desc-qeegms_microstate_events.json"), args.overwrite);
        copy_if_exists(ms / "bad_channels_used.txt", eeg_dir / (stem + "_desc-qeegms_bad_channels_used.txt"), args.overwrite);
        copy_if_exists(ms / "microstates_run_meta.json", eeg_dir / (stem + "_desc-qeegms_microstates_run_meta.json"), args.overwrite);

        // Copy any rendered microstate topomaps.
        for (const auto& p : list_matching_files(ms, "topomap_microstate", ".bmp")) {
          const std::string fname = p.filename().u8string();
          copy_file_or_throw(p, eeg_dir / (stem + "_desc-qeegms_" + fname), args.overwrite);
        }
      }

      // Emit a BIDS-style "events.tsv" alias for the microstate segments/events if present.
      // qeeg_microstates_cli writes microstate_events.tsv with onset/duration/trial_type columns.
      copy_if_exists(ms / "microstate_events.tsv", eeg_dir / (stem + "_desc-qeegms_events.tsv"), args.overwrite);
      copy_if_exists(ms / "microstate_events.json", eeg_dir / (stem + "_desc-qeegms_events.json"), args.overwrite);
    }
    std::cout << "Done. Derivatives written under: " << (deriv_root / ("sub-" + args.ent.sub)).u8string() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
