#include "qeeg/bdf_writer.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_bdf;
  std::string channel_map_path;
  std::string events_out_csv;
  double fs_csv{0.0};
  double record_duration_seconds{1.0};
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-export"};
  std::string phys_dim{"uV"};
  bool plain_bdf{false};
  int annotation_spr{0};
};

static void print_help() {
  std::cout
      << "qeeg_export_bdf_cli\n\n"
      << "Export recordings to BDF (24-bit) or BDF+ (with annotations).\n"
      << "Useful when you want to keep 24-bit dynamic range for interoperability with tools that\n"
      << "expect BioSemi-style BDF, while still benefiting from channel mapping / resampling.\n\n"
      << "Usage:\n"
      << "  qeeg_export_bdf_cli --input <in.edf|in.bdf|in.csv|in.txt> --output <out.bdf> [options]\n\n"
      << "Options:\n"
      << "  --channel-map <map.csv>         Remap/drop channels before writing.\n"
      << "  --fs <Hz>                       Sampling rate hint for CSV/ASCII (0 = infer from time column).\n"
      << "  --record-duration <sec>         BDF datarecord duration in seconds (default 1.0).\n"
      << "                                 If <= 0, a single datarecord is written (no padding).\n"
      << "  --patient-id <text>             BDF header patient id (default 'X').\n"
      << "  --recording-id <text>           BDF header recording id (default 'qeeg-export').\n"
      << "  --phys-dim <text>               Physical dimension string (default 'uV').\n"
      << "  --plain-bdf                     Force classic BDF (no BDF+ annotations channel).\n"
      << "  --annotation-spr <N>            Override annotation samples/record for BDF+ (0 = auto).\n"
      << "  --events-out <events.csv>       Write events/annotations to CSV (sidecar).\n"
      << "  -h, --help                      Show this help.\n\n"
      << "Notes:\n"
      << "  - If the input contains events and --plain-bdf is NOT set, this tool embeds them as a\n"
      << "    BDF+ \"BDF Annotations\" signal (reserved field starts with \"BDF+C\").\n"
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
        args.output_bdf = require_value(i, argc, argv, a);
      } else if (a == "--channel-map") {
        args.channel_map_path = require_value(i, argc, argv, a);
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
      } else if (a == "--plain-bdf") {
        args.plain_bdf = true;
      } else if (a == "--annotation-spr") {
        args.annotation_spr = std::stoi(require_value(i, argc, argv, a));
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.output_bdf.empty()) {
      throw std::runtime_error("Missing required arguments. Need --input and --output.");
    }

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    if (!args.events_out_csv.empty()) {
      write_events_csv(args.events_out_csv, rec);
    }

    BDFWriterOptions wopts;
    wopts.record_duration_seconds = args.record_duration_seconds;
    wopts.patient_id = args.patient_id;
    wopts.recording_id = args.recording_id;
    wopts.physical_dimension = args.phys_dim;
    wopts.write_bdfplus_annotations = !args.plain_bdf;
    wopts.annotation_samples_per_record = args.annotation_spr;

    BDFWriter w;
    w.write(rec, args.output_bdf, wopts);

    std::cout << "Wrote " << ((wopts.write_bdfplus_annotations && !rec.events.empty()) ? "BDF+ (with annotations)" : "BDF")
              << ": " << args.output_bdf << "\n";
    if (!args.events_out_csv.empty()) {
      std::cout << "Wrote events CSV: " << args.events_out_csv << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
