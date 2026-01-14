#include "qeeg/bandpower.hpp"
#include "qeeg/plv.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/run_meta.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_plv"};

  // CSV inputs
  double fs_csv{0.0};

  // Band selection
  std::string band_spec;           // empty => default bands
  std::string band_name{"alpha"}; // default within band_spec

  // If empty => compute full matrix.
  // Otherwise format: CH1:CH2 (several delimiters accepted).
  std::string pair_spec;

  // Which phase-based measure to compute.
  //   plv  : Phase Locking Value
  //   pli  : Phase Lag Index
  //   wpli : Weighted Phase Lag Index
  //   wpli2_debiased : Debiased estimator of squared wPLI
  std::string measure{"plv"};

  // PLV estimator options
  bool plv_zero_phase{true};
  double trim{0.10};

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
      << "qeeg_plv_cli (phase connectivity; PLV / PLI / wPLI / wPLI2_debiased)\n\n"
      << "Usage:\n"
      << "  qeeg_plv_cli --input file.edf --outdir out --band alpha\n"
      << "  qeeg_plv_cli --input file.edf --outdir out --band alpha --pair F3:F4\n"
      << "  qeeg_plv_cli --input file.csv --fs 250 --outdir out --band 8-12\n\n"
      << "Options:\n"
      << "  --input PATH             Input EDF/BDF/CSV\n"
      << "  --fs HZ                  Sampling rate for CSV (optional if first column is time)\n"
      << "  --outdir DIR             Output directory (default: out_plv)\n"
      << "  --bands SPEC             Band spec, e.g. 'alpha:8-12,beta:13-30' (default: built-in EEG bands)\n"
      << "  --band NAME|FMIN-FMAX     Which band to report (default: alpha)\n"
      << "  --measure plv|pli|wpli|wpli2_debiased    Which measure to compute (default: plv)\n"
      << "  --pair CH1:CH2           If set, compute only this pair (otherwise output a full matrix).\n"
      << "                          CH1/CH2 may be channel labels or numeric indices (0- or 1-based).\n"
      << "  --trim FRAC              Edge trim fraction per channel window in [0,0.49] (default: 0.10)\n"
      << "  --plv-zero-phase         Use zero-phase filtering for the PLV internal bandpass (default)\n"
      << "  --plv-causal             Use causal filtering for the PLV internal bandpass\n"
      << "\nOptional preprocessing:\n"
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
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--band" && i + 1 < argc) {
      a.band_name = argv[++i];
    } else if (arg == "--measure" && i + 1 < argc) {
      a.measure = argv[++i];
    } else if (arg == "--pair" && i + 1 < argc) {
      a.pair_spec = argv[++i];
    } else if (arg == "--trim" && i + 1 < argc) {
      a.trim = to_double(argv[++i]);
    } else if (arg == "--plv-zero-phase") {
      a.plv_zero_phase = true;
    } else if (arg == "--plv-causal") {
      a.plv_zero_phase = false;
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

static std::string normalize_measure(const std::string& m) {
  const std::string key = to_lower(trim(m));
  if (key == "plv") return "plv";
  if (key == "pli") return "pli";
  if (key == "wpli" || key == "w-pli" || key == "w_pli") return "wpli";
  if (key == "wpli2_debiased" || key == "wpli_debiased" || key == "dwpli" || key == "wpli2") return "wpli2_debiased";
  throw std::runtime_error("Unknown --measure: '" + m + "' (expected: plv|pli|wpli|wpli2_debiased)");
}

static int find_channel_index(const std::vector<std::string>& channels, const std::string& name) {
  if (channels.empty()) return -1;
  if (name.empty()) return -1;

  // Prefer robust name matching that tolerates common variations like
  // "EEG Fp1-REF" vs "Fp1".
  const std::string want = normalize_channel_name(name);
  for (size_t i = 0; i < channels.size(); ++i) {
    if (normalize_channel_name(channels[i]) == want) return static_cast<int>(i);
  }

  // Convenience: accept numeric indices (0-based or 1-based).
  bool all_digits = true;
  for (char c : name) {
    if (!(c >= '0' && c <= '9')) { all_digits = false; break; }
  }
  if (all_digits) {
    const int idx = to_int(name);
    if (idx >= 0 && idx < static_cast<int>(channels.size())) return idx;
    if (idx >= 1 && idx <= static_cast<int>(channels.size())) return idx - 1;
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

static std::pair<std::string, std::string> parse_pair(const std::string& s) {
  // Accept delimiters ':', '-', ','
  std::string t = trim(s);
  for (char& ch : t) {
    if (ch == ',' || ch == '-') ch = ':';
  }
  auto parts = split(t, ':');
  if (parts.size() != 2) {
    throw std::runtime_error("--pair expects CH1:CH2 (also accepts CH1-CH2 or CH1,CH2)");
  }
  return {trim(parts[0]), trim(parts[1])};
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.trim < 0.0 || args.trim >= 0.5 || !std::isfinite(args.trim)) {
      throw std::runtime_error("--trim must be in [0, 0.49]");
    }

    ensure_directory(args.outdir);

    const std::string measure = normalize_measure(args.measure);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.n_channels() < 2) throw std::runtime_error("Recording must have at least 2 channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    // Optional preprocessing (offline).
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

    const std::vector<BandDefinition> bands = parse_band_spec(args.band_spec);
    const BandDefinition band = resolve_band(bands, args.band_name);

    PlvOptions opt;
    opt.zero_phase = args.plv_zero_phase;
    opt.edge_trim_fraction = args.trim;

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, " << rec.n_samples() << " samples, fs="
              << rec.fs_hz << " Hz\n";
    std::cout << "Band: " << band.name << " (" << band.fmin_hz << "-" << band.fmax_hz << " Hz)\n";
    std::cout << "Measure: " << measure << "\n";
    std::cout << "Internal filtering: " << (opt.zero_phase ? "zero-phase" : "causal")
              << ", trim=" << opt.edge_trim_fraction << "\n";

    if (!args.pair_spec.empty()) {
      const auto pr_names = parse_pair(args.pair_spec);
      const int ia = find_channel_index(rec.channel_names, pr_names.first);
      const int ib = find_channel_index(rec.channel_names, pr_names.second);
      if (ia < 0) throw std::runtime_error("Channel not found: " + pr_names.first);
      if (ib < 0) throw std::runtime_error("Channel not found: " + pr_names.second);
      if (ia == ib) throw std::runtime_error("--pair channels must be different");

      double v = std::numeric_limits<double>::quiet_NaN();
      if (measure == "plv") {
        v = compute_plv(rec.data[static_cast<size_t>(ia)],
                        rec.data[static_cast<size_t>(ib)],
                        rec.fs_hz,
                        band,
                        opt);
      } else if (measure == "pli") {
        v = compute_pli(rec.data[static_cast<size_t>(ia)],
                        rec.data[static_cast<size_t>(ib)],
                        rec.fs_hz,
                        band,
                        opt);
      } else if (measure == "wpli") {
        v = compute_wpli(rec.data[static_cast<size_t>(ia)],
                         rec.data[static_cast<size_t>(ib)],
                         rec.fs_hz,
                         band,
                         opt);
      } else if (measure == "wpli2_debiased") {
        v = compute_wpli2_debiased(rec.data[static_cast<size_t>(ia)],
                                   rec.data[static_cast<size_t>(ib)],
                                   rec.fs_hz,
                                   band,
                                   opt);
      }

      std::cout << measure << "(" << pr_names.first << "," << pr_names.second << ") = " << v << "\n";

      // Always write a summary.
      {
        const std::string fname = args.outdir + "/" + measure + "_band.csv";
        std::ofstream f(fname);
        if (!f) throw std::runtime_error("Failed to write " + fname);
        f << "band,channel_a,channel_b," << measure << "\n";
        f << band.name << "," << pr_names.first << "," << pr_names.second << "," << v << "\n";
      }

      {
        const std::string meta_path = args.outdir + "/plv_run_meta.json";
        std::vector<std::string> outs;
        outs.push_back("plv_run_meta.json");
        outs.push_back(measure + "_band.csv");
        if (!write_run_meta_json(meta_path, "qeeg_plv_cli", args.outdir, args.input_path, outs)) {
          std::cerr << "Warning: failed to write " << meta_path << "\n";
        }
      }

      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    // Matrix mode.
    std::vector<std::vector<double>> mat0;
    if (measure == "plv") {
      mat0 = compute_plv_matrix(rec.data, rec.fs_hz, band, opt);
    } else if (measure == "pli") {
      mat0 = compute_pli_matrix(rec.data, rec.fs_hz, band, opt);
    } else if (measure == "wpli") {
      mat0 = compute_wpli_matrix(rec.data, rec.fs_hz, band, opt);
    } else if (measure == "wpli2_debiased") {
      mat0 = compute_wpli2_debiased_matrix(rec.data, rec.fs_hz, band, opt);
    }
    const size_t C = rec.n_channels();
    if (mat0.size() != C) throw std::runtime_error("PLV: unexpected matrix size");

    std::vector<std::vector<double>> mat = mat0;
    for (size_t i = 0; i < C; ++i) {
      if (mat[i].size() != C) throw std::runtime_error("PLV: unexpected matrix row size");
      for (size_t j = 0; j < C; ++j) {
        if (!std::isfinite(mat[i][j])) mat[i][j] = 0.0;
      }
      // Convention: PLV diagonal = 1; PLI/wPLI diagonal = 0.
      mat[i][i] = (measure == "plv") ? 1.0 : 0.0;
    }

    // Write matrix.
    {
      const std::string fname = args.outdir + "/" + measure + "_matrix_" + to_lower(band.name) + ".csv";
      std::ofstream f(fname);
      if (!f) throw std::runtime_error("Failed to write " + fname);

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
      const std::string fname = args.outdir + "/" + measure + "_pairs.csv";
      std::ofstream f(fname);
      if (!f) throw std::runtime_error("Failed to write " + fname);
      f << "channel_a,channel_b," << measure << "\n";
      for (size_t i = 0; i < C; ++i) {
        for (size_t j = i + 1; j < C; ++j) {
          f << rec.channel_names[i] << "," << rec.channel_names[j] << "," << mat[i][j] << "\n";
        }
      }
    }

    {
      const std::string meta_path = args.outdir + "/plv_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("plv_run_meta.json");
      outs.push_back(measure + "_matrix_" + to_lower(band.name) + ".csv");
      outs.push_back(measure + "_pairs.csv");
      if (!write_run_meta_json(meta_path, "qeeg_plv_cli", args.outdir, args.input_path, outs)) {
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
