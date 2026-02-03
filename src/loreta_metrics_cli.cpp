#include "qeeg/cli_input.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTool = "qeeg_loreta_metrics_cli";

struct Args {

  // Protocol candidate extraction (heuristic; non-clinical)
  bool protocol_json = false;
  std::string protocol_path;  // default: <outdir>/loreta_protocol.json
  int protocol_top = 20;
  bool protocol_only_z = false;
  double protocol_threshold = 0.0;
  std::string input;
  std::string outdir = "out_loreta";
  std::string atlas = "unknown";
  std::string roi_column;  // auto-detect if empty

  // Long-form input support (ROI, metric, band, value columns).
  std::string metric_column;  // auto-detect if empty
  std::string band_column;    // auto-detect if empty
  std::string value_column;   // auto-detect if empty
  std::string metric_name_format = "metric_band";  // metric_band | band_metric

  std::vector<std::string> include_metrics;  // if empty, include all numeric columns
  std::vector<std::string> exclude_metrics;

  std::string csv_wide_name = "loreta_metrics.csv";
  std::string csv_long_name = "loreta_metrics_long.csv";
  bool html_report = false;

  bool json_index = false;
  std::string json_index_path;  // default: <outdir>/loreta_metrics_index.json
};

struct Table {
  std::string roi_col;
  std::vector<std::string> metrics;  // column names
  std::vector<std::string> rois;
  std::vector<std::vector<double>> values;  // NaN means missing
};

static void print_help(const char* argv0) {
  std::cout
      << "Usage: " << argv0 << " --input <roi_metrics.csv|dir> [--outdir out_loreta] [options]\n\n"
      << "Normalize ROI-level LORETA metrics (e.g., Brodmann areas, AAL ROIs) into\n"
      << "a consistent CSV + optional HTML report + optional JSON index.\n\n"
      << "Inputs\n"
      << "  --input PATH            CSV/TSV file, output dir, or run_meta JSON\n"
      << "  --roi-column NAME       Column to treat as ROI label (auto if omitted)\n"
      << "  --metric-column NAME    Long-form: metric/measure column (auto if omitted)\n"
      << "  --band-column NAME      Long-form: band/frequency column (auto if omitted)\n"
      << "  --value-column NAME     Long-form: value column (auto if omitted)\n"
      << "  --metric-name-format F  Long-form: metric_band or band_metric (default: metric_band)\n"
      << "  --atlas NAME            Atlas/ROI system label (e.g., brodmann, aal)\n\n"
      << "Outputs\n"
      << "  --outdir DIR            Output directory (default: out_loreta)\n"
      << "  --csv-wide NAME          Output wide CSV filename (default: loreta_metrics.csv)\n"
      << "  --csv-long NAME          Output long CSV filename (default: loreta_metrics_long.csv)\n"
      << "  --html-report            Write loreta_metrics_report.html\n"
      << "  --json-index             Write loreta_metrics_index.json\n"
      << "  --json-index-path PATH   Override index path\n\n"
      << "  --protocol-json          Write loreta_protocol.json (ranked targets; heuristic)\n"
      << "  --protocol-path PATH     Override protocol path\n"
      << "  --protocol-top N         Max protocol targets (default: 20)\n"
      << "  --protocol-only-z        Only include z-score-like metrics\n"
      << "  --protocol-threshold X   Only include targets where |value| >= X\n\n"
      << "Filtering\n"
      << "  --metrics a,b,c          Keep only these metric columns\n"
      << "  --exclude-metrics x,y    Drop these metric columns\n\n"
      << "Other\n"
      << "  --version                Print version\n"
      << "  --help                   Show this help\n";
}

static std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      cur = qeeg::trim(cur);
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  cur = qeeg::trim(cur);
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}


static bool is_z_metric_name(const std::string& name) {
  std::string s = lower_ascii(qeeg::trim(name));
  if (s.empty()) return false;
  if (s == "z") return true;
  if (s.find("zscore") != std::string::npos) return true;
  if (s.find("z-score") != std::string::npos) return true;
  if (qeeg::starts_with(s, "z_") || qeeg::starts_with(s, "z-")) return true;
  if (s.size() >= 2 && s.substr(s.size() - 2) == "_z") return true;
  if (s.find("_z_") != std::string::npos) return true;

  // Token-style detection: standalone "z" delimited by non-alnum.
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != 'z') continue;
    const bool left_ok = (i == 0) || !std::isalnum(static_cast<unsigned char>(s[i - 1]));
    const bool right_ok = (i + 1 >= s.size()) || !std::isalnum(static_cast<unsigned char>(s[i + 1]));
    if (left_ok && right_ok) return true;
  }
  return false;
}

