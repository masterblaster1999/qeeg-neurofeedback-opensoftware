#include "qeeg/bandpower.hpp"
#include "qeeg/online_pac.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_pac"};

  // CSV inputs
  double fs_csv{0.0};

  // Channel to analyze (case-insensitive). Empty => first.
  std::string channel;

  // Phase and amplitude bands.
  // Either provide explicit edges or use named bands via --phase-band/--amp-band.
  std::string band_spec; // optional; default_eeg_bands() if empty
  std::string phase_band_name{"theta"};
  std::string amp_band_name{"gamma"};
  double phase_lo_hz{0.0};
  double phase_hi_hz{0.0};
  double amp_lo_hz{0.0};
  double amp_hi_hz{0.0};

  // Online/windowed settings
  double window_sec{4.0};
  double update_sec{0.25};

  // PAC estimator options
  std::string method{"mi"};
  size_t bins{18};
  double trim{0.10};
  bool pac_zero_phase{true};

  // Optional preprocessing
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static void print_help() {
  std::cout
    << "qeeg_pac_cli (phase-amplitude coupling; PAC)\n\n"
    << "Usage:\n"
    << "  qeeg_pac_cli --input file.edf --channel Cz --outdir out_pac\n"
    << "  qeeg_pac_cli --input file.csv --fs 250 --channel Cz --outdir out_pac\n\n"
    << "Band selection:\n"
    << "  --phase-band NAME        Phase band name (default: theta)\n"
    << "  --amp-band NAME          Amplitude band name (default: gamma)\n"
    << "  --phase LO HI            Explicit phase band edges in Hz (overrides --phase-band)\n"
    << "  --amp LO HI              Explicit amplitude band edges in Hz (overrides --amp-band)\n"
    << "  --bands SPEC             Optional band spec used for name lookup\n"
    << "                          Example: 'theta:4-8,gamma:30-80'\n\n"
    << "Estimator options:\n"
    << "  --method mi|mvl          PAC estimator (default: mi)\n"
    << "  --bins N                 #phase bins for MI (default: 18)\n"
    << "  --trim FRAC              Edge trim fraction per window (default: 0.10)\n"
    << "  --pac-zero-phase         Use zero-phase filtering for PAC bandpass filters (default)\n"
    << "  --pac-causal             Use causal filtering for PAC bandpass filters\n\n"
    << "Windowing:\n"
    << "  --window S               Window length seconds (default: 4.0)\n"
    << "  --update S               Update seconds (default: 0.25)\n\n"
    << "I/O:\n"
    << "  --input PATH             Input EDF/BDF/CSV\n"
    << "  --fs HZ                  Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR             Output directory (default: out_pac)\n"
    << "  --channel NAME           Channel name (case-insensitive); default: first\n\n"
    << "Optional preprocessing:\n"
    << "  --average-reference      Apply common average reference across channels\n"
    << "  --notch HZ               Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q              Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI         Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase             Offline: forward-backward filtering (less phase distortion)\n"
    << "  -h, --help               Show this help\n";
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
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--channel" && i + 1 < argc) {
      a.channel = argv[++i];
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--phase-band" && i + 1 < argc) {
      a.phase_band_name = argv[++i];
      a.phase_lo_hz = 0.0;
      a.phase_hi_hz = 0.0;
    } else if (arg == "--amp-band" && i + 1 < argc) {
      a.amp_band_name = argv[++i];
      a.amp_lo_hz = 0.0;
      a.amp_hi_hz = 0.0;
    } else if (arg == "--phase" && i + 2 < argc) {
      a.phase_lo_hz = to_double(argv[++i]);
      a.phase_hi_hz = to_double(argv[++i]);
    } else if (arg == "--amp" && i + 2 < argc) {
      a.amp_lo_hz = to_double(argv[++i]);
      a.amp_hi_hz = to_double(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_sec = to_double(argv[++i]);
    } else if (arg == "--update" && i + 1 < argc) {
      a.update_sec = to_double(argv[++i]);
    } else if (arg == "--method" && i + 1 < argc) {
      a.method = to_lower(trim(argv[++i]));
    } else if (arg == "--bins" && i + 1 < argc) {
      a.bins = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--trim" && i + 1 < argc) {
      a.trim = to_double(argv[++i]);
    } else if (arg == "--pac-zero-phase") {
      a.pac_zero_phase = true;
    } else if (arg == "--pac-causal") {
      a.pac_zero_phase = false;
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

static int find_channel_index(const std::vector<std::string>& channels, const std::string& name) {
  if (channels.empty()) return -1;
  if (trim(name).empty()) return 0;
  const std::string target = normalize_channel_name(name);
  for (size_t i = 0; i < channels.size(); ++i) {
    if (normalize_channel_name(channels[i]) == target) return static_cast<int>(i);
  }
  return -1;
}

static BandDefinition resolve_band(const std::vector<BandDefinition>& bands,
                                  const std::string& name,
                                  double lo_override,
                                  double hi_override,
                                  const std::string& label) {
  if (lo_override > 0.0 || hi_override > 0.0) {
    if (!(lo_override > 0.0 && hi_override > lo_override)) {
      throw std::runtime_error(label + ": explicit band requires LO > 0 and HI > LO");
    }
    return {label, lo_override, hi_override};
  }

  const std::string target = to_lower(trim(name));
  for (const auto& b : bands) {
    if (to_lower(b.name) == target) return b;
  }
  throw std::runtime_error(label + ": band name not found: " + name);
}

static double median(std::vector<double> v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = v.size();
  const size_t mid = n / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  double m = v[mid];
  if ((n % 2) == 0) {
    std::nth_element(v.begin(), v.begin() + (mid - 1), v.end());
    m = 0.5 * (m + v[mid - 1]);
  }
  return m;
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }

    if (args.window_sec <= 0.0) throw std::runtime_error("--window must be > 0");
    if (args.update_sec <= 0.0) throw std::runtime_error("--update must be > 0");

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.n_channels() == 0) throw std::runtime_error("Recording has no channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    const int ch_idx = find_channel_index(rec.channel_names, args.channel);
    if (ch_idx < 0) throw std::runtime_error("Channel not found: " + args.channel);
    const std::string ch_name = rec.channel_names[static_cast<size_t>(ch_idx)];

    // Optional preprocessing (offline).
    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    preprocess_recording_inplace(rec, popt);

    const auto bands = parse_band_spec(args.band_spec);
    const BandDefinition phase_band = resolve_band(bands, args.phase_band_name, args.phase_lo_hz, args.phase_hi_hz, "phase");
    const BandDefinition amp_band   = resolve_band(bands, args.amp_band_name, args.amp_lo_hz, args.amp_hi_hz, "amplitude");

    OnlinePacOptions opt;
    opt.window_seconds = args.window_sec;
    opt.update_seconds = args.update_sec;
    opt.pac.zero_phase = args.pac_zero_phase;
    opt.pac.edge_trim_fraction = args.trim;
    opt.pac.n_phase_bins = args.bins;
    if (args.method == "mvl") {
      opt.pac.method = PacMethod::MeanVectorLength;
    } else if (args.method == "mi") {
      opt.pac.method = PacMethod::ModulationIndex;
    } else {
      throw std::runtime_error("--method must be 'mi' or 'mvl'");
    }

    OnlinePAC eng(rec.fs_hz, phase_band, amp_band, opt);
    const auto frames = eng.push_block(rec.data[static_cast<size_t>(ch_idx)]);

    std::ofstream out(args.outdir + "/pac_timeseries.csv");
    if (!out) throw std::runtime_error("Failed to write pac_timeseries.csv");
    out << "t_end_sec,pac\n";

    std::vector<double> values;
    values.reserve(frames.size());

    // Optional: average phase distribution (MI only).
    std::vector<double> dist_acc;
    size_t dist_n = 0;

    for (const auto& fr : frames) {
      if (!std::isfinite(fr.value)) continue;
      out << fr.t_end_sec << "," << fr.value << "\n";
      values.push_back(fr.value);

      if (opt.pac.method == PacMethod::ModulationIndex) {
        // Recompute MI distribution for this window only when needed.
        // We keep this dependency-light by reusing compute_pac on the current window.
        // NOTE: This is extra work; if you don't need it, just ignore pac_phase_distribution.csv.
        //
        // For now, we approximate the distribution by computing it on the last window ending at fr.t_end_sec.
        // We do this by extracting the last window from the channel vector.
        const size_t end = static_cast<size_t>(std::llround(fr.t_end_sec * rec.fs_hz));
        const size_t win = static_cast<size_t>(std::llround(opt.window_seconds * rec.fs_hz));
        if (end > win && end <= rec.n_samples()) {
          const size_t start = end - win;
          std::vector<float> w(rec.data[static_cast<size_t>(ch_idx)].begin() + static_cast<std::ptrdiff_t>(start),
                               rec.data[static_cast<size_t>(ch_idx)].begin() + static_cast<std::ptrdiff_t>(end));
          PacResult pr = compute_pac(w, rec.fs_hz, phase_band, amp_band, opt.pac);
          if (!pr.mean_amp_by_phase_bin.empty()) {
            // Normalize to a probability distribution per window.
            double s = 0.0;
            for (double v : pr.mean_amp_by_phase_bin) s += v;
            if (s > 0.0 && std::isfinite(s)) {
              if (dist_acc.empty()) dist_acc.assign(pr.mean_amp_by_phase_bin.size(), 0.0);
              if (dist_acc.size() == pr.mean_amp_by_phase_bin.size()) {
                for (size_t i = 0; i < dist_acc.size(); ++i) dist_acc[i] += pr.mean_amp_by_phase_bin[i] / s;
                ++dist_n;
              }
            }
          }
        }
      }
    }

    // Summary.
    std::ofstream meta(args.outdir + "/pac_summary.txt");
    if (!meta) throw std::runtime_error("Failed to write pac_summary.txt");

    meta << "Channel: " << ch_name << "\n";
    meta << "Phase band: " << phase_band.fmin_hz << "-" << phase_band.fmax_hz << " Hz\n";
    meta << "Amplitude band: " << amp_band.fmin_hz << "-" << amp_band.fmax_hz << " Hz\n";
    meta << "Method: " << (opt.pac.method == PacMethod::ModulationIndex ? "MI" : "MVL") << "\n";
    meta << "Window: " << opt.window_seconds << " s\n";
    meta << "Update: " << opt.update_seconds << " s\n";
    meta << "PAC zero-phase bandpass: " << (opt.pac.zero_phase ? "true" : "false") << "\n";
    meta << "Edge trim fraction: " << opt.pac.edge_trim_fraction << "\n";
    if (opt.pac.method == PacMethod::ModulationIndex) {
      meta << "Phase bins: " << opt.pac.n_phase_bins << "\n";
    }
    meta << "Frames (finite): " << values.size() << "\n";

    if (!values.empty()) {
      double mean = 0.0;
      for (double v : values) mean += v;
      mean /= static_cast<double>(values.size());
      meta << "Mean: " << mean << "\n";
      meta << "Median: " << median(values) << "\n";
      auto [mn, mx] = std::minmax_element(values.begin(), values.end());
      meta << "Min: " << *mn << "\n";
      meta << "Max: " << *mx << "\n";
    }

    // Optional distribution output.
    if (opt.pac.method == PacMethod::ModulationIndex && !dist_acc.empty() && dist_n > 0) {
      std::ofstream d(args.outdir + "/pac_phase_distribution.csv");
      if (d) {
        d << "bin_index,prob\n";
        for (size_t i = 0; i < dist_acc.size(); ++i) {
          d << i << "," << (dist_acc[i] / static_cast<double>(dist_n)) << "\n";
        }
      }
    }

    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
