#include "qeeg/bmp_writer.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_csv;
  std::string outdir{"out_topomap"};
  std::string montage_spec{"builtin:standard_1020_19"};

  // Rendering options
  bool annotate{false};
  bool html_report{false};

  bool json_index{false};
  std::string json_index_path{}; // default: <outdir>/topomap_index.json

  bool list_montages{false};
  bool list_montages_json{false};

  // Topomap interpolation options
  int grid{256};
  std::string interp{"idw"};
  double idw_power{2.0};
  int spline_terms{50};
  int spline_m{4};
  double spline_lambda{1e-5};

  // Value scaling
  bool have_vlim{false};
  double vmin{0.0};
  double vmax{0.0};
  bool robust{false};
  double robust_lo{0.05};
  double robust_hi{0.95};

  // Column selection
  std::vector<std::string> metrics;  // if empty: render all
  std::vector<std::string> exclude;  // remove specific metrics
};

static void print_help() {
  std::cout
    << "qeeg_topomap_cli\n\n"
    << "Render qEEG scalp topomaps (BMP) from a per-channel CSV table.\n\n"
    << "This tool is useful for \"brain mapping\" derived metrics such as:\n"
    << "  - bandpowers.csv (from qeeg_map_cli)\n"
    << "  - bandratios.csv (from qeeg_bandratios_cli)\n"
    << "  - any custom table: channel,<metric1>,<metric2>,...\n\n"
    << "Usage:\n"
    << "  qeeg_topomap_cli --input bandpowers.csv --outdir out_maps\n"
    << "  qeeg_topomap_cli --input bandratios.csv --metric theta_beta --annotate\n"
    << "  qeeg_topomap_cli --input out_bandpower --metric alpha --annotate\n"
    << "  qeeg_topomap_cli --input out_bandpower/bandpower_run_meta.json --metric alpha\n\n"
    << "Required:\n"
    << "  --input PATH            CSV/TSV table, *_run_meta.json, or an output directory containing a table\n\n"
    << "Options:\n"
    << "  --outdir DIR            Output directory (default: out_topomap)\n"
    << "  --montage SPEC          builtin:standard_1020_19 (default), builtin:standard_1010_61, or montage CSV (name,x,y)\n"
    << "  --list-montages         Print built-in montage keys and exit\n"
    << "  --list-montages-json    Print built-in montage keys as JSON and exit\n"
    << "  --metric NAME           Render only this column (repeatable). Default renders all numeric columns.\n"
    << "  --exclude NAME          Exclude a column (repeatable).\n"
    << "  --grid N                Topomap grid size (default: 256)\n"
    << "  --interp METHOD         idw|spline (default: idw)\n"
    << "  --idw-power P           IDW power (default: 2.0)\n"
    << "  --spline-terms N        Spherical spline Legendre terms (default: 50)\n"
    << "  --spline-m N            Spherical spline order m (default: 4)\n"
    << "  --spline-lambda X       Spline regularization (default: 1e-5)\n"
    << "  --annotate              Draw head outline + electrode markers + colorbar\n"
    << "  --html-report           Write topomap_report.html linking to the generated BMPs\n"
    << "  --json-index [PATH]     Write topomap_index.json for downstream tooling (default: <outdir>/topomap_index.json)\n"
    << "  --vmin X --vmax Y       Fixed colormap limits for all maps (overrides auto/robust scaling)\n"
    << "  --robust                Use percentile scaling (default 5th..95th of interpolated grid values)\n"
    << "  --robust-range LO HI    Percentiles for --robust (e.g., 0.02 0.98)\n"
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
      a.input_csv = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--montage" && i + 1 < argc) {
      a.montage_spec = argv[++i];
    } else if (arg == "--grid" && i + 1 < argc) {
      a.grid = to_int(argv[++i]);
    } else if (arg == "--interp" && i + 1 < argc) {
      a.interp = argv[++i];
    } else if (arg == "--idw-power" && i + 1 < argc) {
      a.idw_power = to_double(argv[++i]);
    } else if (arg == "--spline-terms" && i + 1 < argc) {
      a.spline_terms = to_int(argv[++i]);
    } else if (arg == "--spline-m" && i + 1 < argc) {
      a.spline_m = to_int(argv[++i]);
    } else if (arg == "--spline-lambda" && i + 1 < argc) {
      a.spline_lambda = to_double(argv[++i]);
    } else if (arg == "--annotate") {
      a.annotate = true;
    } else if (arg == "--html-report") {
      a.html_report = true;
    } else if (arg == "--json-index") {
      a.json_index = true;
      // Optional argument: path. If omitted, default will be <outdir>/topomap_index.json
      if ((i + 1) < argc) {
        const std::string next = argv[i + 1];
        if (!next.empty() && next[0] != '-') {
          a.json_index_path = next;
          ++i;
        }
      }
    } else if (arg == "--list-montages") {
      a.list_montages = true;
    } else if (arg == "--list-montages-json") {
      a.list_montages_json = true;
    } else if (arg == "--metric" && i + 1 < argc) {
      a.metrics.push_back(argv[++i]);
    } else if (arg == "--exclude" && i + 1 < argc) {
      a.exclude.push_back(argv[++i]);
    } else if (arg == "--vmin" && i + 1 < argc) {
      a.vmin = to_double(argv[++i]);
      a.have_vlim = true;
    } else if (arg == "--vmax" && i + 1 < argc) {
      a.vmax = to_double(argv[++i]);
      a.have_vlim = true;
    } else if (arg == "--robust") {
      a.robust = true;
    } else if (arg == "--robust-range" && i + 2 < argc) {
      a.robust = true;
      a.robust_lo = to_double(argv[++i]);
      a.robust_hi = to_double(argv[++i]);
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

static bool is_comment_or_empty(const std::string& t) {
  if (t.empty()) return true;
  if (starts_with(t, "#")) return true;
  if (starts_with(t, "//")) return true;
  return false;
}

static size_t count_delim_outside_quotes(const std::string& s, char delim) {
  bool in_quotes = false;
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      if (in_quotes && (i + 1) < s.size() && s[i + 1] == '"') {
        ++i; // escaped quote
        continue;
      }
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && c == delim) ++n;
  }
  return n;
}