static std::optional<std::string> detect_band(const std::string& metric_name) {
  const std::string s = lower_ascii(metric_name);
  struct Band {
    const char* needle;
    const char* band;
  };

  // Order matters: match more specific sub-bands before broad families.
  const Band bands[] = {
      {"alpha1", "alpha1"},
      {"alpha_1", "alpha1"},
      {"alpha-1", "alpha1"},
      {"alpha 1", "alpha1"},
      {"alpha2", "alpha2"},
      {"alpha_2", "alpha2"},
      {"alpha-2", "alpha2"},
      {"alpha 2", "alpha2"},

      {"beta1", "beta1"},
      {"beta_1", "beta1"},
      {"beta-1", "beta1"},
      {"beta 1", "beta1"},
      {"beta2", "beta2"},
      {"beta_2", "beta2"},
      {"beta-2", "beta2"},
      {"beta 2", "beta2"},
      {"beta3", "beta3"},
      {"beta_3", "beta3"},
      {"beta-3", "beta3"},
      {"beta 3", "beta3"},

      {"highbeta", "high_beta"},
      {"hibeta", "high_beta"},
      {"hi_beta", "high_beta"},
      {"high_beta", "high_beta"},
      {"lowbeta", "low_beta"},
      {"lobeta", "low_beta"},
      {"lo_beta", "low_beta"},
      {"low_beta", "low_beta"},

      {"lowgamma", "low_gamma"},
      {"low_gamma", "low_gamma"},
      {"highgamma", "high_gamma"},
      {"high_gamma", "high_gamma"},

      {"delta", "delta"},
      {"theta", "theta"},
      {"alpha", "alpha"},
      {"smr", "smr"},
      {"sigma", "sigma"},
      {"mu", "mu"},
      {"beta", "beta"},
      {"gamma", "gamma"},
  };
  for (const auto& b : bands) {
    if (s.find(b.needle) != std::string::npos) return std::string(b.band);
  }
  return std::nullopt;
}

static std::string base_metric_kind(const std::string& metric_name) {
  const std::string s = lower_ascii(metric_name);
  if (s.find("lagged") != std::string::npos || s.find("phase") != std::string::npos ||
      s.find("sync") != std::string::npos || s.find("coh") != std::string::npos ||
      s.find("coherence") != std::string::npos || s.find("lps") != std::string::npos ||
      s.find("pli") != std::string::npos || s.find("plv") != std::string::npos) {
    return "connectivity";
  }
  if (s.find("csd") != std::string::npos || s.find("current") != std::string::npos ||
      s.find("density") != std::string::npos) {
    return "current_density";
  }
  if (s.find("power") != std::string::npos || s.find("amplitude") != std::string::npos ||
      s.find("amp") != std::string::npos) {
    return "power";
  }
  return "metric";
}

struct ProtocolTarget {
  std::string roi;
  std::string metric;
  std::string metric_kind;  // e.g., current_density / connectivity / power / metric
  std::string value_kind;   // zscore / raw
  std::optional<std::string> band;
  double value = 0.0;
  double abs_value = 0.0;
  std::optional<std::string> suggested_direction;  // only meaningful for zscore values
};

static std::vector<ProtocolTarget> compute_protocol_targets(const Table& t, const Args& args) {
  std::vector<ProtocolTarget> out;
  const double thr = args.protocol_threshold;

  for (size_t j = 0; j < t.metrics.size(); ++j) {
    const std::string& metric = t.metrics[j];
    const bool is_z = is_z_metric_name(metric);
    if (args.protocol_only_z && !is_z) continue;

    for (size_t i = 0; i < t.rois.size(); ++i) {
      const double v = t.values[i][j];
      if (!std::isfinite(v)) continue;
      const double av = std::fabs(v);
      if (thr > 0.0 && av < thr) continue;

      ProtocolTarget pt;
      pt.roi = t.rois[i];
      pt.metric = metric;
      pt.metric_kind = base_metric_kind(metric);
      pt.value_kind = is_z ? "zscore" : "raw";
      pt.band = detect_band(metric);
      pt.value = v;
      pt.abs_value = av;
      if (is_z) {
        if (v > 0) pt.suggested_direction = "decrease";
        else if (v < 0) pt.suggested_direction = "increase";
        else pt.suggested_direction = "none";
      }
      out.push_back(std::move(pt));
    }
  }

  std::sort(out.begin(), out.end(), [](const ProtocolTarget& a, const ProtocolTarget& b) {
    if (a.abs_value != b.abs_value) return a.abs_value > b.abs_value;
    if (a.metric != b.metric) return a.metric < b.metric;
    return a.roi < b.roi;
  });

  if (args.protocol_top > 0 && static_cast<size_t>(args.protocol_top) < out.size()) {
    out.resize(static_cast<size_t>(args.protocol_top));
  }
  return out;
}

static bool is_comment_or_empty(const std::string& line) {
  std::string t = qeeg::trim(line);
  if (t.empty()) return true;
  if (qeeg::starts_with(t, "#")) return true;
  if (qeeg::starts_with(t, "//")) return true;
  return false;
}

static char detect_delim(const std::string& header_line) {
  // Count candidate delimiters outside quotes.
  auto count_delim = [&](char d) -> int {
    bool in_quotes = false;
    int n = 0;
    for (char c : header_line) {
      if (c == '"') in_quotes = !in_quotes;
      if (!in_quotes && c == d) ++n;
    }
    return n;
  };
  const int n_comma = count_delim(',');
  const int n_tab = count_delim('\t');
  const int n_semi = count_delim(';');
  // Prefer the one with the most separators.
  if (n_tab >= n_comma && n_tab >= n_semi && n_tab > 0) return '\t';
  if (n_semi >= n_comma && n_semi > 0) return ';';
  return ',';
}

static std::optional<double> parse_double_opt(const std::string& s) {
  std::string t = qeeg::trim(s);
  if (t.empty()) return std::nullopt;
  // Allow common NA tokens.
  std::string tl = lower_ascii(t);
  if (tl == "na" || tl == "nan" || tl == "null" || tl == "none") return std::nullopt;
  try {
    const double v = qeeg::to_double(t);
    if (!std::isfinite(v)) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

static int find_roi_column(const std::vector<std::string>& headers, const std::string& roi_column_arg) {
  auto norm = [](std::string s) -> std::string {
    s = qeeg::strip_utf8_bom(std::move(s));
    s = qeeg::trim(s);
    return lower_ascii(std::move(s));
  };

  const std::string want = roi_column_arg.empty() ? std::string() : norm(roi_column_arg);
  if (!want.empty()) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (norm(headers[i]) == want) return static_cast<int>(i);
    }
    throw std::runtime_error("ROI column not found: '" + roi_column_arg + "'");
  }

  // Heuristics.
  const std::vector<std::string> candidates = {
      "roi",
      "region",
      "label",
      "ba",
      "brodmann",
      "source",
      "target",
  };
  for (const auto& c : candidates) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (norm(headers[i]) == c) return static_cast<int>(i);
    }
  }
  // Default to first column.
  return headers.empty() ? -1 : 0;
}

