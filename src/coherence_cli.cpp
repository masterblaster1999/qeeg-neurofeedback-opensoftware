#include "qeeg/bandpower.hpp"
#include "qeeg/coherence.hpp"
#include "qeeg/online_coherence.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/run_meta.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_coherence"};

  std::string band_spec;            // empty => default
  std::string band_name{"alpha"}; // default selection within band_spec

  // If empty => compute full matrix for all channel pairs.
  // Otherwise format: CH1:CH2 (several delimiters accepted).
  std::string pair_spec;

  // Which coherence-like measure to report.
  // - msc: magnitude-squared coherence (default)
  // - imcoh: absolute imaginary part of coherency (Nolte-style)
  std::string measure{"msc"};

  bool export_spectrum{false};

  // Optional: sliding-window coherence time series.
  // Only supported when --pair is provided.
  bool timeseries{false};
  double window_seconds{2.0};
  double update_seconds{0.25};

  bool average_reference{false};

  // Optional preprocessing filters
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};

  double fs_csv{0.0};
  size_t nperseg{1024};
  double overlap{0.5};
};

static void print_help() {
  std::cout
    << "qeeg_coherence_cli (first pass connectivity)\n\n"
    << "Usage:\n"
    << "  qeeg_coherence_cli --input file.edf --outdir out --band alpha\n"
    << "  qeeg_coherence_cli --input file.bdf --outdir out --band alpha --pair F3:F4 --export-spectrum\n"
    << "  qeeg_coherence_cli --input file.csv --fs 250 --outdir out --band 8-12\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                 Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR            Output directory (default: out_coherence)\n"
    << "  --bands SPEC            Band spec, e.g. 'alpha:8-12,beta:13-30' (default: built-in EEG bands)\n"
    << "  --band NAME|FMIN-FMAX    Which band to report (default: alpha)\n"
    << "  --pair CH1:CH2          If set, compute only this pair (otherwise output a full matrix)\n"
    << "  --measure msc|imcoh      Connectivity measure (default: msc)\n"
    << "  --export-spectrum        If --pair is used, also write coherence_spectrum.csv\n"
    << "  --timeseries             If --pair is used, also write <measure>_timeseries.csv\n"
    << "  --window SECONDS         Window length for --timeseries (default: 2.0)\n"
    << "  --update SECONDS         Update interval for --timeseries (default: 0.25)\n"
    << "  --average-reference      Apply common average reference across channels\n"
    << "  --notch HZ               Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q              Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI         Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase             Offline: forward-backward filtering (less phase distortion)\n"
    << "  --nperseg N             Welch segment length (default: 1024)\n"
    << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
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
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--band" && i + 1 < argc) {
      a.band_name = argv[++i];
    } else if (arg == "--pair" && i + 1 < argc) {
      a.pair_spec = argv[++i];
    } else if (arg == "--measure" && i + 1 < argc) {
      a.measure = argv[++i];
    } else if (arg == "--export-spectrum") {
      a.export_spectrum = true;
    } else if (arg == "--timeseries") {
      a.timeseries = true;
    } else if ((arg == "--window" || arg == "--window-seconds") && i + 1 < argc) {
      a.timeseries = true;
      a.window_seconds = to_double(argv[++i]);
    } else if ((arg == "--update" || arg == "--update-seconds") && i + 1 < argc) {
      a.timeseries = true;
      a.update_seconds = to_double(argv[++i]);
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
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static int find_channel_index(const std::vector<std::string>& channels, const std::string& name) {
  const std::string target = normalize_channel_name(name);
  for (size_t i = 0; i < channels.size(); ++i) {
    if (normalize_channel_name(channels[i]) == target) return static_cast<int>(i);
  }
  return -1;
}

static bool try_parse_range_band(const std::string& s, BandDefinition* out) {
  // Accept strings like "8-12" (whitespace ok).
  std::string t = trim(s);
  auto parts = split(t, '-');
  if (parts.size() != 2) return false;
  try {
    const double fmin = to_double(parts[0]);
    const double fmax = to_double(parts[1]);
    if (!(fmin >= 0.0 && fmax > fmin)) return false;
    out->name = t;
    out->fmin_hz = fmin;
    out->fmax_hz = fmax;
    return true;
  } catch (...) {
    return false;
  }
}

static BandDefinition resolve_band(const std::vector<BandDefinition>& bands, const std::string& name_or_range) {
  const std::string key = to_lower(trim(name_or_range));
  for (const auto& b : bands) {
    if (to_lower(trim(b.name)) == key) return b;
  }

  BandDefinition custom;
  if (try_parse_range_band(name_or_range, &custom)) return custom;

  std::string msg = "Band not found: '" + name_or_range + "'. Available:";
  for (const auto& b : bands) msg += " " + b.name;
  throw std::runtime_error(msg);
}

static std::pair<std::string,std::string> parse_pair(const std::string& s) {
  // Accept delimiters ':', '-', ','
  std::string t = trim(s);
  for (char& ch : t) {
    if (ch == ',' || ch == '-' ) ch = ':';
  }
  auto parts = split(t, ':');
  if (parts.size() != 2) {
    throw std::runtime_error("--pair expects CH1:CH2 (also accepts CH1-CH2 or CH1,CH2)");
  }
  return {trim(parts[0]), trim(parts[1])};
}

static std::string stem_for_measure(CoherenceMeasure m) {
  if (m == CoherenceMeasure::MagnitudeSquared) return "coherence";
  if (m == CoherenceMeasure::ImaginaryCoherencyAbs) return "imcoh";
  return "coherence";
}

static std::string column_for_measure(CoherenceMeasure m) {
  if (m == CoherenceMeasure::MagnitudeSquared) return "coherence";
  if (m == CoherenceMeasure::ImaginaryCoherencyAbs) return "imcoh";
  return "coherence";
}

static std::string fmt_double(double v, int precision = 6) {
  if (!std::isfinite(v)) return "NaN";
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

static void write_coherence_timeseries_sidecar_json(const Args& args,
                                                    const std::string& stem,
                                                    const std::string& col,
                                                    const BandDefinition& band,
                                                    const std::string& ch_a,
                                                    const std::string& ch_b,
                                                    CoherenceMeasure measure) {
  const std::string outpath = args.outdir + "/" + stem + "_timeseries.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write " + outpath);
  out << std::setprecision(12);

  const std::string ts_suffix = " Sliding-window estimate over a " + fmt_double(args.window_seconds, 3) +
                                " s window, updated every " + fmt_double(args.update_seconds, 3) + " s.";

  const std::string measure_name = coherence_measure_name(measure);

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

  out << "{\n";
  write_entry("t_end_sec",
              "Window end time",
              "Time in seconds at the end of the analysis window (relative to recording start)." + ts_suffix,
              "s");
  out << ",\n";

  const std::string desc = "Band-mean " + measure_name + " integrated from " + fmt_double(band.fmin_hz, 4) +
                           " to " + fmt_double(band.fmax_hz, 4) + " Hz between channels " + ch_a +
                           " and " + ch_b + "." + ts_suffix;
  write_entry(col,
              measure_name + " (" + band.name + ")",
              desc,
              "n/a");

  out << "\n}\n";
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }

    if (args.timeseries) {
      if (args.pair_spec.empty()) {
        throw std::runtime_error("--timeseries is only supported with --pair (matrix time series not supported yet)");
      }
      if (!(args.window_seconds > 0.0)) {
        throw std::runtime_error("--window must be > 0");
      }
      if (!(args.update_seconds > 0.0)) {
        throw std::runtime_error("--update must be > 0");
      }
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.n_channels() < 2) throw std::runtime_error("Recording must have at least 2 channels");
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

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    const std::vector<BandDefinition> bands = parse_band_spec(args.band_spec);
    const BandDefinition band = resolve_band(bands, args.band_name);

    const CoherenceMeasure measure = parse_coherence_measure_token(args.measure);
    const std::string stem = stem_for_measure(measure);
    const std::string col = column_for_measure(measure);

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Band: " << band.name << " (" << band.fmin_hz << "-" << band.fmax_hz << " Hz)\n";
    std::cout << "Measure: " << coherence_measure_name(measure) << "\n";

    if (!args.pair_spec.empty()) {
      const auto pr_names = parse_pair(args.pair_spec);
      const int ia = find_channel_index(rec.channel_names, pr_names.first);
      const int ib = find_channel_index(rec.channel_names, pr_names.second);
      if (ia < 0) throw std::runtime_error("Channel not found: " + pr_names.first);
      if (ib < 0) throw std::runtime_error("Channel not found: " + pr_names.second);
      if (ia == ib) throw std::runtime_error("--pair channels must be different");

      const CoherenceSpectrum spec = welch_coherence_spectrum(rec.data[static_cast<size_t>(ia)],
                                                             rec.data[static_cast<size_t>(ib)],
                                                             rec.fs_hz,
                                                             wopt,
                                                             measure);

      const double mean_c = average_band_value(spec, band);

      std::cout << "Band-mean " << col << "(" << pr_names.first << "," << pr_names.second << ") = "
                << mean_c << "\n";

      // Always write a summary.
      {
        const std::string path = args.outdir + "/" + stem + "_band.csv";
        std::ofstream f(path);
        if (!f) throw std::runtime_error("Failed to write " + path);
        f << "band,channel_a,channel_b," << col << "\n";
        f << band.name << "," << pr_names.first << "," << pr_names.second << "," << mean_c << "\n";
      }

      if (args.export_spectrum) {
        const std::string path = args.outdir + "/" + stem + "_spectrum.csv";
        std::ofstream f(path);
        if (!f) throw std::runtime_error("Failed to write " + path);
        f << "freq_hz," << col << "\n";
        for (size_t i = 0; i < spec.freqs_hz.size(); ++i) {
          f << spec.freqs_hz[i] << "," << spec.values[i] << "\n";
        }
      }

      if (args.timeseries) {
        const std::string ts_path = args.outdir + "/" + stem + "_timeseries.csv";
        std::ofstream out_ts(std::filesystem::u8path(ts_path), std::ios::binary);
        if (!out_ts) throw std::runtime_error("Failed to write " + ts_path);
        out_ts << "t_end_sec," << col << "\n";
        out_ts << std::setprecision(12);

        OnlineCoherenceOptions opt;
        opt.window_seconds = args.window_seconds;
        opt.update_seconds = args.update_seconds;
        opt.welch = wopt;
        opt.measure = measure;

        const std::string ch_a = rec.channel_names[static_cast<size_t>(ia)];
        const std::string ch_b = rec.channel_names[static_cast<size_t>(ib)];

        OnlineWelchCoherence eng({ch_a, ch_b}, rec.fs_hz, {band}, {{0, 1}}, opt);

        const size_t chunk_samples = 512;
        std::vector<std::vector<float>> block(2);
        for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
          const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
          block[0].assign(rec.data[static_cast<size_t>(ia)].begin() + static_cast<std::ptrdiff_t>(pos),
                          rec.data[static_cast<size_t>(ia)].begin() + static_cast<std::ptrdiff_t>(end));
          block[1].assign(rec.data[static_cast<size_t>(ib)].begin() + static_cast<std::ptrdiff_t>(pos),
                          rec.data[static_cast<size_t>(ib)].begin() + static_cast<std::ptrdiff_t>(end));
          const auto frames = eng.push_block(block);
          for (const auto& fr : frames) {
            if (fr.coherences.empty() || fr.coherences[0].empty()) continue;
            out_ts << fr.t_end_sec << "," << fr.coherences[0][0] << "\n";
          }
        }

        write_coherence_timeseries_sidecar_json(args, stem, col, band, ch_a, ch_b, measure);
      }

      {
        const std::string meta_path = args.outdir + "/coherence_run_meta.json";
        std::vector<std::string> outs;
        outs.push_back("coherence_run_meta.json");
        outs.push_back(stem + "_band.csv");
        if (args.export_spectrum) outs.push_back(stem + "_spectrum.csv");
        if (args.timeseries) {
          outs.push_back(stem + "_timeseries.csv");
          outs.push_back(stem + "_timeseries.json");
        }
        if (!write_run_meta_json(meta_path, "qeeg_coherence_cli", args.outdir, args.input_path, outs)) {
          std::cerr << "Warning: failed to write " << meta_path << "\n";
        }
      }

      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    // Matrix mode: compute band-mean measure for all channel pairs.
    const size_t C = rec.n_channels();
    std::vector<std::vector<double>> mat(C, std::vector<double>(C, 0.0));
    for (size_t i = 0; i < C; ++i) mat[i][i] = 1.0;

    for (size_t i = 0; i < C; ++i) {
      for (size_t j = i + 1; j < C; ++j) {
        const auto spec = welch_coherence_spectrum(rec.data[i], rec.data[j], rec.fs_hz, wopt, measure);
        const double v = average_band_value(spec, band);
        const double c = std::isfinite(v) ? v : 0.0;
        mat[i][j] = c;
        mat[j][i] = c;
      }
    }

    // Write matrix.
    {
      std::string fname = args.outdir + "/" + stem + "_matrix_" + to_lower(band.name) + ".csv";
      std::ofstream f(fname);
      if (!f) throw std::runtime_error("Failed to write " + fname);

      // Header row.
      f << "";
      for (const auto& ch : rec.channel_names) f << "," << ch;
      f << "\n";

      for (size_t i = 0; i < C; ++i) {
        f << rec.channel_names[i];
        for (size_t j = 0; j < C; ++j) {
          f << "," << mat[i][j];
        }
        f << "\n";
      }
    }

    // Also write a flat edge list (useful for graph tooling).
    {
      const std::string path = args.outdir + "/" + stem + "_pairs.csv";
      std::ofstream f(path);
      if (!f) throw std::runtime_error("Failed to write " + path);
      f << "channel_a,channel_b," << col << "\n";
      for (size_t i = 0; i < C; ++i) {
        for (size_t j = i + 1; j < C; ++j) {
          f << rec.channel_names[i] << "," << rec.channel_names[j] << "," << mat[i][j] << "\n";
        }
      }
    }

    {
      const std::string meta_path = args.outdir + "/coherence_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("coherence_run_meta.json");
      outs.push_back(stem + "_matrix_" + to_lower(band.name) + ".csv");
      outs.push_back(stem + "_pairs.csv");
      if (!write_run_meta_json(meta_path, "qeeg_coherence_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write " << meta_path << "\n";
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
