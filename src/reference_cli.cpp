#include "qeeg/bandpower.hpp"
#include "qeeg/online_bandpower.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/robust_stats.hpp"
#include "qeeg/running_stats.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace qeeg;

struct Args {
  std::vector<std::string> input_paths;   // one or more
  std::string list_path;                  // optional: text file with one path per line

  std::string outdir{"out_reference"};
  std::string out_csv{"reference.csv"}; // if no path separators, written inside outdir

  // Recording
  double fs_csv{0.0};

  // Bands + PSD
  std::string band_spec; // empty => default
  size_t nperseg{1024};
  double overlap{0.5};

  // Optional: build a reference distribution from sliding windows (more consistent
  // with qeeg_nf_cli real-time bandpower frames).
  // When both are > 0, reference values are accumulated over all emitted frames
  // rather than one value per file.
  double window_seconds{0.0};
  double update_seconds{0.0};
  double chunk_seconds{0.10};

  // If enabled, compute relative bandpower (band / total within a range).
  bool relative_power{false};
  bool relative_range_specified{false};
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};

  bool log10_power{false};
  bool robust{false};

  // Robust mode with windowed references can grow very large. We cap per-key sample
  // storage via reservoir sampling to keep memory bounded.
  std::size_t robust_max_samples_per_key{20000};

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
    << "qeeg_reference_cli (dataset reference builder)\n\n"
    << "Build a simple reference CSV (channel,band,mean,std) from one or more recordings.\n"
    << "This can be passed to qeeg_map_cli --reference to compute z-scores.\n\n"
    << "Usage:\n"
    << "  qeeg_reference_cli --input a.edf --input b.edf --outdir out_ref\n"
    << "  qeeg_reference_cli --list recordings.txt --outdir out_ref\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV (repeatable)\n"
    << "  --list PATH             Text file with one input path per line (\"#\" comments ok)\n"
    << "  --fs HZ                 Sampling rate for CSV inputs (required if any input is CSV)\n"
    << "  --outdir DIR            Output directory (default: out_reference)\n"
    << "  --out PATH              Output CSV file (default: reference.csv). If no path separators,\n"
    << "                         the file is written inside --outdir.\n"
    << "  --bands SPEC            Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
    << "  --nperseg N             Welch segment length (default: 1024)\n"
    << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --window S              Optional: sliding window seconds (enables windowed reference mode when used with --update)\n"
    << "  --update S              Optional: update interval seconds (windowed reference mode)\n"
    << "  --chunk S               Optional: input chunk seconds for windowed mode (default: 0.10)\n"
    << "  --relative              Compute relative power: band_power / total_power\n"
    << "  --relative-range LO HI  Total-power integration range used for --relative.\n"
    << "                         Default: [min_band_fmin, max_band_fmax] from --bands.\n"
    << "  --log10                 Accumulate log10(power) instead of raw power\n"
    << "  --robust                Use median + MAD-derived scale (robust) instead of mean + std\n"
    << "  --robust-max-per-key N  Robust mode: cap stored samples per (channel,band) using reservoir sampling (default: 20000)\n"
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
      a.input_paths.push_back(argv[++i]);
    } else if (arg == "--list" && i + 1 < argc) {
      a.list_path = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      a.out_csv = argv[++i];
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_seconds = to_double(argv[++i]);
    } else if (arg == "--update" && i + 1 < argc) {
      a.update_seconds = to_double(argv[++i]);
    } else if (arg == "--chunk" && i + 1 < argc) {
      a.chunk_seconds = to_double(argv[++i]);
    } else if (arg == "--relative") {
      a.relative_power = true;
    } else if (arg == "--relative-range" && i + 2 < argc) {
      a.relative_power = true;
      a.relative_range_specified = true;
      a.relative_fmin_hz = to_double(argv[++i]);
      a.relative_fmax_hz = to_double(argv[++i]);
    } else if (arg == "--log10") {
      a.log10_power = true;
    } else if (arg == "--robust") {
      a.robust = true;
    } else if (arg == "--robust-max-per-key" && i + 1 < argc) {
      a.robust_max_samples_per_key = static_cast<std::size_t>(std::max(1, to_int(argv[++i])));
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

static void reservoir_update(std::vector<double>* reservoir,
                             std::size_t* seen,
                             double x,
                             std::size_t max_k,
                             std::mt19937* rng) {
  if (!reservoir || !seen || !rng) return;
  if (max_k == 0) max_k = 1;
  (*seen) += 1;
  if (reservoir->size() < max_k) {
    reservoir->push_back(x);
    return;
  }
  // Classic reservoir sampling: replace an existing element with probability k/n.
  std::uniform_int_distribution<std::size_t> dist(0, (*seen) - 1);
  const std::size_t j = dist(*rng);
  if (j < max_k) {
    (*reservoir)[j] = x;
  }
}

static std::string resolve_out_path(const std::string& outdir, const std::string& path_or_name) {
  if (path_or_name.empty()) return path_or_name;
  // If it looks like a filename (no path separators), write inside outdir.
  if (path_or_name.find('/') == std::string::npos && path_or_name.find('\\') == std::string::npos) {
    return outdir + "/" + path_or_name;
  }
  return path_or_name;
}

static void load_list_file(const std::string& path, std::vector<std::string>* out_paths) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open list file: " + path);
  std::string line;
  while (std::getline(f, line)) {
    std::string t = trim(line);
    if (t.empty()) continue;
    if (starts_with(t, "#")) continue;
    out_paths->push_back(t);
  }
}