static Table read_roi_table(const std::string& path, const Args& args) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open input: " + path);

  std::string header_line;
  while (std::getline(f, header_line)) {
    if (!is_comment_or_empty(header_line)) break;
  }
  header_line = qeeg::strip_utf8_bom(std::move(header_line));
  if (qeeg::trim(header_line).empty()) throw std::runtime_error("Empty input file: " + path);

  const char delim = detect_delim(header_line);
  std::vector<std::string> headers = qeeg::split_csv_row(header_line, delim);
  for (auto& h : headers) {
    h = qeeg::trim(qeeg::strip_utf8_bom(std::move(h)));
  }

  const int roi_idx = find_roi_column(headers, args.roi_column);
  if (roi_idx < 0 || roi_idx >= static_cast<int>(headers.size())) {
    throw std::runtime_error("Failed to determine ROI column");
  }

  // Read all data rows.
  std::vector<std::vector<std::string>> raw_rows;
  std::string line;
  while (std::getline(f, line)) {
    if (is_comment_or_empty(line)) continue;
    auto cols = qeeg::split_csv_row(line, delim);
    if (cols.size() < headers.size()) cols.resize(headers.size());
    raw_rows.push_back(std::move(cols));
  }
  if (raw_rows.empty()) {
    throw std::runtime_error("No data rows found in: " + path);
  }

  auto norm_header = [](std::string s) -> std::string {
    s = qeeg::strip_utf8_bom(std::move(s));
    s = qeeg::trim(s);
    return lower_ascii(std::move(s));
  };

  auto find_col_by_names = [&](const std::string& forced, const std::vector<std::string>& names) -> int {
    if (!forced.empty()) {
      const std::string want = norm_header(forced);
      for (size_t i = 0; i < headers.size(); ++i) {
        if (norm_header(headers[i]) == want) return static_cast<int>(i);
      }
      throw std::runtime_error("Column not found: '" + forced + "'");
    }
    for (const auto& n : names) {
      for (size_t i = 0; i < headers.size(); ++i) {
        if (norm_header(headers[i]) == n) return static_cast<int>(i);
      }
    }
    return -1;
  };

  // Scan which columns appear numeric.
  std::vector<int> numeric_cols;
  numeric_cols.reserve(headers.size());
  for (int ci = 0; ci < static_cast<int>(headers.size()); ++ci) {
    if (ci == roi_idx) continue;
    bool any_numeric = false;
    for (const auto& r : raw_rows) {
      if (ci >= static_cast<int>(r.size())) continue;
      const auto v = parse_double_opt(r[static_cast<size_t>(ci)]);
      if (v.has_value()) {
        any_numeric = true;
        break;
      }
    }
    if (any_numeric) numeric_cols.push_back(ci);
  }

  const int metric_idx = find_col_by_names(args.metric_column, {"metric", "measure", "parameter", "var", "variable"});
  const int band_idx = find_col_by_names(args.band_column, {"band", "freq", "frequency"});
  const int value_idx = find_col_by_names(args.value_column, {"value", "val", "score", "z", "zscore", "z-score"});

  const bool value_is_numeric = (value_idx >= 0) && (std::find(numeric_cols.begin(), numeric_cols.end(), value_idx) != numeric_cols.end());
  const bool maybe_long_form = (metric_idx >= 0) && (value_idx >= 0) && value_is_numeric &&
                               (metric_idx != roi_idx) && (value_idx != roi_idx) && (metric_idx != value_idx) &&
                               (numeric_cols.size() <= 3);

  // Long-form mode: rows like ROI, metric, (band), value.
  if (maybe_long_form) {
    const std::string fmt = lower_ascii(qeeg::trim(args.metric_name_format));
    const bool band_metric = (fmt == "band_metric");

    std::unordered_map<std::string, size_t> roi_to_i;
    std::unordered_map<std::string, size_t> metric_to_j;
    std::vector<std::string> rois;
    std::vector<std::string> metrics;

    struct Cell {
      size_t i;
      size_t j;
      double v;
    };
    std::vector<Cell> cells;
    cells.reserve(raw_rows.size());

    size_t n_dupe = 0;

    auto norm_band = [](std::string s) -> std::string {
      s = qeeg::trim(std::move(s));
      // Preserve user-visible text, but normalize common separators.
      for (char& c : s) {
        if (c == ' ' || c == '/' || c == '\\' || c == '-') c = '_';
      }
      return s;
    };

    for (const auto& r : raw_rows) {
      std::string roi = qeeg::trim(r[static_cast<size_t>(roi_idx)]);
      if (roi.empty()) roi = "(missing)";

      std::string metric = qeeg::trim(r[static_cast<size_t>(metric_idx)]);
      if (metric.empty()) metric = "metric";

      std::string band;
      if (band_idx >= 0 && band_idx < static_cast<int>(r.size())) {
        band = norm_band(r[static_cast<size_t>(band_idx)]);
      }

      std::string key = metric;
      if (!qeeg::trim(band).empty()) {
        if (band_metric) key = band + "_" + metric;
        else key = metric + "_" + band;
      }

      const auto vopt = parse_double_opt(r[static_cast<size_t>(value_idx)]);
      if (!vopt.has_value()) continue;
      const double v = *vopt;

      size_t i;
      auto it_roi = roi_to_i.find(roi);
      if (it_roi == roi_to_i.end()) {
        i = rois.size();
        roi_to_i[roi] = i;
        rois.push_back(roi);
      } else {
        i = it_roi->second;
      }

      size_t j;
      auto it_m = metric_to_j.find(key);
      if (it_m == metric_to_j.end()) {
        j = metrics.size();
        metric_to_j[key] = j;
        metrics.push_back(key);
      } else {
        j = it_m->second;
      }

      cells.push_back({i, j, v});
    }

    if (metrics.empty()) {
      throw std::runtime_error("No numeric values found in long-form table (check --value-column)");
    }

    // Apply include/exclude filters to derived metric keys.
    std::unordered_set<std::string> include_set;
    for (const auto& m : args.include_metrics) include_set.insert(m);
    std::unordered_set<std::string> exclude_set;
    for (const auto& m : args.exclude_metrics) exclude_set.insert(m);

    std::vector<size_t> keep_j;
    keep_j.reserve(metrics.size());
    for (size_t j = 0; j < metrics.size(); ++j) {
      const std::string& mname = metrics[j];
      if (!include_set.empty() && include_set.find(mname) == include_set.end()) continue;
      if (exclude_set.find(mname) != exclude_set.end()) continue;
      keep_j.push_back(j);
    }

    if (keep_j.empty()) {
      throw std::runtime_error("All metrics filtered out (check --metrics / --exclude-metrics)");
    }

    // Remap old metric indices to compact ones.
    std::vector<int> old_to_new(metrics.size(), -1);
    std::vector<std::string> metrics_f;
    metrics_f.reserve(keep_j.size());
    for (size_t newj = 0; newj < keep_j.size(); ++newj) {
      const size_t oldj = keep_j[newj];
      old_to_new[oldj] = static_cast<int>(newj);
      metrics_f.push_back(metrics[oldj]);
    }

    Table t;
    t.roi_col = headers[static_cast<size_t>(roi_idx)];
    t.metrics = std::move(metrics_f);
    t.rois = std::move(rois);
    t.values.assign(t.rois.size(), std::vector<double>(t.metrics.size(), std::numeric_limits<double>::quiet_NaN()));

    // Fill cells.
    for (const auto& c : cells) {
      if (c.i >= t.values.size()) continue;
      if (c.j >= old_to_new.size()) continue;
      const int newj = old_to_new[c.j];
      if (newj < 0) continue;
      const size_t jj = static_cast<size_t>(newj);
      const double prev = t.values[c.i][jj];
      if (std::isfinite(prev)) ++n_dupe;
      t.values[c.i][jj] = c.v;  // last-wins
    }

    if (n_dupe > 0) {
      std::cout << "Note: " << n_dupe << " duplicate ROI+metric cells encountered in long-form input (last value kept).\n";
    }

    return t;
  }

  // Wide mode (default): treat each numeric column as a metric.
  std::vector<int> metric_col_idxs;
  std::vector<std::string> metric_names;
  metric_col_idxs.reserve(headers.size());

  std::unordered_set<std::string> include_set;
  for (const auto& m : args.include_metrics) include_set.insert(m);
  std::unordered_set<std::string> exclude_set;
  for (const auto& m : args.exclude_metrics) exclude_set.insert(m);

  for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
    if (i == roi_idx) continue;
    const std::string& col = headers[static_cast<size_t>(i)];
    if (col.empty()) continue;
    if (!include_set.empty() && include_set.find(col) == include_set.end()) continue;
    if (exclude_set.find(col) != exclude_set.end()) continue;

    bool any_numeric = false;
    for (const auto& r : raw_rows) {
      if (i >= static_cast<int>(r.size())) continue;
      const auto v = parse_double_opt(r[static_cast<size_t>(i)]);
      if (v.has_value()) {
        any_numeric = true;
        break;
      }
    }
    if (!any_numeric) continue;
    metric_col_idxs.push_back(i);
    metric_names.push_back(col);
  }

  if (metric_col_idxs.empty()) {
    throw std::runtime_error("No numeric metric columns detected (consider --roi-column / --metrics)");
  }

  Table t;
  t.roi_col = headers[static_cast<size_t>(roi_idx)];
  t.metrics = metric_names;
  t.rois.reserve(raw_rows.size());
  t.values.reserve(raw_rows.size());

  for (const auto& r : raw_rows) {
    std::string roi = qeeg::trim(r[static_cast<size_t>(roi_idx)]);
    if (roi.empty()) roi = "(missing)";
    t.rois.push_back(roi);

    std::vector<double> row;
    row.reserve(metric_col_idxs.size());
    for (int ci : metric_col_idxs) {
      std::optional<double> v;
      if (ci < static_cast<int>(r.size())) v = parse_double_opt(r[static_cast<size_t>(ci)]);
      row.push_back(v.has_value() ? *v : std::numeric_limits<double>::quiet_NaN());
    }
    t.values.push_back(std::move(row));
  }

  return t;
}

