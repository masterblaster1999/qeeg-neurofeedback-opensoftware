#include "qeeg/bandpower.hpp"
#include "qeeg/bmp_writer.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out"};
  std::string montage_spec{"builtin:standard_1020_19"};
  std::string band_spec; // empty => default
  std::string reference_path;

  bool demo{false};
  double fs_csv{0.0};
  double demo_seconds{10.0};

  bool average_reference{false};

  // Optional preprocessing filters
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};

  bool export_psd{false};

  size_t nperseg{1024};
  double overlap{0.5};
  int grid{256};

  // Topomap interpolation
  std::string interp{"idw"};  // idw | spline
  double idw_power{2.0};

  // Spherical spline parameters
  int spline_terms{50};
  int spline_m{4};
  double spline_lambda{1e-5};
};

static void print_help() {
  std::cout
    << "qeeg_map_cli (first pass)\n\n"
    << "Usage:\n"
    << "  qeeg_map_cli --input file.edf --outdir out\n"
    << "  qeeg_map_cli --input file.csv --fs 250 --outdir out\n"
    << "  qeeg_map_cli --demo --fs 250 --seconds 10 --outdir out_demo\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF or CSV\n"
    << "  --fs HZ                 Sampling rate for CSV (required for CSV); also used for --demo\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --montage SPEC          'builtin:standard_1020_19' (default) or PATH to montage CSV\n"
    << "  --bands SPEC            Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
    << "  --reference PATH        Reference CSV (channel,band,mean,std) to compute z-maps\n"
    << "  --nperseg N             Welch segment length (default: 1024)\n"
    << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --grid N                Topomap grid size (default: 256)\n"
    << "  --interp METHOD         Topomap interpolation: idw|spline (default: idw)\n"
    << "  --idw-power P           IDW power parameter (default: 2.0)\n"
    << "  --spline-terms N        Spherical spline Legendre terms (default: 50)\n"
    << "  --spline-m N            Spherical spline order m (default: 4)\n"
    << "  --spline-lambda X       Spline regularization (default: 1e-5)\n"
    << "  --average-reference     Apply common average reference across channels\n"
    << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q             Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase            Offline: forward-backward filtering (less phase distortion)\n"
    << "  --export-psd            Write psd.csv (freq + PSD per channel)\n"
    << "  --demo                  Generate synthetic recording instead of reading file\n"
    << "  --seconds S             Duration for --demo (default: 10)\n"
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
    } else if (arg == "--montage" && i + 1 < argc) {
      a.montage_spec = argv[++i];
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--reference" && i + 1 < argc) {
      a.reference_path = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--grid" && i + 1 < argc) {
      a.grid = to_int(argv[++i]);
    } else if (arg == "--interp" && i + 1 < argc) {
      a.interp = to_lower(argv[++i]);
    } else if (arg == "--idw-power" && i + 1 < argc) {
      a.idw_power = to_double(argv[++i]);
    } else if (arg == "--spline-terms" && i + 1 < argc) {
      a.spline_terms = to_int(argv[++i]);
    } else if (arg == "--spline-m" && i + 1 < argc) {
      a.spline_m = to_int(argv[++i]);
    } else if (arg == "--spline-lambda" && i + 1 < argc) {
      a.spline_lambda = to_double(argv[++i]);
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
    } else if (arg == "--export-psd") {
      a.export_psd = true;
    } else if (arg == "--demo") {
      a.demo = true;
    } else if (arg == "--seconds" && i + 1 < argc) {
      a.demo_seconds = to_double(argv[++i]);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static Montage load_montage(const std::string& spec) {
  std::string low = to_lower(spec);
  if (low == "builtin:standard_1020_19" || low == "standard_1020_19" ||
      low == "builtin" || low == "default") {
    return Montage::builtin_standard_1020_19();
  }
  return Montage::load_csv(spec);
}

static EEGRecording make_demo_recording(const Montage& montage, double fs_hz, double seconds) {
  if (fs_hz <= 0.0) throw std::runtime_error("--demo requires --fs > 0");
  if (seconds <= 0.0) seconds = 10.0;

  EEGRecording rec;
  rec.fs_hz = fs_hz;

  // We'll use montage channel names (lowercase keys). For output readability,
  // try to use canonical 19-channel names in this order if present.
  const std::vector<std::string> canonical = {
    "Fp1","Fp2","F7","F3","Fz","F4","F8",
    "T3","C3","Cz","C4","T4",
    "T5","P3","Pz","P4","T6","O1","O2"
  };

  for (const auto& ch : canonical) {
    if (montage.has(ch)) rec.channel_names.push_back(ch);
  }
  if (rec.channel_names.empty()) {
    // Fallback to montage keys (already lowercase), not ideal but functional.
    rec.channel_names = montage.channel_names();
  }

  const size_t n = static_cast<size_t>(std::round(seconds * fs_hz));
  rec.data.assign(rec.channel_names.size(), std::vector<float>(n, 0.0f));

  std::mt19937 rng(12345);
  std::normal_distribution<double> noise(0.0, 1.0);
  const double pi = std::acos(-1.0);

  // Build spatial patterns based on electrode x,y
  for (size_t c = 0; c < rec.channel_names.size(); ++c) {
    Vec2 p;
    montage.get(rec.channel_names[c], &p);

    // Spatial weighting examples:
    double frontal = std::max(0.0, p.y);  // y>0 => frontal
    double occip = std::max(0.0, -p.y);
    double left = std::max(0.0, -p.x);
    double right = std::max(0.0, p.x);

    // Base amplitudes (arbitrary units)
    double a_delta = 5.0 * (0.2 + 0.8 * occip);
    double a_theta = 3.0 * (0.3 + 0.7 * frontal);
    double a_alpha = 8.0 * (0.2 + 0.8 * occip);
    double a_beta  = 2.0 * (0.5 + 0.5 * (left + right) * 0.5);

    // Slight lateralization
    a_alpha *= (1.0 + 0.2 * (right - left));
    a_theta *= (1.0 + 0.1 * (left - right));

    for (size_t i = 0; i < n; ++i) {
      double t = static_cast<double>(i) / fs_hz;
      double v =
          a_delta * std::sin(2.0 * pi * 2.0  * t) +
          a_theta * std::sin(2.0 * pi * 6.0  * t) +
          a_alpha * std::sin(2.0 * pi * 10.0 * t) +
          a_beta  * std::sin(2.0 * pi * 20.0 * t) +
          0.8 * noise(rng);
      rec.data[c][i] = static_cast<float>(v);
    }
  }

  return rec;
}

static std::pair<double,double> minmax_ignore_nan(const std::vector<float>& v) {
  double mn = std::numeric_limits<double>::infinity();
  double mx = -std::numeric_limits<double>::infinity();
  for (float x : v) {
    if (std::isnan(x)) continue;
    mn = std::min(mn, static_cast<double>(x));
    mx = std::max(mx, static_cast<double>(x));
  }
  if (!std::isfinite(mn) || !std::isfinite(mx)) return {0.0, 1.0};
  if (mx <= mn) return {mn, mn + 1e-12};
  return {mn, mx};
}

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);

    ensure_directory(args.outdir);

    Montage montage = load_montage(args.montage_spec);

    EEGRecording rec;
    if (args.demo) {
      rec = make_demo_recording(montage, args.fs_csv, args.demo_seconds);
    } else {
      if (args.input_path.empty()) {
        print_help();
        throw std::runtime_error("--input is required (or use --demo)");
      }
      rec = read_recording_auto(args.input_path, args.fs_csv);
    }

    if (rec.n_channels() < 3) throw std::runtime_error("Need at least 3 channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";

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

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    // Compute PSD for each channel
    std::vector<PsdResult> psds(rec.n_channels());
    for (size_t c = 0; c < rec.n_channels(); ++c) {
      psds[c] = welch_psd(rec.data[c], rec.fs_hz, wopt);
    }

    // Optional PSD export
    if (args.export_psd) {
      std::cout << "Writing psd.csv...\n";
      std::ofstream psd_out(args.outdir + "/psd.csv");
      if (!psd_out) throw std::runtime_error("Failed to write psd.csv");

      // Assume same freqs for all channels (true given same fs and nperseg here)
      psd_out << "freq_hz";
      for (const auto& ch : rec.channel_names) psd_out << "," << ch;
      psd_out << "\n";

      const auto& f0 = psds[0].freqs_hz;
      for (size_t k = 0; k < f0.size(); ++k) {
        psd_out << f0[k];
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          psd_out << "," << psds[c].psd[k];
        }
        psd_out << "\n";
      }
    }

    // Compute bandpowers
    std::vector<std::vector<double>> bandpower_matrix(bands.size(),
                                                      std::vector<double>(rec.n_channels(), 0.0));
    for (size_t b = 0; b < bands.size(); ++b) {
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        bandpower_matrix[b][c] = integrate_bandpower(psds[c], bands[b].fmin_hz, bands[b].fmax_hz);
      }
    }

    // Load reference if provided
    bool have_ref = false;
    ReferenceStats ref;
    if (!args.reference_path.empty()) {
      std::cout << "Loading reference: " << args.reference_path << "\n";
      ref = load_reference_csv(args.reference_path);
      have_ref = true;
    }

    // Write bandpowers.csv
    {
      std::ofstream out(args.outdir + "/bandpowers.csv");
      if (!out) throw std::runtime_error("Failed to write bandpowers.csv");
      out << "channel";
      for (const auto& b : bands) out << "," << b.name;
      if (have_ref) {
        for (const auto& b : bands) out << "," << b.name << "_z";
      }
      out << "\n";

      for (size_t c = 0; c < rec.n_channels(); ++c) {
        out << rec.channel_names[c];
        for (size_t b = 0; b < bands.size(); ++b) {
          out << "," << bandpower_matrix[b][c];
        }
        if (have_ref) {
          for (size_t b = 0; b < bands.size(); ++b) {
            double z = 0.0;
            bool ok = compute_zscore(ref, rec.channel_names[c], bands[b].name, bandpower_matrix[b][c], &z);
            out << "," << (ok ? z : std::numeric_limits<double>::quiet_NaN());
          }
        }
        out << "\n";
      }
    }

    // Render maps per band
    TopomapOptions topt;
    topt.grid_size = args.grid;
    topt.idw_power = args.idw_power;

    if (args.interp == "spline" || args.interp == "spherical_spline" || args.interp == "spherical-spline") {
      topt.method = TopomapInterpolation::SPHERICAL_SPLINE;
    } else {
      topt.method = TopomapInterpolation::IDW;
    }
    topt.spline.n_terms = args.spline_terms;
    topt.spline.m = args.spline_m;
    topt.spline.lambda = args.spline_lambda;

    for (size_t b = 0; b < bands.size(); ++b) {
      std::cout << "Rendering band: " << bands[b].name << "\n";
      // per-channel values in rec channel order
      std::vector<double> values(rec.n_channels());
      for (size_t c = 0; c < rec.n_channels(); ++c) values[c] = bandpower_matrix[b][c];

      Grid2D grid = make_topomap(montage, rec.channel_names, values, topt);
      auto [vmin, vmax] = minmax_ignore_nan(grid.values);

      const std::string outpath = args.outdir + "/topomap_" + bands[b].name + ".bmp";
      render_grid_to_bmp(outpath, grid.size, grid.values, vmin, vmax);

      if (have_ref) {
        std::vector<double> zvals(rec.n_channels(), std::numeric_limits<double>::quiet_NaN());
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          double z = 0.0;
          if (compute_zscore(ref, rec.channel_names[c], bands[b].name, bandpower_matrix[b][c], &z)) {
            zvals[c] = z;
          }
        }
        Grid2D zg = make_topomap(montage, rec.channel_names, zvals, topt);
        // common visualization range
        const std::string zout = args.outdir + "/topomap_" + bands[b].name + "_z.bmp";
        render_grid_to_bmp(zout, zg.size, zg.values, -3.0, 3.0);
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
