#include "qeeg/cli_input.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/run_meta.hpp"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTool = "qeeg_loreta_connectivity_cli";

struct Args {
  std::string input;
  std::string outdir = "out_loreta_connectivity";
  std::string atlas = "unknown";

  // Mode: auto (default) chooses between edge-list and matrix parsing.
  //   - edges: expects columns for roi_a/roi_b and a numeric value (optionally band/metric)
  //   - matrix: expects a square matrix CSV with labels in first row/col
  std::string mode = "auto";  // auto | edges | matrix

  // Edge-list parsing.
  std::string roi_a_column;   // auto-detect if empty
  std::string roi_b_column;   // auto-detect if empty
  std::string band_column;    // auto-detect if empty
  std::string metric_column;  // auto-detect if empty
  std::string value_column;   // auto-detect if empty

  // Output naming.
  std::string measure_id;  // optional override; otherwise derived from metric/value/file

  bool directed = false;  // if true, don't mirror edges into symmetric matrix

  // Outputs.
  bool json_index = false;
  std::string json_index_path;  // default: <outdir>/loreta_connectivity_index.json

  // Protocol candidate extraction (heuristic; non-clinical)
  bool protocol_json = false;
  std::string protocol_path;  // default: <outdir>/loreta_connectivity_protocol.json
  int protocol_top = 50;
  bool protocol_only_z = false;
  double protocol_threshold = 0.0;
};

static std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static bool is_comment_or_empty(const std::string& line) {
  const std::string t = qeeg::trim(line);
  return t.empty() || (!t.empty() && t[0] == '#');
}

static char detect_delim(const std::string& header_line) {
  // Prefer comma unless tabs dominate.
  size_t commas = 0;
  size_t tabs = 0;
  size_t semis = 0;
  bool in_quotes = false;
  for (char c : header_line) {
    if (c == '"') in_quotes = !in_quotes;
    if (in_quotes) continue;
    if (c == ',') ++commas;
    else if (c == '\t') ++tabs;
    else if (c == ';') ++semis;
  }
  if (tabs > commas && tabs >= semis) return '\t';
  if (semis > commas && semis > tabs) return ';';
  return ',';
}

static std::optional<double> parse_double_opt(const std::string& s) {
  const std::string t = qeeg::trim(s);
  if (t.empty()) return std::nullopt;
  try {
    const double v = qeeg::to_double(t);
    if (!std::isfinite(v)) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

static std::string safe_id(std::string s) {
  // Filename/id-friendly: lowercase, alnum -> keep, others -> '_'.
  s = qeeg::strip_utf8_bom(std::move(s));
  s = qeeg::trim(std::move(s));
  s = lower_ascii(std::move(s));
  std::string out;
  out.reserve(s.size());
  char prev = 0;
  for (unsigned char uc : s) {
    char c = static_cast<char>(uc);
    const bool ok = std::isalnum(uc);
    char w = ok ? c : '_';
    if (w == '_' && prev == '_') continue;
    out.push_back(w);
    prev = w;
  }
  while (!out.empty() && out.front() == '_') out.erase(out.begin());
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.empty()) out = "id";
  return out;
}

static bool is_z_metric_name(const std::string& metric_name) {
  const std::string s = lower_ascii(metric_name);
  if (s.find("zscore") != std::string::npos) return true;
  if (s.find("z-score") != std::string::npos) return true;
  if (s.find("z_score") != std::string::npos) return true;
  if (s.size() >= 2 && s.substr(s.size() - 2) == "_z") return true;
  if (s.find("_z_") != std::string::npos) return true;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != 'z') continue;
    const bool left_ok = (i == 0) || !std::isalnum(static_cast<unsigned char>(s[i - 1]));
    const bool right_ok = (i + 1 >= s.size()) || !std::isalnum(static_cast<unsigned char>(s[i + 1]));
    if (left_ok && right_ok) return true;
  }
  return false;
}

static std::optional<std::string> detect_band(const std::string& s0) {
  const std::string s = lower_ascii(s0);
  struct Band {
    const char* needle;
    const char* band;
  };
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
    rel = fs::path(target).filename();
  }
  std::string s = posix_slashes(rel.generic_string());
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