static std::string posix_slashes(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
  }
  return p;
}

static std::string safe_relpath_posix(const std::string& target, const std::string& base_dir) {
  fs::path t = fs::weakly_canonical(fs::path(target));
  fs::path b = fs::weakly_canonical(fs::path(base_dir));
  std::error_code ec;
  fs::path rel = fs::relative(t, b, ec);
  if (ec) {
    // If relpath fails, fall back to basename.
    rel = fs::path(target).filename();
  }
  std::string s = posix_slashes(rel.generic_string());
  // Basic safety: no parent traversal.
  if (s.find("..") != std::string::npos) {
    s = fs::path(target).filename().string();
  }
  return s;
}

static std::string json_escape(const std::string& s) {
  std::ostringstream o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (c < 0x20) {
          o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        } else {
          o << char(c);
        }
        break;
    }
  }
  return o.str();
}

static std::string json_number_or_null(double v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream o;
  o << std::setprecision(17) << v;
  return o.str();
}

static void write_csv_wide(const std::string& path, const Table& t) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to write: " + path);
  f << t.roi_col;
  for (const auto& m : t.metrics) f << ',' << qeeg::csv_escape(m);
  f << "\n";

  for (size_t i = 0; i < t.rois.size(); ++i) {
    f << qeeg::csv_escape(t.rois[i]);
    for (size_t j = 0; j < t.metrics.size(); ++j) {
      f << ',';
      const double v = t.values[i][j];
      if (std::isfinite(v)) {
        f << std::setprecision(12) << v;
      }
    }
    f << "\n";
  }
}

