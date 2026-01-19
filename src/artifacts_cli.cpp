#include "qeeg/artifacts.hpp"
#include "qeeg/bids.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_artifacts"};

  double fs_csv{0.0};

  double window_sec{1.0};
  double step_sec{0.5};
  double baseline_sec{10.0};

  double ptp_z{6.0};
  double rms_z{6.0};
  double kurtosis_z{6.0};
  double ptp_z_low{0.0};
  double rms_z_low{0.0};
  size_t min_bad_channels{1};

  // How much of a time gap (in seconds) to allow when merging overlapping
  // bad windows into contiguous artifact segments.
  double merge_gap_sec{0.0};

  // Optional: write artifact_events.tsv / artifact_events.json as a
  // BIDS-style events file describing the merged artifact segments.
  bool export_bids_events{false};

  bool average_reference{false};

  // Optional preprocessing filters
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static void print_help() {
  std::cout
    << "qeeg_artifacts_cli (first pass artifact window detection)\n\n"
    << "Usage:\n"
    << "  qeeg_artifacts_cli --input file.edf --outdir out_art --window 1.0 --step 0.5\n"
    << "  qeeg_artifacts_cli --input file.csv --fs 250 --outdir out_art --baseline 10\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                 Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR            Output directory (default: out_artifacts)\n"
    << "  --window SEC            Sliding window length (default: 1.0)\n"
    << "  --step SEC              Step between window starts (default: 0.5)\n"
    << "  --baseline SEC          Baseline duration for robust thresholds (default: 10)\n"
    << "  --ptp-z Z               Peak-to-peak robust z threshold (default: 6; <=0 disables)\n"
    << "  --rms-z Z               RMS robust z threshold (default: 6; <=0 disables)\n"
    << "  --kurtosis-z Z          Kurtosis robust z threshold (default: 6; <=0 disables)\n"
    << "  --ptp-z-low Z           Low PTP robust z threshold for flatline/dropouts (default: 0; <=0 disables)\n"
    << "  --rms-z-low Z           Low RMS robust z threshold for flatline/dropouts (default: 0; <=0 disables)\n"
    << "  --min-bad-channels N    Mark window bad if >=N channels are flagged (default: 1)\n"
    << "  --merge-gap SEC         Merge bad windows with gaps <=SEC into segments (default: 0)\n"
    << "  --export-bids-events     Write artifact_events.tsv and artifact_events.json (merged segments)\n"
    << "  --average-reference      Apply common average reference across channels\n"
    << "  --notch HZ               Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q              Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI         Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase             Offline: forward-backward filtering (less phase distortion)\n"
    << "  -h, --help              Show this help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_path = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
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
    } else if (arg == "--ptp-z-low" && i + 1 < argc) {
      a.ptp_z_low = to_double(argv[++i]);
    } else if (arg == "--rms-z-low" && i + 1 < argc) {
      a.rms_z_low = to_double(argv[++i]);
    } else if (arg == "--min-bad-channels" && i + 1 < argc) {
      a.min_bad_channels = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--merge-gap" && i + 1 < argc) {
      a.merge_gap_sec = to_double(argv[++i]);
    } else if (arg == "--export-bids-events") {
      a.export_bids_events = true;
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
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string join_bad_channels(const std::vector<std::string>& names,
                                     const std::vector<ArtifactChannelMetrics>& metrics) {
  std::ostringstream oss;
  bool first = true;
  for (size_t i = 0; i < metrics.size() && i < names.size(); ++i) {
    if (!metrics[i].bad) continue;
    if (!first) oss << ";";
    first = false;
    oss << names[i];
  }
  return oss.str();
}

static std::string join_bad_channel_counts(const std::vector<std::string>& names,
                                           const std::vector<size_t>& counts) {
  std::ostringstream oss;
  bool first = true;
  const size_t m = std::min(names.size(), counts.size());
  for (size_t i = 0; i < m; ++i) {
    if (counts[i] == 0) continue;
    if (!first) oss << ";";
    first = false;
    oss << names[i] << ":" << counts[i];
  }
  return oss.str();
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.n_channels() == 0) throw std::runtime_error("Recording must have at least 1 channel");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

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
      std::cout << "Preprocessing:\n";
      if (popt.average_reference) {
        std::cout << "  - CAR (average reference)\n";
      }
      if (popt.notch_hz > 0.0) {
        std::cout << "  - notch " << popt.notch_hz << " Hz (Q=" << popt.notch_q << ")\n";
      }
      if (popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0) {
        std::cout << "  - bandpass " << popt.bandpass_low_hz << ".." << popt.bandpass_high_hz << " Hz\n";
      }
      if (popt.zero_phase && (popt.notch_hz > 0.0 || popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0)) {
        std::cout << "  - zero-phase (forward-backward)\n";
      }
      preprocess_recording_inplace(rec, popt);
    }

    ArtifactDetectionOptions aopt;
    aopt.window_seconds = args.window_sec;
    aopt.step_seconds = args.step_sec;
    aopt.baseline_seconds = args.baseline_sec;
    aopt.ptp_z = args.ptp_z;
    aopt.rms_z = args.rms_z;
    aopt.kurtosis_z = args.kurtosis_z;
    aopt.ptp_z_low = args.ptp_z_low;
    aopt.rms_z_low = args.rms_z_low;
    aopt.min_bad_channels = args.min_bad_channels;

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Artifact windows: window=" << aopt.window_seconds << "s, step=" << aopt.step_seconds
              << "s, baseline=" << aopt.baseline_seconds << "s\n";

    const ArtifactDetectionResult res = detect_artifacts(rec, aopt);

    const auto ch_bad_counts = artifact_bad_counts_per_channel(res);
    const auto segments = artifact_bad_segments(res, args.merge_gap_sec);

    // Write per-window summary.
    {
      std::ofstream f(args.outdir + "/artifact_windows.csv");
      f << "window_index,t_start_sec,t_end_sec,bad,bad_channel_count,max_ptp_z,max_rms_z,max_kurtosis_z,bad_channels\n";
      for (size_t wi = 0; wi < res.windows.size(); ++wi) {
        const auto& w = res.windows[wi];
        double max_ptp = 0.0, max_rms = 0.0, max_kurt = 0.0;
        for (const auto& ch : w.channels) {
          max_ptp = std::max(max_ptp, ch.ptp_z);
          max_rms = std::max(max_rms, ch.rms_z);
          max_kurt = std::max(max_kurt, ch.kurtosis_z);
        }
        f << wi << "," << w.t_start_sec << "," << w.t_end_sec << "," << (w.bad ? 1 : 0)
          << "," << w.bad_channel_count << "," << max_ptp << "," << max_rms << "," << max_kurt
          << "," << join_bad_channels(res.channel_names, w.channels) << "\n";
      }
    }

    // Write per-window per-channel details.
    {
      std::ofstream f(args.outdir + "/artifact_channels.csv");
      f << "window_index,t_start_sec,t_end_sec,channel,bad,ptp,rms,kurtosis,ptp_z,rms_z,kurtosis_z\n";
      for (size_t wi = 0; wi < res.windows.size(); ++wi) {
        const auto& w = res.windows[wi];
        for (size_t ch = 0; ch < w.channels.size() && ch < res.channel_names.size(); ++ch) {
          const auto& m = w.channels[ch];
          f << wi << "," << w.t_start_sec << "," << w.t_end_sec << "," << res.channel_names[ch]
            << "," << (m.bad ? 1 : 0)
            << "," << m.ptp << "," << m.rms << "," << m.kurtosis
            << "," << m.ptp_z << "," << m.rms_z << "," << m.kurtosis_z << "\n";
        }
      }
    }

    // Write a tiny human-readable summary.
    {
      std::ofstream f(args.outdir + "/artifact_summary.txt");
      const double total = static_cast<double>(res.windows.size());
      const double bad = static_cast<double>(res.total_bad_windows);
      const double frac = (total > 0.0) ? (bad / total) : 0.0;
      f << "qeeg_artifacts_cli summary\n";
      f << "input: " << args.input_path << "\n";
      f << "fs_hz: " << rec.fs_hz << "\n";
      f << "channels: " << rec.n_channels() << "\n";
      f << "samples: " << rec.n_samples() << "\n\n";
      f << "window_sec: " << res.opt.window_seconds << "\n";
      f << "step_sec: " << res.opt.step_seconds << "\n";
      f << "baseline_sec: " << res.opt.baseline_seconds << "\n";
      f << "ptp_z: " << res.opt.ptp_z << "\n";
      f << "rms_z: " << res.opt.rms_z << "\n";
      f << "kurtosis_z: " << res.opt.kurtosis_z << "\n";
      f << "ptp_z_low: " << res.opt.ptp_z_low << "\n";
      f << "rms_z_low: " << res.opt.rms_z_low << "\n";
      f << "min_bad_channels: " << res.opt.min_bad_channels << "\n\n";
      f << "windows_total: " << res.windows.size() << "\n";
      f << "windows_bad: " << res.total_bad_windows << "\n";
      f << "bad_fraction: " << frac << "\n";
      f << "segments: " << segments.size() << "\n";
    }

    // Write per-channel summary.
    {
      std::ofstream f(args.outdir + "/artifact_channel_summary.csv");
      const double total = static_cast<double>(res.windows.size());
      f << "channel,bad_window_count,bad_window_fraction\n";
      for (size_t ch = 0; ch < res.channel_names.size() && ch < ch_bad_counts.size(); ++ch) {
        const double frac = (total > 0.0) ? (static_cast<double>(ch_bad_counts[ch]) / total) : 0.0;
        f << res.channel_names[ch] << "," << ch_bad_counts[ch] << "," << frac << "\n";
      }
    }

    // Write merged segments.
    {
      std::ofstream f(args.outdir + "/artifact_segments.csv");
      f << "segment_index,t_start_sec,t_end_sec,duration_sec,first_window,last_window,windows,max_bad_channels,bad_channel_counts\n";
      for (size_t si = 0; si < segments.size(); ++si) {
        const auto& s = segments[si];
        const double dur = std::max(0.0, s.t_end_sec - s.t_start_sec);
        f << si << "," << s.t_start_sec << "," << s.t_end_sec << "," << dur
          << "," << s.first_window << "," << s.last_window
          << "," << s.window_count << "," << s.max_bad_channels
          << "," << join_bad_channel_counts(res.channel_names, s.bad_windows_per_channel)
          << "\n";
      }
    }

    // Optional: BIDS-style events export describing the merged artifact segments.
    if (args.export_bids_events) {
      std::vector<AnnotationEvent> ev;
      ev.reserve(segments.size());
      for (const auto& s : segments) {
        AnnotationEvent e;
        e.onset_sec = s.t_start_sec;
        e.duration_sec = std::max(0.0, s.t_end_sec - s.t_start_sec);
        e.text = "artifact";
        ev.push_back(std::move(e));
      }

      BidsEventsTsvOptions eopt;
      eopt.include_trial_type = true;
      eopt.include_trial_type_levels = true;
      eopt.include_sample = true;
      eopt.sample_index_base = 0;

      write_bids_events_tsv(args.outdir + "/artifact_events.tsv", ev, eopt, rec.fs_hz);
      write_bids_events_json(args.outdir + "/artifact_events.json", eopt, ev);
    }

    // Lightweight run manifest for qeeg_ui_cli / qeeg_ui_server_cli.
    {
      const std::string meta_path = args.outdir + "/artifact_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("artifact_windows.csv");
      outs.push_back("artifact_channels.csv");
      outs.push_back("artifact_channel_summary.csv");
      outs.push_back("artifact_segments.csv");
      outs.push_back("artifact_summary.txt");
      if (args.export_bids_events) {
        outs.push_back("artifact_events.tsv");
        outs.push_back("artifact_events.json");
      }
      outs.push_back("artifact_run_meta.json");
      if (!write_run_meta_json(meta_path, "qeeg_artifacts_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    }

    std::cout << "Wrote artifact report to: " << args.outdir << "\n";
    std::cout << "  - artifact_windows.csv\n";
    std::cout << "  - artifact_channels.csv\n";
    std::cout << "  - artifact_channel_summary.csv\n";
    std::cout << "  - artifact_segments.csv\n";
    std::cout << "  - artifact_summary.txt\n";
    if (args.export_bids_events) {
      std::cout << "  - artifact_events.tsv\n";
      std::cout << "  - artifact_events.json\n";
    }
    std::cout << "  - artifact_run_meta.json\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