static void print_help() {
  std::cout
    << "qeeg_loreta_connectivity_cli\n\n"
    << "Parse ROI-to-ROI connectivity exports (e.g., eLORETA/sLORETA outputs) into standard connectivity matrices.\n"
    << "The outputs are compatible with scripts/render_connectivity_report.py and the reports dashboard.\n\n"
    << "Notes:\n"
    << "  - Research/educational inspection only (no clinical inference).\n"
    << "  - Many LORETA connectivity measures are symmetric; by default we mirror values.\n\n"
    << "Usage:\n"
    << "  qeeg_loreta_connectivity_cli --input <csv/tsv> [--outdir DIR] [options]\n\n"
    << "Input options:\n"
    << "  --mode MODE              auto | edges | matrix (default: auto)\n"
    << "  --roi-a-column NAME      Edge-list: column name for ROI A (auto if omitted)\n"
    << "  --roi-b-column NAME      Edge-list: column name for ROI B (auto if omitted)\n"
    << "  --band-column NAME       Edge-list: band/frequency column name (auto if omitted)\n"
    << "  --metric-column NAME     Edge-list: metric/measure column name (auto if omitted)\n"
    << "  --value-column NAME      Edge-list: value column name (auto if omitted)\n"
    << "  --measure-id ID          Override output measure id (used in filenames)\n"
    << "  --directed               Do not mirror edges into a symmetric matrix\n\n"
    << "Outputs:\n"
    << "  --json-index             Write loreta_connectivity_index.json\n"
    << "  --json-index-path PATH   Override index JSON path (default: <outdir>/loreta_connectivity_index.json)\n"
    << "  --protocol-json          Write loreta_connectivity_protocol.json (ranked edges by |value|)\n"
    << "  --protocol-path PATH     Override protocol JSON path\n"
    << "  --protocol-top N         Max edges to include (default: 50)\n"
    << "  --protocol-only-z        Include only z-score-like measures\n"
    << "  --protocol-threshold X   Only include edges with |value| >= X\n\n"
    << "Other:\n"
    << "  --atlas NAME             Optional atlas label (default: unknown)\n"
    << "  --version                Print version\n"
    << "  --help                   Show help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;

  // Manual parse to keep dependencies minimal.
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto next = [&](const std::string& opt) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("Missing value for " + opt);
      return std::string(argv[++i]);
    };

    if (s == "--help" || s == "-h") {
      print_help();
      std::exit(0);
    } else if (s == "--version") {
      std::cout << qeeg::version_string() << "\n";
      std::exit(0);
    } else if (s == "--input") {
      a.input = next("--input");
    } else if (s == "--outdir") {
      a.outdir = next("--outdir");
    } else if (s == "--atlas") {
      a.atlas = next("--atlas");
    } else if (s == "--mode") {
      a.mode = lower_ascii(next("--mode"));
    } else if (s == "--roi-a-column") {
      a.roi_a_column = next("--roi-a-column");
    } else if (s == "--roi-b-column") {
      a.roi_b_column = next("--roi-b-column");
    } else if (s == "--band-column") {
      a.band_column = next("--band-column");
    } else if (s == "--metric-column") {
      a.metric_column = next("--metric-column");
    } else if (s == "--value-column") {
      a.value_column = next("--value-column");
    } else if (s == "--measure-id") {
      a.measure_id = next("--measure-id");
    } else if (s == "--directed") {
      a.directed = true;
    } else if (s == "--json-index") {
      a.json_index = true;
    } else if (s == "--json-index-path") {
      a.json_index = true;
      a.json_index_path = next("--json-index-path");
    } else if (s == "--protocol-json") {
      a.protocol_json = true;
    } else if (s == "--protocol-path") {
      a.protocol_json = true;
      a.protocol_path = next("--protocol-path");
    } else if (s == "--protocol-top") {
      a.protocol_json = true;
      try {
        a.protocol_top = std::stoi(next("--protocol-top"));
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
        a.protocol_threshold = std::stod(next("--protocol-threshold"));
      } catch (...) {
        throw std::runtime_error("Invalid number for --protocol-threshold");
      }
      if (a.protocol_threshold < 0) a.protocol_threshold = 0.0;
    } else {
      throw std::runtime_error("Unknown arg: " + s);
    }
  }

  if (a.input.empty()) throw std::runtime_error("--input is required");
  if (!(a.mode == "auto" || a.mode == "edges" || a.mode == "matrix")) {
    throw std::runtime_error("--mode must be auto|edges|matrix");
  }
  return a;
}

struct Edge {
  std::string a;
  std::string b;
  double v = 0.0;
};

struct GroupKey {
  std::string measure;
  std::string band;

  bool operator==(const GroupKey& o) const { return measure == o.measure && band == o.band; }
};

struct GroupKeyHash {
  std::size_t operator()(const GroupKey& k) const {
    std::hash<std::string> h;
    return (h(k.measure) * 1315423911u) ^ h(k.band);
  }
};

struct Matrix {
  std::vector<std::string> rois;
  std::vector<std::vector<double>> values;  // NxN
};

