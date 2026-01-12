#include "qeeg/brainvision_writer.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_vhdr;
  std::string channel_map_path;
  std::string events_out_csv;
  std::vector<std::string> extra_events;
  std::string nf_outdir;
  double fs_csv{0.0};

  BrainVisionBinaryFormat binary_format{BrainVisionBinaryFormat::Float32};
  std::string unit{"uV"};

  // INT_16 settings
  double int16_resolution{0.0};
  int int16_target_max_digital{30000};
};

static void print_help() {
  std::cout
      << "qeeg_export_brainvision_cli\n\n"
      << "Export recordings to BrainVision Core Data Format (.vhdr/.vmrk/.eeg).\n"
      << "This can improve compatibility with a wide range of EEG tools (MNE, FieldTrip, BrainVision Analyzer, etc.).\n\n"
      << "Usage:\n"
      << "  qeeg_export_brainvision_cli --input <in.edf|in.bdf|in.csv|in.txt> --output <out.vhdr> [options]\n\n"
      << "Options:\n"
      << "  --channel-map <map.csv>         Remap/drop channels before writing.\n"
      << "  --fs <Hz>                       Sampling rate hint for CSV/ASCII (0 = infer from time column).\n"
      << "  --extra-events <file.{csv|tsv}> Merge additional events before writing (repeatable).\n"
      << "  --nf-outdir <dir>               Convenience: merge nf_cli derived events from <dir>/nf_derived_events.tsv/.csv\n"
      << "  --events-out <events.csv>       Write events/annotations to CSV (sidecar).\n"
      << "  --float32                       Write IEEE_FLOAT_32 samples (default).\n"
      << "  --int16                         Write INT_16 samples with per-channel resolution.\n"
      << "  --int16-resolution <uV>         Fixed resolution in physical units (uV) for all channels (0 = auto).\n"
      << "  --int16-target-max <N>          Auto-resolution target max digital value (default 30000).\n"
      << "  --unit <text>                   Channel unit string (default 'uV').\n"
      << "  -h, --help                      Show this help.\n\n"
      << "Notes:\n"
      << "  - Output is a 3-file set: out.vhdr, out.vmrk, out.eeg.\n"
      << "  - Events from EDF+/BDF+ annotations (and CSV marker columns) are written into the .vmrk file.\n";
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
      } else if (is_flag(a, "--output", "-o")) {
        args.output_vhdr = require_value(i, argc, argv, a);
      } else if (a == "--channel-map") {
        args.channel_map_path = require_value(i, argc, argv, a);
      } else if (a == "--extra-events") {
        args.extra_events.push_back(require_value(i, argc, argv, a));
      } else if (a == "--nf-outdir") {
        args.nf_outdir = require_value(i, argc, argv, a);
      } else if (a == "--events-out") {
        args.events_out_csv = require_value(i, argc, argv, a);
      } else if (a == "--fs") {
        args.fs_csv = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--float32") {
        args.binary_format = BrainVisionBinaryFormat::Float32;
      } else if (a == "--int16") {
        args.binary_format = BrainVisionBinaryFormat::Int16;
      } else if (a == "--int16-resolution") {
        args.int16_resolution = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--int16-target-max") {
        args.int16_target_max_digital = std::stoi(require_value(i, argc, argv, a));
      } else if (a == "--unit") {
        args.unit = require_value(i, argc, argv, a);
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.output_vhdr.empty()) {
      throw std::runtime_error("Missing required arguments. Need --input and --output.");
    }

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    // Merge additional events (e.g., NF-derived segments) into the recording.
    // Supports qeeg events CSV as well as BIDS-style events.tsv.
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
    merge_events(&rec.events, extra_all);

    if (!args.events_out_csv.empty()) {
      write_events_csv(args.events_out_csv, rec);
    }

    BrainVisionWriterOptions wopts;
    wopts.binary_format = args.binary_format;
    wopts.unit = args.unit;
    wopts.int16_resolution = args.int16_resolution;
    wopts.int16_target_max_digital = args.int16_target_max_digital;

    BrainVisionWriter w;
    w.write(rec, args.output_vhdr, wopts);

    std::cout << "Wrote BrainVision set: " << args.output_vhdr << "\n";
    if (!args.events_out_csv.empty()) {
      std::cout << "Wrote events CSV: " << args.events_out_csv << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