static void write_csv_long(const std::string& path, const Table& t) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to write: " + path);
  f << "roi,metric,value\n";
  for (size_t i = 0; i < t.rois.size(); ++i) {
    for (size_t j = 0; j < t.metrics.size(); ++j) {
      f << qeeg::csv_escape(t.rois[i]) << ',' << qeeg::csv_escape(t.metrics[j]) << ',';
      const double v = t.values[i][j];
      if (std::isfinite(v)) f << std::setprecision(12) << v;
      f << "\n";
    }
  }
}

static void write_html_report(
    const std::string& path,
    const Args& args,
    const Table& t,
    const std::string& input_path,
    const std::string& csv_wide_rel,
    const std::string& csv_long_rel,
    const std::optional<std::string>& json_index_rel,
    const std::optional<std::string>& protocol_rel,
    const std::vector<ProtocolTarget>& proto_targets) {
  std::ostringstream h;
  h << "<!doctype html>\n"
       "<html><head><meta charset=\"utf-8\">\n"
       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
       "<title>LORETA ROI metrics</title>\n"
       "<style>\n"
       ":root{--bg:#0b0d10;--fg:#e7e7e7;--muted:#9aa4ad;--panel:#12161b;--border:#25303a;--accent:#6cb4ff;}\n"
       "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg);}\n"
       "header{padding:16px 18px;border-bottom:1px solid var(--border);background:linear-gradient(180deg,#0b0d10,#0a0c0f);}\n"
       "h1{margin:0;font-size:18px;}\n"
       ".meta{margin-top:6px;color:var(--muted);font-size:12px;}\n"
       ".wrap{padding:14px 18px;}\n"
       ".card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:12px 12px;margin:12px 0;}\n"
       "a{color:var(--accent);}\n"
       "table{border-collapse:collapse;width:100%;font-size:12px;}\n"
       "th,td{border-bottom:1px solid var(--border);padding:6px 8px;text-align:right;white-space:nowrap;}\n"
       "th:first-child,td:first-child{text-align:left;}\n"
       "th{position:sticky;top:0;background:var(--panel);}\n"
       "input{padding:6px 8px;border-radius:8px;border:1px solid var(--border);background:#0b0d10;color:var(--fg);width:100%;max-width:520px;}\n"
       ".small{font-size:12px;color:var(--muted);}\n"
       "</style>\n"
       "<script>\n"
       "function filterTable(){\n"
       "  const q = document.getElementById('filter').value.toLowerCase();\n"
       "  const rows = document.querySelectorAll('tbody tr');\n"
       "  for(const r of rows){\n"
       "    const text = r.innerText.toLowerCase();\n"
       "    r.style.display = (text.indexOf(q) >= 0) ? '' : 'none';\n"
       "  }\n"
       "}\n"
       "</script>\n"
       "</head><body>\n";

  h << "<header><h1>LORETA ROI metrics</h1>\n";
  h << "<div class=\"meta\">Atlas: <b>" << qeeg::svg_escape(args.atlas) << "</b> · "
    << "ROIs: <b>" << t.rois.size() << "</b> · Metrics: <b>" << t.metrics.size() << "</b></div>\n";
  h << "</header>\n";

  h << "<div class=\"wrap\">\n";
  h << "<div class=\"card\">\n";
  h << "<div class=\"small\">Input: " << qeeg::svg_escape(input_path) << "</div>\n";
  h << "<div style=\"margin-top:10px;display:flex;gap:10px;flex-wrap:wrap;align-items:center\">\n";
  h << "<a href=\"" << qeeg::url_escape(csv_wide_rel) << "\" download>Download CSV (wide)</a>\n";
  h << "<a href=\"" << qeeg::url_escape(csv_long_rel) << "\" download>Download CSV (long)</a>\n";
  if (json_index_rel.has_value()) {
    h << "<a href=\"" << qeeg::url_escape(*json_index_rel) << "\" download>Download JSON index</a>\n";
  }
  if (protocol_rel.has_value()) {
    h << "<a href=\"" << qeeg::url_escape(*protocol_rel) << "\" download>Download protocol JSON</a>\n";
  }
  h << "</div>\n";
  h << "<div style=\"margin-top:12px\"><input id=\"filter\" oninput=\"filterTable()\" placeholder=\"Filter...\"></div>\n";
  h << "</div>\n";

  if (!proto_targets.empty()) {
    h << "<div class=\"card\">\n";
    h << "<div style=\"font-weight:700;margin-bottom:6px\">Protocol candidates (heuristic)</div>\n";
    h << "<div class=\"small\">This is a ranked list of ROI x metric values (sorted by |value|). "
         "For z-score-like metrics, suggested_direction indicates movement toward 0.</div>\n";
    h << "<table>\n<thead><tr>";
    h << "<th>#</th><th style=\"text-align:left\">ROI</th><th style=\"text-align:left\">Metric</th><th>Value</th><th>|Value|</th><th style=\"text-align:left\">Kind</th><th style=\"text-align:left\">Direction</th>";
    h << "</tr></thead>\n<tbody>\n";
    for (size_t i = 0; i < proto_targets.size(); ++i) {
      const auto& pt = proto_targets[i];
      h << "<tr>";
      h << "<td>" << (i + 1) << "</td>";
      h << "<td style=\"text-align:left\">" << qeeg::svg_escape(pt.roi) << "</td>";
      h << "<td style=\"text-align:left\">" << qeeg::svg_escape(pt.metric) << "</td>";
      h << "<td>" << std::setprecision(10) << pt.value << "</td>";
      h << "<td>" << std::setprecision(10) << pt.abs_value << "</td>";
      std::string kind = pt.metric_kind + "/" + pt.value_kind;
      if (pt.band.has_value()) kind += " (" + *pt.band + ")";
      h << "<td style=\"text-align:left\">" << qeeg::svg_escape(kind) << "</td>";
      if (pt.suggested_direction.has_value()) {
        h << "<td style=\"text-align:left\">" << qeeg::svg_escape(*pt.suggested_direction) << "</td>";
      } else {
        h << "<td style=\"text-align:left\"></td>";
      }
      h << "</tr>\n";
    }
    h << "</tbody></table>\n";
    h << "</div>\n";
  }

  h << "<div class=\"card\">\n";
  h << "<table>\n<thead><tr>";
  h << "<th>" << qeeg::svg_escape(t.roi_col) << "</th>";
  for (const auto& m : t.metrics) {
    h << "<th>" << qeeg::svg_escape(m) << "</th>";
  }
  h << "</tr></thead>\n<tbody>\n";
  for (size_t i = 0; i < t.rois.size(); ++i) {
    h << "<tr><td>" << qeeg::svg_escape(t.rois[i]) << "</td>";
    for (size_t j = 0; j < t.metrics.size(); ++j) {
      const double v = t.values[i][j];
      if (std::isfinite(v)) {
        h << "<td>" << std::fixed << std::setprecision(6) << v << "</td>";
      } else {
        h << "<td></td>";
      }
    }
    h << "</tr>\n";
  }
  h << "</tbody></table>\n";
  h << "</div>\n";
  h << "</div></body></html>\n";

  qeeg::write_text_file(path, h.str());
}