static Matrix make_matrix(const std::vector<Edge>& edges, bool directed) {
  std::unordered_set<std::string> roi_set;
  roi_set.reserve(edges.size() * 2 + 8);
  for (const auto& e : edges) {
    roi_set.insert(e.a);
    roi_set.insert(e.b);
  }
  std::vector<std::string> rois(roi_set.begin(), roi_set.end());
  std::sort(rois.begin(), rois.end());

  std::unordered_map<std::string, size_t> idx;
  idx.reserve(rois.size() * 2 + 1);
  for (size_t i = 0; i < rois.size(); ++i) idx[rois[i]] = i;

  const size_t n = rois.size();
  std::vector<std::vector<double>> m(n, std::vector<double>(n, std::numeric_limits<double>::quiet_NaN()));

  for (const auto& e : edges) {
    auto ia = idx.find(e.a);
    auto ib = idx.find(e.b);
    if (ia == idx.end() || ib == idx.end()) continue;
    const size_t i = ia->second;
    const size_t j = ib->second;
    m[i][j] = e.v;
    if (!directed) m[j][i] = e.v;
  }

  Matrix out;
  out.rois = std::move(rois);
  out.values = std::move(m);
  return out;
}

static void write_matrix_csv(const std::string& path, const Matrix& m) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  // Header row: blank, then labels.
  f << ",";
  for (size_t j = 0; j < m.rois.size(); ++j) {
    if (j) f << ',';
    f << qeeg::csv_escape(m.rois[j]);
  }
  f << "\n";

  for (size_t i = 0; i < m.rois.size(); ++i) {
    f << qeeg::csv_escape(m.rois[i]);
    for (size_t j = 0; j < m.rois.size(); ++j) {
      f << ',';
      const double v = m.values[i][j];
      if (std::isfinite(v)) f << std::setprecision(12) << v;
    }
    f << "\n";
  }
}

static std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>> read_table(const std::string& path,
                                                                                           char* out_delim) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open input: " + path);

  std::string header_line;
  while (std::getline(f, header_line)) {
    if (!is_comment_or_empty(header_line)) break;
  }
  header_line = qeeg::strip_utf8_bom(std::move(header_line));
  if (qeeg::trim(header_line).empty()) throw std::runtime_error("Empty input file: " + path);

  const char delim = detect_delim(header_line);
  if (out_delim) *out_delim = const_cast<char&>(delim);

  std::vector<std::string> headers = qeeg::split_csv_row(header_line, delim);
  for (auto& h : headers) {
    h = qeeg::trim(qeeg::strip_utf8_bom(std::move(h)));
  }

  std::vector<std::vector<std::string>> rows;
  std::string line;
  while (std::getline(f, line)) {
    if (is_comment_or_empty(line)) continue;
    auto cols = qeeg::split_csv_row(line, delim);
    if (cols.size() < headers.size()) cols.resize(headers.size());
    rows.push_back(std::move(cols));
  }
  return {headers, rows};
}

static int find_col_ci(const std::vector<std::string>& headers, const std::string& forced,
                       const std::vector<std::string>& names) {
  auto norm = [](std::string s) -> std::string {
    s = qeeg::strip_utf8_bom(std::move(s));
    s = qeeg::trim(std::move(s));
    return lower_ascii(std::move(s));
  };

  if (!forced.empty()) {
    const std::string want = norm(forced);
    for (size_t i = 0; i < headers.size(); ++i) {
      if (norm(headers[i]) == want) return static_cast<int>(i);
    }
    throw std::runtime_error("Column not found: '" + forced + "'");
  }

  for (const auto& n : names) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (norm(headers[i]) == n) return static_cast<int>(i);
    }
  }
  return -1;
}

static bool looks_like_matrix_header(const std::vector<std::string>& headers) {
  if (headers.size() < 3) return false;
  const std::string h0 = qeeg::trim(headers[0]);
  // Common matrix CSV has first cell empty (or a label like "roi").
  if (h0.empty()) return true;
  const std::string h0l = lower_ascii(h0);
  if (h0l == "roi" || h0l == "region" || h0l == "label") return true;
  return false;
}

