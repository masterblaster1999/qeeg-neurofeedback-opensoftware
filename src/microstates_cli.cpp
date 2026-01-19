#include "qeeg/bids.hpp"
#include "qeeg/bmp_writer.hpp"
#include "qeeg/channel_qc_io.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/microstates.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
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

struct Args {
  std::string input_path;
  std::string outdir{"out"};
  std::string montage_spec{"builtin:standard_1020_19"};

  // Optional channel QC input (qeeg_channel_qc_cli output folder/file)
  std::string channel_qc;

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

  bool export_segments{false};
  bool export_bids_events{false};

  // Optional: write report.html linking to microstate outputs and topomaps.
  bool html_report{false};

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
    << "  --fs HZ                 Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --montage SPEC          builtin:standard_1020_19 (default), builtin:standard_1010_61, or PATH to montage CSV\n"
    << "  --channel-qc PATH       Channel QC (channel_qc.csv, bad_channels.txt, or qc outdir) to exclude bad channels\n"
    << "  --start S               Start time in seconds (default: 0)\n"
    << "  --duration S            Duration in seconds (0 => full remainder)\n"
    << "  --k N                   Number of microstates (default: 4)\n"
    << "  --peak-fraction F        Fraction of GFP peaks used for clustering (default: 0.10)\n"
    << "  --max-peaks N            Cap number of peaks for clustering (default: 1000)\n"
    << "  --min-peak-distance-ms M Minimum spacing between selected GFP peaks (default: 0)\n"
    << "  --min-duration-ms M      Minimum microstate segment duration (merge shorter) (default: 0)\n"
    << "  --no-polarity-invariant  Treat maps as signed (disable polarity invariance)\n"
    << "  --export-segments         Write microstate_segments.csv (segment list)\n"
    << "  --export-bids-events      Write microstate_events.tsv and microstate_events.json (segment list as BIDS-style events)\n"
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
    << "  --html-report            Write report.html linking to CSVs and topomaps (BMP)\n"
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
    } else if (arg == "--channel-qc" && i + 1 < argc) {
      a.channel_qc = argv[++i];
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
    } else if (arg == "--export-segments") {
      a.export_segments = true;
    } else if (arg == "--export-bids-events") {
      a.export_bids_events = true;
    } else if (arg == "--html-report") {
      a.html_report = true;
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

static void write_bad_channels_used(const std::string& outdir,
                                    const EEGRecording& rec,
                                    const std::vector<bool>& bad,
                                    const std::vector<std::string>& reasons) {
  const std::string path = outdir + "/bad_channels_used.txt";
  std::ofstream f(path);
  if (!f) {
    std::cerr << "Warning: failed to write bad_channels_used.txt to: " << path << "\n";
    return;
  }
  for (size_t i = 0; i < rec.channel_names.size(); ++i) {
    if (!bad[i]) continue;
    f << rec.channel_names[i];
    if (i < reasons.size() && !reasons[i].empty()) f << "\t" << reasons[i];
    f << "\n";
  }
}

static void write_microstates_run_meta(const std::string& outdir,
                                       const Args& a,
                                       const std::string& qc_resolved_path,
                                       size_t qc_bad_count,
                                       size_t channels_used,
                                       const std::vector<std::string>& outputs) {
  const std::filesystem::path meta_path = std::filesystem::u8path(outdir) / "microstates_run_meta.json";
  std::ofstream meta(meta_path, std::ios::binary);
  if (!meta) {
    std::cerr << "Warning: failed to write microstates_run_meta.json to: " << meta_path.u8string() << "\n";
    return;
  }

  meta << std::setprecision(12);

  auto write_string_or_null = [&](const std::string& s) {
    if (s.empty()) meta << "null";
    else meta << "\"" << json_escape(s) << "\"";
  };

  meta << "{\n";
  meta << "  \"Tool\": \"qeeg_microstates_cli\",\n";
  meta << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";
  meta << "  \"Input\": {\n";
  meta << "    \"Path\": ";
  write_string_or_null(a.input_path);
  meta << ",\n";
  meta << "    \"FsCsvHz\": " << a.fs_csv << "\n";
  meta << "  },\n";
  meta << "  \"OutputDir\": \"" << json_escape(outdir) << "\",\n";

  meta << "  \"Options\": {\n";
  meta << "    \"Montage\": \"" << json_escape(a.montage_spec) << "\",\n";
  meta << "    \"StartSec\": " << a.start_sec << ",\n";
  meta << "    \"DurationSec\": " << a.duration_sec << ",\n";
  meta << "    \"K\": " << a.k << ",\n";
  meta << "    \"PeakFraction\": " << a.peak_fraction << ",\n";
  meta << "    \"MaxPeaks\": " << a.max_peaks << ",\n";
  meta << "    \"MinPeakDistanceMs\": " << a.min_peak_distance_ms << ",\n";
  meta << "    \"MinDurationMs\": " << a.min_duration_ms << ",\n";
  meta << "    \"PolarityInvariant\": " << (a.polarity_invariant ? "true" : "false") << ",\n";
  meta << "    \"DemeanTopography\": " << (a.demean_topography ? "true" : "false") << ",\n";
  meta << "    \"ExportSegments\": " << (a.export_segments ? "true" : "false") << ",\n";
  meta << "    \"ExportBidsEvents\": " << (a.export_bids_events ? "true" : "false") << ",\n";
  meta << "    \"HtmlReport\": " << (a.html_report ? "true" : "false") << ",\n";
  meta << "    \"Grid\": " << a.grid << ",\n";
  meta << "    \"Interp\": \"" << json_escape(a.interp) << "\",\n";
  meta << "    \"IdwPower\": " << a.idw_power << ",\n";
  meta << "    \"SplineTerms\": " << a.spline_terms << ",\n";
  meta << "    \"SplineM\": " << a.spline_m << ",\n";
  meta << "    \"SplineLambda\": " << a.spline_lambda << ",\n";
  meta << "    \"Annotate\": " << (a.annotate ? "true" : "false") << ",\n";
  meta << "    \"AverageReference\": " << (a.average_reference ? "true" : "false") << ",\n";
  meta << "    \"NotchHz\": " << a.notch_hz << ",\n";
  meta << "    \"NotchQ\": " << a.notch_q << ",\n";
  meta << "    \"BandpassLowHz\": " << a.bandpass_low_hz << ",\n";
  meta << "    \"BandpassHighHz\": " << a.bandpass_high_hz << ",\n";
  meta << "    \"ZeroPhase\": " << (a.zero_phase ? "true" : "false") << "\n";
  meta << "  },\n";

  meta << "  \"ChannelQC\": {\n";
  meta << "    \"Path\": ";
  write_string_or_null(a.channel_qc);
  meta << ",\n";
  meta << "    \"Resolved\": ";
  write_string_or_null(qc_resolved_path);
  meta << ",\n";
  meta << "    \"BadChannelCount\": " << qc_bad_count << ",\n";
  meta << "    \"ChannelsUsed\": " << channels_used << "\n";
  meta << "  },\n";

  meta << "  \"Outputs\": [\n";
  for (size_t i = 0; i < outputs.size(); ++i) {
    meta << "    \"" << json_escape(outputs[i]) << "\"";
    if (i + 1 < outputs.size()) meta << ",";
    meta << "\n";
  }
  meta << "  ]\n";
  meta << "}\n";
}



static std::string fmt_double(double v, int precision = 6) {
  if (!std::isfinite(v)) return "n/a";
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(precision) << v;
  return oss.str();
}

static std::string yesno(bool v) { return v ? "yes" : "no"; }

static void write_microstates_report_html(const Args& a,
                                          const EEGRecording& rec,
                                          const EEGRecording& rec_used,
                                          const MicrostatesOptions& msopt,
                                          const MicrostatesResult& r,
                                          bool have_qc,
                                          const std::vector<bool>& qc_bad,
                                          const std::vector<std::string>& qc_reasons,
                                          const std::string& qc_resolved_path) {
  if (!a.html_report) return;

  const std::string outpath = a.outdir + "/report.html";
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write report.html: " + outpath);

  std::string input_label;
  try {
    input_label = std::filesystem::u8path(a.input_path).filename().u8string();
  } catch (...) {
    input_label = a.input_path;
  }
  if (input_label.empty()) input_label = "(none)";

  size_t qc_bad_count = 0;
  for (bool b : qc_bad) if (b) ++qc_bad_count;

  std::string qc_label = "n/a";
  if (have_qc) {
    const std::string src = qc_resolved_path.empty() ? a.channel_qc : qc_resolved_path;
    try {
      qc_label = std::filesystem::u8path(src).filename().u8string();
    } catch (...) {
      qc_label = src;
    }
  }

  const int K = static_cast<int>(r.templates.size());

  out << "<!doctype html>\n"
      << "<html lang=\"en\">\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>Microstates Report</title>\n"
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
      << "    .kv { display:grid; grid-template-columns: 240px 1fr; gap: 6px 10px; font-size: 13px; }\n"
      << "    .kv .k { color: var(--muted); }\n"
      << "    .links { display:flex; flex-wrap: wrap; gap: 10px; }\n"
      << "    table { width:100%; border-collapse: collapse; font-size: 12px; }\n"
      << "    th, td { border-bottom: 1px solid var(--border); padding: 6px 6px; text-align: right; }\n"
      << "    th:first-child, td:first-child { text-align: left; }\n"
      << "    thead th { position: sticky; top: 0; background: rgba(15,23,42,0.95); }\n"
      << "    tr.bad td { background: rgba(248,113,113,0.12); }\n"
      << "    td.status { text-align: left; color: var(--muted); }\n"
      << "    .small { font-size: 12px; color: var(--muted); }\n"
      << "    .maps { display:grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }\n"
      << "    .map h3 { margin: 0 0 8px 0; font-size: 14px; }\n"
      << "    img { width: 100%; height: auto; border-radius: 10px; border: 1px solid var(--border); background: white; }\n"
      << "    .tag { display:inline-block; padding: 2px 8px; border:1px solid var(--border); border-radius: 999px; font-size: 12px; color: var(--muted); }\n"
      << "    code { color: #e2e8f0; }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div class=\"wrap\">\n"
      << "    <div class=\"top\">\n"
      << "      <div>\n"
      << "        <h1>Microstates Report</h1>\n"
      << "        <div class=\"sub\">Generated by <code>qeeg_microstates_cli</code>. Files are linked relative to this report.</div>\n"
      << "      </div>\n"
      << "      <div class=\"tag\">k=" << K << "</div>\n"
      << "    </div>\n"
      << "    <div style=\"height:12px\"></div>\n"
      << "    <div class=\"grid\">\n"
      << "      <div class=\"card\">\n"
      << "        <div style=\"font-weight:700; margin-bottom:8px\">Summary</div>\n"
      << "        <div class=\"kv\">\n"
      << "          <div class=\"k\">Input</div><div>" << svg_escape(input_label) << "</div>\n"
      << "          <div class=\"k\">Sampling rate</div><div>" << fmt_double(rec.fs_hz, 3) << " Hz</div>\n"
      << "          <div class=\"k\">Channels (total)</div><div>" << rec.n_channels() << "</div>\n"
      << "          <div class=\"k\">Channels (used)</div><div>" << rec_used.n_channels() << "</div>\n"
      << "          <div class=\"k\">Samples</div><div>" << rec.n_samples() << "</div>\n"
      << "          <div class=\"k\">Start</div><div>" << fmt_double(a.start_sec, 3) << " s</div>\n"
      << "          <div class=\"k\">Duration</div><div>" << (a.duration_sec > 0.0 ? (fmt_double(a.duration_sec, 3) + " s") : std::string("full")) << "</div>\n"
      << "          <div class=\"k\">Montage</div><div>" << svg_escape(a.montage_spec) << "</div>\n"
      << "          <div class=\"k\">Polarity invariant</div><div>" << yesno(msopt.polarity_invariant) << "</div>\n"
      << "          <div class=\"k\">Demean topography</div><div>" << yesno(msopt.demean_topography) << "</div>\n"
      << "          <div class=\"k\">GEV</div><div>" << fmt_double(r.gev, 6) << "</div>\n"
      << "          <div class=\"k\">Channel QC</div><div>" << yesno(have_qc) << (have_qc ? (" (" + svg_escape(qc_label) + ")") : "") << "</div>\n"
      << "          <div class=\"k\">Bad channels excluded</div><div>" << (have_qc ? std::to_string(qc_bad_count) : std::string("n/a")) << "</div>\n"
      << "          <div class=\"k\">Export segments</div><div>" << yesno(a.export_segments) << "</div>\n"
      << "          <div class=\"k\">Export BIDS events</div><div>" << yesno(a.export_bids_events) << "</div>\n"
      << "          <div class=\"k\">Interpolation</div><div>" << svg_escape(a.interp) << " (grid " << a.grid << ")</div>\n"
      << "          <div class=\"k\">Annotate BMPs</div><div>" << yesno(a.annotate) << "</div>\n"
      << "        </div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div style=\"font-weight:700; margin-bottom:8px\">Outputs</div>\n"
      << "        <div class=\"links\">\n"
      << "          <a href=\"" << url_escape("microstate_templates.csv") << "\">microstate_templates.csv</a>\n"
      << "          <a href=\"" << url_escape("microstate_timeseries.csv") << "\">microstate_timeseries.csv</a>\n"
      << "          <a href=\"" << url_escape("microstate_transition_counts.csv") << "\">microstate_transition_counts.csv</a>\n"
      << "          <a href=\"" << url_escape("microstate_transition_probs.csv") << "\">microstate_transition_probs.csv</a>\n"
      << "          <a href=\"" << url_escape("microstate_state_stats.csv") << "\">microstate_state_stats.csv</a>\n"
      << "          <a href=\"" << url_escape("microstate_summary.txt") << "\">microstate_summary.txt</a>\n";
  if (a.export_segments) {
    out << "          <a href=\"" << url_escape("microstate_segments.csv") << "\">microstate_segments.csv</a>\n";
  }
  if (a.export_bids_events) {
    out << "          <a href=\"" << url_escape("microstate_events.tsv") << "\">microstate_events.tsv</a>\n"
        << "          <a href=\"" << url_escape("microstate_events.json") << "\">microstate_events.json</a>\n";
  }
  if (have_qc) {
    out << "          <a href=\"" << url_escape("bad_channels_used.txt") << "\">bad_channels_used.txt</a>\n";
  }
  out << "          <a href=\"" << url_escape("microstates_run_meta.json") << "\">microstates_run_meta.json</a>\n"
      << "        </div>\n"
      << "        <div style=\"height:8px\"></div>\n"
      << "        <div class=\"small\">Note: Most modern browsers can display BMP. If images do not render, convert BMP → PNG.</div>\n"
      << "      </div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Per-state stats</div>\n"
      << "      <div style=\"max-height:360px; overflow:auto; border:1px solid var(--border); border-radius:10px\">\n"
      << "      <table>\n"
      << "        <thead>\n"
      << "          <tr>\n"
      << "            <th>State</th>\n"
      << "            <th>Coverage</th>\n"
      << "            <th>Mean duration (s)</th>\n"
      << "            <th>Occurrence (/s)</th>\n"
      << "          </tr>\n"
      << "        </thead>\n"
      << "        <tbody>\n";
  for (int k = 0; k < K; ++k) {
    out << "          <tr>\n"
        << "            <td>" << svg_escape(state_name(k)) << "</td>\n"
        << "            <td>" << fmt_double(r.coverage[static_cast<size_t>(k)], 6) << "</td>\n"
        << "            <td>" << fmt_double(r.mean_duration_sec[static_cast<size_t>(k)], 6) << "</td>\n"
        << "            <td>" << fmt_double(r.occurrence_per_sec[static_cast<size_t>(k)], 6) << "</td>\n"
        << "          </tr>\n";
  }
  out << "        </tbody>\n"
      << "      </table>\n"
      << "      </div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Transition counts</div>\n"
      << "      <div style=\"max-height:360px; overflow:auto; border:1px solid var(--border); border-radius:10px\">\n"
      << "      <table>\n"
      << "        <thead>\n"
      << "          <tr>\n"
      << "            <th>from\\to</th>\n";
  for (int k = 0; k < K; ++k) {
    out << "            <th>" << svg_escape(state_name(k)) << "</th>\n";
  }
  out << "          </tr>\n"
      << "        </thead>\n"
      << "        <tbody>\n";
  for (int i = 0; i < K; ++i) {
    out << "          <tr>\n"
        << "            <td>" << svg_escape(state_name(i)) << "</td>\n";
    for (int j = 0; j < K; ++j) {
      const int c = r.transition_counts[static_cast<size_t>(i)][static_cast<size_t>(j)];
      out << "            <td>" << c << "</td>\n";
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
      << "      <div class=\"maps\">\n";
  for (int k = 0; k < K; ++k) {
    const std::string fname = "topomap_microstate_" + state_name(k) + ".bmp";
    out << "        <div class=\"map\">\n"
        << "          <h3>State " << svg_escape(state_name(k)) << "</h3>\n"
        << "          <img src=\"" << url_escape(fname) << "\" alt=\"" << svg_escape(fname) << "\"/>\n"
        << "        </div>\n";
  }
  out << "      </div>\n"
      << "    </div>\n";

  if (have_qc && qc_bad_count > 0) {
    out << "    <div style=\"height:14px\"></div>\n"
        << "    <div class=\"card\">\n"
        << "      <div style=\"font-weight:700; margin-bottom:8px\">Bad channels (excluded)</div>\n"
        << "      <div class=\"small\">Channels marked bad by QC were excluded from estimation and rendered as NaN in templates.</div>\n"
        << "      <div style=\"height:8px\"></div>\n"
        << "      <div style=\"max-height:220px; overflow:auto; border:1px solid var(--border); border-radius:10px\">\n"
        << "      <table>\n"
        << "        <thead><tr><th>Channel</th><th>Reason</th></tr></thead>\n"
        << "        <tbody>\n";
    for (size_t c = 0; c < rec.channel_names.size(); ++c) {
      if (!qc_bad[c]) continue;
      out << "          <tr class=\"bad\"><td>" << svg_escape(rec.channel_names[c]) << "</td><td class=\"status\">";
      if (c < qc_reasons.size() && !qc_reasons[c].empty()) out << svg_escape(qc_reasons[c]);
      out << "</td></tr>\n";
    }
    out << "        </tbody>\n"
        << "      </table>\n"
        << "      </div>\n"
        << "    </div>\n";
  }

  out << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"small\">Generated at " << svg_escape(now_string_local()) << ".</div>\n"
      << "  </div>\n"
      << "</body>\n"
      << "</html>\n";

  std::cout << "Wrote HTML report: " << outpath << "\n";
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

    // Optional: load channel-level QC labels and exclude bad channels from the analysis.
    bool have_qc = false;
    std::string qc_resolved_path;
    std::vector<bool> qc_bad(rec.n_channels(), false);
    std::vector<std::string> qc_reasons(rec.n_channels());
    size_t qc_bad_count = 0;

    if (!a.channel_qc.empty()) {
      std::cout << "Loading channel QC: " << a.channel_qc << "\n";
      const ChannelQcMap qc = load_channel_qc_any(a.channel_qc, &qc_resolved_path);
      have_qc = true;

      for (size_t c = 0; c < rec.n_channels(); ++c) {
        const std::string key = normalize_channel_name(rec.channel_names[c]);
        const auto it = qc.find(key);
        if (it != qc.end() && it->second.bad) {
          qc_bad[c] = true;
          qc_reasons[c] = it->second.reasons;
          ++qc_bad_count;
        }
      }

      std::cout << "Channel QC loaded from: " << qc_resolved_path
                << " (" << qc_bad_count << "/" << rec.n_channels() << " channels marked bad)\n";

      // Persist the applied mask for provenance.
      write_bad_channels_used(a.outdir, rec, qc_bad, qc_reasons);
    }

    // Build the channel subset used for microstate estimation.
    EEGRecording rec_used;
    rec_used.fs_hz = rec.fs_hz;
    std::vector<size_t> used_to_orig;
    used_to_orig.reserve(rec.n_channels());
    rec_used.channel_names.reserve(rec.n_channels());
    rec_used.data.reserve(rec.n_channels());
    for (size_t c = 0; c < rec.n_channels(); ++c) {
      if (have_qc && qc_bad[c]) continue;
      used_to_orig.push_back(c);
      rec_used.channel_names.push_back(rec.channel_names[c]);
      rec_used.data.push_back(rec.data[c]);
    }
    if (rec_used.n_channels() < 2) {
      throw std::runtime_error("Need >=2 usable channels after excluding QC-bad channels");
    }

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

    MicrostatesResult r = estimate_microstates(rec_used, msopt);
    const int K = static_cast<int>(r.templates.size());

    // Expand templates to the original channel list (fill excluded channels with NaN).
    std::vector<std::vector<double>> templates_full;
    templates_full.resize(r.templates.size());
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    for (size_t k = 0; k < r.templates.size(); ++k) {
      templates_full[k].assign(rec.n_channels(), NaN);
      const auto& tpl = r.templates[k];
      for (size_t j = 0; j < tpl.size() && j < used_to_orig.size(); ++j) {
        templates_full[k][used_to_orig[j]] = tpl[j];
      }
    }

    std::vector<std::string> outputs;
    outputs.reserve(32);
    auto emit_out = [&](const std::string& rel) { outputs.push_back(rel); };

    // --- Write templates ---
    {
      std::ofstream f(a.outdir + "/microstate_templates.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");
      f << "microstate";
      for (const auto& ch : rec.channel_names) f << "," << ch;
      f << "\n";
      for (int k = 0; k < K; ++k) {
        f << state_name(k);
        for (double v : templates_full[static_cast<size_t>(k)]) {
          f << "," << v;
        }
        f << "\n";
      }
    }
    emit_out("microstate_templates.csv");

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
    emit_out("microstate_timeseries.csv");

    // --- Optional: segments (also used for BIDS-style events export) ---
    std::vector<MicrostateSegment> segs;
    if (a.export_segments || a.export_bids_events) {
      segs = microstate_segments(r.labels, r.corr, r.gfp, rec.fs_hz, /*include_undefined=*/false);
    }

    if (a.export_segments) {
      const double t0 = (a.start_sec > 0.0 ? a.start_sec : 0.0);
      const long long sample0 = (a.start_sec > 0.0)
          ? static_cast<long long>(std::llround(a.start_sec * rec.fs_hz))
          : 0;

      std::ofstream f(a.outdir + "/microstate_segments.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");
      f << "segment_index,label,start_sec,end_sec,duration_sec,mean_corr,mean_gfp,start_sample,end_sample\n";
      for (size_t si = 0; si < segs.size(); ++si) {
        const auto& s = segs[si];
        f << si
          << "," << state_name(s.label)
          << "," << (s.start_sec + t0)
          << "," << (s.end_sec + t0)
          << "," << s.duration_sec
          << "," << s.mean_corr
          << "," << s.mean_gfp
          << "," << (static_cast<long long>(s.start_sample) + sample0)
          << "," << (static_cast<long long>(s.end_sample) + sample0)
          << "\n";
      }
    }
    if (a.export_segments) emit_out("microstate_segments.csv");

    if (a.export_bids_events) {
      // Represent each microstate segment as a BIDS-style event.
      std::vector<AnnotationEvent> events;
      events.reserve(segs.size());
      for (const auto& s : segs) {
        AnnotationEvent ev;
        ev.onset_sec = s.start_sec + (a.start_sec > 0.0 ? a.start_sec : 0.0);
        ev.duration_sec = s.duration_sec;
        ev.text = "MS:" + state_name(s.label);
        events.push_back(std::move(ev));
      }

      const std::string p_tsv = a.outdir + "/microstate_events.tsv";
      const std::string p_json = a.outdir + "/microstate_events.json";
      write_events_tsv(p_tsv, events);

      BidsEventsTsvOptions ev_opt;
      ev_opt.include_trial_type = true;
      ev_opt.include_trial_type_levels = true;
      write_bids_events_json(p_json, ev_opt, events);

      emit_out("microstate_events.tsv");
      emit_out("microstate_events.json");
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
    emit_out("microstate_transition_counts.csv");


    // --- Transition probabilities (row-normalized) ---
    {
      std::ofstream f(a.outdir + "/microstate_transition_probs.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");

      f << "from\\to";
      for (int k = 0; k < K; ++k) f << "," << state_name(k);
      f << "\n";

      for (int i = 0; i < K; ++i) {
        f << state_name(i);
        int row_sum = 0;
        for (int j = 0; j < K; ++j) {
          row_sum += r.transition_counts[static_cast<size_t>(i)][static_cast<size_t>(j)];
        }
        for (int j = 0; j < K; ++j) {
          const int c = r.transition_counts[static_cast<size_t>(i)][static_cast<size_t>(j)];
          const double p = (row_sum > 0) ? (static_cast<double>(c) / static_cast<double>(row_sum)) : 0.0;
          f << "," << p;
        }
        f << "\n";
      }
    }
    emit_out("microstate_transition_probs.csv");

    // --- Per-state stats (CSV) ---
    {
      std::ofstream f(a.outdir + "/microstate_state_stats.csv");
      if (!f) throw std::runtime_error("Failed to open output CSV");
      f << "microstate,coverage,mean_duration_sec,occurrence_per_sec,gev_contrib,gev_frac\n";
      for (int k = 0; k < K; ++k) {
        const double gev_c = (static_cast<size_t>(k) < r.gev_state.size()) ? r.gev_state[static_cast<size_t>(k)] : 0.0;
        const double gev_f = (r.gev > 0.0) ? (gev_c / r.gev) : 0.0;
        f << state_name(k)
          << "," << r.coverage[static_cast<size_t>(k)]
          << "," << r.mean_duration_sec[static_cast<size_t>(k)]
          << "," << r.occurrence_per_sec[static_cast<size_t>(k)]
          << "," << gev_c
          << "," << gev_f
          << "\n";
      }
    }
    emit_out("microstate_state_stats.csv");

    // --- Summary ---
    {
      std::ofstream f(a.outdir + "/microstate_summary.txt");
      if (!f) throw std::runtime_error("Failed to open summary file");
      f << "qeeg_microstates_cli summary\n";
      f << "input: " << a.input_path << "\n";
      f << "fs_hz: " << rec.fs_hz << "\n";
      f << "channels_total: " << rec.n_channels() << "\n";
      f << "channels_used: " << rec_used.n_channels() << "\n";
      if (have_qc) {
        f << "channel_qc: " << a.channel_qc << "\n";
        f << "channel_qc_resolved: " << qc_resolved_path << "\n";
        f << "bad_channels_excluded: " << qc_bad_count << "\n";
      }
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
      f << "state,coverage,mean_duration_sec,occurrence_per_sec,gev_contrib,gev_frac\n";
      for (int k = 0; k < K; ++k) {
        const double gev_c = (static_cast<size_t>(k) < r.gev_state.size()) ? r.gev_state[static_cast<size_t>(k)] : 0.0;
        const double gev_f = (r.gev > 0.0) ? (gev_c / r.gev) : 0.0;
        f << state_name(k) << "," << r.coverage[static_cast<size_t>(k)]
          << "," << r.mean_duration_sec[static_cast<size_t>(k)]
          << "," << r.occurrence_per_sec[static_cast<size_t>(k)]
          << "," << gev_c
          << "," << gev_f
          << "\n";
      }
    }
    emit_out("microstate_summary.txt");

    // --- Render template topomaps ---
    // Use a symmetric scale shared across all maps for comparability.
    double max_abs = 0.0;
    for (const auto& tpl : templates_full) {
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
    for (size_t c = 0; c < rec.channel_names.size(); ++c) {
      if (have_qc && qc_bad[c]) continue;
      const auto& ch = rec.channel_names[c];
      Vec2 p;
      if (montage.get(ch, &p)) electrodes.push_back(p);
    }

    for (int k = 0; k < K; ++k) {
      std::vector<double> values = templates_full[static_cast<size_t>(k)];
      Grid2D grid = make_topomap(montage, rec.channel_names, values, topt);
      std::string path = a.outdir + "/topomap_microstate_" + state_name(k) + ".bmp";
      if (a.annotate) {
        render_grid_to_bmp_annotated(path, grid.size, grid.values, vmin, vmax, electrodes);
      } else {
        render_grid_to_bmp(path, grid.size, grid.values, vmin, vmax);
      }
      emit_out("topomap_microstate_" + state_name(k) + ".bmp");
    }

    // If QC was provided, expose the applied mask.
    if (have_qc) {
      emit_out("bad_channels_used.txt");
    }

    // Optional: HTML report (quick visual summary + links).
    if (a.html_report) {
      write_microstates_report_html(a, rec, rec_used, msopt, r,
                                   have_qc, qc_bad, qc_reasons, qc_resolved_path);
      emit_out("report.html");
    }

    // Write lightweight run metadata JSON for downstream interoperability.
    emit_out("microstates_run_meta.json");
    write_microstates_run_meta(a.outdir,
                               a,
                               qc_resolved_path,
                               qc_bad_count,
                               rec_used.n_channels(),
                               outputs);

    std::cout << "Wrote microstate outputs to: " << a.outdir << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