static void write_protocol_json(
    const std::string& protocol_path,
    const Args& args,
    const Table& t,
    const std::string& input_path,
    const std::string& outdir,
    const std::optional<std::string>& index_path_opt,
    const std::vector<ProtocolTarget>& targets) {
  const std::string schema_url =
      "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/qeeg_loreta_protocol.schema.json";

  const fs::path proto_abs = fs::absolute(fs::u8path(protocol_path));
  const fs::path proto_dir = proto_abs.parent_path();
  const fs::path outdir_abs = fs::absolute(fs::u8path(outdir));

  const std::string outdir_rel = safe_relpath_posix(outdir_abs.string(), proto_dir.string());

  std::optional<std::string> index_rel;
  if (index_path_opt.has_value() && !index_path_opt->empty()) {
    const fs::path idx_abs = fs::absolute(fs::u8path(*index_path_opt));
    index_rel = safe_relpath_posix(idx_abs.string(), proto_dir.string());
  }

  std::ostringstream o;
  o << "{\n";
  o << "  \"$schema\": \"" << schema_url << "\",\n";
  o << "  \"schema_version\": 1,\n";
  o << "  \"generated_utc\": \"" << json_escape(qeeg::now_string_utc()) << "\",\n";
  o << "  \"tool\": \"" << kTool << "\",\n";
  o << "  \"input_path\": \"" << json_escape(posix_slashes(input_path)) << "\",\n";
  o << "  \"outdir\": \"" << json_escape(outdir_rel) << "\",\n";
  if (index_rel.has_value()) {
    o << "  \"metrics_index_json\": \"" << json_escape(*index_rel) << "\",\n";
  } else {
    o << "  \"metrics_index_json\": null,\n";
  }
  o << "  \"atlas\": { \"name\": \"" << json_escape(args.atlas) << "\" },\n";
  o << "  \"roi_column\": \"" << json_escape(t.roi_col) << "\",\n";
  o << "  \"params\": {\n";
  o << "    \"top_n\": " << args.protocol_top << ",\n";
  o << "    \"only_z\": " << (args.protocol_only_z ? "true" : "false") << ",\n";
  o << "    \"threshold_abs\": " << std::setprecision(17) << args.protocol_threshold << "\n";
  o << "  },\n";
  o << "  \"targets\": [\n";
  for (size_t i = 0; i < targets.size(); ++i) {
    const ProtocolTarget& t0 = targets[i];
    o << "    {\n";
    o << "      \"rank\": " << (i + 1) << ",\n";
    o << "      \"roi\": \"" << json_escape(t0.roi) << "\",\n";
    o << "      \"metric\": \"" << json_escape(t0.metric) << "\",\n";
    o << "      \"metric_kind\": \"" << json_escape(t0.metric_kind) << "\",\n";
    o << "      \"value_kind\": \"" << json_escape(t0.value_kind) << "\",\n";
    if (t0.band.has_value()) {
      o << "      \"band\": \"" << json_escape(*t0.band) << "\",\n";
    } else {
      o << "      \"band\": null,\n";
    }
    o << "      \"value\": " << json_number_or_null(t0.value) << ",\n";
    o << "      \"abs_value\": " << json_number_or_null(t0.abs_value) << ",\n";
    if (t0.suggested_direction.has_value()) {
      o << "      \"suggested_direction\": \"" << json_escape(*t0.suggested_direction) << "\"\n";
    } else {
      o << "      \"suggested_direction\": null\n";
    }
    o << "    }";
    if (i + 1 != targets.size()) o << ",";
    o << "\n";
  }
  o << "  ]\n";
  o << "}\n";

  qeeg::write_text_file(protocol_path, o.str());
}