template <typename T>
static std::vector<std::pair<std::string, std::string>> sorted_keys(
    const std::unordered_map<std::string, T>& stats) {
  // stats map key is "band|channel" (lowercased). Convert to (channel, band) for output sorting.
  std::vector<std::pair<std::string, std::string>> keys;
  keys.reserve(stats.size());
  for (const auto& kv : stats) {
    const std::string& key = kv.first;
    const auto parts = split(key, '|');
    if (parts.size() == 2) {
      keys.emplace_back(parts[1], parts[0]);
    }
  }
  std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;  // channel
    return a.second < b.second;                         // band
  });
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    std::vector<std::string> inputs = args.input_paths;
    if (!args.list_path.empty()) {
      load_list_file(args.list_path, &inputs);
    }

    if (inputs.empty()) {
      print_help();
      throw std::runtime_error("At least one --input (or --list) is required");
    }

    if (args.overlap < 0.0 || args.overlap >= 1.0) {
      throw std::runtime_error("--overlap must be in [0,1)");
    }
    if (args.nperseg < 16) {
      throw std::runtime_error("--nperseg too small (>=16 recommended)");
    }

    const bool windowed_mode = (args.window_seconds > 0.0 && args.update_seconds > 0.0);
    if ((args.window_seconds > 0.0) != (args.update_seconds > 0.0)) {
      throw std::runtime_error("Windowed reference mode requires both --window and --update to be set > 0");
    }
    if (windowed_mode) {
      if (!(args.update_seconds > 0.0 && args.window_seconds > 0.0)) {
        throw std::runtime_error("--window and --update must be > 0");
      }
      if (args.chunk_seconds <= 0.0) {
        throw std::runtime_error("--chunk must be > 0 in windowed mode");
      }
    }

    ensure_directory(args.outdir);
    const std::string out_csv = resolve_out_path(args.outdir, args.out_csv);

    const std::vector<BandDefinition> bands = parse_band_spec(args.band_spec);
    if (bands.empty()) throw std::runtime_error("No bands specified");

    // Determine total-power integration range for relative bandpower.
    double rel_lo = 0.0;
    double rel_hi = 0.0;
    if (args.relative_power) {
      if (args.relative_range_specified) {
        rel_lo = args.relative_fmin_hz;
        rel_hi = args.relative_fmax_hz;
      } else {
        rel_lo = bands[0].fmin_hz;
        rel_hi = bands[0].fmax_hz;
        for (const auto& b : bands) {
          rel_lo = std::min(rel_lo, b.fmin_hz);
          rel_hi = std::max(rel_hi, b.fmax_hz);
        }
      }
      if (!(rel_hi > rel_lo)) {
        throw std::runtime_error("--relative-range must satisfy LO < HI");
      }
    }

    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    // Accumulate one sample per input file.
    // - default: mean/std via RunningStats
    // - --robust: keep per-key vectors and compute median/MAD-derived scale at end
    std::unordered_map<std::string, RunningStats> stats;               // key: band|channel
    std::unordered_map<std::string, std::vector<double>> robust_vals;  // key: band|channel

    // For robust reservoir sampling.
    std::unordered_map<std::string, std::size_t> robust_seen; // key: band|channel -> total samples observed
    robust_seen.reserve(1024);
    std::mt19937 rng(1337);

    size_t n_ok = 0;
    for (const auto& path : inputs) {
      if (trim(path).empty()) continue;

      EEGRecording rec = read_recording_auto(path, args.fs_csv);
      if (rec.n_channels() < 1) {
        std::cerr << "Skipping (no channels): " << path << "\n";
        continue;
      }
      if (rec.fs_hz <= 0.0) {
        std::cerr << "Skipping (invalid fs): " << path << "\n";
        continue;
      }

      // Optional preprocessing (offline).
      const bool do_pre = popt.average_reference || popt.notch_hz > 0.0 ||
                          popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0;
      if (do_pre) {
        preprocess_recording_inplace(rec, popt);
      }

      // Compute per-channel PSD, then integrate each band.
      const double eps = 1e-20;
      if (!windowed_mode) {
        std::vector<PsdResult> psds(rec.n_channels());
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          psds[c] = welch_psd(rec.data[c], rec.fs_hz, wopt);
        }

        std::vector<double> total_power;
        if (args.relative_power) {
          total_power.resize(rec.n_channels(), 0.0);
          for (size_t c = 0; c < rec.n_channels(); ++c) {
            total_power[c] = integrate_bandpower(psds[c], rel_lo, rel_hi);
          }
        }

        for (size_t b = 0; b < bands.size(); ++b) {
          for (size_t c = 0; c < rec.n_channels(); ++c) {
            double v = integrate_bandpower(psds[c], bands[b].fmin_hz, bands[b].fmax_hz);
            if (args.relative_power) {
              v = v / std::max(eps, total_power[c]);
            }
            if (args.log10_power) v = std::log10(std::max(eps, v));

            const std::string key = to_lower(bands[b].name) + "|" + to_lower(rec.channel_names[c]);
            if (args.robust) {
              // One sample per file, so we can store directly.
              robust_vals[key].push_back(v);
            } else {
              stats[key].add(v);
            }
          }
        }
      } else {
        // Windowed mode: accumulate values from all emitted frames.
        OnlineBandpowerOptions opt;
        opt.window_seconds = args.window_seconds;
        opt.update_seconds = args.update_seconds;
        opt.welch = wopt;
        opt.relative_power = args.relative_power;
        opt.relative_fmin_hz = rel_lo;
        opt.relative_fmax_hz = rel_hi;
        opt.log10_power = args.log10_power;
        OnlineWelchBandpower eng(rec.channel_names, rec.fs_hz, bands, opt);

        const size_t chunk_samples = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(args.chunk_seconds * rec.fs_hz)));
        std::vector<std::vector<float>> block(rec.n_channels());

        for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
          const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
          for (size_t c = 0; c < rec.n_channels(); ++c) {
            block[c].assign(rec.data[c].begin() + static_cast<std::ptrdiff_t>(pos),
                            rec.data[c].begin() + static_cast<std::ptrdiff_t>(end));
          }
          const auto frames = eng.push_block(block);
          for (const auto& fr : frames) {
            for (size_t b = 0; b < fr.bands.size(); ++b) {
              for (size_t c = 0; c < fr.channel_names.size(); ++c) {
                const double v = fr.powers[b][c];
                if (!std::isfinite(v)) continue;
                const std::string key = to_lower(fr.bands[b].name) + "|" + to_lower(fr.channel_names[c]);
                if (args.robust) {
                  std::size_t& seen = robust_seen[key];
                  reservoir_update(&robust_vals[key], &seen, v, args.robust_max_samples_per_key, &rng);
                } else {
                  stats[key].add(v);
                }
              }
            }
          }
        }
      }

      ++n_ok;
      std::cout << "Processed: " << path << " (channels=" << rec.n_channels()
                << ", fs=" << rec.fs_hz << " Hz, samples=" << rec.n_samples() << ")\n";
    }

    if (n_ok < 1) {
      throw std::runtime_error("No valid inputs processed");
    }

    std::ofstream out(out_csv);
    if (!out) throw std::runtime_error("Failed to write: " + out_csv);

    out << "# qeeg_reference_cli\n";
    out << "# n_files=" << n_ok << "\n";
    out << "# log10_power=" << (args.log10_power ? 1 : 0) << "\n";
    out << "# relative_power=" << (args.relative_power ? 1 : 0) << "\n";
    if (args.relative_power) {
      out << "# relative_fmin_hz=" << rel_lo << "\n";
      out << "# relative_fmax_hz=" << rel_hi << "\n";
    }
    out << "# robust=" << (args.robust ? 1 : 0) << "\n";
    out << "# welch_nperseg=" << args.nperseg << "\n";
    out << "# welch_overlap=" << args.overlap << "\n";
    out << "# windowed_mode=" << (windowed_mode ? 1 : 0) << "\n";
    if (windowed_mode) {
      out << "# window_seconds=" << args.window_seconds << "\n";
      out << "# update_seconds=" << args.update_seconds << "\n";
      out << "# chunk_seconds=" << args.chunk_seconds << "\n";
      out << "# robust_max_samples_per_key=" << args.robust_max_samples_per_key << "\n";
    }
    out << "# band_spec=" << (args.band_spec.empty() ? std::string("<default>") : args.band_spec) << "\n";
    out << "# channel,band,mean,std,n\n";

    if (args.robust) {
      const auto keys = sorted_keys(robust_vals);
      for (const auto& cb : keys) {
        const std::string& ch = cb.first;
        const std::string& band = cb.second;
        const std::string key = band + "|" + ch;

        auto it = robust_vals.find(key);
        if (it == robust_vals.end()) continue;
        const auto& v = it->second;
        if (v.size() < 2) continue;

        std::vector<double> tmp = v;
        const double med = median_inplace(&tmp);
        const double scale = robust_scale(v, med);
        if (!std::isfinite(med) || !std::isfinite(scale) || !(scale > 0.0)) continue;

        // Keep output compatible with load_reference_csv(): first 4 columns are channel,band,mean,std
        out << ch << "," << band << "," << med << "," << scale << "," << v.size() << "\n";
      }
    } else {
      const auto keys = sorted_keys(stats);
      for (const auto& cb : keys) {
        const std::string& ch = cb.first;
        const std::string& band = cb.second;
        const std::string key = band + "|" + ch;

        auto it = stats.find(key);
        if (it == stats.end()) continue;
        const RunningStats& rs = it->second;
        if (rs.n() < 2) continue; // need variance
        const double mean = rs.mean();
        const double stdv = rs.stddev_sample();
        if (!std::isfinite(mean) || !std::isfinite(stdv) || !(stdv > 0.0)) continue;

        // Keep output compatible with load_reference_csv(): first 4 columns are channel,band,mean,std
        out << ch << "," << band << "," << mean << "," << stdv << "," << rs.n() << "\n";
      }
    }

    std::cout << "Wrote reference: " << out_csv << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