static std::optional<std::string> parse_matrix_csv(const std::string& path, const char delim, Matrix* out) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open input: " + path);

  std::string header_line;
  while (std::getline(f, header_line)) {
    if (!is_comment_or_empty(header_line)) break;
  }
  header_line = qeeg::strip_utf8_bom(std::move(header_line));
  if (qeeg::trim(header_line).empty()) return std::nullopt;

  auto headers = qeeg::split_csv_row(header_line, delim);
  for (auto& h : headers) {
    h = qeeg::trim(qeeg::strip_utf8_bom(std::move(h)));
  }
  if (!looks_like_matrix_header(headers)) return std::nullopt;

  // Assume first row: blank + col labels.
  std::vector<std::string> col_labels;
  col_labels.reserve(headers.size() > 0 ? headers.size() - 1 : 0);
  for (size_t i = 1; i < headers.size(); ++i) {
    col_labels.push_back(qeeg::trim(headers[i]));
  }
  if (col_labels.size() < 2) return std::nullopt;

  std::vector<std::string> row_labels;
  std::vector<std::vector<double>> values;

  std::string line;
  while (std::getline(f, line)) {
    if (is_comment_or_empty(line)) continue;
    auto cols = qeeg::split_csv_row(line, delim);
    if (cols.empty()) continue;
    if (cols.size() < headers.size()) cols.resize(headers.size());

    std::string rlabel = qeeg::trim(cols[0]);
    if (rlabel.empty()) continue;

    std::vector<double> row;
    row.reserve(col_labels.size());
    for (size_t j = 0; j < col_labels.size(); ++j) {
      const size_t ci = j + 1;
      std::optional<double> v;
      if (ci < cols.size()) v = parse_double_opt(cols[ci]);
      row.push_back(v.has_value() ? *v : std::numeric_limits<double>::quiet_NaN());
    }

    row_labels.push_back(rlabel);
    values.push_back(std::move(row));
  }

  if (row_labels.size() < 2) return std::nullopt;

  // Best-effort sanity: square matrix expected.
  const size_t n = col_labels.size();
  if (row_labels.size() != n) {
    // Some exports include an extra header row, but in general treat mismatch as invalid.
    // Still accept if it looks close.
    if (row_labels.size() < n / 2 || row_labels.size() > n * 2) {
      return std::nullopt;
    }
  }

  // Use column labels as primary ordering.
  Matrix m;
  m.rois = col_labels;
  m.values = values;
  // Ensure rows are length n.
  for (auto& r : m.values) {
    if (r.size() < n) r.resize(n, std::numeric_limits<double>::quiet_NaN());
    if (r.size() > n) r.resize(n);
  }

  *out = std::move(m);
  return std::string();
}

struct ProtoEdge {
  std::string a;
  std::string b;
  std::string measure;
  std::string band;
  std::string value_kind;
  double value = 0.0;
  double abs_value = 0.0;
  std::optional<std::string> suggested_direction;
};

static std::vector<ProtoEdge> compute_protocol_edges(const std::unordered_map<GroupKey, std::vector<Edge>, GroupKeyHash>& groups,
                                                     const Args& args) {
  std::vector<ProtoEdge> out;
  const double thr = args.protocol_threshold;

  for (const auto& kv : groups) {
    const GroupKey& g = kv.first;
    const std::vector<Edge>& edges = kv.second;

    const bool is_z = is_z_metric_name(g.measure);
    if (args.protocol_only_z && !is_z) continue;

    for (const auto& e : edges) {
      if (!std::isfinite(e.v)) continue;
      const double av = std::fabs(e.v);
      if (thr > 0.0 && av < thr) continue;

      ProtoEdge pe;
      pe.a = e.a;
      pe.b = e.b;
      pe.measure = g.measure;
      pe.band = g.band;
      pe.value_kind = is_z ? "zscore" : "raw";
      pe.value = e.v;
      pe.abs_value = av;
      if (is_z) {
        pe.suggested_direction = (e.v > 0.0) ? std::optional<std::string>("decrease")
                                             : std::optional<std::string>("increase");
      }
      out.push_back(std::move(pe));
    }
  }

  std::sort(out.begin(), out.end(), [](const ProtoEdge& a, const ProtoEdge& b) {
    if (a.abs_value != b.abs_value) return a.abs_value > b.abs_value;
    if (a.measure != b.measure) return a.measure < b.measure;
    if (a.band != b.band) return a.band < b.band;
    if (a.a != b.a) return a.a < b.a;
    return a.b < b.b;
  });

  if (args.protocol_top > 0 && static_cast<int>(out.size()) > args.protocol_top) {
    out.resize(static_cast<size_t>(args.protocol_top));
  }

  return out;
}

