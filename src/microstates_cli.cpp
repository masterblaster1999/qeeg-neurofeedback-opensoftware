#include "qeeg/bmp_writer.hpp"
#include "qeeg/microstates.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/topomap.hpp"
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
  std::string outdir{"out"};
  std::string montage_spec{"builtin:standard_1020_19"};

  // CSV inputs
  double fs_csv{0.0};

  // Analysis window (optional)
  double start_sec{0.0};
  double duration_sec{0.0};  // 0 => full

  // Microstates
  int k{4};
  double peak_fraction{0.10};
  size_t max_peaks{1000};
  double min_peak_distance_ms{0.0};
  double min_duration_ms{0.0};

  bool polarity_invariant{true};
  bool demean_topography{true};

  // Rendering
  int grid{256};
  std::string interp{"idw"};
  double idw_power{2.0};
  bool annotate{true};

  // Spherical spline
  int spline_terms{50};
  int spline_m{4};
  double spline_lambda{1e-5};

  // Preprocess
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static void print_help() {
  std::cout
    << "qeeg_microstates_cli (first pass microstate analysis)\n\n"
    << "Usage:\n"
    << "  qeeg_microstates_cli --input file.edf --outdir out_ms\n"
    << "  qeeg_microstates_cli --input file.csv --fs 250 --outdir out_ms\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV\n"
    << "  --fs HZ                 Sampling rate for CSV (required for CSV)\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --montage SPEC          builtin:standard_1020_19 (default) or PATH to montage CSV\n"
    << "  --start S               Start time in seconds (default: 0)\n"
    << "  --duration S            Duration in seconds (0 => full remainder)\n"
    << "  --k N                   Number of microstates (default: 4)\n"
    << "  --peak-fraction F        Fraction of GFP peaks used for clustering (default: 0.10)\n"
    << "  --max-peaks N            Cap number of peaks for clustering (default: 1000)\n"
    << "  --min-peak-distance-ms M Minimum spacing between selected GFP peaks (default: 0)\n"
    << "  --min-duration-ms M      Minimum microstate segment duration (merge shorter) (default: 0)\n"
    << "  --no-polarity-invariant  Treat maps as signed (disable polarity invariance)\n"
    << "  --no-demean              Do not subtract channel-mean from each topography\n"
    << "  --grid N                 Topomap grid size (default: 256)\n"
    << "  --interp METHOD          Topomap interpolation: idw|spline (default: idw)\n"
    << "  --idw-power P            IDW power parameter (default: 2.0)\n"
    << "  --spline-terms N         Spherical spline Legendre terms (default: 50)\n"
    << "  --spline-m N             Spherical spline order m (default: 4)\n"
    << "  --spline-lambda X        Spline regularization (default: 1e-5)\n"
    << "  --no-annotate            Do not draw head outline/electrodes + colorbar\n"
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
    } else if (arg == "--montage" && i + 1 < argc) {
      a.montage_spec = argv[++i];
    } else if (arg == "--start" && i + 1 < argc) {
      a.start_sec = to_double(argv[++i]);
    } else if (arg == "--duration" && i + 1 < argc) {
      a.duration_sec = to_double(argv[++i]);
    } else if (arg == "--k" && i + 1 < argc) {
      a.k = to_int(argv[++i]);
    } else if (arg == "--peak-fraction" && i + 1 < argc) {
      a.peak_fraction = to_double(argv[++i]);
    } else if (arg == "--max-peaks" && i + 1 < argc) {
      a.max_peaks = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--min-peak-distance-ms" && i + 1 < argc) {
      a.min_peak_distance_ms = to_double(argv[++i]);
    } else if (arg == "--min-duration-ms" && i + 1 < argc) {
      a.min_duration_ms = to_double(argv[++i]);
    } else if (arg == "--no-polarity-invariant") {
      a.polarity_invariant = false;
    } else if (arg == "--no-demean") {
      a.demean_topography = false;
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
    } else if (arg == "--no-annotate") {
      a.annotate = false;
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

static Montage load_montage(const std::string& spec) {
  std::string low = to_lower(spec);
  if (low == "builtin:standard_1020_19" || low == "standard_1020_19" ||
      low == "builtin" || low == "default") {
    return Montage::builtin_standard_1020_19();
  }
  return Montage::load_csv(spec);
}

static std::string state_name(int k) {
  if (k >= 0 && k < 26) {
    char c = static_cast<char>('A' + k);
    return std::string(1, c);
  }
  return std::to_string(k);
}

static EEGRecording slice_recording(const EEGRecording& rec, double start_sec, double duration_sec) {
  if (start_sec <= 0.0 && duration_sec <= 0.0) return rec;
  EEGRecording out = rec;

  const double fs = rec.fs_hz;
  const size_t N = rec.n_samples();
  size_t start = 0;
  if (start_sec > 0.0) {
    start = static_cast<size_t>(std::llround(start_sec * fs));
    if (start > N) start = N;
  }

  size_t end = N;
  if (duration_sec > 0.0) {
    size_t len = static_cast<size_t>(std::llround(duration_sec * fs));
    end = std::min(N, start + len);
  }

  for (size_t c = 0; c < out.data.size(); ++c) {
    if (start >= out.data[c].size()) {
      out.data[c].clear();
      continue;
    }
    out.data[c] = std::vector<float>(out.data[c].begin() + static_cast<std::ptrdiff_t>(start),
                                     out.data[c].begin() + static_cast<std::ptrdiff_t>(end));
  }

  // Note: events are left unchanged (still relative to file start). This CLI focuses
  // on continuous analysis, so we don't export events here.
  return out;
}

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    if (a.input_path.empty()) throw std::runtime_error("--input is required");

    ensure_directory(a.outdir);

    EEGRecording rec = read_recording_auto(a.input_path, a.fs_csv);
    if (rec.n_channels() < 2) throw std::runtime_error("Need >=2 channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    rec = slice_recording(rec, a.start_sec, a.duration_sec);
    if (rec.n_samples() < 3) throw std::runtime_error("Not enough samples after slicing");

    PreprocessOptions popt;
    popt.average_reference = a.average_reference;
    popt.notch_hz = a.notch_hz;
    popt.notch_q = a.notch_q;
    popt.bandpass_low_hz = a.bandpass_low_hz;
    popt.bandpass_high_hz = a.bandpass_high_hz;
    popt.zero_phase = a.zero_phase;
    preprocess_recording_inplace(rec, popt);

    Montage montage = load_montage(a.montage_spec);

    MicrostatesOptions msopt;
    msopt.k = a.k;
    msopt.peak_pick_fraction = a.peak_fraction;
    msopt.max_peaks = a.max_peaks;
    msopt.min_peak_distance_samples = (a.min_peak_distance_ms > 0.0)
        ? static_cast<size_t>(std::llround(a.min_peak_distance_ms * 1e-3 * rec.fs_hz))
        : 0;
    msopt.min_segment_samples = (a.min_duration_ms > 0.0)
        ? static_cast<int>(std::llround(a.min_duration_ms * 1e-3 * rec.fs_hz))
        : 0;
    msopt.polarity_invariant = a.polarity_invariant;
    msopt.demean_topography = a.demean_topography;

    MicrostatesResult r = estimate_microstates(rec, msopt);
    const int K = static_cast<int>(r.templates.size());

    // --- Write templates ---
    {
      std::ofstream f(a.outdir + "/microstate_templates.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");
      f << "microstate";
      for (const auto& ch : rec.channel_names) f << "," << ch;
      f << "\n";
      for (int k = 0; k < K; ++k) {
        f << state_name(k);
        for (double v : r.templates[static_cast<size_t>(k)]) {
          f << "," << v;
        }
        f << "\n";
      }
    }

    // --- Write time series ---
    {
      std::ofstream f(a.outdir + "/microstate_timeseries.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");
      f << "time_sec,label,gfp,corr\n";
      const double inv_fs = 1.0 / rec.fs_hz;
      for (size_t t = 0; t < rec.n_samples(); ++t) {
        double time = (a.start_sec > 0.0 ? a.start_sec : 0.0) + static_cast<double>(t) * inv_fs;
        int lab = (t < r.labels.size()) ? r.labels[t] : -1;
        f << time << ",";
        if (lab >= 0) {
          f << state_name(lab);
        } else {
          f << "";
        }
        f << "," << r.gfp[t] << "," << r.corr[t] << "\n";
      }
    }

    // --- Transition matrix ---
    {
      std::ofstream f(a.outdir + "/microstate_transition_counts.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");

      f << "from\\to";
      for (int k = 0; k < K; ++k) f << "," << state_name(k);
      f << "\n";
      for (int i = 0; i < K; ++i) {
        f << state_name(i);
        for (int j = 0; j < K; ++j) {
          int c = r.transition_counts[static_cast<size_t>(i)][static_cast<size_t>(j)];
          f << "," << c;
        }
        f << "\n";
      }
    }

    // --- Summary ---
    {
      std::ofstream f(a.outdir + "/microstate_summary.txt");
      if (!f) throw std::runtime_error("Failed to open summary file");
      f << "qeeg_microstates_cli summary\n";
      f << "input: " << a.input_path << "\n";
      f << "fs_hz: " << rec.fs_hz << "\n";
      f << "channels: " << rec.n_channels() << "\n";
      f << "samples: " << rec.n_samples() << "\n";
      f << "start_sec: " << a.start_sec << "\n";
      f << "duration_sec: " << (a.duration_sec > 0.0 ? a.duration_sec : (static_cast<double>(rec.n_samples()) / rec.fs_hz)) << "\n\n";

      f << "k: " << K << "\n";
      f << "peak_fraction: " << msopt.peak_pick_fraction << "\n";
      f << "max_peaks: " << msopt.max_peaks << "\n";
      f << "min_peak_distance_samples: " << msopt.min_peak_distance_samples << "\n";
      f << "min_segment_samples: " << msopt.min_segment_samples << "\n";
      f << "polarity_invariant: " << (msopt.polarity_invariant ? 1 : 0) << "\n";
      f << "demean_topography: " << (msopt.demean_topography ? 1 : 0) << "\n";
      f << "GEV: " << r.gev << "\n\n";

      f << "Per-state stats:\n";
      f << "state,coverage,mean_duration_sec,occurrence_per_sec\n";
      for (int k = 0; k < K; ++k) {
        f << state_name(k) << "," << r.coverage[static_cast<size_t>(k)]
          << "," << r.mean_duration_sec[static_cast<size_t>(k)]
          << "," << r.occurrence_per_sec[static_cast<size_t>(k)] << "\n";
      }
    }

    // --- Render template topomaps ---
    // Use a symmetric scale shared across all maps for comparability.
    double max_abs = 0.0;
    for (const auto& tpl : r.templates) {
      for (double v : tpl) max_abs = std::max(max_abs, std::fabs(v));
    }
    if (!(max_abs > 0.0)) max_abs = 1.0;
    const double vmin = -max_abs;
    const double vmax = +max_abs;

    TopomapOptions topt;
    topt.grid_size = a.grid;
    if (a.interp == "spline") {
      topt.method = TopomapInterpolation::SPHERICAL_SPLINE;
      topt.spline.n_terms = a.spline_terms;
      topt.spline.m = a.spline_m;
      topt.spline.lambda = a.spline_lambda;
    } else {
      topt.method = TopomapInterpolation::IDW;
      topt.idw_power = a.idw_power;
    }

    std::vector<Vec2> electrodes;
    electrodes.reserve(rec.channel_names.size());
    for (const auto& ch : rec.channel_names) {
      Vec2 p;
      if (montage.get(ch, &p)) electrodes.push_back(p);
    }

    for (int k = 0; k < K; ++k) {
      std::vector<double> values = r.templates[static_cast<size_t>(k)];
      Grid2D grid = make_topomap(montage, rec.channel_names, values, topt);
      std::string path = a.outdir + "/topomap_microstate_" + state_name(k) + ".bmp";
      if (a.annotate) {
        render_grid_to_bmp_annotated(path, grid.size, grid.values, vmin, vmax, electrodes);
      } else {
        render_grid_to_bmp(path, grid.size, grid.values, vmin, vmax);
      }
    }

    std::cout << "Wrote microstate outputs to: " << a.outdir << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