static char detect_delim(const std::string& line) {
  const size_t n_comma = count_delim_outside_quotes(line, ',');
  const size_t n_semi = count_delim_outside_quotes(line, ';');
  const size_t n_tab = count_delim_outside_quotes(line, '\t');
  char best = ',';
  size_t best_n = n_comma;
  if (n_semi > best_n) {
    best = ';';
    best_n = n_semi;
  }
  if (n_tab > best_n) {
    best = '\t';
    best_n = n_tab;
  }
  return best;
}

static std::vector<std::string> parse_row(const std::string& raw, char delim) {
  auto cols = split_csv_row(raw, delim);
  for (auto& c : cols) c = trim(c);
  return cols;
}

static std::string norm_key(const std::string& s) {
  return to_lower(trim(s));
}

static int find_channel_col(const std::vector<std::string>& header) {
  for (size_t i = 0; i < header.size(); ++i) {
    const std::string k = norm_key(header[i]);
    if (k == "channel" || k == "name" || k == "ch") return static_cast<int>(i);
  }
  return 0; // fallback
}

struct ChannelTable {
  std::vector<std::string> channels;            // row-wise channels
  std::vector<std::string> metrics;             // metric column names
  std::vector<std::vector<double>> values;      // values[metric][row]
  char delim{','};
};

