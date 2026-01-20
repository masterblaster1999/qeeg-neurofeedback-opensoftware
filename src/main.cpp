#include "qeeg/bandpower.hpp"
#include "qeeg/channel_qc_io.hpp"
#include "qeeg/bmp_writer.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"
#include "qeeg/svg_utils.hpp"

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out"};
  std::string montage_spec{"builtin:standard_1020_19"};
  std::string band_spec; // empty => default
  std::string reference_path;

  std::string channel_qc; // optional: qeeg_channel_qc_cli output to mask bad channels

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

  // Optional: write a lightweight HTML report that links to CSVs + BMPs.
  bool html_report{false};

  // Apply log10 transform to bandpower values before writing CSV and/or computing z-scores.
  // Useful for compatibility with reference files built via qeeg_reference_cli --log10.
  bool log10_power{false};
  bool log10_specified{false}; // internal: whether user passed --log10

  // If enabled, compute relative bandpower values (band_power / total_power) before
  // optionally applying log10 and/or z-scoring.
  bool relative_power{false};
  bool relative_specified{false};       // internal: whether user passed --relative/--relative-range
  bool relative_range_specified{false}; // internal: whether user passed --relative-range
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};

  // Output visualization
  bool annotate{false}; // draw head outline/electrodes + colorbar on topomaps

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
    << "  qeeg_map_cli --input file_with_time.csv --outdir out\n"
    << "  qeeg_map_cli --demo --fs 250 --seconds 10 --outdir out_demo\n\n"
    << "Options:\n"
    << "  --input SPEC            Input recording (EDF/BDF/BrainVision .vhdr or CSV/ASCII)\n"
    << "                         Also accepts a directory or *_run_meta.json for CLI chaining\n"
    << "  --fs HZ                 Sampling rate for CSV (optional if first column is time); required for --demo\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --montage SPEC          'builtin:standard_1020_19' (default), 'builtin:standard_1010_61', or PATH to montage CSV\n"
    << "  --bands SPEC            Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
    << "                         IAF-relative convenience forms:\n"
    << "                           --bands iaf=10.2\n"
    << "                           --bands iaf:out_iaf   (reads out_iaf/iaf_band_spec.txt or out_iaf/iaf_summary.txt)\n"
    << "  --reference PATH        Reference CSV (channel,band,mean,std) to compute z-maps\n"
    << "  --channel-qc PATH       Channel QC (channel_qc.csv, bad_channels.txt, or qc outdir) to mask bad channels\n"
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
    << "  --log10                 Use log10(power) instead of raw bandpower (matches qeeg_reference_cli --log10)\n"
    << "  --relative              Use relative power: band_power / total_power\n"
    << "  --relative-range LO HI  Total-power integration range used for --relative.\n"
    << "                         Default: [min_band_fmin, max_band_fmax] from --bands.\n"
    << "  --annotate              Annotate topomaps with head outline/electrodes + colorbar\n"
    << "  --html-report           Write report.html linking to bandpowers.csv and topomaps (BMP)\n"
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
    } else if (arg == "--channel-qc" && i + 1 < argc) {
      a.channel_qc = argv[++i];
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
    } else if (arg == "--log10") {
      a.log10_power = true;
      a.log10_specified = true;
    } else if (arg == "--relative") {
      a.relative_power = true;
      a.relative_specified = true;
    } else if (arg == "--relative-range" && i + 2 < argc) {
      a.relative_power = true;
      a.relative_specified = true;
      a.relative_range_specified = true;
      a.relative_fmin_hz = to_double(argv[++i]);
      a.relative_fmax_hz = to_double(argv[++i]);
    } else if (arg == "--annotate") {
      a.annotate = true;
    } else if (arg == "--html-report") {
      a.html_report = true;
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

  // Convenience aliases
  if (low == "builtin" || low == "default") {
    return Montage::builtin_standard_1020_19();
  }

  // Support: builtin:<key>
  std::string key = low;
  if (starts_with(key, "builtin:")) {
    key = key.substr(std::string("builtin:").size());
  }

  if (key == "standard_1020_19" || key == "1020_19" || key == "standard_1020" || key == "1020") {
    return Montage::builtin_standard_1020_19();
  }
  if (key == "standard_1010_61" || key == "1010_61" || key == "standard_1010" || key == "1010" ||
      key == "standard_10_10" || key == "10_10" || key == "10-10") {
    return Montage::builtin_standard_1010_61();
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


static std::string yesno(bool v) { return v ? "yes" : "no"; }

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

  // This mirrors the BIDS convention used by *_events.json: top-level keys match
  // column names, with LongName/Description/Units describing each column.
  // (Even though bandpowers.csv is a derivative artifact, this format is widely
  // usable by downstream tooling.)
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
    std::string desc = "Bandpower integrated from " + fmt_double(b.fmin_hz, 4) + " to " + fmt_double(b.fmax_hz, 4) + " Hz.";
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

static void write_map_run_meta_json(const Args& args,
                                    const EEGRecording& rec,
                                    const std::vector<BandDefinition>& bands,
                                    bool have_ref,
                                    bool have_qc,
                                    const std::vector<bool>* qc_bad,
                                    const std::vector<std::string>* qc_reasons,
                                    const std::string& qc_resolved_path,
                                    bool rel_range_used,
                                    double rel_lo_hz,
                                    double rel_hi_hz) {
  const std::string outpath = args.outdir + "/map_run_meta.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write map_run_meta.json: " + outpath);
  out << std::setprecision(12);

  auto write_string_or_null = [&](const std::string& s) {
    if (s.empty()) out << "null";
    else out << "\"" << json_escape(s) << "\"";
  };

  auto write_outputs_array = [&]() {
    out << "  \"Outputs\": [\n";
    bool first = true;
    auto emit = [&](const std::string& rel) {
      if (!first) out << ",\n";
      first = false;
      out << "    \"" << json_escape(rel) << "\"";
    };

    emit("bandpowers.csv");
    emit("bandpowers.json");
    emit("map_run_meta.json");
    if (args.export_psd) emit("psd.csv");
    if (args.html_report) emit("report.html");
    if (have_qc) emit("bad_channels_used.txt");

    for (const auto& b : bands) {
      emit("topomap_" + b.name + ".bmp");
      if (have_ref) emit("topomap_" + b.name + "_z.bmp");
    }

    out << "\n  ]";
  };

  out << "{\n";
  out << "  \"Tool\": \"qeeg_map_cli\",\n";
  out << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";

  out << "  \"Input\": {\n";
  out << "    \"Demo\": " << (args.demo ? "true" : "false") << ",\n";
  out << "    \"Path\": ";
  write_string_or_null(args.input_path);
  out << "\n  },\n";

  out << "  \"OutputDir\": \"" << json_escape(args.outdir) << "\",\n";
  out << "  \"SamplingFrequencyHz\": " << rec.fs_hz << ",\n";
  out << "  \"ChannelCount\": " << rec.n_channels() << ",\n";

  out << "  \"Channels\": [";
  for (size_t i = 0; i < rec.channel_names.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << json_escape(rec.channel_names[i]) << "\"";
  }
  out << "],\n";

  out << "  \"MontageSpec\": \"" << json_escape(args.montage_spec) << "\",\n";
  out << "  \"BandSpec\": ";
  write_string_or_null(args.band_spec);
  out << ",\n";

  out << "  \"Bands\": [\n";
  for (size_t i = 0; i < bands.size(); ++i) {
    const auto& b = bands[i];
    out << "    { \"Name\": \"" << json_escape(b.name) << "\", \"FminHz\": " << b.fmin_hz
        << ", \"FmaxHz\": " << b.fmax_hz << " }";
    if (i + 1 < bands.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"Welch\": { \"Nperseg\": " << args.nperseg << ", \"Overlap\": " << args.overlap << " },\n";

  out << "  \"Transforms\": {\n";
  out << "    \"RelativePower\": " << (args.relative_power ? "true" : "false") << ",\n";
  if (args.relative_power && rel_range_used) {
    out << "    \"RelativePowerRangeHz\": [" << rel_lo_hz << ", " << rel_hi_hz << "],\n";
  }
  out << "    \"Log10Power\": " << (args.log10_power ? "true" : "false") << "\n";
  out << "  },\n";

  out << "  \"Preprocess\": {\n";
  out << "    \"AverageReference\": " << (args.average_reference ? "true" : "false") << ",\n";
  out << "    \"NotchHz\": " << args.notch_hz << ",\n";
  out << "    \"NotchQ\": " << args.notch_q << ",\n";
  out << "    \"BandpassLowHz\": " << args.bandpass_low_hz << ",\n";
  out << "    \"BandpassHighHz\": " << args.bandpass_high_hz << ",\n";
  out << "    \"ZeroPhase\": " << (args.zero_phase ? "true" : "false") << "\n";
  out << "  },\n";

  out << "  \"Topomap\": {\n";
  out << "    \"Grid\": " << args.grid << ",\n";
  out << "    \"Interpolation\": \"" << json_escape(args.interp) << "\",\n";
  out << "    \"IdwPower\": " << args.idw_power << ",\n";
  out << "    \"SplineTerms\": " << args.spline_terms << ",\n";
  out << "    \"SplineM\": " << args.spline_m << ",\n";
  out << "    \"SplineLambda\": " << args.spline_lambda << ",\n";
  out << "    \"Annotate\": " << (args.annotate ? "true" : "false") << "\n";
  out << "  },\n";

  out << "  \"Reference\": {\n";
  out << "    \"Provided\": " << (have_ref ? "true" : "false") << ",\n";
  out << "    \"Path\": ";
  write_string_or_null(args.reference_path);
  out << "\n  },\n";

  out << "  \"ChannelQC\": {\n";
  out << "    \"Provided\": " << (have_qc ? "true" : "false") << ",\n";
  out << "    \"Path\": ";
  if (have_qc) {
    const std::string src = qc_resolved_path.empty() ? args.channel_qc : qc_resolved_path;
    write_string_or_null(src);
  } else {
    out << "null";
  }
  out << ",\n";

  if (have_qc && qc_bad && qc_reasons) {
    size_t bad_count = 0;
    for (bool b : *qc_bad) if (b) ++bad_count;
    out << "    \"BadChannelCount\": " << bad_count << ",\n";
    out << "    \"BadChannels\": [\n";
    bool first_bad = true;
    for (size_t c = 0; c < qc_bad->size() && c < rec.channel_names.size(); ++c) {
      if (!(*qc_bad)[c]) continue;
      if (!first_bad) out << ",\n";
      first_bad = false;
      out << "      { \"Channel\": \"" << json_escape(rec.channel_names[c]) << "\"";
      const std::string reasons = (c < qc_reasons->size()) ? (*qc_reasons)[c] : std::string();
      if (!reasons.empty()) {
        out << ", \"Reasons\": \"" << json_escape(reasons) << "\"";
      }
      out << " }";
    }
    out << "\n    ]\n";
  } else {
    out << "    \"BadChannelCount\": 0,\n";
    out << "    \"BadChannels\": []\n";
  }
  out << "  },\n";

  write_outputs_array();
  out << "\n}\n";
}

static void write_map_report_html(const Args& args,
                                  const EEGRecording& rec,
                                  const std::vector<BandDefinition>& bands,
                                  const std::vector<std::vector<double>>& bandpower_matrix,
                                  const std::vector<std::vector<double>>* z_matrix,
                                  const std::vector<bool>* qc_bad,
                                  const std::vector<std::string>* qc_reasons,
                                  const std::string& qc_resolved_path,
                                  bool rel_range_used,
                                  double rel_lo_hz,
                                  double rel_hi_hz) {
  if (!args.html_report) return;

  const std::string outpath = args.outdir + "/report.html";
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write report.html: " + outpath);

  const std::string input_label = args.demo
    ? std::string("demo (synthetic)")
    : (args.input_path.empty() ? std::string("(none)") : std::filesystem::u8path(args.input_path).filename().u8string());

  const bool have_ref = (z_matrix != nullptr && !z_matrix->empty());

  const bool have_qc = (qc_bad != nullptr && !qc_bad->empty());
  size_t qc_bad_count = 0;
  if (have_qc) {
    for (bool b : *qc_bad) if (b) ++qc_bad_count;
  }

  std::string qc_label = "n/a";
  if (have_qc) {
    const std::string src = qc_resolved_path.empty() ? args.channel_qc : qc_resolved_path;
    try {
      qc_label = std::filesystem::u8path(src).filename().u8string();
    } catch (...) {
      qc_label = src;
    }
  }


  out << "<!doctype html>\n"
      << "<html lang=\"en\">\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>qEEG Map Report</title>\n"
      << "  <style>\n"
      << "    :root { --bg:#0b1020; --panel:#111a33; --panel2:#0f172a; --text:#e5e7eb; --muted:#94a3b8; --accent:#38bdf8; --border:rgba(255,255,255,0.10); }\n"
      << "    html,body { margin:0; height:100%; background:var(--bg); color:var(--text); font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial; }\n"
      << "    a { color: var(--accent); text-decoration: none; }\n"
      << "    a:hover { text-decoration: underline; }\n"
      << "    .wrap { max-width: 1180px; margin: 0 auto; padding: 18px; }\n"
      << "    .top { display:flex; align-items:baseline; justify-content:space-between; gap:10px; }\n"
      << "    h1 { margin:0 0 6px 0; font-size: 22px; }\n"
      << "    .sub { color: var(--muted); font-size: 13px; }\n"
      << "    .grid { display:grid; grid-template-columns: 1fr 1fr; gap: 12px; }\n"
      << "    .card { background: rgba(17,26,51,0.6); border:1px solid var(--border); border-radius: 12px; padding: 12px; }\n"
      << "    .kv { display:grid; grid-template-columns: 220px 1fr; gap: 6px 10px; font-size: 13px; }\n"
      << "    .kv .k { color: var(--muted); }\n"
      << "    .links { display:flex; flex-wrap: wrap; gap: 10px; }\n"
      << "    table { width:100%; border-collapse: collapse; font-size: 12px; }\n"
      << "    th, td { border-bottom: 1px solid var(--border); padding: 6px 6px; text-align: right; }\n"
      << "    th:first-child, td:first-child { text-align: left; }\n"
      << "    thead th { position: sticky; top: 0; background: rgba(15,23,42,0.95); }\n"
      << "    tr.bad td { background: rgba(248,113,113,0.12); }\n"
      << "    td.status { text-align: left; color: var(--muted); }\n"
      << "    .small { font-size: 12px; color: var(--muted); }\n"
      << "    .bands { display:grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }\n"
      << "    .band h3 { margin: 0 0 8px 0; font-size: 14px; }\n"
      << "    img { width: 100%; height: auto; border-radius: 10px; border: 1px solid var(--border); background: white; }\n"
      << "    .tag { display:inline-block; padding: 2px 8px; border:1px solid var(--border); border-radius: 999px; font-size: 12px; color: var(--muted); }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div class=\"wrap\">\n"
      << "    <div class=\"top\">\n"
      << "      <div>\n"
      << "        <h1>qEEG Map Report</h1>\n"
      << "        <div class=\"sub\">Generated by <code>qeeg_map_cli</code>. Files are linked relative to this report.</div>\n"
      << "      </div>\n"
      << "      <div class=\"tag\">" << (have_ref ? "z-maps enabled" : "raw maps") << "</div>\n"
      << "    </div>\n"
      << "    <div style=\"height:12px\"></div>\n"
      << "    <div class=\"grid\">\n"
      << "      <div class=\"card\">\n"
      << "        <div style=\"font-weight:700; margin-bottom:8px\">Summary</div>\n"
      << "        <div class=\"kv\">\n"
      << "          <div class=\"k\">Input</div><div>" << svg_escape(input_label) << "</div>\n"
      << "          <div class=\"k\">Sampling rate</div><div>" << fmt_double(rec.fs_hz, 3) << " Hz</div>\n"
      << "          <div class=\"k\">Channels</div><div>" << rec.n_channels() << "</div>\n"
      << "          <div class=\"k\">Samples</div><div>" << rec.n_samples() << "</div>\n"
      << "          <div class=\"k\">Montage</div><div>" << svg_escape(args.montage_spec) << "</div>\n"
      << "          <div class=\"k\">Bands</div><div>" << bands.size() << "</div>\n"
      << "          <div class=\"k\">Interpolation</div><div>" << svg_escape(args.interp) << " (grid " << args.grid << ")</div>\n"
      << "          <div class=\"k\">Relative power</div><div>" << yesno(args.relative_power) << (rel_range_used ? (" (range " + fmt_double(rel_lo_hz, 2) + "–" + fmt_double(rel_hi_hz, 2) + " Hz)") : "") << "</div>\n"
      << "          <div class=\"k\">log10(power)</div><div>" << yesno(args.log10_power) << "</div>\n"
      << "          <div class=\"k\">Channel QC</div><div>" << yesno(have_qc) << (have_qc ? (" (" + svg_escape(qc_label) + ")") : "") << "</div>\n"
      << "          <div class=\"k\">Bad channels</div><div>" << (have_qc ? std::to_string(qc_bad_count) : std::string("n/a")) << "</div>\n"
      << "          <div class=\"k\">Annotate BMPs</div><div>" << yesno(args.annotate) << "</div>\n"
      << "        </div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div style=\"font-weight:700; margin-bottom:8px\">Outputs</div>\n"
      << "        <div class=\"links\">\n"
      << "          <a href=\"" << url_escape("bandpowers.csv") << "\">bandpowers.csv</a>\n";
  out << "          <a href=\"" << url_escape("bandpowers.json") << "\">bandpowers.json</a>\n";
  out << "          <a href=\"" << url_escape("map_run_meta.json") << "\">map_run_meta.json</a>\n";
  if (have_qc) {
    out << "          <a href=\"" << url_escape("bad_channels_used.txt") << "\">bad_channels_used.txt</a>\n";
  }
  if (args.export_psd) {
    out << "          <a href=\"" << url_escape("psd.csv") << "\">psd.csv</a>\n";
  }
  out << "          <span class=\"small\">(Topomaps below are BMP files: <code>topomap_&lt;band&gt;.bmp</code>" 
      << (have_ref ? " and <code>topomap_&lt;band&gt;_z.bmp</code>." : ".)") << "</span>\n"
      << "        </div>\n"
      << "        <div style=\"height:8px\"></div>\n"
      << "        <div class=\"small\">Note: Most modern browsers can display BMP. If images do not render, convert BMP → PNG.</div>\n"
      << "      </div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Bandpowers</div>\n"
      << "      <div class=\"small\" style=\"margin-bottom:8px\">Values shown are after optional <code>--relative</code> and/or <code>--log10</code> transforms. Z-scores are computed using <code>--reference</code> when provided.</div>\n"
      << "      <div style=\"max-height:520px; overflow:auto; border:1px solid var(--border); border-radius:10px\">\n"
      << "      <table>\n"
      << "        <thead>\n"
      << "          <tr>\n"
      << "            <th>Channel</th>\n";
  if (have_qc) {
    out << "            <th>Status</th>\n";
  }
  for (const auto& b : bands) {
    out << "            <th>" << svg_escape(b.name) << "</th>\n";
  }
  if (have_ref) {
    for (const auto& b : bands) {
      out << "            <th>" << svg_escape(b.name) << " z</th>\n";
    }
  }
  out << "          </tr>\n"
      << "        </thead>\n"
      << "        <tbody>\n";
  for (size_t c = 0; c < rec.n_channels(); ++c) {
    const bool is_bad = have_qc && qc_bad && (*qc_bad)[c];

    out << "          <tr" << (is_bad ? " class=\"bad\"" : "") << ">\n"
        << "            <td>" << svg_escape(rec.channel_names[c]) << "</td>\n";

    if (have_qc) {
      out << "            <td class=\"status\">" << (is_bad ? "bad" : "good");
      if (is_bad && qc_reasons && c < qc_reasons->size() && !(*qc_reasons)[c].empty()) {
        out << " (" << svg_escape((*qc_reasons)[c]) << ")";
      }
      out << "</td>\n";
    }

    for (size_t bi = 0; bi < bands.size(); ++bi) {
      out << "            <td>" << fmt_double(bandpower_matrix[bi][c]) << "</td>\n";
    }
    if (have_ref) {
      for (size_t bi = 0; bi < bands.size(); ++bi) {
        out << "            <td>" << fmt_double((*z_matrix)[bi][c]) << "</td>\n";
      }
    }
    out << "          </tr>\n";
  }
  out << "        </tbody>\n"
      << "      </table>\n"
      << "      </div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Topomaps</div>\n"
      << "      <div class=\"bands\">\n";
  for (const auto& b : bands) {
    const std::string raw = "topomap_" + b.name + ".bmp";
    const std::string z = "topomap_" + b.name + "_z.bmp";
    out << "        <div class=\"band\">\n"
        << "          <h3>" << svg_escape(b.name) << "</h3>\n"
        << "          <div class=\"small\" style=\"margin-bottom:6px\">" << fmt_double(b.fmin_hz, 2) << "–" << fmt_double(b.fmax_hz, 2) << " Hz</div>\n"
        << "          <img src=\"" << url_escape(raw) << "\" alt=\"" << svg_escape(raw) << "\"/>\n";
    if (have_ref) {
      out << "          <div style=\"height:8px\"></div>\n"
          << "          <div class=\"small\" style=\"margin-bottom:6px\">Z-map (fixed range −3..+3)</div>\n"
          << "          <img src=\"" << url_escape(z) << "\" alt=\"" << svg_escape(z) << "\"/>\n";
    }
    out << "        </div>\n";
  }
  out << "      </div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"small\">Generated at " << svg_escape(now_string_local()) << ".</div>\n"
      << "  </div>\n"
      << "</body>\n"
      << "</html>\n";

  std::cout << "Wrote HTML report: " << outpath << "\n";
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

      const ResolvedInputPath in = resolve_input_recording_path(args.input_path);
      if (!in.note.empty()) {
        std::cerr << in.note << "\n";
      }
      args.input_path = in.path;

      rec = read_recording_auto(args.input_path, args.fs_csv);
    }

    if (rec.n_channels() < 3) throw std::runtime_error("Need at least 3 channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";

    // Optional: load channel-level QC labels and mask bad channels in outputs.
    bool have_qc = false;
    std::string qc_resolved_path;
    std::vector<bool> qc_bad(rec.n_channels(), false);
    std::vector<std::string> qc_reasons(rec.n_channels());

    if (!args.channel_qc.empty()) {
      std::cout << "Loading channel QC: " << args.channel_qc << "\n";
      ChannelQcMap qc = load_channel_qc_any(args.channel_qc, &qc_resolved_path);
      have_qc = true;

      size_t nbad = 0;
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        const std::string key = normalize_channel_name(rec.channel_names[c]);
        const auto it = qc.find(key);
        if (it != qc.end() && it->second.bad) {
          qc_bad[c] = true;
          qc_reasons[c] = it->second.reasons;
          ++nbad;
        }
      }

      std::cout << "Channel QC loaded from: " << qc_resolved_path
                << " (" << nbad << "/" << rec.n_channels() << " channels marked bad)\n";

      // Persist the applied mask for provenance (useful when sharing maps/CSVs).
      const std::string bad_out = args.outdir + "/bad_channels_used.txt";
      std::ofstream bout(bad_out);
      if (!bout) {
        std::cerr << "Warning: failed to write bad_channels_used.txt to: " << bad_out << "\n";
      } else {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          if (!qc_bad[c]) continue;
          bout << rec.channel_names[c];
          if (!qc_reasons[c].empty()) bout << "\t" << qc_reasons[c];
          bout << "\n";
        }
      }
    }


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
    std::vector<Vec2> electrode_positions_unit;
    if (args.annotate) {
      electrode_positions_unit.reserve(rec.n_channels());
      for (const auto& ch_name : rec.channel_names) {
        Vec2 p;
        if (montage.get(ch_name, &p)) electrode_positions_unit.push_back(p);
      }
    }

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

      // If the reference file contains metadata (written by qeeg_reference_cli),
      // use it to avoid accidental scale mismatches.
      if (ref.meta_log10_power_present) {
        if (!args.log10_specified) {
          args.log10_power = ref.meta_log10_power;
          if (args.log10_power) {
            std::cout << "Reference metadata: log10_power=1 (applying log10 transform to bandpower)\n";
          }
        } else if (args.log10_power != ref.meta_log10_power) {
          std::cerr << "Warning: --log10 does not match reference metadata log10_power="
                    << (ref.meta_log10_power ? 1 : 0)
                    << ". Z-scores may be invalid.\n";
        }
      }

      if (ref.meta_relative_power_present) {
        if (!args.relative_specified) {
          args.relative_power = ref.meta_relative_power;
          if (args.relative_power) {
            std::cout << "Reference metadata: relative_power=1 (computing relative bandpower)\n";
          }
        } else if (args.relative_power != ref.meta_relative_power) {
          std::cerr << "Warning: --relative does not match reference metadata relative_power="
                    << (ref.meta_relative_power ? 1 : 0)
                    << ". Z-scores may be invalid.\n";
        }
      }

      const bool ref_has_rel_range = ref.meta_relative_fmin_hz_present && ref.meta_relative_fmax_hz_present &&
                                     (ref.meta_relative_fmax_hz > ref.meta_relative_fmin_hz);
      if (args.relative_power && ref_has_rel_range) {
        if (!args.relative_range_specified) {
          args.relative_fmin_hz = ref.meta_relative_fmin_hz;
          args.relative_fmax_hz = ref.meta_relative_fmax_hz;
          std::cout << "Reference metadata: relative_range=[" << args.relative_fmin_hz
                    << "," << args.relative_fmax_hz << "] Hz\n";
        } else {
          const double eps = 1e-9;
          if (std::fabs(args.relative_fmin_hz - ref.meta_relative_fmin_hz) > eps ||
              std::fabs(args.relative_fmax_hz - ref.meta_relative_fmax_hz) > eps) {
            std::cerr << "Warning: --relative-range does not match reference metadata relative_range=["
                      << ref.meta_relative_fmin_hz << "," << ref.meta_relative_fmax_hz
                      << "] Hz. Z-scores may be invalid.\n";
          }
        }
      }

      if (ref.meta_robust_present) {
        std::cout << "Reference metadata: robust=" << (ref.meta_robust ? 1 : 0) << "\n";
      }
    }

    bool rel_range_used = false;
    double rel_range_lo_hz = 0.0;
    double rel_range_hi_hz = 0.0;

    // Optional: apply relative transform to bandpowers.
    // This must happen before optional log10 so that qeeg_reference_cli --relative --log10 matches.
    if (args.relative_power) {
      if (args.relative_range_specified && !(args.relative_fmax_hz > args.relative_fmin_hz)) {
        throw std::runtime_error("--relative-range must satisfy LO < HI");
      }

      double rel_lo = args.relative_fmin_hz;
      double rel_hi = args.relative_fmax_hz;
      if (!(rel_hi > rel_lo)) {
        // Default: use the span of the provided bands.
        rel_lo = bands[0].fmin_hz;
        rel_hi = bands[0].fmax_hz;
        for (const auto& b : bands) {
          rel_lo = std::min(rel_lo, b.fmin_hz);
          rel_hi = std::max(rel_hi, b.fmax_hz);
        }
      }
      if (!(rel_hi > rel_lo)) {
        throw std::runtime_error("Relative power range invalid (need LO < HI)");
      }

      rel_range_used = true;
      rel_range_lo_hz = rel_lo;
      rel_range_hi_hz = rel_hi;

      const double eps = 1e-20;
      std::vector<double> total_power(rec.n_channels(), 0.0);
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        total_power[c] = integrate_bandpower(psds[c], rel_lo, rel_hi);
      }
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          bandpower_matrix[b][c] = bandpower_matrix[b][c] / std::max(eps, total_power[c]);
        }
      }

      std::cout << "Relative power: dividing each band by total power in [" << rel_lo
                << "," << rel_hi << "] Hz\n";
    }

    // Optional: apply log10 transform to bandpowers.
    if (args.log10_power) {
      const double eps = 1e-20;
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          bandpower_matrix[b][c] = std::log10(std::max(eps, bandpower_matrix[b][c]));
        }
      }
    }

    // If channel QC is provided, mask bad channels as NaN so they are excluded from maps and CSVs.
    if (have_qc) {
      const double NaN = std::numeric_limits<double>::quiet_NaN();
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          if (qc_bad[c]) bandpower_matrix[b][c] = NaN;
        }
      }
    }

    std::vector<std::vector<double>> z_matrix;
    if (have_ref) {
      z_matrix.assign(bands.size(), std::vector<double>(rec.n_channels(), std::numeric_limits<double>::quiet_NaN()));
      for (size_t b = 0; b < bands.size(); ++b) {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          double z = 0.0;
          if (compute_zscore(ref, rec.channel_names[c], bands[b].name, bandpower_matrix[b][c], &z)) {
            z_matrix[b][c] = z;
          }
        }
      }
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
            out << "," << z_matrix[b][c];
          }
        }
        out << "\n";
      }
    }

    // Write a small JSON sidecar describing columns in bandpowers.csv.
    // This helps downstream tooling interpret the exported derivative table.
    write_bandpowers_sidecar_json(args, bands, have_ref,
                                 rel_range_used, rel_range_lo_hz, rel_range_hi_hz);

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
      if (args.annotate) {
        render_grid_to_bmp_annotated(outpath, grid.size, grid.values, vmin, vmax, electrode_positions_unit);
      } else {
        render_grid_to_bmp(outpath, grid.size, grid.values, vmin, vmax);
      }

      if (have_ref) {
        const auto& zvals = z_matrix[b];
        Grid2D zg = make_topomap(montage, rec.channel_names, zvals, topt);
        // common visualization range
        const std::string zout = args.outdir + "/topomap_" + bands[b].name + "_z.bmp";
        if (args.annotate) {
          // Common visualization range for z-maps
          AnnotatedTopomapOptions aopt;
          render_grid_to_bmp_annotated(zout, zg.size, zg.values, -3.0, 3.0, electrode_positions_unit, aopt);
        } else {
          render_grid_to_bmp(zout, zg.size, zg.values, -3.0, 3.0);
        }
      }
    }

    write_map_report_html(args, rec, bands, bandpower_matrix,
                          have_ref ? &z_matrix : nullptr,
                          have_qc ? &qc_bad : nullptr,
                          have_qc ? &qc_reasons : nullptr,
                          qc_resolved_path,
                          rel_range_used, rel_range_lo_hz, rel_range_hi_hz);

    // Persist a run-level JSON metadata file for provenance.
    write_map_run_meta_json(args, rec, bands,
                            have_ref,
                            have_qc,
                            have_qc ? &qc_bad : nullptr,
                            have_qc ? &qc_reasons : nullptr,
                            qc_resolved_path,
                            rel_range_used, rel_range_lo_hz, rel_range_hi_hz);

    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
