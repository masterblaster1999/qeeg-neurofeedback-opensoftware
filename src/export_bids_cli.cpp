#include "qeeg/bids.hpp"
#include "qeeg/brainvision_writer.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/channel_qc_io.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/line_noise.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

  // Optional additional events to merge into rec.events before writing
  // *_events.tsv/json. Accepts qeeg events CSV or BIDS events.tsv.
  std::vector<std::string> extra_events;

  // Convenience: load nf_cli-derived events from <nf_outdir>/nf_derived_events.tsv/.csv
  // without having to specify --extra-events explicitly.
  std::string nf_outdir;

  // Optional: mark bad channels in BIDS channels.tsv based on qeeg_channel_qc_cli output.
  // Accepts a path to:
  //   - channel_qc.csv
  //   - bad_channels.txt
  //   - the channel_qc_cli outdir containing those files
  std::string channel_qc;

  // Optional extra columns in *_events.tsv
  bool events_sample{false};
  int events_sample_base{0}; // 0 or 1
  bool events_value{false};

  // Optionally include trial_type Levels mapping in *_events.json.
  bool events_levels{false};

  // Optional: electrode positions (digitized) and coordinate system.
  // When provided, writes *_electrodes.tsv and *_coordsystem.json.
  std::string electrodes_in;     // CSV/TSV with header: name,x,y(,z)[,type,material,impedance]
  // Convenience: generate electrodes.tsv from a qeeg montage spec (builtin or montage CSV).
  // Writes x/y from montage positions and sets z=n/a.
  std::string electrodes_from_montage;
  std::string eeg_coord_system;  // e.g., CapTrak / EEGLAB / EEGLAB-HJ / Other
  std::string eeg_coord_units;   // m|mm|cm|n/a
  std::string eeg_coord_desc;    // required if eeg_coord_system == Other

  bool overwrite{false};
};

