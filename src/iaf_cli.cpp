#include "qeeg/bmp_writer.hpp"
#include "qeeg/iaf.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

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
  std::string outdir{"out_iaf"};
  std::string montage_spec{"builtin:standard_1020_19"};
  std::string channels_list;   // comma-separated; empty => all channels
  bool occipital{false};       // use a small occipital/parietal set for aggregate IAF
  std::string aggregate{"median"}; // median|mean|none

  // CSV inputs
  double fs_csv{0.0};

  // Preprocess (offline)
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};

  // Welch PSD params
  size_t nperseg{1024};
  double overlap{0.5};

  // IAF estimator params
  double alpha_min_hz{7.0};
  double alpha_max_hz{13.0};
  bool detrend_1_f{true};
  double detrend_min_hz{2.0};
  double detrend_max_hz{40.0};
  double smooth_hz{1.0};
  double min_prom_db{0.5};
  bool require_local_max{true};

  // Output
  bool topomap{true};
  bool annotate{true};
  int grid{256};
  std::string interp{"idw"};
  double idw_power{2.0};
  int spline_terms{50};
  int spline_m{4};
  double spline_lambda{1e-5};

  bool write_bandspec{true};
};

static void print_help() {
  std::cout
    << "qeeg_iaf_cli (Individual Alpha Frequency / alpha peak estimation)\n\n"
    << "Usage:\n"
    << "  qeeg_iaf_cli --input file.edf --outdir out_iaf\n"
    << "  qeeg_iaf_cli --input file.csv --fs 250 --outdir out_iaf\n\n"
    << "Options:\n"
    << "  --input PATH              Input EDF/BDF/CSV\n"
    << "  --fs HZ                   Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR              Output directory (default: out_iaf)\n"
    << "  --channels LIST           Comma-separated channel list used for aggregate IAF (default: all)\n"
    << "  --occipital               Use a default occipital/parietal set for aggregate (O1,O2,Oz,Pz,P3,P4)\n"
    << "  --aggregate MODE          Aggregate mode: median|mean|none (default: median)\n"
    << "  --alpha MIN MAX           Alpha peak search band in Hz (default: 7 13)\n"
    << "  --no-detrend              Disable 1/f detrending (enabled by default)\n"
    << "  --detrend-range MIN MAX   Detrend fit range in Hz (default: 2 40)\n"
    << "  --smooth-hz HZ            Frequency smoothing width (Hz; default: 1.0; 0 disables)\n"
    << "  --min-prom-db DB          Minimum peak prominence in dB (default: 0.5; <=0 disables)\n"
    << "  --no-local-max            Do not require local maximum vs neighbors\n"
    << "  --nperseg N               Welch segment length (default: 1024)\n"
    << "  --overlap FRAC            Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --average-reference        Apply common average reference across channels\n"
    << "  --notch HZ                 Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q                Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI           Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase              Offline: forward-backward filtering (less phase distortion)\n"
    << "  --no-topomap              Do not render topomap_iaf.bmp\n"
    << "  --no-annotate             Render plain BMP (no head outline/electrodes/colorbar)\n"
    << "  --montage SPEC            builtin:standard_1020_19 (default) or path to montage CSV\n"
    << "  --grid N                  Topomap grid size (default: 256)\n"
    << "  --interp METHOD           idw|spline (default: idw)\n"
    << "  --idw-power P             IDW power parameter (default: 2.0)\n"
    << "  --spline-terms N          Spherical spline Legendre terms (default: 50)\n"
    << "  --spline-m N              Spherical spline order m (default: 4)\n"
    << "  --spline-lambda X         Spline regularization (default: 1e-5)\n"
    << "  --no-bandspec             Do not write a recommended IAF-relative band spec file\n"
    << "  -h, --help                Show this help\n";
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
    } else if (arg == "--channels" && i + 1 < argc) {
      a.channels_list = argv[++i];
    } else if (arg == "--occipital") {
      a.occipital = true;
    } else if (arg == "--aggregate" && i + 1 < argc) {
      a.aggregate = to_lower(argv[++i]);
    } else if (arg == "--alpha" && i + 2 < argc) {
      a.alpha_min_hz = to_double(argv[++i]);
      a.alpha_max_hz = to_double(argv[++i]);
    } else if (arg == "--no-detrend") {
      a.detrend_1_f = false;
    } else if (arg == "--detrend-range" && i + 2 < argc) {
      a.detrend_min_hz = to_double(argv[++i]);
      a.detrend_max_hz = to_double(argv[++i]);
    } else if (arg == "--smooth-hz" && i + 1 < argc) {
      a.smooth_hz = to_double(argv[++i]);
    } else if (arg == "--min-prom-db" && i + 1 < argc) {
      a.min_prom_db = to_double(argv[++i]);
    } else if (arg == "--no-local-max") {
      a.require_local_max = false;
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
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
    } else if (arg == "--no-topomap") {
      a.topomap = false;
    } else if (arg == "--no-annotate") {
      a.annotate = false;
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
    } else if (arg == "--no-bandspec") {
      a.write_bandspec = false;
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

static int find_channel_index(const std::vector<std::string>& names, const std::string& want) {
  if (names.empty()) return -1;
  const std::string lw = normalize_channel_name(want);
  for (size_t i = 0; i < names.size(); ++i) {
    if (normalize_channel_name(names[i]) == lw) return static_cast<int>(i);
  }
  return -1;
}

static double median_inplace(std::vector<double>* v) {
  if (!v || v->empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = v->size();
  const size_t mid = n / 2;
  std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid), v->end());
  double m = (*v)[mid];
  if (n % 2 == 0) {
    std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid - 1), v->end());
    m = 0.5 * (m + (*v)[mid - 1]);
  }
  return m;
}