static ChannelTable read_channel_table(const Args& args) {
  std::ifstream f(args.input_csv);
  if (!f) throw std::runtime_error("Failed to open input CSV: " + args.input_csv);

  std::string line;
  size_t lineno = 0;
  bool saw_header = false;
  ChannelTable t;
  std::vector<size_t> metric_col_indices;
  int channel_col = 0;

  // Normalize selection lists for case-insensitive matching.
  std::vector<std::string> want;
  want.reserve(args.metrics.size());
  for (const auto& m : args.metrics) want.push_back(norm_key(m));

  std::vector<std::string> exclude;
  exclude.reserve(args.exclude.size());
  for (const auto& m : args.exclude) exclude.push_back(norm_key(m));

  while (std::getline(f, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string raw = trim(line);
    if (!saw_header) {
      raw = strip_utf8_bom(raw);
    }
    if (is_comment_or_empty(raw)) continue;

    if (!saw_header) {
      t.delim = detect_delim(raw);
      const auto header = parse_row(raw, t.delim);
      if (header.size() < 2) {
        throw std::runtime_error("Input CSV must have at least 2 columns (channel + metric): " + args.input_csv);
      }
      channel_col = find_channel_col(header);

      // Determine which metric columns to use.
      for (size_t i = 0; i < header.size(); ++i) {
        if (static_cast<int>(i) == channel_col) continue;
        const std::string name = trim(header[i]);
        if (name.empty()) continue;
        const std::string k = norm_key(name);
        if (!exclude.empty() && std::find(exclude.begin(), exclude.end(), k) != exclude.end()) {
          continue;
        }
        if (!want.empty() && std::find(want.begin(), want.end(), k) == want.end()) {
          continue;
        }
        t.metrics.push_back(name);
        metric_col_indices.push_back(i);
      }
      if (t.metrics.empty()) {
        throw std::runtime_error("No metric columns selected. Use --metric to select an existing column.");
      }
      t.values.assign(t.metrics.size(), std::vector<double>{});
      saw_header = true;
      continue;
    }

    const auto cols = parse_row(raw, t.delim);
    if (cols.empty()) continue;
    if (channel_col < 0 || static_cast<size_t>(channel_col) >= cols.size()) {
      std::cerr << "Warning: skipping row " << lineno << " (missing channel column)\n";
      continue;
    }

    const std::string ch = trim(cols[static_cast<size_t>(channel_col)]);
    if (ch.empty()) continue;

    t.channels.push_back(ch);

    const double NaN = std::numeric_limits<double>::quiet_NaN();
    for (size_t mi = 0; mi < t.metrics.size(); ++mi) {
      const size_t ci = metric_col_indices[mi];
      double v = NaN;
      if (ci < cols.size()) {
        const std::string s = trim(cols[ci]);
        if (!s.empty()) {
          try {
            v = to_double(s);
          } catch (...) {
            v = NaN;
          }
        }
      }
      t.values[mi].push_back(v);
    }
  }

  if (!saw_header) {
    throw std::runtime_error("Input CSV appears empty: " + args.input_csv);
  }
  if (t.channels.empty()) {
    throw std::runtime_error("No data rows found in input CSV: " + args.input_csv);
  }
  return t;
}

static std::pair<double, double> minmax_ignore_nan(const std::vector<float>& v) {
  double vmin = std::numeric_limits<double>::infinity();
  double vmax = -std::numeric_limits<double>::infinity();
  for (float x : v) {
    if (!std::isfinite(x)) continue;
    vmin = std::min(vmin, static_cast<double>(x));
    vmax = std::max(vmax, static_cast<double>(x));
  }
  if (!std::isfinite(vmin) || !std::isfinite(vmax)) {
    return {0.0, 1.0};
  }
  if (!(vmax > vmin)) {
    vmax = vmin + 1e-12;
  }
  return {vmin, vmax};
}

static double quantile_sorted(const std::vector<double>& sorted, double q01) {
  if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
  if (q01 <= 0.0) return sorted.front();
  if (q01 >= 1.0) return sorted.back();
  const double pos = q01 * static_cast<double>(sorted.size() - 1);
  const size_t i0 = static_cast<size_t>(std::floor(pos));
  const size_t i1 = static_cast<size_t>(std::ceil(pos));
  const double t = pos - static_cast<double>(i0);
  if (i0 == i1) return sorted[i0];
  return (1.0 - t) * sorted[i0] + t * sorted[i1];
}

static std::pair<double, double> robust_limits(const std::vector<float>& grid,
                                               double lo,
                                               double hi) {
  std::vector<double> vals;
  vals.reserve(grid.size());
  for (float x : grid) {
    if (!std::isfinite(x)) continue;
    vals.push_back(static_cast<double>(x));
  }
  if (vals.size() < 8) {
    return minmax_ignore_nan(grid);
  }
  std::sort(vals.begin(), vals.end());
  double v0 = quantile_sorted(vals, lo);
  double v1 = quantile_sorted(vals, hi);
  if (!std::isfinite(v0) || !std::isfinite(v1) || !(v1 > v0)) {
    return minmax_ignore_nan(grid);
  }
  return {v0, v1};
}

static std::string fmt_double(double x, int digits = 6) {
  if (!std::isfinite(x)) return "nan";
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(digits) << x;
  return oss.str();
}

static void write_html_report(const Args& args,
                              const ChannelTable& table,
                              const std::vector<std::string>& rendered_metrics,
                              const std::vector<std::string>& bmp_files) {
  const std::string outpath = args.outdir + "/topomap_report.html";
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);

  out << "<!doctype html>\n"
      << "<html>\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>qEEG Topomap Report</title>\n"
      << "  <style>\n"
      << "    :root { --bg:#0b1020; --panel:#111a33; --text:#e5e7eb; --muted:#94a3b8; --accent:#38bdf8; --border:rgba(255,255,255,0.10); }\n"
      << "    html,body { margin:0; height:100%; background:var(--bg); color:var(--text); font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial; }\n"
      << "    .wrap { max-width: 1180px; margin: 0 auto; padding: 18px; }\n"
      << "    a { color: var(--accent); text-decoration: none; }\n"
      << "    a:hover { text-decoration: underline; }\n"
      << "    .card { background: rgba(17,26,51,0.6); border:1px solid var(--border); border-radius: 12px; padding: 12px; }\n"
      << "    .kv { display:grid; grid-template-columns: 220px 1fr; gap: 6px 10px; font-size: 13px; }\n"
      << "    .kv .k { color: var(--muted); }\n"
      << "    .maps { display:grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }\n"
      << "    img { width: 100%; height: auto; border-radius: 10px; border: 1px solid var(--border); background: white; }\n"
      << "    h1 { margin:0 0 6px 0; font-size: 22px; }\n"
      << "    .sub { color: var(--muted); font-size: 13px; }\n"
      << "    .small { font-size: 12px; color: var(--muted); }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div class=\"wrap\">\n"
      << "    <h1>qEEG Topomap Report</h1>\n"
      << "    <div class=\"sub\">Generated by <code>qeeg_topomap_cli</code></div>\n"
      << "    <div style=\"height:12px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Summary</div>\n"
      << "      <div class=\"kv\">\n"
      << "        <div class=\"k\">Input CSV</div><div>" << svg_escape(args.input_csv) << "</div>\n"
      << "        <div class=\"k\">Rows</div><div>" << table.channels.size() << "</div>\n"
      << "        <div class=\"k\">Montage</div><div>" << svg_escape(args.montage_spec) << "</div>\n"
      << "        <div class=\"k\">Interpolation</div><div>" << svg_escape(args.interp) << " (grid " << args.grid << ")</div>\n"
      << "        <div class=\"k\">Annotate BMPs</div><div>" << (args.annotate ? "yes" : "no") << "</div>\n"
      << "        <div class=\"k\">Scaling</div><div>";
  if (args.have_vlim) {
    out << "fixed [" << fmt_double(args.vmin, 4) << ", " << fmt_double(args.vmax, 4) << "]";
  } else if (args.robust) {
    out << "robust percentiles [" << fmt_double(args.robust_lo, 3) << ", " << fmt_double(args.robust_hi, 3) << "]";
  } else {
    out << "auto min/max per map";
  }
  out << "</div>\n"
      << "      </div>\n"
      << "      <div style=\"height:8px\"></div>\n"
      << "      <div class=\"small\">Note: Most modern browsers can display BMP. If images do not render, convert BMP → PNG.</div>\n"
      << "    </div>\n"
      << "    <div style=\"height:14px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div style=\"font-weight:700; margin-bottom:8px\">Topomaps</div>\n"
      << "      <div class=\"maps\">\n";

  for (size_t i = 0; i < rendered_metrics.size(); ++i) {
    out << "        <div>\n"
        << "          <div class=\"small\" style=\"margin-bottom:6px\">" << svg_escape(rendered_metrics[i]) << "</div>\n"
        << "          <img src=\"" << url_escape(bmp_files[i]) << "\" alt=\"" << svg_escape(bmp_files[i]) << "\"/>\n"
        << "        </div>\n";
  }

  out << "      </div>\n"
      << "    </div>\n"
      << "  </div>\n"
      << "</body>\n"
      << "</html>\n";

  std::cout << "Wrote HTML report: " << outpath << "\n";
}