static void write_index_json(
    const std::string& index_path,
    const Args& args,
    const Table& t,
    const std::string& input_path,
    const std::string& outdir,
    const std::string& run_meta_name,
    const std::string& csv_wide_name,
    const std::string& csv_long_name,
    const std::optional<std::string>& report_name,
    const std::optional<std::string>& protocol_path_opt) {
  const std::string schema_url =
      "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/qeeg_loreta_metrics_index.schema.json";

  const fs::path idx_abs = fs::absolute(fs::u8path(index_path));
  const fs::path idx_dir_abs = idx_abs.parent_path();
  const fs::path outdir_abs = fs::absolute(fs::u8path(outdir));

  const std::string outdir_rel = safe_relpath_posix(outdir_abs.string(), idx_dir_abs.string());
  const std::string run_meta_rel =
      safe_relpath_posix((outdir_abs / fs::u8path(run_meta_name)).string(), idx_dir_abs.string());
  const std::string csv_wide_rel =
      safe_relpath_posix((outdir_abs / fs::u8path(csv_wide_name)).string(), idx_dir_abs.string());
  const std::string csv_long_rel =
      safe_relpath_posix((outdir_abs / fs::u8path(csv_long_name)).string(), idx_dir_abs.string());
  std::optional<std::string> report_rel;
  if (report_name.has_value()) {
    report_rel = safe_relpath_posix((outdir_abs / fs::u8path(*report_name)).string(), idx_dir_abs.string());
  }

  std::optional<std::string> protocol_rel;
  if (protocol_path_opt.has_value() && !protocol_path_opt->empty()) {
    const fs::path proto_abs = fs::absolute(fs::u8path(*protocol_path_opt));
    protocol_rel = safe_relpath_posix(proto_abs.string(), idx_dir_abs.string());
  }

  std::ostringstream o;
  o << "{\n";
  o << "  \"$schema\": \"" << schema_url << "\",\n";
  o << "  \"schema_version\": 1,\n";
  o << "  \"generated_utc\": \"" << json_escape(qeeg::now_string_utc()) << "\",\n";
  o << "  \"tool\": \"" << kTool << "\",\n";
  o << "  \"input_path\": \"" << json_escape(posix_slashes(input_path)) << "\",\n";
  o << "  \"outdir\": \"" << json_escape(outdir_rel) << "\",\n";
  o << "  \"run_meta_json\": \"" << json_escape(run_meta_rel) << "\",\n";
  if (report_rel.has_value()) {
    o << "  \"report_html\": \"" << json_escape(*report_rel) << "\",\n";
  } else {
    o << "  \"report_html\": null,\n";
  }
  if (protocol_rel.has_value()) {
    o << "  \"protocol_json\": \"" << json_escape(*protocol_rel) << "\",\n";
  } else {
    o << "  \"protocol_json\": null,\n";
  }
  o << "  \"atlas\": { \"name\": \"" << json_escape(args.atlas) << "\" },\n";
  o << "  \"roi_column\": \"" << json_escape(t.roi_col) << "\",\n";
  o << "  \"csv_wide\": \"" << json_escape(csv_wide_rel) << "\",\n";
  o << "  \"csv_long\": \"" << json_escape(csv_long_rel) << "\",\n";

  o << "  \"metrics\": [";
  for (size_t i = 0; i < t.metrics.size(); ++i) {
    if (i) o << ", ";
    o << "\"" << json_escape(t.metrics[i]) << "\"";
  }
  o << "],\n";

  o << "  \"rois\": [\n";
  for (size_t i = 0; i < t.rois.size(); ++i) {
    o << "    { \"roi\": \"" << json_escape(t.rois[i]) << "\", \"values\": [";
    for (size_t j = 0; j < t.metrics.size(); ++j) {
      if (j) o << ", ";
      o << json_number_or_null(t.values[i][j]);
    }
    o << "] }";
    if (i + 1 != t.rois.size()) o << ",";
    o << "\n";
  }
  o << "  ]\n";
  o << "}\n";

  qeeg::write_text_file(index_path, o.str());
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto need = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + flag);
      return std::string(argv[++i]);
    };

    if (s == "--help" || s == "-h") {
      print_help(argv[0]);
      std::exit(0);
    } else if (s == "--version") {
      std::cout << kTool << " " << qeeg::version_string() << "\n";
      std::exit(0);
    } else if (s == "--input") {
      a.input = need("--input");
    } else if (s == "--outdir") {
      a.outdir = need("--outdir");
    } else if (s == "--atlas") {
      a.atlas = need("--atlas");
    } else if (s == "--roi-column") {
      a.roi_column = need("--roi-column");
    } else if (s == "--metric-column") {
      a.metric_column = need("--metric-column");
    } else if (s == "--band-column") {
      a.band_column = need("--band-column");
    } else if (s == "--value-column") {
      a.value_column = need("--value-column");
    } else if (s == "--metric-name-format") {
      a.metric_name_format = need("--metric-name-format");
    } else if (s == "--metrics") {
      a.include_metrics = split_list(need("--metrics"));
    } else if (s == "--exclude-metrics") {
      a.exclude_metrics = split_list(need("--exclude-metrics"));
    } else if (s == "--csv-wide") {
      a.csv_wide_name = need("--csv-wide");
    } else if (s == "--csv-long") {
      a.csv_long_name = need("--csv-long");
    } else if (s == "--html-report") {
      a.html_report = true;
    } else if (s == "--json-index") {
      a.json_index = true;
    } else if (s == "--json-index-path") {
      a.json_index = true;
      a.json_index_path = need("--json-index-path");
    } else if (s == "--protocol-json") {
      a.protocol_json = true;
    } else if (s == "--protocol-path") {
      a.protocol_json = true;
      a.protocol_path = need("--protocol-path");
    } else if (s == "--protocol-top") {
      a.protocol_json = true;
      try {
        a.protocol_top = std::stoi(need("--protocol-top"));
      } catch (...) {
        throw std::runtime_error("Invalid integer for --protocol-top");
      }
      if (a.protocol_top < 0) a.protocol_top = 0;
    } else if (s == "--protocol-only-z") {
      a.protocol_json = true;
      a.protocol_only_z = true;
    } else if (s == "--protocol-threshold") {
      a.protocol_json = true;
      try {
        a.protocol_threshold = std::stod(need("--protocol-threshold"));
      } catch (...) {
        throw std::runtime_error("Invalid number for --protocol-threshold");
      }
      if (a.protocol_threshold < 0) a.protocol_threshold = 0.0;
    } else {
      throw std::runtime_error("Unknown arg: " + s);
    }
  }
  if (a.input.empty()) throw std::runtime_error("--input is required");
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);

    // Allow chaining: --input can be a CSV/TSV, a *_run_meta.json file, or an output directory.
    std::string input_path;
    {
      qeeg::ResolveInputTableOptions opt;
      opt.preferred_filenames = {
          "loreta_metrics.csv",
          "roi_metrics.csv",
          "roi_table.csv",
          "loreta.csv",
      };
      const qeeg::ResolvedInputPath rp = qeeg::resolve_input_table_path(args.input, opt);
      if (!rp.note.empty()) std::cout << rp.note << "\n";
      input_path = rp.path;
    }

    qeeg::ensure_directory(args.outdir);

    Table t = read_roi_table(input_path, args);

    const std::string csv_wide_path = (fs::path(args.outdir) / args.csv_wide_name).string();
    const std::string csv_long_path = (fs::path(args.outdir) / args.csv_long_name).string();
    const std::string report_name = "loreta_metrics_report.html";
    const std::string report_path = (fs::path(args.outdir) / report_name).string();
    const std::string run_meta_name = "loreta_metrics_run_meta.json";
    const std::string run_meta_path = (fs::path(args.outdir) / run_meta_name).string();

    const std::string index_default_name = "loreta_metrics_index.json";
    const std::string index_path =
        args.json_index_path.empty() ? (fs::path(args.outdir) / index_default_name).string() : args.json_index_path;
    const std::string index_name = fs::path(index_path).filename().string();

    
    // Optional protocol extraction.
    const std::string protocol_default_name = "loreta_protocol.json";
    std::optional<std::string> protocol_path_opt;
    std::optional<std::string> protocol_rel;
    std::vector<ProtocolTarget> protocol_targets;
    std::string protocol_name;
    if (args.protocol_json) {
      const std::string protocol_path =
          args.protocol_path.empty() ? (fs::path(args.outdir) / protocol_default_name).string() : args.protocol_path;
      protocol_path_opt = protocol_path;
      protocol_name = fs::path(protocol_path).filename().string();
      protocol_rel = safe_relpath_posix(protocol_path, args.outdir);
      protocol_targets = compute_protocol_targets(t, args);
    }
    write_csv_wide(csv_wide_path, t);
    write_csv_long(csv_long_path, t);

    std::optional<std::string> report_rel;
    if (args.html_report) {
      const std::string csv_wide_rel = safe_relpath_posix(csv_wide_path, args.outdir);
      const std::string csv_long_rel = safe_relpath_posix(csv_long_path, args.outdir);
      std::optional<std::string> index_rel;
      if (args.json_index) {
        index_rel = safe_relpath_posix(index_path, args.outdir);
      }
      write_html_report(report_path, args, t, input_path, csv_wide_rel, csv_long_rel, index_rel, protocol_rel, protocol_targets);
      report_rel = report_name;
    }

    if (args.protocol_json) {
      const std::string protocol_path = *protocol_path_opt;
      write_protocol_json(protocol_path, args, t, input_path, args.outdir,
                          args.json_index ? std::optional<std::string>(index_path) : std::optional<std::string>(),
                          protocol_targets);
    }

    if (args.json_index) {
      write_index_json(index_path, args, t, input_path, args.outdir, run_meta_name, args.csv_wide_name, args.csv_long_name,
                       report_rel, protocol_path_opt);
    }

    std::vector<std::string> outputs;
    outputs.push_back(args.csv_wide_name);
    outputs.push_back(args.csv_long_name);
    if (args.html_report) outputs.push_back(report_name);
    if (args.json_index) outputs.push_back(index_name);
    if (args.protocol_json) outputs.push_back(protocol_name);
    outputs.push_back(run_meta_name);

    qeeg::write_run_meta_json(run_meta_path, kTool, input_path, args.outdir, outputs);

    std::cout << "Wrote " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 2;
  }
}