static void print_help() {
  std::cout
      << "qeeg_export_bids_cli\n\n"
      << "Export a recording (EDF/BDF/CSV/BrainVision) into a BIDS EEG folder layout.\n"
      << "Writes: data file (EDF or BrainVision) + *_eeg.json + *_channels.tsv (+ events.tsv/json if present).\n"
      << "Optionally writes *_electrodes.tsv and *_coordsystem.json when --electrodes or --electrodes-from-montage is provided.\n\n"
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
      << "  --extra-events <file.{csv|tsv}> Load additional events and merge them before writing events.tsv\n"
      << "                               (repeatable; supports qeeg events CSV or BIDS events.tsv).\n"
      << "  --nf-outdir <dir>               Convenience: merge nf_cli derived events from <dir>/nf_derived_events.tsv/.csv\n"
      << "  --channel-qc <path>            Mark bad channels in *_channels.tsv using qeeg_channel_qc_cli output\n"
      << "                               (path can be channel_qc.csv, bad_channels.txt, or the channel_qc_cli outdir).\n"
      << "  --events-sample                Add a 'sample' column to *_events.tsv (derived from onset * SamplingFrequency).\n"
      << "  --events-sample-base <0|1>     Base for the 'sample' column (default: 0).\n"
      << "  --events-value                 Add a 'value' column (integer parsed from annotation text when possible).\n"
      << "  --events-levels                Include a trial_type Levels map in *_events.json (only if unique values are few).\n"
      << "  --electrodes <file.{tsv|csv}>  Input electrode positions table; writes *_electrodes.tsv and *_coordsystem.json.\n"
      << "                               Header must include: name,x,y (z optional; optional: type,material,impedance).\n"
      << "  --electrodes-from-montage <SPEC> Generate electrodes.tsv from a qeeg montage spec (builtin:standard_1020_19, builtin:standard_1010_61, or montage CSV name,x,y).\n"
      << "                               This writes x/y from the montage and sets z to n/a.\n"
      << "                               Note: BIDS intends electrodes.tsv/coordsystem.json for *digitized* (measured) electrode positions;\n"
      << "                               template/idealized montages may not be appropriate for all workflows.\n"
      << "  --eeg-coord-system <value>     EEGCoordinateSystem for *_coordsystem.json (e.g., CapTrak, EEGLAB, EEGLAB-HJ, Other).\n"
      << "  --eeg-coord-units <m|mm|cm|n/a> EEGCoordinateUnits for *_coordsystem.json.\n"
      << "  --eeg-coord-desc <text>        EEGCoordinateSystemDescription (REQUIRED if --eeg-coord-system Other).\n"
      << "                               If not provided, qeeg_export_bids_cli defaults to Other / n/a with an auto-generated description.\n"
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

static int parse_int_or_throw(const std::string& s, const std::string& flag) {
  try {
    size_t pos = 0;
    const int v = std::stoi(s, &pos, 10);
    if (pos != s.size()) {
      throw std::runtime_error("Trailing characters");
    }
    return v;
  } catch (const std::exception&) {
    throw std::runtime_error("Failed to parse integer value for " + flag + ": '" + s + "'");
  }
}

static Montage load_montage_spec(const std::string& spec) {
  std::string low = to_lower(spec);

  // Convenience aliases
  if (low == "builtin" || low == "default") {
    return Montage::builtin_standard_1020_19();
  }

  // Support: builtin:<key>
  std::string key = low;
  if (starts_with(key, "builtin:")) {
    key = key.substr(std::string("builtin:").size());
  }

  if (key == "standard_1020_19" || key == "1020_19" || key == "standard_1020" || key == "1020") {
    return Montage::builtin_standard_1020_19();
  }
  if (key == "standard_1010_61" || key == "1010_61" || key == "standard_1010" || key == "1010" ||
      key == "standard_10_10" || key == "10_10" || key == "10-10") {
    return Montage::builtin_standard_1010_61();
  }

  return Montage::load_csv(spec);
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
      } else if (a == "--extra-events") {
        args.extra_events.push_back(require_value(i, argc, argv, a));
      } else if (a == "--nf-outdir") {
        args.nf_outdir = require_value(i, argc, argv, a);
      } else if (a == "--channel-qc") {
        args.channel_qc = require_value(i, argc, argv, a);
      } else if (a == "--events-sample") {
        args.events_sample = true;
      } else if (a == "--events-sample-base") {
        args.events_sample_base = parse_int_or_throw(require_value(i, argc, argv, a), a);
      } else if (a == "--events-value") {
        args.events_value = true;
      } else if (a == "--events-levels") {
        args.events_levels = true;
      } else if (a == "--electrodes") {
        args.electrodes_in = require_value(i, argc, argv, a);
      } else if (a == "--electrodes-from-montage") {
        args.electrodes_from_montage = require_value(i, argc, argv, a);
      } else if (a == "--eeg-coord-system") {
        args.eeg_coord_system = require_value(i, argc, argv, a);
      } else if (a == "--eeg-coord-units") {
        args.eeg_coord_units = require_value(i, argc, argv, a);
      } else if (a == "--eeg-coord-desc") {
        args.eeg_coord_desc = require_value(i, argc, argv, a);
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

    if (!(args.events_sample_base == 0 || args.events_sample_base == 1)) {
      throw std::runtime_error("Invalid --events-sample-base (use 0 or 1): " + std::to_string(args.events_sample_base));
    }

    if (!args.electrodes_in.empty() && !args.electrodes_from_montage.empty()) {
      throw std::runtime_error("Use only one of --electrodes or --electrodes-from-montage");
    }

    // Load recording.
    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    // Optional channel map.
    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    // Optional extra events table(s) to merge before exporting.
    // This enables cross-tool workflows, e.g.:
    //   - nf_cli -> nf_derived_events.tsv/.csv -> export as BIDS events.tsv
    //   - hand-edited BIDS events.tsv -> add to exported dataset
    std::vector<std::string> extra_paths = args.extra_events;
    if (!args.nf_outdir.empty()) {
      const auto p = find_nf_derived_events_table(args.nf_outdir);
      if (p) {
        extra_paths.push_back(*p);
      } else {
        std::cerr << "Warning: --nf-outdir provided, but nf_derived_events.tsv/.csv was not found in: "
                  << args.nf_outdir << "\n"
                  << "         Did you run qeeg_nf_cli with --export-derived-events or --biotrace-ui?\n";
      }
    }

    std::vector<AnnotationEvent> extra_all;
    for (const auto& p : extra_paths) {
      const auto extra = read_events_table(p);
      extra_all.insert(extra_all.end(), extra.begin(), extra.end());
    }
    // Also normalizes + de-duplicates source events for deterministic exports.
    merge_events(&rec.events, extra_all);

    // Optional: prepare electrodes + coordsystem sidecars. This can be driven either
    // by a digitized electrode table (CSV/TSV) or by a qeeg montage spec.
    const bool want_electrodes = !args.electrodes_in.empty() || !args.electrodes_from_montage.empty();
    std::vector<BidsElectrode> electrodes;
    std::string electrodes_source;
    if (want_electrodes) {
      if (!args.electrodes_in.empty()) {
        electrodes = load_bids_electrodes_table(args.electrodes_in);
        electrodes_source = args.electrodes_in;
      } else {
        // Generate electrodes from montage positions, matching the *exported* channel names.
        const Montage m = load_montage_spec(args.electrodes_from_montage);
        electrodes.reserve(rec.channel_names.size());
        for (const auto& ch : rec.channel_names) {
          Vec2 p;
          BidsElectrode e;
          e.name = ch;
          if (m.get(ch, &p)) {
            e.x = p.x;
            e.y = p.y;
          }
          // Montage is 2D; z is unknown.
          e.z.reset();
          electrodes.push_back(std::move(e));
        }
        electrodes_source = std::string("montage:") + args.electrodes_from_montage;
      }
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
    const std::string stem_electrodes = format_bids_filename_stem(ent, "electrodes");
    const std::string stem_coordsystem = format_bids_filename_stem(ent, "coordsystem");

    // Output file paths.
    std::filesystem::path eeg_json = eeg_dir / (stem_eeg + ".json");
    std::filesystem::path channels_tsv = eeg_dir / (stem_channels + ".tsv");
    std::filesystem::path events_tsv = eeg_dir / (stem_events + ".tsv");
    std::filesystem::path events_json = eeg_dir / (stem_events + ".json");
    std::filesystem::path electrodes_tsv = eeg_dir / (stem_electrodes + ".tsv");
    std::filesystem::path coordsystem_json = eeg_dir / (stem_coordsystem + ".json");

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

    if (want_electrodes) {
      ensure_writable(electrodes_tsv, args.overwrite);
      ensure_writable(coordsystem_json, args.overwrite);
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
    // Optionally apply channel-level QC results so BIDS validators + downstream tooling
    // can detect and act on bad channels.
    if (!args.channel_qc.empty()) {
      std::string resolved;
      const ChannelQcMap qc = load_channel_qc_any(args.channel_qc, &resolved);

      std::vector<std::string> status(rec.channel_names.size(), "good");
      std::vector<std::string> status_desc(rec.channel_names.size());

      size_t matched = 0;
      size_t bad = 0;
      for (size_t i = 0; i < rec.channel_names.size(); ++i) {
        const std::string key = normalize_channel_name(rec.channel_names[i]);
        if (key.empty()) continue;
        const auto it = qc.find(key);
        if (it == qc.end()) continue;
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
        std::cerr << "Warning: --channel-qc loaded from '" << resolved
                  << "', but no channels matched the exported recording.\n"
                  << "         Ensure qeeg_channel_qc_cli was run on the same (mapped) channels.\n";
      } else {
        std::cout << "Channel QC: loaded '" << resolved << "' (matched=" << matched
                  << ", bad=" << bad << ")\n";
      }

      write_bids_channels_tsv(channels_tsv.u8string(), rec, status, status_desc, args.eeg_reference);
    } else {
      write_bids_channels_tsv(channels_tsv.u8string(), rec, /*status=*/{}, /*status_desc=*/{}, args.eeg_reference);
    }

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
      BidsEventsTsvOptions ev_opts;
      ev_opts.include_sample = args.events_sample;
      ev_opts.sample_index_base = args.events_sample_base;
      ev_opts.include_value = args.events_value;
      ev_opts.include_trial_type_levels = args.events_levels;

      write_bids_events_tsv(events_tsv.u8string(), rec.events, ev_opts, rec.fs_hz);
      write_bids_events_json(events_json.u8string(), ev_opts, rec.events);
    }

    // Optional: electrodes.tsv + coordsystem.json (electrode positions).
    if (want_electrodes) {
      BidsCoordsystemJsonEegMetadata cs;
      cs.eeg_coordinate_system = trim(args.eeg_coord_system);
      cs.eeg_coordinate_units = trim(args.eeg_coord_units);
      cs.eeg_coordinate_system_description = trim(args.eeg_coord_desc);

      // Provide sensible BIDS-compliant defaults so users can export quick-and-dirty
      // electrode layouts (e.g., 2D montage coordinates) without extra flags.
      if (cs.eeg_coordinate_system.empty()) cs.eeg_coordinate_system = "Other";
      if (cs.eeg_coordinate_units.empty()) cs.eeg_coordinate_units = "n/a";
      if (cs.eeg_coordinate_system == "Other" && cs.eeg_coordinate_system_description.empty()) {
        cs.eeg_coordinate_system_description =
            "Auto-generated by qeeg_export_bids_cli from " + electrodes_source +
            ". Provide --eeg-coord-system/--eeg-coord-units for digitized coordinates.";
      }

      write_bids_coordsystem_json(coordsystem_json.u8string(), cs);
      write_bids_electrodes_tsv(electrodes_tsv.u8string(), electrodes);
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
    if (want_electrodes) {
      std::cout << "  Electrodes: " << electrodes_tsv.u8string() << "\n";
      std::cout << "  Coordsystem: " << coordsystem_json.u8string() << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
