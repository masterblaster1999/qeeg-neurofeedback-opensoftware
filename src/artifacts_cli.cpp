#include "qeeg/artifacts.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
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
  size_t min_bad_channels{1};

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
    << "  --min-bad-channels N    Mark window bad if >=N channels are flagged (default: 1)\n"
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
    } else if (arg == "--min-bad-channels" && i + 1 < argc) {
      a.min_bad_channels = static_cast<size_t>(to_int(argv[++i]));
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
    aopt.min_bad_channels = args.min_bad_channels;

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Artifact windows: window=" << aopt.window_seconds << "s, step=" << aopt.step_seconds
              << "s, baseline=" << aopt.baseline_seconds << "s\n";

    const ArtifactDetectionResult res = detect_artifacts(rec, aopt);

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
      f << "min_bad_channels: " << res.opt.min_bad_channels << "\n\n";
      f << "windows_total: " << res.windows.size() << "\n";
      f << "windows_bad: " << res.total_bad_windows << "\n";
      f << "bad_fraction: " << frac << "\n";
    }

    std::cout << "Wrote artifact report to: " << args.outdir << "\n";
    std::cout << "  - artifact_windows.csv\n";
    std::cout << "  - artifact_channels.csv\n";
    std::cout << "  - artifact_summary.txt\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
