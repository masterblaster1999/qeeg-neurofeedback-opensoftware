#include "qeeg/bids.hpp"
#include "qeeg/brainvision_writer.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/line_noise.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string out_dir;
  std::string sub;
  std::string task;
  std::string ses;
  std::string acq;
  std::string run;

  std::string format{"edf"}; // edf|brainvision

  std::string channel_map_path;
  double fs_csv{0.0};

  std::string dataset_name{"qeeg-export"};
  std::string eeg_reference{"n/a"};
  std::string eeg_ground;
  std::string cap_manufacturer;
  std::string cap_model;

  std::string powerline{"auto"}; // auto|n/a|50|60|<Hz>
  std::string software_filters{"n/a"}; // n/a or raw JSON object

  bool no_events{false};
  bool overwrite{false};
};

static void print_help() {
  std::cout
      << "qeeg_export_bids_cli\n\n"
      << "Export a recording (EDF/BDF/CSV/BrainVision) into a BIDS EEG folder layout.\n"
      << "Writes: data file (EDF or BrainVision) + *_eeg.json + *_channels.tsv (+ events.tsv/json if present).\n\n"
      << "Usage:\n"
      << "  qeeg_export_bids_cli --input <in.edf|in.bdf|in.csv|in.txt|in.vhdr> --out-dir <bids_root> --sub <label> --task <label> [options]\n\n"
      << "Required:\n"
      << "  --input <path>                 Input file path (EDF/BDF/CSV/ASCII/BrainVision .vhdr).\n"
      << "  --out-dir <dir>                BIDS dataset root output directory.\n"
      << "  --sub <label>                  Subject label (alphanumeric).\n"
      << "  --task <label>                 Task label (alphanumeric).\n\n"
      << "Options:\n"
      << "  --ses <label>                  Session label (alphanumeric).\n"
      << "  --acq <label>                  Acquisition label (alphanumeric).\n"
      << "  --run <index>                  Run index (alphanumeric; typically digits).\n"
      << "  --format <edf|brainvision>     Output data format (default: edf).\n"
      << "  --channel-map <map.csv>        Remap/drop channels before writing.\n"
      << "  --fs <Hz>                      Sampling rate hint for CSV/ASCII (0 = infer from time column).\n"
      << "  --dataset-name <text>          dataset_description.json Name (created if missing).\n"
      << "  --eeg-reference <text>         EEGReference field for *_eeg.json (default: n/a).\n"
      << "  --eeg-ground <text>            EEGGround field for *_eeg.json.\n"
      << "  --cap-manufacturer <text>      CapManufacturer field for *_eeg.json.\n"
      << "  --cap-model <text>             CapManufacturersModelName field for *_eeg.json.\n"
      << "  --powerline <auto|n/a|Hz>      PowerLineFrequency. 'auto' uses a 50/60 Hz detector.\n"
      << "  --software-filters <n/a|JSON>  SoftwareFilters. Use 'n/a' or a raw JSON object string.\n"
      << "  --no-events                    Do not write *_events.tsv/json even if events exist.\n"
      << "  --overwrite                    Overwrite output files if they already exist.\n"
      << "  -h, --help                     Show this help.\n\n"
      << "Notes:\n"
      << "  - Output path: <out-dir>/sub-<sub>/[ses-<ses>/]eeg/*.\n"
      << "  - BIDS requires dataset_description.json at the dataset root; this tool creates it if missing.\n";
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

static double parse_double_or_throw(const std::string& s, const std::string& flag) {
  try {
    return std::stod(s);
  } catch (const std::exception&) {
    throw std::runtime_error("Failed to parse numeric value for " + flag + ": '" + s + "'");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args args;

    if (argc <= 1) {
      print_help();
      return 1;
    }

    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];

      if (is_flag(a, "-h", "--help")) {
        print_help();
        return 0;
      } else if (is_flag(a, "--input", "-i")) {
        args.input_path = require_value(i, argc, argv, a);
      } else if (a == "--out-dir") {
        args.out_dir = require_value(i, argc, argv, a);
      } else if (a == "--sub") {
        args.sub = require_value(i, argc, argv, a);
      } else if (a == "--task") {
        args.task = require_value(i, argc, argv, a);
      } else if (a == "--ses") {
        args.ses = require_value(i, argc, argv, a);
      } else if (a == "--acq") {
        args.acq = require_value(i, argc, argv, a);
      } else if (a == "--run") {
        args.run = require_value(i, argc, argv, a);
      } else if (a == "--format") {
        args.format = to_lower(require_value(i, argc, argv, a));
      } else if (a == "--channel-map") {
        args.channel_map_path = require_value(i, argc, argv, a);
      } else if (a == "--fs") {
        args.fs_csv = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--dataset-name") {
        args.dataset_name = require_value(i, argc, argv, a);
      } else if (a == "--eeg-reference") {
        args.eeg_reference = require_value(i, argc, argv, a);
      } else if (a == "--eeg-ground") {
        args.eeg_ground = require_value(i, argc, argv, a);
      } else if (a == "--cap-manufacturer") {
        args.cap_manufacturer = require_value(i, argc, argv, a);
      } else if (a == "--cap-model") {
        args.cap_model = require_value(i, argc, argv, a);
      } else if (a == "--powerline") {
        args.powerline = to_lower(require_value(i, argc, argv, a));
      } else if (a == "--software-filters") {
        args.software_filters = require_value(i, argc, argv, a);
      } else if (a == "--no-events") {
        args.no_events = true;
      } else if (a == "--overwrite") {
        args.overwrite = true;
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.out_dir.empty() || args.sub.empty() || args.task.empty()) {
      throw std::runtime_error("Missing required arguments. Need --input, --out-dir, --sub, --task.");
    }

    // Validate BIDS labels (strict alnum-only).
    if (!is_valid_bids_label(args.sub)) {
      throw std::runtime_error("Invalid --sub label. Use alphanumeric only (no '_' or '-'): " + args.sub);
    }
    if (!is_valid_bids_label(args.task)) {
      throw std::runtime_error("Invalid --task label. Use alphanumeric only (no '_' or '-'): " + args.task);
    }
    if (!args.ses.empty() && !is_valid_bids_label(args.ses)) {
      throw std::runtime_error("Invalid --ses label. Use alphanumeric only (no '_' or '-'): " + args.ses);
    }
    if (!args.acq.empty() && !is_valid_bids_label(args.acq)) {
      throw std::runtime_error("Invalid --acq label. Use alphanumeric only (no '_' or '-'): " + args.acq);
    }
    if (!args.run.empty() && !is_valid_bids_label(args.run)) {
      throw std::runtime_error("Invalid --run label. Use alphanumeric only (no '_' or '-'): " + args.run);
    }

    if (args.format != "edf" && args.format != "brainvision") {
      throw std::runtime_error("Invalid --format (expected edf or brainvision): " + args.format);
    }

    // Load recording.
    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    // Optional channel map.
    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    // Prepare BIDS paths.
    BidsEntities ent;
    ent.sub = args.sub;
    ent.task = args.task;
    ent.ses = args.ses;
    ent.acq = args.acq;
    ent.run = args.run;

    std::filesystem::path root = std::filesystem::u8path(args.out_dir);
    std::filesystem::path eeg_dir = root / (std::string("sub-") + ent.sub);
    if (!ent.ses.empty()) eeg_dir /= (std::string("ses-") + ent.ses);
    eeg_dir /= "eeg";
    std::filesystem::create_directories(eeg_dir);

    // dataset_description.json at root (create if missing).
    BidsDatasetDescription d;
    d.name = args.dataset_name;
    write_bids_dataset_description(root.u8string(), d, /*overwrite=*/false);

    const std::string stem_eeg = format_bids_filename_stem(ent, "eeg");
    const std::string stem_channels = format_bids_filename_stem(ent, "channels");
    const std::string stem_events = format_bids_filename_stem(ent, "events");

    // Output file paths.
    std::filesystem::path eeg_json = eeg_dir / (stem_eeg + ".json");
    std::filesystem::path channels_tsv = eeg_dir / (stem_channels + ".tsv");
    std::filesystem::path events_tsv = eeg_dir / (stem_events + ".tsv");
    std::filesystem::path events_json = eeg_dir / (stem_events + ".json");

    // Data file(s).
    std::filesystem::path data_primary;
    std::filesystem::path data_secondary;
    std::filesystem::path data_tertiary;

    if (args.format == "edf") {
      data_primary = eeg_dir / (stem_eeg + ".edf");
      ensure_writable(data_primary, args.overwrite);
    } else {
      data_primary = eeg_dir / (stem_eeg + ".vhdr");
      data_secondary = eeg_dir / (stem_eeg + ".vmrk");
      data_tertiary = eeg_dir / (stem_eeg + ".eeg");
      ensure_writable(data_primary, args.overwrite);
      ensure_writable(data_secondary, args.overwrite);
      ensure_writable(data_tertiary, args.overwrite);
    }

    // Sidecars.
    ensure_writable(eeg_json, args.overwrite);
    ensure_writable(channels_tsv, args.overwrite);
    if (!args.no_events && !rec.events.empty()) {
      ensure_writable(events_tsv, args.overwrite);
      ensure_writable(events_json, args.overwrite);
    }

    // Write data.
    if (args.format == "edf") {
      EDFWriterOptions wopts;
      wopts.patient_id = ent.sub;
      wopts.recording_id = "qeeg-bids-export";
      wopts.physical_dimension = "uV";
      // Prefer classic EDF without an EDF+ annotations channel; BIDS stores events in events.tsv.
      wopts.write_edfplus_annotations = false;

      EDFWriter w;
      w.write(rec, data_primary.u8string(), wopts);
    } else {
      BrainVisionWriterOptions wopts;
      wopts.binary_format = BrainVisionBinaryFormat::Float32;
      wopts.unit = "uV";
      // Keep markers consistent with events.tsv when possible.
      wopts.write_events = !args.no_events;

      BrainVisionWriter w;
      w.write(rec, data_primary.u8string(), wopts);
    }

    // Write channels.tsv.
    write_bids_channels_tsv(channels_tsv.u8string(), rec);

    // Write eeg.json metadata.
    BidsEegJsonMetadata meta;
    meta.eeg_reference = args.eeg_reference;
    meta.task_name = ent.task;
    meta.eeg_ground = args.eeg_ground;
    meta.cap_manufacturer = args.cap_manufacturer;
    meta.cap_model = args.cap_model;

    const std::string pl = to_lower(args.powerline);
    if (pl == "n/a" || pl == "na" || pl == "0") {
      meta.power_line_frequency_hz.reset();
    } else if (pl == "auto" || pl.empty()) {
      // Best-effort: detect whether 50 or 60 Hz appears as a prominent narrow-band peak.
      LineNoiseEstimate est = detect_line_noise_50_60(rec);
      if (est.recommended_hz > 0.0) {
        meta.power_line_frequency_hz = est.recommended_hz;
      }
    } else {
      meta.power_line_frequency_hz = parse_double_or_throw(pl, "--powerline");
    }

    // SoftwareFilters is REQUIRED by BIDS EEG.
    // We support "n/a" or a raw JSON object string (passed through).
    const std::string sf_trim = trim(args.software_filters);
    if (!sf_trim.empty() && to_lower(sf_trim) != "n/a" && to_lower(sf_trim) != "na") {
      // Accept raw JSON objects only.
      const std::string t = trim(sf_trim);
      if (!t.empty() && t.front() == '{') {
        meta.software_filters_raw_json = t;
      } else {
        throw std::runtime_error("--software-filters must be 'n/a' or a raw JSON object string (starting with '{').");
      }
    }

    write_bids_eeg_json(eeg_json.u8string(), rec, meta);

    // Write events sidecars if present.
    if (!args.no_events && !rec.events.empty()) {
      write_bids_events_tsv(events_tsv.u8string(), rec.events);
      write_bids_events_json(events_json.u8string());
    }

    std::cout << "Wrote BIDS EEG export to: " << eeg_dir.u8string() << "\n";
    if (args.format == "edf") {
      std::cout << "  Data: " << data_primary.u8string() << "\n";
    } else {
      std::cout << "  Data: " << data_primary.u8string() << " (+ .vmrk/.eeg)\n";
    }
    std::cout << "  Sidecar: " << eeg_json.u8string() << "\n";
    std::cout << "  Channels: " << channels_tsv.u8string() << "\n";
    if (!args.no_events && !rec.events.empty()) {
      std::cout << "  Events: " << events_tsv.u8string() << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
