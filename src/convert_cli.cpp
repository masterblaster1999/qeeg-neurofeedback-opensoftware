#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_csv;
  std::string channel_map_path;
  std::string channel_map_template_out;
  std::string events_out_csv;
  std::string events_out_tsv;
  std::vector<std::string> extra_events;
  std::string nf_outdir;
  double fs_csv{0.0};
  bool write_time{true};
};

static void print_help() {
  std::cout
      << "qeeg_convert_cli\n\n"
      << "Convert EEG recordings to a simple, analysis-friendly CSV.\n"
      << "Intended for interoperability with BioTrace+/NeXus exports (EDF/BDF/ASCII).\n\n"
      << "Usage:\n"
      << "  qeeg_convert_cli --input <path> --output <out.csv> [options]\n"
      << "  qeeg_convert_cli --input <path> --channel-map-template <map.csv> [options]\n\n"
      << "Input formats:\n"
      << "  .edf/.edf+/.bdf/.bdf+   (recommended for BioTrace+ exports)\n"
      << "  .csv/.txt/.tsv/.asc     (ASCII exports)\n\n"
      << "Options:\n"
      << "  --fs <Hz>                    Sampling rate for CSV/TXT inputs (if no time column).\n"
      << "  --channel-map <path>         CSV mapping file to rename/drop channels.\n"
      << "                               Format: old,new   (or old=new). Use new=DROP to drop.\n"
      << "  --channel-map-template <path>Write a template mapping CSV for this recording (old,new).\n"
      << "  --events-out <path>          Write annotations/events to CSV (onset_sec,duration_sec,text).\n"
      << "  --events-out-tsv <path>      Write annotations/events to TSV (onset,duration,trial_type).\n"
      << "  --extra-events <file.{csv|tsv}> Merge additional events before writing (repeatable).\n"
      << "  --nf-outdir <dir|file>       Convenience: also merge nf_derived_events.tsv/.csv from a qeeg_nf_cli --outdir.\n"
      << "  --no-time                    Do not write a leading time column.\n"
      << "  -h, --help                   Show help.\n";
}

static void ensure_parent_dir(const std::string& path) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  const std::filesystem::path parent = p.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_path = argv[++i];
    } else if ((arg == "--output" || arg == "--out") && i + 1 < argc) {
      a.output_csv = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--channel-map" && i + 1 < argc) {
      a.channel_map_path = argv[++i];
    } else if (arg == "--channel-map-template" && i + 1 < argc) {
      a.channel_map_template_out = argv[++i];
    } else if (arg == "--events-out" && i + 1 < argc) {
      a.events_out_csv = argv[++i];
    } else if (arg == "--events-out-tsv" && i + 1 < argc) {
      a.events_out_tsv = argv[++i];
    } else if (arg == "--extra-events" && i + 1 < argc) {
      a.extra_events.push_back(argv[++i]);
    } else if (arg == "--nf-outdir" && i + 1 < argc) {
      a.nf_outdir = argv[++i];
    } else if (arg == "--no-time") {
      a.write_time = false;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }

  if (a.input_path.empty()) {
    print_help();
    throw std::runtime_error("Missing required --input");
  }

  if (a.output_csv.empty() && a.channel_map_template_out.empty() && a.events_out_csv.empty() && a.events_out_tsv.empty()) {
    print_help();
    throw std::runtime_error("Provide --output, --channel-map-template, and/or an --events-out option");
  }

  return a;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_template_out.empty()) {
      write_channel_map_template(args.channel_map_template_out, rec);
    }

    if (!args.channel_map_path.empty()) {
      const ChannelMap m = load_channel_map_file(args.channel_map_path);
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

    if (!args.output_csv.empty()) {
      if (rec.fs_hz <= 0.0) {
        throw std::runtime_error(
            "Invalid sampling rate (fs_hz). If converting CSV/TXT inputs, pass --fs <Hz>.");
      }
      if (rec.n_channels() == 0 || rec.n_samples() == 0) {
        throw std::runtime_error("Empty recording (no channels or no samples).");
      }

      ensure_parent_dir(args.output_csv);
      write_recording_csv(args.output_csv, rec, args.write_time);
    }

    if (!args.events_out_csv.empty()) {
      ensure_parent_dir(args.events_out_csv);
      write_events_csv(args.events_out_csv, rec);
    }

    if (!args.events_out_tsv.empty()) {
      ensure_parent_dir(args.events_out_tsv);
      write_events_tsv(args.events_out_tsv, rec);
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
