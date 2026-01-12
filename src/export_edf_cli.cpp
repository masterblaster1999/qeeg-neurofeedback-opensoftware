#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_edf;
  std::string channel_map_path;
  std::string events_out_csv;
  std::vector<std::string> extra_events;
  std::string nf_outdir;
  double fs_csv{0.0};
  double record_duration_seconds{1.0};
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-export"};
  std::string phys_dim{"uV"};
  bool plain_edf{false};
  int annotation_spr{0};
};

static void print_help() {
  std::cout
      << "qeeg_export_edf_cli\n\n"
      << "Export recordings to EDF (16-bit) or EDF+ (with annotations).\n"
      << "Useful for interoperability when your source is CSV/ASCII or when you want a clean EDF after\n"
      << "channel remapping and resampling.\n\n"
      << "Usage:\n"
      << "  qeeg_export_edf_cli --input <in.edf|in.bdf|in.csv|in.txt> --output <out.edf> [options]\n\n"
      << "Options:\n"
      << "  --channel-map <map.csv>         Remap/drop channels before writing.\n"
      << "  --fs <Hz>                       Sampling rate hint for CSV/ASCII (0 = infer from time column).\n"
      << "  --record-duration <sec>         EDF datarecord duration in seconds (default 1.0).\n"
      << "                                 If <= 0, a single datarecord is written (no padding).\n"
      << "  --patient-id <text>             EDF header patient id (default 'X').\n"
      << "  --recording-id <text>           EDF header recording id (default 'qeeg-export').\n"
      << "  --phys-dim <text>               Physical dimension string (default 'uV').\n"
      << "  --plain-edf                     Force classic EDF (no EDF+ annotations channel).\n"
      << "  --annotation-spr <N>            Override annotation samples/record for EDF+ (0 = auto).\n"
      << "  --extra-events <file.{csv|tsv}> Merge additional events before writing (repeatable).\n"
      << "  --nf-outdir <dir>               Convenience: merge nf_cli derived events from <dir>/nf_derived_events.tsv/.csv\n"
      << "  --events-out <events.csv>       Write events/annotations to CSV (sidecar).\n"
      << "  -h, --help                      Show this help.\n\n"
      << "Notes:\n"
      << "  - If the input contains events and --plain-edf is NOT set, this tool embeds them as an\n"
      << "    EDF+ \"EDF Annotations\" signal (reserved field \"EDF+C\").\n"
      << "  - If your source is BioTrace+/NeXus, export to EDF or ASCII first (not .bcd/.mbd).\n";
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
        args.output_edf = require_value(i, argc, argv, a);
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
      } else if (a == "--record-duration") {
        args.record_duration_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--patient-id") {
        args.patient_id = require_value(i, argc, argv, a);
      } else if (a == "--recording-id") {
        args.recording_id = require_value(i, argc, argv, a);
      } else if (a == "--phys-dim") {
        args.phys_dim = require_value(i, argc, argv, a);
      } else if (a == "--plain-edf") {
        args.plain_edf = true;
      } else if (a == "--annotation-spr") {
        args.annotation_spr = std::stoi(require_value(i, argc, argv, a));
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.output_edf.empty()) {
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

    EDFWriterOptions wopts;
    wopts.record_duration_seconds = args.record_duration_seconds;
    wopts.patient_id = args.patient_id;
    wopts.recording_id = args.recording_id;
    wopts.physical_dimension = args.phys_dim;
    wopts.write_edfplus_annotations = !args.plain_edf;
    wopts.annotation_samples_per_record = args.annotation_spr;

    EDFWriter w;
    w.write(rec, args.output_edf, wopts);

    std::cout << "Wrote " << ((wopts.write_edfplus_annotations && !rec.events.empty()) ? "EDF+ (with annotations)" : "EDF")
              << ": " << args.output_edf << "\n";
    if (!args.events_out_csv.empty()) {
      std::cout << "Wrote events CSV: " << args.events_out_csv << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
