#include "qeeg/artifacts.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/recording_ops.hpp"
#include "qeeg/segments.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string outdir{"out_clean"};
  double fs_csv{0.0};

  // Optional channel mapping.
  std::string channel_map_path;

  // Artifact detection.
  double window_sec{1.0};
  double step_sec{0.5};
  double baseline_sec{10.0};
  double ptp_z{6.0};
  double rms_z{6.0};
  double kurtosis_z{6.0};
  std::size_t min_bad_channels{1};
  double merge_gap_sec{0.0};
  double pad_sec{0.0};
  double min_good_sec{0.0};

  // Optional preprocessing.
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};

  // Export options.
  bool export_csv{false};
  bool export_edf{false};
  std::size_t max_segments{0}; // 0 => all

  // EDF writer options (when --export-edf)
  double record_duration_seconds{1.0};
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-clean"};
  std::string phys_dim{"uV"};
};

static void print_help() {
  std::cout
      << "qeeg_clean_cli (artifact-based segment extraction)\n\n"
      << "This tool runs the sliding-window artifact detector and extracts *good* (clean)\n"
      << "contiguous segments as defined by the complement of bad windows/segments.\n\n"
      << "Outputs:\n"
      << "  bad_segments.csv   (time ranges flagged as bad)\n"
      << "  good_segments.csv  (time ranges considered good)\n"
      << "  clean_summary.txt  (quick summary)\n"
      << "  (optional) segment_<k>.csv / segment_<k>.edf and segment_<k>_events.csv\n\n"
      << "Usage:\n"
      << "  qeeg_clean_cli --input file.edf --outdir out_clean --pad 0.25 --min-good 2 --export-csv\n"
      << "  qeeg_clean_cli --input file.csv --fs 250 --outdir out_clean --window 1 --step 0.5 --export-edf\n\n"
      << "Options:\n"
      << "  --input PATH            Input EDF/BDF/CSV (CSV requires --fs unless a time column exists)\n"
      << "  --fs HZ                 Sampling rate hint for CSV inputs\n"
      << "  --outdir DIR            Output directory (default: out_clean)\n"
      << "  --channel-map PATH      Rename/drop channels before analysis (new=DROP to drop)\n"
      << "\nArtifact detection:\n"
      << "  --window SEC            Sliding window length (default: 1.0)\n"
      << "  --step SEC              Step between window starts (default: 0.5)\n"
      << "  --baseline SEC          Baseline duration for robust thresholds (default: 10.0)\n"
      << "  --ptp-z Z               Peak-to-peak robust z threshold (default: 6; <=0 disables)\n"
      << "  --rms-z Z               RMS robust z threshold (default: 6; <=0 disables)\n"
      << "  --kurtosis-z Z          Kurtosis robust z threshold (default: 6; <=0 disables)\n"
      << "  --min-bad-channels N    A window is bad if >=N channels are flagged (default: 1)\n"
      << "  --merge-gap SEC         Merge bad windows with gaps <=SEC into segments (default: 0)\n"
      << "  --pad SEC               Expand bad segments by SEC on each side (default: 0)\n"
      << "  --min-good SEC          Drop good segments shorter than SEC (default: 0; keep all)\n"
      << "\nOptional preprocessing:\n"
      << "  --average-reference     Apply common average reference (CAR)\n"
      << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
      << "  --notch-q Q             Notch Q factor (default: 30)\n"
      << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
      << "  --zero-phase            Offline: forward-backward filtering\n"
      << "\nExport:\n"
      << "  --export-csv            Write each good segment as segment_<k>.csv (+ events sidecar)\n"
      << "  --export-edf            Write each good segment as segment_<k>.edf (+ events sidecar)\n"
      << "  --max-segments N        Export at most N good segments (0 = all)\n"
      << "  --record-duration SEC   EDF record duration in seconds (default: 1.0; <=0 => one record)\n"
      << "  --patient-id TEXT       EDF header patient id (default: X)\n"
      << "  --recording-id TEXT     EDF header recording id (default: qeeg-clean)\n"
      << "  --phys-dim TEXT         EDF physical dimension (default: uV)\n"
      << "  -h, --help              Show help\n";
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
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--channel-map" && i + 1 < argc) {
      a.channel_map_path = argv[++i];
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_sec = to_double(argv[++i]);
    } else if (arg == "--step" && i + 1 < argc) {
      a.step_sec = to_double(argv[++i]);
    } else if (arg == "--baseline" && i + 1 < argc) {
      a.baseline_sec = to_double(argv[++i]);
    } else if (arg == "--ptp-z" && i + 1 < argc) {
      a.ptp_z = to_double(argv[++i]);
    } else if (arg == "--rms-z" && i + 1 < argc) {
      a.rms_z = to_double(argv[++i]);
    } else if (arg == "--kurtosis-z" && i + 1 < argc) {
      a.kurtosis_z = to_double(argv[++i]);
    } else if (arg == "--min-bad-channels" && i + 1 < argc) {
      a.min_bad_channels = static_cast<std::size_t>(to_int(argv[++i]));
    } else if (arg == "--merge-gap" && i + 1 < argc) {
      a.merge_gap_sec = to_double(argv[++i]);
    } else if (arg == "--pad" && i + 1 < argc) {
      a.pad_sec = to_double(argv[++i]);
    } else if (arg == "--min-good" && i + 1 < argc) {
      a.min_good_sec = to_double(argv[++i]);
    } else if (arg == "--average-reference") {
      a.average_reference = true;
    } else if (arg == "--notch" && i + 1 < argc) {
      a.notch_hz = to_double(argv[++i]);
    } else if (arg == "--notch-q" && i + 1 < argc) {
      a.notch_q = to_double(argv[++i]);
    } else if (arg == "--bandpass" && i + 2 < argc) {
      a.bandpass_low_hz = to_double(argv[++i]);
      a.bandpass_high_hz = to_double(argv[++i]);
    } else if (arg == "--zero-phase") {
      a.zero_phase = true;
    } else if (arg == "--export-csv") {
      a.export_csv = true;
    } else if (arg == "--export-edf") {
      a.export_edf = true;
    } else if (arg == "--max-segments" && i + 1 < argc) {
      a.max_segments = static_cast<std::size_t>(to_int(argv[++i]));
    } else if (arg == "--record-duration" && i + 1 < argc) {
      a.record_duration_seconds = to_double(argv[++i]);
    } else if (arg == "--patient-id" && i + 1 < argc) {
      a.patient_id = argv[++i];
    } else if (arg == "--recording-id" && i + 1 < argc) {
      a.recording_id = argv[++i];
    } else if (arg == "--phys-dim" && i + 1 < argc) {
      a.phys_dim = argv[++i];
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static void write_segments_csv(const std::string& path,
                               const std::vector<IndexSegment>& segs,
                               double fs,
                               std::size_t total_samples) {
  std::ofstream o(path);
  if (!o) throw std::runtime_error("Failed to write: " + path);

  o << "segment_index,t_start_sec,t_end_sec,duration_sec,sample_start,sample_end,n_samples\n";
  o << std::fixed << std::setprecision(6);
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const auto& s = segs[i];
    const std::size_t ss = std::min(s.start, total_samples);
    const std::size_t ee = std::min(s.end, total_samples);
    const double t0 = static_cast<double>(ss) / fs;
    const double t1 = static_cast<double>(ee) / fs;
    const double dur = std::max(0.0, t1 - t0);
    const std::size_t n = (ee > ss) ? (ee - ss) : 0;
    o << i << "," << t0 << "," << t1 << "," << dur
      << "," << ss << "," << ee << "," << n << "\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.n_channels() == 0 || rec.n_samples() == 0) {
      throw std::runtime_error("Empty recording (no channels or no samples)");
    }
    if (!(rec.fs_hz > 0.0)) {
      throw std::runtime_error("Invalid sampling rate (fs_hz). For CSV inputs, pass --fs or include a time column.");
    }

    if (!args.channel_map_path.empty()) {
      const ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
      if (rec.n_channels() == 0) {
        throw std::runtime_error("All channels were dropped by the channel-map");
      }
    }

    // Optional preprocessing.
    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    const bool do_pre = popt.average_reference || popt.notch_hz > 0.0 ||
                        popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0;
    if (do_pre) {
      preprocess_recording_inplace(rec, popt);
    }

    ArtifactDetectionOptions aopt;
    aopt.window_seconds = args.window_sec;
    aopt.step_seconds = args.step_sec;
    aopt.baseline_seconds = args.baseline_sec;
    aopt.ptp_z = args.ptp_z;
    aopt.rms_z = args.rms_z;
    aopt.kurtosis_z = args.kurtosis_z;
    aopt.min_bad_channels = args.min_bad_channels;

    const ArtifactDetectionResult res = detect_artifacts(rec, aopt);

    const double fs = rec.fs_hz;
    const std::size_t N = rec.n_samples();
    const std::size_t pad_n = (args.pad_sec > 0.0) ? static_cast<std::size_t>(std::llround(args.pad_sec * fs)) : 0;
    const std::size_t min_good_n = (args.min_good_sec > 0.0) ? static_cast<std::size_t>(std::llround(args.min_good_sec * fs)) : 0;

    // Build bad segments (in sample indices) from merged bad windows.
    std::vector<IndexSegment> bad;
    {
      const auto bad_segs = artifact_bad_segments(res, args.merge_gap_sec);
      bad.reserve(bad_segs.size());
      for (const auto& s : bad_segs) {
        std::size_t ss = static_cast<std::size_t>(std::llround(s.t_start_sec * fs));
        std::size_t ee = static_cast<std::size_t>(std::llround(s.t_end_sec * fs));
        if (ss > N) ss = N;
        if (ee > N) ee = N;
        if (ee < ss) ee = ss;

        // Pad and clamp.
        if (pad_n > 0) {
          ss = (ss > pad_n) ? (ss - pad_n) : 0;
          ee = std::min(N, ee + pad_n);
        }
        if (ee > ss) {
          bad.push_back(IndexSegment{ss, ee});
        }
      }
      bad = merge_segments(std::move(bad), 0);
    }

    const std::vector<IndexSegment> good = filter_min_length(complement_segments(bad, N), min_good_n);

    write_segments_csv(args.outdir + "/bad_segments.csv", bad, fs, N);
    write_segments_csv(args.outdir + "/good_segments.csv", good, fs, N);

    // Summary.
    {
      std::ofstream f(args.outdir + "/clean_summary.txt");
      const double dur_total = static_cast<double>(N) / fs;
      std::size_t bad_samples = 0;
      for (const auto& s : bad) bad_samples += s.length();
      const double dur_bad = static_cast<double>(bad_samples) / fs;
      const double frac_bad = (dur_total > 0.0) ? (dur_bad / dur_total) : 0.0;

      f << "qeeg_clean_cli summary\n";
      f << "input: " << args.input_path << "\n";
      f << "fs_hz: " << fs << "\n";
      f << "channels: " << rec.n_channels() << "\n";
      f << "samples: " << N << "\n";
      f << "duration_sec: " << dur_total << "\n\n";

      f << "artifact_window_sec: " << aopt.window_seconds << "\n";
      f << "artifact_step_sec: " << aopt.step_seconds << "\n";
      f << "artifact_baseline_sec: " << aopt.baseline_seconds << "\n";
      f << "ptp_z: " << aopt.ptp_z << "\n";
      f << "rms_z: " << aopt.rms_z << "\n";
      f << "kurtosis_z: " << aopt.kurtosis_z << "\n";
      f << "min_bad_channels: " << aopt.min_bad_channels << "\n";
      f << "merge_gap_sec: " << args.merge_gap_sec << "\n";
      f << "pad_sec: " << args.pad_sec << "\n";
      f << "min_good_sec: " << args.min_good_sec << "\n\n";

      f << "bad_segments: " << bad.size() << "\n";
      f << "good_segments: " << good.size() << "\n";
      f << "bad_duration_sec: " << dur_bad << "\n";
      f << "bad_fraction: " << frac_bad << "\n";
    }

    // Optional segment export.
    if (args.export_csv || args.export_edf) {
      std::size_t exported = 0;
      EDFWriter w;
      EDFWriterOptions wopts;
      wopts.record_duration_seconds = args.record_duration_seconds;
      wopts.patient_id = args.patient_id;
      wopts.recording_id = args.recording_id;
      wopts.physical_dimension = args.phys_dim;

      for (std::size_t i = 0; i < good.size(); ++i) {
        if (args.max_segments > 0 && exported >= args.max_segments) break;
        const auto& seg = good[i];
        if (seg.end <= seg.start) continue;

        EEGRecording srec = slice_recording_samples(rec, seg.start, seg.end, true);
        const std::string stem = args.outdir + "/segment_" + std::to_string(i);

        if (args.export_csv) {
          write_recording_csv(stem + ".csv", srec, true);
        }

        if (args.export_edf) {
          // Make recording-id unique per segment while staying within EDF header limits.
          EDFWriterOptions local = wopts;
          local.recording_id = (wopts.recording_id + "_" + std::to_string(i));
          w.write(srec, stem + ".edf", local);
        }

        if (!srec.events.empty()) {
          write_events_csv(stem + "_events.csv", srec);
        }

        ++exported;
      }
    }

    std::cout << "Wrote cleaning report to: " << args.outdir << "\n";
    std::cout << "  - bad_segments.csv\n";
    std::cout << "  - good_segments.csv\n";
    std::cout << "  - clean_summary.txt\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
