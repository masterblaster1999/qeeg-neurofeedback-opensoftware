#include "qeeg/bandpower.hpp"
#include "qeeg/online_bandpower.hpp"
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
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string outdir{"out_bandpower"};

  // Recording
  double fs_csv{0.0};

  // Bands + PSD
  std::string band_spec; // empty => default
  size_t nperseg{1024};
  double overlap{0.5};

  // Transform
  bool relative_power{false};
  bool relative_range_specified{false};
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};

  bool log10_power{false};

  // Optional z-score reference
  std::string reference_path;

  // Optional sliding-window bandpower time series (adds bandpower_timeseries.csv)
  bool timeseries{false};
  double window_seconds{2.0};
  double update_seconds{0.25};

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
      << "qeeg_bandpower_cli\n\n"
      << "Compute per-channel bandpower features (CSV + JSON sidecar).\n"
      << "This is a lightweight alternative to qeeg_map_cli when you only need the\n"
      << "tabular bandpower outputs (no topomaps).\n\n"
      << "Usage:\n"
      << "  qeeg_bandpower_cli --input file.edf --outdir out_bp\n"
      << "  qeeg_bandpower_cli --input file.csv --fs 250 --outdir out_bp\n"
      << "  qeeg_bandpower_cli --input file.edf --outdir out_bp --relative --log10\n"
      << "  qeeg_bandpower_cli --input file.edf --outdir out_bp --timeseries --window 2.0 --update 0.25\n\n"
      << "Options:\n"
      << "  --input PATH            Input EDF/BDF/CSV/ASCII/BrainVision (.vhdr)\n"
      << "  --fs HZ                 Sampling rate hint for CSV (0 = infer from time column)\n"
      << "  --outdir DIR            Output directory (default: out_bandpower)\n"
      << "  --bands SPEC            Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
      << "                         Also supports: --bands iaf=10.2  or  --bands iaf:out_iaf\n"
      << "  --nperseg N             Welch segment length (default: 1024)\n"
      << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
      << "  --relative              Compute relative power: band_power / total_power\n"
      << "  --relative-range LO HI  Total-power integration range used for --relative.\n"
      << "                         Default: [min_band_fmin, max_band_fmax] from --bands.\n"
      << "  --log10                 Apply log10 transform to (relative) bandpower values\n"
      << "  --reference PATH        Reference CSV (channel,band,mean,std) to append _z columns\n"
      << "  --timeseries            Also write bandpower_timeseries.csv (sliding window)\n"
      << "  --window SECONDS        Window length for --timeseries (default: 2.0)\n"
      << "  --update SECONDS        Update interval for --timeseries (default: 0.25)\n"
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
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--relative") {
      a.relative_power = true;
    } else if (arg == "--relative-range" && i + 2 < argc) {
      a.relative_power = true;
      a.relative_range_specified = true;
      a.relative_fmin_hz = to_double(argv[++i]);
      a.relative_fmax_hz = to_double(argv[++i]);
    } else if (arg == "--log10") {
      a.log10_power = true;
    } else if (arg == "--timeseries") {
      a.timeseries = true;
    } else if ((arg == "--window" || arg == "--window-seconds") && i + 1 < argc) {
      a.timeseries = true;
      a.window_seconds = to_double(argv[++i]);
    } else if ((arg == "--update" || arg == "--update-seconds") && i + 1 < argc) {
      a.timeseries = true;
      a.update_seconds = to_double(argv[++i]);
    } else if (arg == "--reference" && i + 1 < argc) {
      a.reference_path = argv[++i];
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

static std::string fmt_double(double v, int precision = 6) {
  if (!std::isfinite(v)) return "NaN";
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

static void write_bandpowers_sidecar_json(const Args& args,
                                         const std::vector<BandDefinition>& bands,
                                         bool have_ref,
                                         bool rel_range_used,
                                         double rel_lo_hz,
                                         double rel_hi_hz) {
  const std::string outpath = args.outdir + "/bandpowers.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write bandpowers.json: " + outpath);
  out << std::setprecision(12);

  // Mirrors the BIDS *_events.json convention: top-level keys match CSV columns.
  const bool rel = args.relative_power;
  const bool lg = args.log10_power;

  auto units_for_power = [&]() -> std::string {
    if (rel) return "n/a";
    if (lg) return "log10(a.u.)";
    return "a.u.";
  };

  auto desc_suffix = [&]() -> std::string {
    std::string s;
    if (rel) {
      if (rel_range_used) {
        s += " Values are relative power fractions (band / total) where total is integrated over [" +
             fmt_double(rel_lo_hz, 4) + "," + fmt_double(rel_hi_hz, 4) + "] Hz.";
      } else {
        s += " Values are relative power fractions (band / total).";
      }
    }
    if (lg) {
      s += " Values are log10-transformed.";
    }
    return s;
  };

  out << "{\n";
  bool first = true;

  auto write_entry = [&](const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units) {
    if (!first) out << ",\n";
    first = false;
    out << "  \"" << json_escape(key) << "\": {\n";
    out << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    out << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units.empty()) {
      out << ",\n    \"Units\": \"" << json_escape(units) << "\"";
    }
    out << "\n  }";
  };

  write_entry("channel",
              "Channel label",
              "EEG channel label (one row per channel).",
              "");

  for (const auto& b : bands) {
    std::string desc = "Bandpower integrated from " + fmt_double(b.fmin_hz, 4) +
                       " to " + fmt_double(b.fmax_hz, 4) + " Hz.";
    desc += desc_suffix();
    write_entry(b.name,
                b.name + " band power",
                desc,
                units_for_power());
  }

  if (have_ref) {
    for (const auto& b : bands) {
      const std::string col = b.name + "_z";
      write_entry(col,
                  b.name + " z-score",
                  "Z-score computed relative to the provided reference CSV (channel,band,mean,std).",
                  "z");
    }
  }

  out << "\n}\n";
}


