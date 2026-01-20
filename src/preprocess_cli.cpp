#include "qeeg/channel_map.hpp"
#include "qeeg/bdf_writer.hpp"
#include "qeeg/brainvision_writer.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/line_noise.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/recording_ops.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/run_meta.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_path;
  double fs_csv{0.0};

  std::string channel_map_path;
  std::string events_out_csv;

  // Preprocessing
  bool average_reference{false};
  bool zero_phase{false};

  // Manual notch
  double notch_hz{0.0};
  bool notch_specified{false};
  double notch_q{30.0};

  // Auto notch (if enabled and no manual notch was provided)
  bool auto_notch{false};
  double auto_notch_seconds{30.0};
  double auto_notch_min_ratio{3.0};
  size_t auto_notch_max_channels{8};
  size_t auto_notch_nperseg{1024};
  double auto_notch_overlap{0.5};

  // Bandpass
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};

  // Output options (EDF)
  double record_duration_seconds{1.0};
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-preprocess"};
  std::string phys_dim{"uV"};
  // If true, disable writing EDF+/BDF+ annotations even if events exist.
  // (EDF: omit "EDF Annotations" signal; BDF: omit "BDF Annotations" signal.)
  bool plain_edf{false};
  int annotation_spr{0};

  // Output options (BrainVision)
  BrainVisionBinaryFormat bv_binary_format{BrainVisionBinaryFormat::Float32};
  std::string bv_unit{"uV"};
  double bv_int16_resolution{0.0};
  int bv_int16_target_max_digital{30000};

  // Output options (CSV)
  bool write_time{true};
};