static void write_protocol_json(const std::string& path, const Args& args, const std::string& input_path,
                                const std::string& outdir, const std::optional<std::string>& index_rel,
                                const std::vector<ProtoEdge>& edges) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  const std::string gen = qeeg::now_string_utc();

  f << "{\n";
  f << "  \"$schema\": \"https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/qeeg_loreta_connectivity_protocol.schema.json\",\n";
  f << "  \"schema_version\": 1,\n";
  f << "  \"generated_utc\": \"" << json_escape(gen) << "\",\n";
  f << "  \"tool\": \"" << kTool << "\",\n";
  f << "  \"input_path\": \"" << json_escape(posix_slashes(input_path)) << "\",\n";
  f << "  \"outdir\": \"" << json_escape(posix_slashes(outdir)) << "\",\n";
  f << "  \"connectivity_index_json\": " << (index_rel ? ("\"" + json_escape(*index_rel) + "\"") : "null") << ",\n";
  f << "  \"atlas\": {\"name\": \"" << json_escape(args.atlas) << "\"},\n";
  f << "  \"params\": {\n";
  f << "    \"top_n\": " << args.protocol_top << ",\n";
  f << "    \"only_z\": " << (args.protocol_only_z ? "true" : "false") << ",\n";
  f << "    \"threshold_abs\": " << std::setprecision(17) << args.protocol_threshold << "\n";
  f << "  },\n";
  f << "  \"edges\": [\n";

  for (size_t i = 0; i < edges.size(); ++i) {
    const auto& e = edges[i];
    f << "    {\n";
    f << "      \"rank\": " << (i + 1) << ",\n";
    f << "      \"roi_a\": \"" << json_escape(e.a) << "\",\n";
    f << "      \"roi_b\": \"" << json_escape(e.b) << "\",\n";
    f << "      \"measure\": \"" << json_escape(e.measure) << "\",\n";
    f << "      \"band\": \"" << json_escape(e.band) << "\",\n";
    f << "      \"value_kind\": \"" << json_escape(e.value_kind) << "\",\n";
    f << "      \"value\": " << json_number_or_null(e.value) << ",\n";
    f << "      \"abs_value\": " << json_number_or_null(e.abs_value) << ",\n";
    f << "      \"suggested_direction\": "
      << (e.suggested_direction ? ("\"" + json_escape(*e.suggested_direction) + "\"") : "null") << "\n";
    f << "    }";
    if (i + 1 < edges.size()) f << ',';
    f << "\n";
  }

  f << "  ]\n";
  f << "}\n";
}

struct MatrixStats {
  int n_rois = 0;
  int n_edges = 0;  // finite upper triangle count
  double min_v = std::numeric_limits<double>::quiet_NaN();
  double max_v = std::numeric_limits<double>::quiet_NaN();
  double mean_v = std::numeric_limits<double>::quiet_NaN();
};

static MatrixStats matrix_stats(const Matrix& m) {
  MatrixStats st;
  st.n_rois = static_cast<int>(m.rois.size());
  std::vector<double> vals;
  const size_t n = m.rois.size();
  vals.reserve(n * (n - 1) / 2);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      const double v = m.values[i][j];
      if (std::isfinite(v)) vals.push_back(v);
    }
  }
  st.n_edges = static_cast<int>(vals.size());
  if (!vals.empty()) {
    auto mm = std::minmax_element(vals.begin(), vals.end());
    st.min_v = *mm.first;
    st.max_v = *mm.second;
    double sum = 0.0;
    for (double v : vals) sum += v;
    st.mean_v = sum / static_cast<double>(vals.size());
  }
  return st;
}

struct MeasureInfo {
  std::string measure;
  std::string value_kind;
  std::vector<std::string> bands;
  std::vector<std::string> matrix_csvs;  // parallel to bands
  std::vector<MatrixStats> stats;         // parallel
};

static void write_index_json(const std::string& path, const Args& args, const std::string& input_path,
                             const std::string& outdir, const std::string& run_meta_name,
                             const std::vector<MeasureInfo>& measures,
                             const std::optional<std::string>& protocol_rel) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  const std::string gen = qeeg::now_string_utc();

  f << "{\n";
  f << "  \"$schema\": \"https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/qeeg_loreta_connectivity_index.schema.json\",\n";
  f << "  \"schema_version\": 1,\n";
  f << "  \"generated_utc\": \"" << json_escape(gen) << "\",\n";
  f << "  \"tool\": \"" << kTool << "\",\n";
  f << "  \"input_path\": \"" << json_escape(posix_slashes(input_path)) << "\",\n";
  f << "  \"outdir\": \"" << json_escape(posix_slashes(outdir)) << "\",\n";
  f << "  \"run_meta_json\": \"" << json_escape(run_meta_name) << "\",\n";
  f << "  \"protocol_json\": " << (protocol_rel ? ("\"" + json_escape(*protocol_rel) + "\"") : "null") << ",\n";
  f << "  \"atlas\": {\"name\": \"" << json_escape(args.atlas) << "\"},\n";
  f << "  \"measures\": [\n";

  for (size_t i = 0; i < measures.size(); ++i) {
    const auto& m = measures[i];
    f << "    {\n";
    f << "      \"measure\": \"" << json_escape(m.measure) << "\",\n";
    f << "      \"value_kind\": \"" << json_escape(m.value_kind) << "\",\n";
    f << "      \"bands\": [";
    for (size_t j = 0; j < m.bands.size(); ++j) {
      if (j) f << ',';
      f << "\"" << json_escape(m.bands[j]) << "\"";
    }
    f << "],\n";

    f << "      \"matrices\": [\n";
    for (size_t j = 0; j < m.bands.size(); ++j) {
      const MatrixStats& st = m.stats[j];
      f << "        {\n";
      f << "          \"band\": \"" << json_escape(m.bands[j]) << "\",\n";
      f << "          \"matrix_csv\": \"" << json_escape(m.matrix_csvs[j]) << "\",\n";
      f << "          \"n_rois\": " << st.n_rois << ",\n";
      f << "          \"n_edges\": " << st.n_edges << ",\n";
      f << "          \"min\": " << json_number_or_null(st.min_v) << ",\n";
      f << "          \"max\": " << json_number_or_null(st.max_v) << ",\n";
      f << "          \"mean\": " << json_number_or_null(st.mean_v) << "\n";
      f << "        }";
      if (j + 1 < m.bands.size()) f << ',';
      f << "\n";
    }
    f << "      ]\n";
    f << "    }";
    if (i + 1 < measures.size()) f << ',';
    f << "\n";
  }

  f << "  ]\n";
  f << "}\n";
}