static void write_bandpower_timeseries_sidecar_json(const Args& args,
                                                   const std::vector<BandDefinition>& bands,
                                                   const std::vector<std::string>& channels,
                                                   bool have_ref,
                                                   bool rel_range_used,
                                                   double rel_lo_hz,
                                                   double rel_hi_hz) {
  const std::string outpath = args.outdir + "/bandpower_timeseries.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write bandpower_timeseries.json: " + outpath);
  out << std::setprecision(12);

  // Mirrors the BIDS *_events.json convention: top-level keys match CSV columns.
  const bool rel = args.relative_power;
  const bool lg = args.log10_power;

  auto units_for_power = [&]() -> std::string {
    if (rel) return "n/a";
    if (lg) return "log10(a.u.)";
    return "a.u.";
  };

  auto desc_suffix = [&]() -> std::string {
    std::string s;
    if (rel) {
      if (rel_range_used) {
        s += " Values are relative power fractions (band / total) where total is integrated over [" +
             fmt_double(rel_lo_hz, 4) + "," + fmt_double(rel_hi_hz, 4) + "] Hz.";
      } else {
        s += " Values are relative power fractions (band / total).";
      }
    }
    if (lg) {
      s += " Values are log10-transformed.";
    }
    return s;
  };

  const std::string ts_suffix = " Sliding-window estimate over a " + fmt_double(args.window_seconds, 3) +
                                 " s window, updated every " + fmt_double(args.update_seconds, 3) + " s.";

  out << "{\n";
  bool first = true;

  auto write_entry = [&](const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units) {
    if (!first) out << ",\n";
    first = false;
    out << "  \"" << json_escape(key) << "\": {\n";
    out << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    out << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units.empty()) {
      out << ",\n    \"Units\": \"" << json_escape(units) << "\"";
    }
    out << "\n  }";
  };

  write_entry("t_end_sec",
              "Window end time",
              "Time in seconds at the end of the analysis window (relative to recording start)." + ts_suffix,
              "s");

  for (const auto& b : bands) {
    for (const auto& ch : channels) {
      const std::string key = b.name + "_" + ch;
      std::string desc = "Bandpower integrated from " + fmt_double(b.fmin_hz, 4) +
                         " to " + fmt_double(b.fmax_hz, 4) + " Hz for channel " + ch + ".";
      desc += ts_suffix;
      desc += desc_suffix();
      write_entry(key,
                  b.name + " band power (" + ch + ")",
                  desc,
                  units_for_power());
    }
  }

  if (have_ref) {
    for (const auto& b : bands) {
      for (const auto& ch : channels) {
        const std::string key = b.name + "_" + ch + "_z";
        write_entry(key,
                    b.name + " z-score (" + ch + ")",
                    "Z-score computed relative to the provided reference CSV (channel,band,mean,std)." + ts_suffix,
                    "z");
      }
    }
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
    if (args.relative_range_specified) {
      if (args.relative_fmin_hz < 0.0 || args.relative_fmax_hz <= args.relative_fmin_hz) {
        throw std::runtime_error("--relative-range must satisfy 0 <= LO < HI");
      }
    }
    if (args.timeseries) {
      if (!(args.window_seconds > 0.0)) {
        throw std::runtime_error("--window must be > 0");
      }
      if (!(args.update_seconds > 0.0)) {
        throw std::runtime_error("--update must be > 0");
      }
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

    std::vector<BandDefinition> bands = args.band_spec.empty() ? default_eeg_bands()
                                                               : parse_band_spec(args.band_spec);
    if (bands.empty()) {
      throw std::runtime_error("No bands specified (use --bands or rely on defaults)");
    }

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    // Compute bandpower matrix [band][channel]
    std::vector<std::vector<double>> bandpower_matrix;
    bandpower_matrix.assign(bands.size(),
                            std::vector<double>(rec.n_channels(), 0.0));

    // Optional: compute total power range for relative power.
    bool rel_range_used = false;
    double rel_lo_hz = 0.0;
    double rel_hi_hz = 0.0;
    if (args.relative_power) {
      if (args.relative_range_specified) {
        rel_range_used = true;
        rel_lo_hz = args.relative_fmin_hz;
        rel_hi_hz = args.relative_fmax_hz;
      } else {
        // Default: span of provided bands.
        rel_range_used = true;
        rel_lo_hz = bands.front().fmin_hz;
        rel_hi_hz = bands.front().fmax_hz;
        for (const auto& b : bands) {
          rel_lo_hz = std::min(rel_lo_hz, b.fmin_hz);
          rel_hi_hz = std::max(rel_hi_hz, b.fmax_hz);
        }
      }
    }

    std::vector<double> total_power;
    if (args.relative_power) {
      total_power.assign(rec.n_channels(), 0.0);
    }

    for (size_t c = 0; c < rec.n_channels(); ++c) {
      const PsdResult psd = welch_psd(rec.data[c], rec.fs_hz, wopt);

      if (args.relative_power) {
        total_power[c] = integrate_bandpower(psd, rel_lo_hz, rel_hi_hz);
      }

      for (size_t b = 0; b < bands.size(); ++b) {
        bandpower_matrix[b][c] = integrate_bandpower(psd, bands[b].fmin_hz, bands[b].fmax_hz);
      }
    }

    if (args.relative_power) {
      const double eps = 1e-20;
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          bandpower_matrix[b][c] = bandpower_matrix[b][c] / std::max(eps, total_power[c]);
        }
      }
    }

    if (args.log10_power) {
      const double eps = 1e-20;
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          bandpower_matrix[b][c] = std::log10(std::max(eps, bandpower_matrix[b][c]));
        }
      }
    }

    bool have_ref = false;
    ReferenceStats ref;
    std::vector<std::vector<double>> z_matrix;
    if (!args.reference_path.empty()) {
      ref = load_reference_csv(args.reference_path);
      have_ref = true;
      z_matrix.assign(bands.size(),
                      std::vector<double>(rec.n_channels(), std::numeric_limits<double>::quiet_NaN()));
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          double z = 0.0;
          if (compute_zscore(ref, rec.channel_names[c], bands[b].name, bandpower_matrix[b][c], &z)) {
            z_matrix[b][c] = z;
          }
        }
      }
    }

    // Write bandpowers.csv (wide format; matches qeeg_map_cli)
    {
      const std::string csv_path = args.outdir + "/bandpowers.csv";
      std::ofstream out(std::filesystem::u8path(csv_path), std::ios::binary);
      if (!out) throw std::runtime_error("Failed to write bandpowers.csv: " + csv_path);

      out << "channel";
      for (const auto& b : bands) out << "," << b.name;
      if (have_ref) {
        for (const auto& b : bands) out << "," << b.name << "_z";
      }
      out << "\n";

      out << std::setprecision(12);
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        out << rec.channel_names[c];
        for (size_t b = 0; b < bands.size(); ++b) {
          out << "," << bandpower_matrix[b][c];
        }
        if (have_ref) {
          for (size_t b = 0; b < bands.size(); ++b) {
            out << "," << z_matrix[b][c];
          }
        }
        out << "\n";
      }
    }

    // JSON sidecar describing columns in bandpowers.csv
    write_bandpowers_sidecar_json(args, bands, have_ref, rel_range_used, rel_lo_hz, rel_hi_hz);

    std::vector<std::string> outs;
    outs.push_back("bandpowers.csv");
    outs.push_back("bandpowers.json");

    if (args.timeseries) {
      const std::string ts_path = args.outdir + "/bandpower_timeseries.csv";
      std::ofstream out_ts(std::filesystem::u8path(ts_path), std::ios::binary);
      if (!out_ts) throw std::runtime_error("Failed to write bandpower_timeseries.csv: " + ts_path);

      out_ts << "t_end_sec";
      for (const auto& b : bands) {
        for (const auto& ch : rec.channel_names) {
          out_ts << "," << b.name << "_" << ch;
        }
      }
      if (have_ref) {
        for (const auto& b : bands) {
          for (const auto& ch : rec.channel_names) {
            out_ts << "," << b.name << "_" << ch << "_z";
          }
        }
      }
      out_ts << "\n";
      out_ts << std::setprecision(12);

      OnlineBandpowerOptions opt;
      opt.window_seconds = args.window_seconds;
      opt.update_seconds = args.update_seconds;
      opt.welch = wopt;
      opt.relative_power = args.relative_power;
      if (args.relative_range_specified) {
        opt.relative_fmin_hz = args.relative_fmin_hz;
        opt.relative_fmax_hz = args.relative_fmax_hz;
      }
      opt.log10_power = args.log10_power;

      OnlineWelchBandpower eng(rec.channel_names, rec.fs_hz, bands, opt);

      const size_t chunk_samples = 512;
      std::vector<std::vector<float>> block(rec.n_channels());
      for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
        const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          block[c].assign(rec.data[c].begin() + static_cast<std::ptrdiff_t>(pos),
                          rec.data[c].begin() + static_cast<std::ptrdiff_t>(end));
        }

        const auto frames = eng.push_block(block);
        for (const auto& fr : frames) {
          out_ts << fr.t_end_sec;
          for (size_t b = 0; b < fr.bands.size(); ++b) {
            for (size_t c = 0; c < fr.channel_names.size(); ++c) {
              out_ts << "," << fr.powers[b][c];
            }
          }
          if (have_ref) {
            for (size_t b = 0; b < fr.bands.size(); ++b) {
              for (size_t c = 0; c < fr.channel_names.size(); ++c) {
                const double v = fr.powers[b][c];
                double z = std::numeric_limits<double>::quiet_NaN();
                if (std::isfinite(v)) {
                  double tmp = 0.0;
                  if (compute_zscore(ref, fr.channel_names[c], fr.bands[b].name, v, &tmp)) {
                    z = tmp;
                  }
                }
                out_ts << "," << z;
              }
            }
          }
          out_ts << "\n";
        }
      }

      write_bandpower_timeseries_sidecar_json(args, bands, rec.channel_names, have_ref,
                                              rel_range_used, rel_lo_hz, rel_hi_hz);
      outs.push_back("bandpower_timeseries.csv");
      outs.push_back("bandpower_timeseries.json");
    }

    // Lightweight run manifest for qeeg_ui_cli / qeeg_ui_server_cli
    {
      const std::string meta_path = args.outdir + "/bandpower_run_meta.json";
      outs.push_back("bandpower_run_meta.json");
      if (!write_run_meta_json(meta_path, "qeeg_bandpower_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    }

    std::cout << "Wrote: " << args.outdir << "/bandpowers.csv" << "\n";
    if (args.timeseries) {
      std::cout << "Wrote: " << args.outdir << "/bandpower_timeseries.csv" << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