// ---- Machine-readable JSON index -------------------------------------------

struct IndexChannel {
  std::string channel; // original label from the input table
  std::string key;     // normalized key (qeeg::normalize_channel_name)
  double x{0.0};
  double y{0.0};
  double value{0.0};
};

struct IndexMap {
  std::string metric;   // original metric column name
  std::string file;     // relative to outdir (usually just a filename)
  double vmin{0.0};
  double vmax{1.0};
  int n_channels{0};
  std::vector<IndexChannel> channels; // channels used (finite values with montage positions)
};

static std::string json_number(double x, int digits = 10) {
  if (!std::isfinite(x)) return "null";
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(digits) << x;
  return oss.str();
}

static std::string posix_slashes(std::string s) {
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

static std::string safe_relpath_posix(const std::filesystem::path& target_abs,
                                      const std::filesystem::path& base_abs) {
  try {
    std::filesystem::path rel = std::filesystem::relative(target_abs, base_abs);
    std::string s = rel.generic_string();
    if (s.empty()) return s;

    // Reject obvious escape paths. Keep the output safe for downstream tools that treat
    // these as paths relative to the index file.
    if (starts_with(s, "../") || s == ".." || s.find("/../") != std::string::npos) {
      return posix_slashes(target_abs.generic_string());
    }

    // Avoid drive-prefixed paths leaking into a "relative" output.
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
      return posix_slashes(target_abs.generic_string());
    }
    return posix_slashes(s);
  } catch (...) {
    return posix_slashes(target_abs.generic_string());
  }
}