static std::string stem_hint_from_path(const std::string& p) {
  const fs::path fp = fs::path(p);
  std::string stem = fp.stem().string();
  // If file is like <measure>_matrix_<band>.csv, keep the measure part.
  const std::string low = lower_ascii(stem);
  const std::string key = "_matrix_";
  const size_t pos = low.find(key);
  if (pos != std::string::npos) {
    stem = stem.substr(0, pos);
  }
  return stem;
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
          "loreta_connectivity.csv",
          "connectivity.csv",
          "loreta_pairs.csv",
          "connectivity_pairs.csv",
      };
      opt.preferred_contains = {"loreta", "connect", "matrix", "pairs"};
      const qeeg::ResolvedInputPath rp = qeeg::resolve_input_table_path(args.input, opt);
      if (!rp.note.empty()) std::cout << rp.note << "\n";
      input_path = rp.path;
    }

    qeeg::ensure_directory(args.outdir);

    char delim = ',';
    auto [headers, rows] = read_table(input_path, &delim);

    // Decide mode.
    bool use_matrix = false;
    if (args.mode == "matrix") {
      use_matrix = true;
    } else if (args.mode == "edges") {
      use_matrix = false;
    } else {
      // auto: if we can find edge columns, treat as edge list; else if header looks like matrix, treat as matrix.
      bool has_a = false;
      bool has_b = false;
      {
        int a_idx = -1;
        int b_idx = -1;
        try {
          a_idx = find_col_ci(headers, args.roi_a_column,
                              {"roi_a", "roi1", "region_a", "from", "source", "seed", "a", "channel_a", "chan_a"});
          b_idx = find_col_ci(headers, args.roi_b_column,
                              {"roi_b", "roi2", "region_b", "to", "target", "sink", "b", "channel_b", "chan_b"});
        } catch (...) {
          // ignore
        }
        has_a = (a_idx >= 0);
        has_b = (b_idx >= 0);
      }
      if (has_a && has_b) use_matrix = false;
      else use_matrix = looks_like_matrix_header(headers);
    }

    // Group edges by (measure, band). Even matrix mode is converted to this representation.
    std::unordered_map<GroupKey, std::vector<Edge>, GroupKeyHash> groups;

    if (use_matrix) {
      // Parse as matrix.
      Matrix m;
      // Already read headers/rows. Re-parse directly for robustness.
      Matrix parsed;
      {
        // We re-open so we can reuse the same parser.
        std::ifstream f(input_path);
        if (!f) throw std::runtime_error("Failed to open input: " + input_path);
      }

      Matrix mm;
      {
        // Parse via helper (re-reads file internally).
        char d2 = detect_delim(headers.empty() ? std::string() : headers[0]);
        (void)d2;
      }

      char d = detect_delim(std::string());
      (void)d;

      // Instead of trying to reuse read_table() output, re-read with the matrix parser.
      // (Keeps delimiter/BOM handling consistent).
      char d3 = ',';
      {
        std::ifstream f(input_path);
        if (!f) throw std::runtime_error("Failed to open input: " + input_path);
        std::string header_line;
        while (std::getline(f, header_line)) {
          if (!is_comment_or_empty(header_line)) break;
        }
        header_line = qeeg::strip_utf8_bom(std::move(header_line));
        if (qeeg::trim(header_line).empty()) throw std::runtime_error("Empty input file: " + input_path);
        d3 = detect_delim(header_line);
      }
      Matrix mat;
      const auto ok = parse_matrix_csv(input_path, d3, &mat);
      if (!ok.has_value()) {
        throw std::runtime_error("Failed to parse matrix CSV (try --mode edges): " + input_path);
      }

      // Derive band from filename or from --measure-id/--band-column (band-column not used in matrix mode).
      std::string band = "all";
      {
        const std::string stem = fs::path(input_path).stem().string();
        const auto b = detect_band(stem);
        if (b.has_value()) band = *b;
        // If input filename contains _matrix_<band>, use that.
        const std::string low = lower_ascii(stem);
        const size_t pos = low.find("_matrix_");
        if (pos != std::string::npos) {
          const std::string tail = stem.substr(pos + std::string("_matrix_").size());
          const std::string tid = safe_id(tail);
          if (!tid.empty()) band = tid;
        }
      }

      std::string measure = args.measure_id.empty() ? safe_id(stem_hint_from_path(input_path)) : safe_id(args.measure_id);

      // Convert upper triangle into edges.
      std::vector<Edge> edges;
      const size_t n = mat.rois.size();
      edges.reserve(n * (n - 1) / 2);
      for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
          const double v = mat.values[i][j];
          if (!std::isfinite(v)) continue;
          edges.push_back({mat.rois[i], mat.rois[j], v});
        }
      }

      groups[{measure, safe_id(band)}] = std::move(edges);

    } else {
      // Parse edge-list.
      const int a_idx = find_col_ci(headers, args.roi_a_column,
                                    {"roi_a", "roi1", "region_a", "from", "source", "seed", "a", "channel_a", "chan_a"});
      const int b_idx = find_col_ci(headers, args.roi_b_column,
                                    {"roi_b", "roi2", "region_b", "to", "target", "sink", "b", "channel_b", "chan_b"});
      const int band_idx = find_col_ci(headers, args.band_column, {"band", "freq", "frequency"});
      const int metric_idx = find_col_ci(headers, args.metric_column, {"metric", "measure", "type"});

      // Detect numeric columns.
      std::vector<int> numeric_cols;
      numeric_cols.reserve(headers.size());
      for (int ci = 0; ci < static_cast<int>(headers.size()); ++ci) {
        if (ci == a_idx || ci == b_idx || ci == band_idx || ci == metric_idx) continue;
        bool any_numeric = false;
        for (const auto& r : rows) {
          if (ci >= static_cast<int>(r.size())) continue;
          if (parse_double_opt(r[static_cast<size_t>(ci)]).has_value()) {
            any_numeric = true;
            break;
          }
        }
        if (any_numeric) numeric_cols.push_back(ci);
      }

      int value_idx = -1;
      if (!args.value_column.empty()) {
        value_idx = find_col_ci(headers, args.value_column, {});
      } else {
        // Prefer column called "value" or "z"; else take first numeric.
        value_idx = find_col_ci(headers, "", {"value", "val", "score", "z", "zscore", "z-score"});
        if (value_idx < 0 && !numeric_cols.empty()) value_idx = numeric_cols[0];
      }
      if (value_idx < 0) throw std::runtime_error("Failed to detect value column (use --value-column)");

      // Band default.
      const std::string default_band = "all";

      for (const auto& r : rows) {
        if (a_idx >= static_cast<int>(r.size()) || b_idx >= static_cast<int>(r.size()) || value_idx >= static_cast<int>(r.size())) continue;
        std::string a = qeeg::trim(r[static_cast<size_t>(a_idx)]);
        std::string b = qeeg::trim(r[static_cast<size_t>(b_idx)]);
        if (a.empty() || b.empty()) continue;

        std::string band = default_band;
        if (band_idx >= 0 && band_idx < static_cast<int>(r.size())) {
          const std::string bb = qeeg::trim(r[static_cast<size_t>(band_idx)]);
          if (!bb.empty()) band = bb;
        }

        std::string metric;
        if (metric_idx >= 0 && metric_idx < static_cast<int>(r.size())) {
          metric = qeeg::trim(r[static_cast<size_t>(metric_idx)]);
        }

        const auto vopt = parse_double_opt(r[static_cast<size_t>(value_idx)]);
        if (!vopt.has_value()) continue;

        std::string measure;
        if (!args.measure_id.empty()) {
          measure = safe_id(args.measure_id);
        } else if (!metric.empty()) {
          measure = safe_id(metric);
        } else {
          // fall back to value column header, then file stem
          const std::string vname = headers[static_cast<size_t>(value_idx)];
          if (!vname.empty() && lower_ascii(vname) != "value") measure = safe_id(vname);
          else measure = safe_id(stem_hint_from_path(input_path));
        }

        // If no explicit band column, try to infer from metric/value column names.
        std::string band2 = band;
        if (band2 == default_band) {
          if (auto db = detect_band(metric); db.has_value()) band2 = *db;
          else if (auto db2 = detect_band(headers[static_cast<size_t>(value_idx)]); db2.has_value()) band2 = *db2;
        }

        groups[{measure, safe_id(band2)}].push_back({a, b, *vopt});
      }

      if (groups.empty()) {
        throw std::runtime_error("No edges parsed from input (check column names)");
      }
    }

    // Write matrices.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> written;  // measure -> band -> filename
    std::unordered_map<std::string, std::unordered_map<std::string, MatrixStats>> stats_map;

    for (const auto& kv : groups) {
      const GroupKey& g = kv.first;
      const std::vector<Edge>& edges = kv.second;
      Matrix m = make_matrix(edges, args.directed);

      const std::string band = g.band.empty() ? std::string("all") : g.band;
      const std::string meas = g.measure.empty() ? std::string("loreta_connectivity") : g.measure;

      const std::string fname = meas + "_matrix_" + band + ".csv";
      const std::string out_path = (fs::path(args.outdir) / fname).string();
      write_matrix_csv(out_path, m);

      written[meas][band] = fname;
      stats_map[meas][band] = matrix_stats(m);
    }

    // Build per-measure info for index.
    std::vector<MeasureInfo> measure_infos;
    measure_infos.reserve(written.size());
    for (const auto& mkv : written) {
      const std::string& meas = mkv.first;
      MeasureInfo mi;
      mi.measure = meas;
      mi.value_kind = is_z_metric_name(meas) ? "zscore" : "raw";
      for (const auto& bkv : mkv.second) {
        mi.bands.push_back(bkv.first);
      }
      std::sort(mi.bands.begin(), mi.bands.end());
      mi.matrix_csvs.reserve(mi.bands.size());
      mi.stats.reserve(mi.bands.size());
      for (const auto& band : mi.bands) {
        mi.matrix_csvs.push_back(mkv.second.at(band));
        mi.stats.push_back(stats_map[meas][band]);
      }
      measure_infos.push_back(std::move(mi));
    }
    std::sort(measure_infos.begin(), measure_infos.end(), [](const MeasureInfo& a, const MeasureInfo& b) { return a.measure < b.measure; });

    // Optional protocol.
    std::optional<std::string> protocol_rel;
    std::string protocol_name;
    if (args.protocol_json) {
      const std::string protocol_default_name = "loreta_connectivity_protocol.json";
      const std::string protocol_path =
          args.protocol_path.empty() ? (fs::path(args.outdir) / protocol_default_name).string() : args.protocol_path;
      protocol_name = fs::path(protocol_path).filename().string();

      // Index relpath (if we write it).
      std::optional<std::string> index_rel;
      if (args.json_index) {
        const std::string index_default_name = "loreta_connectivity_index.json";
        const std::string index_path =
            args.json_index_path.empty() ? (fs::path(args.outdir) / index_default_name).string() : args.json_index_path;
        index_rel = safe_relpath_posix(index_path, args.outdir);
      }

      std::vector<ProtoEdge> proto_edges = compute_protocol_edges(groups, args);
      write_protocol_json(protocol_path, args, input_path, args.outdir, index_rel, proto_edges);
      protocol_rel = safe_relpath_posix(protocol_path, args.outdir);
    }

    // JSON index.
    std::optional<std::string> index_path_opt;
    std::string index_name;
    if (args.json_index) {
      const std::string index_default_name = "loreta_connectivity_index.json";
      const std::string index_path =
          args.json_index_path.empty() ? (fs::path(args.outdir) / index_default_name).string() : args.json_index_path;
      index_name = fs::path(index_path).filename().string();
      write_index_json(index_path, args, input_path, args.outdir, "loreta_connectivity_run_meta.json", measure_infos,
                       protocol_rel);
      index_path_opt = index_path;
    }

    // Run meta.
    const std::string run_meta_name = "loreta_connectivity_run_meta.json";
    const std::string run_meta_path = (fs::path(args.outdir) / run_meta_name).string();

    std::vector<std::string> outputs;
    for (const auto& m : measure_infos) {
      for (const auto& p : m.matrix_csvs) outputs.push_back(p);
    }
    if (args.json_index) outputs.push_back(index_name.empty() ? std::string("loreta_connectivity_index.json") : index_name);
    if (args.protocol_json) outputs.push_back(protocol_name.empty() ? std::string("loreta_connectivity_protocol.json") : protocol_name);
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