static double mean(const std::vector<double>& v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  double s = 0.0;
  size_t n = 0;
  for (double x : v) {
    if (!std::isfinite(x)) continue;
    s += x;
    ++n;
  }
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  return s / static_cast<double>(n);
}

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");
    if (rec.n_channels() == 0 || rec.n_samples() < 8) throw std::runtime_error("Recording too small");

    // Offline preprocessing
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

    IafOptions iopt;
    iopt.alpha_min_hz = args.alpha_min_hz;
    iopt.alpha_max_hz = args.alpha_max_hz;
    iopt.detrend_1_f = args.detrend_1_f;
    iopt.detrend_min_hz = args.detrend_min_hz;
    iopt.detrend_max_hz = args.detrend_max_hz;
    iopt.smooth_hz = args.smooth_hz;
    iopt.min_prominence_db = args.min_prom_db;
    iopt.require_local_max = args.require_local_max;

    std::vector<IafEstimate> per_ch(rec.n_channels());
    for (size_t c = 0; c < rec.n_channels(); ++c) {
      PsdResult psd = welch_psd(rec.data[c], rec.fs_hz, wopt);
      per_ch[c] = estimate_iaf(psd, iopt);
    }

    // Determine channel set for aggregate IAF.
    std::vector<int> agg_ch;
    if (args.occipital) {
      const std::vector<std::string> wanted = {"O1","O2","Oz","Pz","P3","P4"};
      for (const auto& w : wanted) {
        int idx = find_channel_index(rec.channel_names, w);
        if (idx >= 0) agg_ch.push_back(idx);
      }
    } else if (!args.channels_list.empty()) {
      for (const auto& tok : split(args.channels_list, ',')) {
        std::string name = trim(tok);
        if (name.empty()) continue;
        int idx = find_channel_index(rec.channel_names, name);
        if (idx >= 0) {
          agg_ch.push_back(idx);
        } else {
          std::cerr << "Warning: channel not found: " << name << "\n";
        }
      }
    }
    if (agg_ch.empty()) {
      // Default: all channels.
      agg_ch.reserve(static_cast<size_t>(rec.n_channels()));
      for (size_t c = 0; c < rec.n_channels(); ++c) agg_ch.push_back(static_cast<int>(c));
    }

    std::vector<double> iaf_vals;
    for (int idx : agg_ch) {
      if (idx < 0 || idx >= static_cast<int>(rec.n_channels())) continue;
      if (per_ch[static_cast<size_t>(idx)].found) {
        iaf_vals.push_back(per_ch[static_cast<size_t>(idx)].iaf_hz);
      }
    }

    double iaf_agg = std::numeric_limits<double>::quiet_NaN();
    if (!iaf_vals.empty()) {
      if (args.aggregate == "mean") {
        iaf_agg = mean(iaf_vals);
      } else if (args.aggregate == "median") {
        iaf_agg = median_inplace(&iaf_vals);
      } else if (args.aggregate == "none") {
        // keep NaN
      } else {
        std::cerr << "Warning: unknown --aggregate mode: " << args.aggregate << " (using median)\n";
        iaf_agg = median_inplace(&iaf_vals);
      }
    }

    // Write per-channel CSV.
    {
      std::ofstream f(args.outdir + "/iaf_by_channel.csv");
      if (!f) throw std::runtime_error("Failed to write iaf_by_channel.csv");
      f << "channel,iaf_hz,found,peak_value_db,prominence_db\n";
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        const auto& e = per_ch[c];
        f << rec.channel_names[c] << "," << e.iaf_hz << "," << (e.found ? 1 : 0)
          << "," << e.peak_value_db << "," << e.prominence_db << "\n";
      }
    }

    // Write meta/summary.
    {
      std::ofstream f(args.outdir + "/iaf_summary.txt");
      if (!f) throw std::runtime_error("Failed to write iaf_summary.txt");
      f << "input=" << args.input_path << "\n";
      f << "fs_hz=" << rec.fs_hz << "\n";
      f << "n_channels=" << rec.n_channels() << "\n";
      f << "n_samples=" << rec.n_samples() << "\n";
      f << "\n";
      f << "welch_nperseg=" << args.nperseg << "\n";
      f << "welch_overlap=" << args.overlap << "\n";
      f << "\n";
      f << "alpha_min_hz=" << args.alpha_min_hz << "\n";
      f << "alpha_max_hz=" << args.alpha_max_hz << "\n";
      f << "detrend_1_f=" << (args.detrend_1_f ? 1 : 0) << "\n";
      f << "detrend_min_hz=" << args.detrend_min_hz << "\n";
      f << "detrend_max_hz=" << args.detrend_max_hz << "\n";
      f << "smooth_hz=" << args.smooth_hz << "\n";
      f << "min_prom_db=" << args.min_prom_db << "\n";
      f << "require_local_max=" << (args.require_local_max ? 1 : 0) << "\n";
      f << "\n";
      f << "aggregate_mode=" << args.aggregate << "\n";
      f << "aggregate_iaf_hz=" << iaf_agg << "\n";
    }

    if (args.write_bandspec && std::isfinite(iaf_agg)) {
      std::vector<BandDefinition> bands = individualized_bands_from_iaf(iaf_agg);
      const std::string spec = bands_to_spec_string(bands);
      std::ofstream f(args.outdir + "/iaf_band_spec.txt");
      if (!f) throw std::runtime_error("Failed to write iaf_band_spec.txt");
      f << spec << "\n";
      std::cout << "Recommended IAF-relative band spec:\n  " << spec << "\n";
    }

    // Optional topomap of IAF per channel.
    if (args.topomap) {
      Montage montage = load_montage(args.montage_spec);

      std::vector<double> vals(rec.n_channels(), std::numeric_limits<double>::quiet_NaN());
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        if (per_ch[c].found) vals[c] = per_ch[c].iaf_hz;
      }

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

      Grid2D grid = make_topomap(montage, rec.channel_names, vals, topt);
      const std::string outpath = args.outdir + "/topomap_iaf.bmp";

      // Use alpha search range as display bounds by default.
      const double vmin = args.alpha_min_hz;
      const double vmax = args.alpha_max_hz;

      if (args.annotate) {
        std::vector<Vec2> electrode_positions_unit;
        electrode_positions_unit.reserve(rec.n_channels());
        for (const auto& ch_name : rec.channel_names) {
          Vec2 p;
          if (montage.get(ch_name, &p)) electrode_positions_unit.push_back(p);
        }

        AnnotatedTopomapOptions aopt;
        aopt.colorbar.enabled = true;
        render_grid_to_bmp_annotated(outpath, grid.size, grid.values, vmin, vmax, electrode_positions_unit, aopt);
      } else {
        render_grid_to_bmp(outpath, grid.size, grid.values, vmin, vmax);
      }
    }

    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    if (std::isfinite(iaf_agg)) {
      std::cout << "Aggregate IAF (" << args.aggregate << ") = " << iaf_agg << " Hz\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