static void write_topomap_index_json(const std::string& index_path,
                                     const Args& args,
                                     const Montage& montage,
                                     const TopomapOptions& topt,
                                     const std::vector<IndexMap>& maps,
                                     const std::string& run_meta_filename,
                                     const std::string& report_html_filename_or_empty) {
  std::filesystem::path idx_path = std::filesystem::path(index_path);
  if (idx_path.empty()) {
    throw std::runtime_error("write_topomap_index_json: empty index_path");
  }

  std::filesystem::path idx_dir = idx_path.parent_path();
  if (idx_dir.empty()) idx_dir = std::filesystem::path(".");

  const std::filesystem::path idx_dir_abs = std::filesystem::absolute(idx_dir);
  const std::filesystem::path outdir_abs = std::filesystem::absolute(std::filesystem::path(args.outdir));

  const std::string outdir_rel = safe_relpath_posix(outdir_abs, idx_dir_abs);
  const std::filesystem::path run_meta_abs = outdir_abs / run_meta_filename;
  const std::string run_meta_rel = safe_relpath_posix(run_meta_abs, idx_dir_abs);

  std::string report_rel;
  if (!report_html_filename_or_empty.empty()) {
    const std::filesystem::path rep_abs = outdir_abs / report_html_filename_or_empty;
    report_rel = safe_relpath_posix(rep_abs, idx_dir_abs);
  }

  std::ofstream out(index_path);
  if (!out) throw std::runtime_error("Failed to write JSON index: " + index_path);

  const std::string schema_url =
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/qeeg_topomap_index.schema.json";

  out << "{\n";
  out << "  \"$schema\": \"" << json_escape(schema_url) << "\",\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"generated_utc\": \"" << json_escape(now_string_utc()) << "\",\n";
  out << "  \"tool\": \"qeeg_topomap_cli\",\n";
  out << "  \"input_path\": \"" << json_escape(args.input_csv) << "\",\n";
  out << "  \"outdir\": \"" << json_escape(outdir_rel) << "\",\n";
  out << "  \"run_meta_json\": \"" << json_escape(run_meta_rel) << "\",\n";
  if (!report_rel.empty()) {
    out << "  \"report_html\": \"" << json_escape(report_rel) << "\",\n";
  } else {
    out << "  \"report_html\": null,\n";
  }

  // Render/interpolation metadata.
  out << "  \"render\": {\n";
  out << "    \"annotate\": " << (args.annotate ? "true" : "false") << "\n";
  out << "  },\n";

  const std::string method =
    (topt.method == TopomapInterpolation::SPHERICAL_SPLINE) ? "spline" : "idw";

  out << "  \"interpolation\": {\n";
  out << "    \"method\": \"" << json_escape(method) << "\",\n";
  out << "    \"grid\": " << topt.grid_size << ",\n";
  out << "    \"idw_power\": " << json_number(topt.idw_power, 6) << ",\n";
  out << "    \"spline_terms\": " << topt.spline.n_terms << ",\n";
  out << "    \"spline_m\": " << topt.spline.m << ",\n";
  out << "    \"spline_lambda\": " << json_number(topt.spline.lambda, 10) << "\n";
  out << "  },\n";

  const std::string scale_mode = args.have_vlim ? "fixed" : (args.robust ? "robust" : "auto");
  out << "  \"scaling\": {\n";
  out << "    \"mode\": \"" << json_escape(scale_mode) << "\",\n";
  out << "    \"fixed_vmin\": " << (args.have_vlim ? json_number(args.vmin, 10) : "null") << ",\n";
  out << "    \"fixed_vmax\": " << (args.have_vlim ? json_number(args.vmax, 10) : "null") << ",\n";
  out << "    \"robust_lo\": " << (args.robust ? json_number(args.robust_lo, 10) : "null") << ",\n";
  out << "    \"robust_hi\": " << (args.robust ? json_number(args.robust_hi, 10) : "null") << "\n";
  out << "  },\n";

  // Montage coordinates (for UI previews / QA).
  {
    std::vector<std::string> names = montage.channel_names();
    std::sort(names.begin(), names.end());
    out << "  \"montage\": {\n";
    out << "    \"spec\": \"" << json_escape(args.montage_spec) << "\",\n";
    out << "    \"n_channels\": " << names.size() << ",\n";
    out << "    \"channels\": [\n";
    for (size_t i = 0; i < names.size(); ++i) {
      Vec2 p;
      (void)montage.get(names[i], &p);
      out << "      {\"key\": \"" << json_escape(names[i]) << "\", \"x\": " << json_number(p.x, 8)
          << ", \"y\": " << json_number(p.y, 8) << "}";
      if (i + 1 < names.size()) out << ",";
      out << "\n";
    }
    out << "    ]\n";
    out << "  },\n";
  }

  // Maps (ordered as rendered).
  out << "  \"maps\": [\n";
  for (size_t mi = 0; mi < maps.size(); ++mi) {
    const IndexMap& m = maps[mi];
    const std::filesystem::path bmp_abs = outdir_abs / m.file;
    const std::string bmp_rel = safe_relpath_posix(bmp_abs, idx_dir_abs);

    out << "    {\n";
    out << "      \"metric\": \"" << json_escape(m.metric) << "\",\n";
    out << "      \"file\": \"" << json_escape(bmp_rel) << "\",\n";
    out << "      \"vmin\": " << json_number(m.vmin, 10) << ",\n";
    out << "      \"vmax\": " << json_number(m.vmax, 10) << ",\n";
    out << "      \"n_channels\": " << m.n_channels << ",\n";
    out << "      \"channels\": [\n";

    for (size_t ci = 0; ci < m.channels.size(); ++ci) {
      const IndexChannel& c = m.channels[ci];
      out << "        {\"channel\": \"" << json_escape(c.channel) << "\", "
          << "\"key\": \"" << json_escape(c.key) << "\", "
          << "\"x\": " << json_number(c.x, 8) << ", "
          << "\"y\": " << json_number(c.y, 8) << ", "
          << "\"value\": " << json_number(c.value, 10) << "}";
      if (ci + 1 < m.channels.size()) out << ",";
      out << "\n";
    }

    out << "      ]\n";
    out << "    }";
    if (mi + 1 < maps.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";

  std::cout << "Wrote JSON index: " << index_path << "\n";
}

static void print_montages_text() {
  std::cout << "builtin:standard_1020_19\t19 channels\n";
  std::cout << "builtin:standard_1010_61\t61 channels\n";
}

static void print_montages_json() {
  const Montage m19 = Montage::builtin_standard_1020_19();
  const Montage m61 = Montage::builtin_standard_1010_61();

  std::ostream& out = std::cout;

  auto write_montage = [](std::ostream& out, const std::string& key, const Montage& m) {
    std::vector<std::string> names = m.channel_names();
    std::sort(names.begin(), names.end());
    out << "    {\n";
    out << "      \"key\": \"" << json_escape(key) << "\",\n";
    out << "      \"n_channels\": " << names.size() << ",\n";
    out << "      \"channels\": [\n";
    for (size_t i = 0; i < names.size(); ++i) {
      Vec2 p;
      (void)m.get(names[i], &p);
      out << "        {\"key\": \"" << json_escape(names[i]) << "\", \"x\": " << json_number(p.x, 8)
          << ", \"y\": " << json_number(p.y, 8) << "}";
      if (i + 1 < names.size()) out << ",";
      out << "\n";
    }
    out << "      ]\n";
    out << "    }";
  };

  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"generated_utc\": \"" << json_escape(now_string_utc()) << "\",\n";
  out << "  \"montages\": [\n";
  write_montage(out, "builtin:standard_1020_19", m19);
  out << ",\n";
  write_montage(out, "builtin:standard_1010_61", m61);
  out << "\n";
  out << "  ]\n";
  out << "}\n";
}


} // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.list_montages || args.list_montages_json) {
      if (args.list_montages_json) {
        print_montages_json();
      } else {
        print_montages_text();
      }
      return 0;
    }
    if (args.input_csv.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.have_vlim && !(args.vmax > args.vmin)) {
      throw std::runtime_error("Invalid --vmin/--vmax: require vmax > vmin");
    }
    if (args.robust && !(args.robust_hi > args.robust_lo) ) {
      throw std::runtime_error("Invalid --robust-range: require HI > LO");
    }
    if (args.robust && (args.robust_lo < 0.0 || args.robust_hi > 1.0)) {
      throw std::runtime_error("Invalid --robust-range: percentiles must be in [0,1]");
    }

    // Allow chaining: --input can be a CSV/TSV, a *_run_meta.json file, or an output directory.
    {
      ResolveInputTableOptions opt;
      opt.preferred_filenames = {"bandpowers.csv", "bandpowers.tsv", "bandratios.csv", "bandratios.tsv"};
      ResolvedInputPath rp = resolve_input_table_path(args.input_csv, opt);
      if (!rp.note.empty()) std::cout << rp.note << "\n";
      args.input_csv = rp.path;
    }

    ensure_directory(args.outdir);

    const Montage montage = load_montage(args.montage_spec);
    const ChannelTable table = read_channel_table(args);

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

    // Prepare electrode positions for optional annotation overlay (only channels present in the table).
    std::vector<Vec2> electrode_positions_unit;
    electrode_positions_unit.reserve(table.channels.size());
    for (const auto& ch : table.channels) {
      Vec2 p;
      if (montage.get(ch, &p)) {
        electrode_positions_unit.push_back(p);
      }
    }

    std::vector<std::string> outputs;
    std::vector<std::string> rendered_metrics;
    std::vector<std::string> rendered_files;
    std::vector<IndexMap> index_maps;
    index_maps.reserve(table.metrics.size());


    for (size_t mi = 0; mi < table.metrics.size(); ++mi) {
      const std::string metric = table.metrics[mi];
      const auto& vals = table.values[mi];

      // Gather channels used (finite values with montage positions).
      IndexMap idx;
      idx.metric = metric;
      idx.channels.clear();
      idx.channels.reserve(table.channels.size());

      for (size_t i = 0; i < table.channels.size() && i < vals.size(); ++i) {
        const double v = vals[i];
        if (!std::isfinite(v)) continue;
        Vec2 p;
        if (!montage.get(table.channels[i], &p)) continue;

        IndexChannel c;
        c.channel = table.channels[i];
        c.key = normalize_channel_name(table.channels[i]);
        c.x = p.x;
        c.y = p.y;
        c.value = v;
        idx.channels.push_back(std::move(c));
      }

      std::sort(idx.channels.begin(), idx.channels.end(),
                [](const IndexChannel& a, const IndexChannel& b) { return a.key < b.key; });

      idx.n_channels = static_cast<int>(idx.channels.size());
      if (idx.n_channels < 3) {
        std::cerr << "Skipping metric '" << metric << "' (need >= 3 channels with finite values and montage positions; got "
                  << idx.n_channels << ")\n";
        continue;
      }

      std::cout << "Rendering metric: " << metric << "\n";
      Grid2D grid = make_topomap(montage, table.channels, vals, topt);

      double vmin = 0.0;
      double vmax = 1.0;
      if (args.have_vlim) {
        vmin = args.vmin;
        vmax = args.vmax;
      } else if (args.robust) {
        auto lim = robust_limits(grid.values, args.robust_lo, args.robust_hi);
        vmin = lim.first;
        vmax = lim.second;
      } else {
        auto lim = minmax_ignore_nan(grid.values);
        vmin = lim.first;
        vmax = lim.second;
      }

      idx.vmin = vmin;
      idx.vmax = vmax;

      // Sanitize metric name for file output (cross-platform, conservative).
      std::string safe = metric;
      for (char& c : safe) {
        const bool ok =
          (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.';
        if (!ok) c = '_';
      }
      while (!safe.empty() && safe.front() == '_') safe.erase(safe.begin());
      while (!safe.empty() && safe.back() == '_') safe.pop_back();
      if (safe.empty()) safe = "metric";

      const std::string bmp = "topomap_" + safe + ".bmp";
      const std::string outpath = args.outdir + "/" + bmp;
      if (args.annotate) {
        render_grid_to_bmp_annotated(outpath, grid.size, grid.values, vmin, vmax, electrode_positions_unit);
      } else {
        render_grid_to_bmp(outpath, grid.size, grid.values, vmin, vmax);
      }
      outputs.push_back(bmp);
      rendered_metrics.push_back(metric);
      rendered_files.push_back(bmp);
      idx.file = bmp;
      index_maps.push_back(std::move(idx));
    }

    if (rendered_files.empty()) {
      throw std::runtime_error("No maps rendered (no metrics had enough data). Check montage/channel labels and numeric values.");
    }

    if (args.html_report) {
      write_html_report(args, table, rendered_metrics, rendered_files);
      outputs.push_back("topomap_report.html");
    }

    if (args.json_index) {
      const std::string index_path = args.json_index_path.empty()
                                     ? (args.outdir + "/topomap_index.json")
                                     : args.json_index_path;
      const std::string run_meta_name = "topomap_run_meta.json";
      const std::string report_name = args.html_report ? "topomap_report.html" : std::string();
      write_topomap_index_json(index_path, args, montage, topt, index_maps, run_meta_name, report_name);

      // If the index lives inside --outdir, include it in Outputs for UI discovery.
      try {
        const std::filesystem::path outdir_abs = std::filesystem::absolute(std::filesystem::path(args.outdir));
        const std::filesystem::path idx_abs = std::filesystem::absolute(std::filesystem::path(index_path));
        std::filesystem::path rel = std::filesystem::relative(idx_abs, outdir_abs);
        std::string rel_s = posix_slashes(rel.generic_string());
        if (!rel_s.empty() && !starts_with(rel_s, "../") && rel_s != ".." && rel_s.find("/../") == std::string::npos) {
          outputs.push_back(rel_s);
        } else {
          std::cerr << "Note: --json-index path is outside --outdir; not adding to run meta Outputs\n";
        }
      } catch (...) {
        std::cerr << "Note: Failed to compute relative path for JSON index; not adding to run meta Outputs\n";
      }
    }

    // Write lightweight run metadata so qeeg_ui_cli / qeeg_ui_server_cli can discover outputs.
    const std::string meta_path = args.outdir + "/topomap_run_meta.json";
    outputs.push_back("topomap_run_meta.json");
    (void)write_run_meta_json(meta_path,
                              "qeeg_topomap_cli",
                              args.outdir,
                              args.input_csv,
                              outputs);

    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