static void print_help() {
  std::cout
      << "qeeg_preprocess_cli\n\n"
      << "Apply basic preprocessing (CAR, notch, bandpass) and export to EDF/EDF+, BDF/BDF+, BrainVision, or CSV.\n"
      << "Designed for interoperability with BioTrace+/NeXus exports and quick dataset hygiene.\n\n"
      << "Usage:\n"
      << "  qeeg_preprocess_cli --input <file|dir|*_run_meta.json> --output <out.edf|out.bdf|out.vhdr|out.csv> [options]\n\n"
      << "Input formats:\n"
      << "  .edf/.edf+/.bdf/.bdf+   (recommended)\n"
      << "  .csv/.txt/.tsv/.asc     (ASCII exports; pass --fs if there is no time column)\n\n"
      << "Preprocessing options:\n"
      << "  --channel-map <map.csv>      Remap/drop channels before preprocessing.\n"
      << "  --average-reference          Apply common average reference (CAR).\n"
      << "  --notch <Hz>                 Apply a notch filter at Hz (e.g., 50 or 60).\n"
      << "  --notch-q <Q>                Notch Q factor (default 30).\n"
      << "  --auto-notch                 Auto-detect 50/60 Hz line noise and apply a notch if strong.\n"
      << "  --auto-notch-seconds <S>     Seconds used for auto-notch detection (default 30; <=0 uses full).\n"
      << "  --auto-notch-min-ratio <R>   Minimum median PSD peak ratio required (default 3).\n"
      << "  --auto-notch-max-ch <N>      Max channels used for detection (default 8).\n"
      << "  --auto-notch-nperseg <N>     Welch nperseg for detection (default 1024).\n"
      << "  --auto-notch-overlap <F>     Welch overlap fraction in [0,1) for detection (default 0.5).\n"
      << "  --bandpass <LO> <HI>         Apply a simple bandpass (highpass LO then lowpass HI).\n"
      << "  --zero-phase                 Offline forward-backward filtering (less phase distortion).\n\n"
      << "Input/CSV options:\n"
      << "  --fs <Hz>                    Sampling rate hint for CSV/ASCII (0 = infer from time column).\n\n"
      << "Output (EDF/BDF) options:\n"
      << "  --record-duration <sec>      EDF/BDF datarecord duration (default 1.0; <=0 writes a single record).\n"
      << "  --patient-id <text>          EDF/BDF header patient id (default 'X').\n"
      << "  --recording-id <text>        EDF/BDF header recording id (default 'qeeg-preprocess').\n"
      << "  --phys-dim <text>            Physical dimension string (default 'uV').\n"
      << "  --plain-edf                  Force classic EDF/BDF (no EDF+/BDF+ annotations channel).\n"
      << "  --plain-bdf                  Alias for --plain-edf when writing .bdf outputs.\n"
      << "  --annotation-spr <N>         Override annotation samples/record (0 = auto).\n\n"
      << "Output (BrainVision) options (only used when --output ends with .vhdr):\n"
      << "  --float32                    Write IEEE_FLOAT_32 samples (default).\n"
      << "  --int16                      Write INT_16 samples with per-channel resolution.\n"
      << "  --int16-resolution <uV>      Fixed resolution in physical units (uV) for all channels (0 = auto).\n"
      << "  --int16-target-max <N>       Auto-resolution target max digital value (default 30000).\n"
      << "  --unit <text>                Channel unit string (default 'uV').\n\n"
      << "Output (CSV) options:\n"
      << "  --no-time                    Do not write a leading time column.\n\n"
      << "Events:\n"
      << "  --events-out <events.csv>    Write events/annotations to a sidecar CSV.\n\n"
      << "Other:\n"
      << "  -h, --help                   Show help.\n";
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

static void ensure_parent_dir(const std::string& path) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  const std::filesystem::path parent = p.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
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
      const std::string a = argv[i];

      if (is_flag(a, "-h", "--help")) {
        print_help();
        return 0;
      } else if (is_flag(a, "--input", "-i")) {
        args.input_path = require_value(i, argc, argv, a);
      } else if (is_flag(a, "--output", "-o")) {
        args.output_path = require_value(i, argc, argv, a);
      } else if (a == "--fs") {
        args.fs_csv = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--channel-map") {
        args.channel_map_path = require_value(i, argc, argv, a);
      } else if (a == "--events-out") {
        args.events_out_csv = require_value(i, argc, argv, a);
      } else if (a == "--average-reference") {
        args.average_reference = true;
      } else if (a == "--zero-phase") {
        args.zero_phase = true;
      } else if (a == "--notch") {
        args.notch_hz = std::stod(require_value(i, argc, argv, a));
        args.notch_specified = true;
      } else if (a == "--notch-q") {
        args.notch_q = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--auto-notch") {
        args.auto_notch = true;
      } else if (a == "--auto-notch-seconds") {
        args.auto_notch_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--auto-notch-min-ratio") {
        args.auto_notch_min_ratio = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--auto-notch-max-ch") {
        args.auto_notch_max_channels = static_cast<size_t>(std::stoll(require_value(i, argc, argv, a)));
      } else if (a == "--auto-notch-nperseg") {
        args.auto_notch_nperseg = static_cast<size_t>(std::stoll(require_value(i, argc, argv, a)));
      } else if (a == "--auto-notch-overlap") {
        args.auto_notch_overlap = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--bandpass") {
        if (i + 2 >= argc) throw std::runtime_error("Missing values for --bandpass <LO> <HI>");
        args.bandpass_low_hz = std::stod(argv[++i]);
        args.bandpass_high_hz = std::stod(argv[++i]);
      } else if (a == "--record-duration") {
        args.record_duration_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--patient-id") {
        args.patient_id = require_value(i, argc, argv, a);
      } else if (a == "--recording-id") {
        args.recording_id = require_value(i, argc, argv, a);
      } else if (a == "--phys-dim") {
        args.phys_dim = require_value(i, argc, argv, a);
      } else if (a == "--plain-edf" || a == "--plain-bdf") {
        args.plain_edf = true;
      } else if (a == "--annotation-spr") {
        args.annotation_spr = std::stoi(require_value(i, argc, argv, a));
      } else if (a == "--float32") {
        args.bv_binary_format = BrainVisionBinaryFormat::Float32;
      } else if (a == "--int16") {
        args.bv_binary_format = BrainVisionBinaryFormat::Int16;
      } else if (a == "--int16-resolution") {
        args.bv_int16_resolution = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--int16-target-max") {
        args.bv_int16_target_max_digital = std::stoi(require_value(i, argc, argv, a));
      } else if (a == "--unit") {
        args.bv_unit = require_value(i, argc, argv, a);
      } else if (a == "--no-time") {
        args.write_time = false;
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.output_path.empty()) {
      throw std::runtime_error("Missing required arguments. Need --input and --output.");
    }

    const ResolvedInputPath in = resolve_input_recording_path(args.input_path);
    if (!in.note.empty()) {
      std::cerr << in.note << "\n";
    }
    args.input_path = in.path;

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    PreprocessOptions opt;
    opt.average_reference = args.average_reference;
    opt.notch_hz = args.notch_specified ? args.notch_hz : 0.0;
    opt.notch_q = args.notch_q;
    opt.bandpass_low_hz = args.bandpass_low_hz;
    opt.bandpass_high_hz = args.bandpass_high_hz;
    opt.zero_phase = args.zero_phase;

    if (args.auto_notch && !args.notch_specified) {
      EEGRecording probe = rec;
      if (args.auto_notch_seconds > 0.0) {
        probe = slice_recording_time(rec, /*start_sec=*/0.0, /*duration_sec=*/args.auto_notch_seconds,
                                     /*adjust_events=*/false);
      }

      WelchOptions wopt;
      wopt.nperseg = args.auto_notch_nperseg;
      wopt.overlap_fraction = args.auto_notch_overlap;

      const LineNoiseEstimate est = detect_line_noise_50_60(
          probe, wopt, args.auto_notch_max_channels, args.auto_notch_min_ratio);

      if (est.recommended_hz > 0.0) {
        opt.notch_hz = est.recommended_hz;
        std::cout << "Auto-notch: recommended " << est.recommended_hz << " Hz (median ratio="
                  << est.strength_ratio << ")\n";
      } else {
        std::cout << "Auto-notch: no strong 50/60 Hz peak found (min ratio=" << args.auto_notch_min_ratio
                  << ")\n";
      }
    }

    preprocess_recording_inplace(rec, opt);

    if (!args.events_out_csv.empty()) {
      ensure_parent_dir(args.events_out_csv);
      write_events_csv(args.events_out_csv, rec);
    }

    const std::string out_low = to_lower(args.output_path);
    if (ends_with(out_low, ".edf") || ends_with(out_low, ".edf+") || ends_with(out_low, ".rec")) {
      ensure_parent_dir(args.output_path);

      EDFWriterOptions wopts;
      wopts.record_duration_seconds = args.record_duration_seconds;
      wopts.patient_id = args.patient_id;
      wopts.recording_id = args.recording_id;
      wopts.physical_dimension = args.phys_dim;
      wopts.write_edfplus_annotations = !args.plain_edf;
      wopts.annotation_samples_per_record = args.annotation_spr;

      EDFWriter w;
      w.write(rec, args.output_path, wopts);

      std::cout << "Wrote "
                << ((wopts.write_edfplus_annotations && !rec.events.empty()) ? "EDF+ (with annotations)" : "EDF")
                << ": " << args.output_path << "\n";
    } else if (ends_with(out_low, ".bdf") || ends_with(out_low, ".bdf+")) {
      ensure_parent_dir(args.output_path);

      BDFWriterOptions wopts;
      wopts.record_duration_seconds = args.record_duration_seconds;
      wopts.patient_id = args.patient_id;
      wopts.recording_id = args.recording_id;
      wopts.physical_dimension = args.phys_dim;
      wopts.write_bdfplus_annotations = !args.plain_edf;
      wopts.annotation_samples_per_record = args.annotation_spr;

      BDFWriter w;
      w.write(rec, args.output_path, wopts);

      std::cout << "Wrote "
                << ((wopts.write_bdfplus_annotations && !rec.events.empty()) ? "BDF+ (with annotations)" : "BDF")
                << ": " << args.output_path << "\n";
    } else if (ends_with(out_low, ".vhdr")) {
      ensure_parent_dir(args.output_path);

      BrainVisionWriterOptions wopts;
      wopts.binary_format = args.bv_binary_format;
      wopts.unit = args.bv_unit;
      wopts.int16_resolution = args.bv_int16_resolution;
      wopts.int16_target_max_digital = args.bv_int16_target_max_digital;

      BrainVisionWriter w;
      w.write(rec, args.output_path, wopts);

      std::cout << "Wrote BrainVision set: " << args.output_path << "\n";
    } else if (ends_with(out_low, ".csv")) {
      ensure_parent_dir(args.output_path);
      write_recording_csv(args.output_path, rec, args.write_time);
      std::cout << "Wrote CSV: " << args.output_path << "\n";
    } else {
      throw std::runtime_error("Unsupported output extension (use .edf/.bdf/.vhdr or .csv): " + args.output_path);
    }

    if (!args.events_out_csv.empty()) {
      std::cout << "Wrote events CSV: " << args.events_out_csv << "\n";
    }

    // Write run meta (enables CLI chaining: later tools can accept this run meta or outdir as --input).
    try {
      const std::filesystem::path out_path = std::filesystem::u8path(args.output_path);
      const std::filesystem::path out_dir = out_path.parent_path();
      const std::string outdir_str = out_dir.empty() ? std::string(".") : out_dir.u8string();

      std::vector<std::string> outs;
      outs.push_back(out_path.filename().u8string());
      if (ends_with(to_lower(args.output_path), ".vhdr")) {
        // BrainVisionWriter also emits .eeg and .vmrk next to the .vhdr.
        const std::string stem = out_path.stem().u8string();
        outs.push_back(stem + ".eeg");
        outs.push_back(stem + ".vmrk");
      }
      if (!args.events_out_csv.empty()) {
        const std::filesystem::path ev = std::filesystem::u8path(args.events_out_csv);
        if (ev.parent_path() == out_dir) {
          outs.push_back(ev.filename().u8string());
        }
      }

      const std::string meta_name = "preprocess_run_meta.json";
      const std::string meta_path = outdir_str + "/" + meta_name;
      outs.push_back(meta_name);

      if (!write_run_meta_json(meta_path, "qeeg_preprocess_cli", outdir_str, args.input_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "Warning: failed to write preprocess_run_meta.json: " << e.what() << "\n";
    }

    // Echo effective preprocessing summary.
    std::cout << "Preprocess summary:"
              << " average_reference=" << (opt.average_reference ? "on" : "off")
              << " notch_hz=" << opt.notch_hz
              << " bandpass_low_hz=" << opt.bandpass_low_hz
              << " bandpass_high_hz=" << opt.bandpass_high_hz
              << " zero_phase=" << (opt.zero_phase ? "on" : "off") << "\n";

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
