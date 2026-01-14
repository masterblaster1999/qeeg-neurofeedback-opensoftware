#include "qeeg/spectral_features.hpp"

#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

#include <cmath>
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
  std::string outdir{"out_spectral"};

  // Recording
  double fs_csv{0.0}; // only for CSV inputs; 0 = infer from time column

  // PSD
  size_t nperseg{1024};
  double overlap{0.5};

  // Feature range
  double fmin_hz{1.0};
  double fmax_hz{40.0};

  // Spectral edge fraction, e.g. 0.95 => SEF95.
  double edge{0.95};

  // Optional preprocessing
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static std::string fmt_double(double v, int precision = 6) {
  if (!std::isfinite(v)) return "NaN";
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

static void print_help() {
  std::cout
      << "qeeg_spectral_features_cli\n\n"
      << "Compute quick per-channel spectral summary features from Welch PSD.\n"
      << "Outputs a CSV + JSON sidecar + run manifest for qeeg_ui_cli.\n\n"
      << "Features (per channel):\n"
      << "  - total_power  : integral(PSD) over [fmin,fmax]\n"
      << "  - entropy      : normalized spectral entropy over [fmin,fmax] (0..1)\n"
      << "  - mean_hz      : power-weighted mean frequency (spectral centroid)\n"
      << "  - peak_hz      : frequency of max PSD (simple argmax)\n"
      << "  - median_hz    : spectral edge frequency at 50% cumulative power\n"
      << "  - sefXX_hz     : spectral edge frequency at edge% cumulative power (default 95%)\n\n"
      << "Usage:\n"
      << "  qeeg_spectral_features_cli --input file.edf --outdir out_spec\n"
      << "  qeeg_spectral_features_cli --input file.csv --fs 250 --outdir out_spec\n"
      << "  qeeg_spectral_features_cli --input file.edf --outdir out_spec --range 1 40 --edge 0.95\n\n"
      << "Options:\n"
      << "  --input PATH            Input EDF/BDF/CSV/ASCII/BrainVision (.vhdr)\n"
      << "  --fs HZ                 Sampling rate hint for CSV (0 = infer from time column)\n"
      << "  --outdir DIR            Output directory (default: out_spectral)\n"
      << "  --nperseg N             Welch segment length (default: 1024)\n"
      << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
      << "  --range LO HI           Frequency range in Hz (default: 1 40)\n"
      << "  --edge X                Spectral edge fraction in (0,1] (default: 0.95)\n"
      << "  --average-reference     Apply common average reference across channels\n"
      << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
      << "  --notch-q Q             Notch Q factor (default: 30)\n"
      << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
      << "  --zero-phase            Offline: forward-backward filtering (less phase distortion)\n"
      << "  -h, --help              Show this help\n";
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
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if ((arg == "--range" || arg == "--freq-range") && i + 2 < argc) {
      a.fmin_hz = to_double(argv[++i]);
      a.fmax_hz = to_double(argv[++i]);
    } else if (arg == "--edge" && i + 1 < argc) {
      a.edge = to_double(argv[++i]);
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

static std::string edge_col_name(double edge) {
  // e.g. 0.95 -> sef95_hz
  const int pct = static_cast<int>(std::llround(edge * 100.0));
  return "sef" + std::to_string(pct) + "_hz";
}

static void write_sidecar_json(const Args& args) {
  const std::string outpath = args.outdir + "/spectral_features.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write spectral_features.json: " + outpath);
  out << std::setprecision(12);

  const std::string edge_col = edge_col_name(args.edge);

  auto write_entry = [&](const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units) {
    out << "  \"" << json_escape(key) << "\": {\n";
    out << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    out << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units.empty()) {
      out << ",\n    \"Units\": \"" << json_escape(units) << "\"";
    }
    out << "\n  }";
  };

  const std::string range = "[" + fmt_double(args.fmin_hz, 4) + "," + fmt_double(args.fmax_hz, 4) + "] Hz";

  out << "{\n";
  write_entry("channel",
              "Channel label",
              "EEG channel label (one row per channel).",
              "");
  out << ",\n";

  write_entry("total_power",
              "Total power",
              "Total power (integral of PSD) within " + range + ".",
              "a.u.");
  out << ",\n";

  write_entry("entropy",
              "Spectral entropy (normalized)",
              "Normalized spectral entropy within " + range + ". Values are in [0,1] (higher means flatter spectrum).",
              "n/a");
  out << ",\n";

  write_entry("mean_hz",
              "Mean frequency (spectral centroid)",
              "Power-weighted mean frequency within " + range + ".",
              "Hz");
  out << ",\n";

  write_entry("peak_hz",
              "Peak frequency",
              "Frequency of maximum PSD within " + range + " (simple argmax; includes exact range boundaries).",
              "Hz");
  out << ",\n";

  write_entry("median_hz",
              "Median frequency (SEF50)",
              "Spectral edge frequency at 50% cumulative power within " + range + ".",
              "Hz");
  out << ",\n";

  {
    const int pct = static_cast<int>(std::llround(args.edge * 100.0));
    write_entry(edge_col,
                "Spectral edge frequency (SEF" + std::to_string(pct) + ")",
                "Spectral edge frequency at " + std::to_string(pct) + "% cumulative power within " + range + ".",
                "Hz");
  }

  out << "\n}\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.overlap < 0.0 || args.overlap >= 1.0) {
      throw std::runtime_error("--overlap must be in [0,1)");
    }
    if (args.nperseg < 16) {
      throw std::runtime_error("--nperseg too small (>=16 recommended)");
    }
    if (args.fmin_hz < 0.0 || !(args.fmax_hz > args.fmin_hz)) {
      throw std::runtime_error("--range must satisfy 0 <= LO < HI");
    }
    if (!(args.edge > 0.0 && args.edge <= 1.0)) {
      throw std::runtime_error("--edge must be in (0,1]");
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    // Preprocess (offline, in-place)
    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    preprocess_recording_inplace(rec, popt);

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    // Clamp analysis range to Nyquist.
    const double nyq = 0.5 * rec.fs_hz;
    const double fmin = std::max(0.0, args.fmin_hz);
    const double fmax = std::min(args.fmax_hz, nyq);
    if (!(fmax > fmin)) {
      throw std::runtime_error("--range is outside the PSD support (check fs / Nyquist)");
    }

    const std::string edge_col = edge_col_name(args.edge);

    // Compute features.
    struct Row {
      std::string ch;
      double total_power{0.0};
      double entropy{0.0};
      double mean_hz{0.0};
      double peak_hz{0.0};
      double median_hz{0.0};
      double edge_hz{0.0};
    };
    std::vector<Row> rows;
    rows.reserve(rec.n_channels());

    for (size_t c = 0; c < rec.n_channels(); ++c) {
      PsdResult psd = welch_psd(rec.data[c], rec.fs_hz, wopt);
      Row r;
      r.ch = rec.channel_names[c];
      r.total_power = spectral_total_power(psd, fmin, fmax);
      r.entropy = spectral_entropy(psd, fmin, fmax, /*normalize=*/true);
      r.mean_hz = spectral_mean_frequency(psd, fmin, fmax);
      r.peak_hz = spectral_peak_frequency(psd, fmin, fmax);
      r.median_hz = spectral_edge_frequency(psd, fmin, fmax, 0.5);
      r.edge_hz = spectral_edge_frequency(psd, fmin, fmax, args.edge);
      rows.push_back(r);
    }

    // Write CSV
    {
      const std::string csv_path = args.outdir + "/spectral_features.csv";
      std::ofstream out(std::filesystem::u8path(csv_path), std::ios::binary);
      if (!out) throw std::runtime_error("Failed to write spectral_features.csv: " + csv_path);

      out << "channel,total_power,entropy,mean_hz,peak_hz,median_hz," << edge_col << "\n";
      out << std::setprecision(12);
      for (const auto& r : rows) {
        out << r.ch << "," << r.total_power << "," << r.entropy << "," << r.mean_hz << "," << r.peak_hz << ","
            << r.median_hz << "," << r.edge_hz << "\n";
      }
    }

    // JSON sidecar describing columns
    write_sidecar_json(args);

    // Run meta for qeeg_ui_cli
    {
      const std::string meta_path = args.outdir + "/spectral_features_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("spectral_features.csv");
      outs.push_back("spectral_features.json");
      outs.push_back("spectral_features_run_meta.json");
      if (!write_run_meta_json(meta_path, "qeeg_spectral_features_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    }

    std::cout << "Wrote: " << args.outdir << "/spectral_features.csv\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
