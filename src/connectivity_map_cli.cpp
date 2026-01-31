#include "qeeg/bmp_writer.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/connectivity_graph.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_csv;
  std::string outdir{"out_connectivity"};
  std::string montage_spec{"builtin:standard_1020_19"};

  // If empty: auto-detect (coherence/imcoh/first numeric column).
  std::string metric;

  // Edge filtering
  double min_weight{0.0};
  bool have_max_weight{false};
  double max_weight{0.0};
  bool abs_weight{false};
  size_t max_edges{0}; // 0 => no limit

  // Visualization scaling
  bool have_vlim{false};
  double vmin{0.0};
  double vmax{1.0};
  double min_width{0.5};
  double max_width{4.0};

  // Rendering
  int size_px{900};
  bool labels{false};
  bool html_report{false};
  std::string title;
};

static void print_help() {
  std::cout
    << "qeeg_connectivity_map_cli\n\n"
    << "Render qEEG connectivity \"brain maps\" (scalp network diagrams) as SVG.\n"
    << "\n"
    << "In addition to the SVG map, this tool writes small summary tables:\n"
    << "  - connectivity_edges_used.csv (filtered/trimmed edges used for the map)\n"
    << "  - connectivity_nodes.csv (per-node degree/strength summary)\n"
    << "  - connectivity_region_pairs.csv (coarse lobe/hemisphere region summary)\n"
    << "\n"
    << "Typical inputs:\n"
    << "  - *_pairs.csv from qeeg_coherence_cli (coherence_pairs.csv / imcoh_pairs.csv)\n"
    << "  - *_matrix_*.csv from qeeg_coherence_cli (coherence_matrix_alpha.csv, etc.)\n"
    << "\n"
    << "Usage:\n"
    << "  qeeg_connectivity_map_cli --input out_coherence/coherence_pairs.csv --outdir out_conn\n"
    << "  qeeg_connectivity_map_cli --input out_coherence/imcoh_pairs.csv --metric imcoh --min 0.05 --labels\n"
    << "  qeeg_connectivity_map_cli --input out_coherence --metric imcoh --min 0.05 --labels\n"
    << "  qeeg_connectivity_map_cli --input out_coherence/coherence_run_meta.json --metric coherence\n"
    << "\n"
    << "Required:\n"
    << "  --input PATH            Edge list / matrix (.csv/.tsv), *_run_meta.json, or an output directory\n"
    << "\n"
    << "Options:\n"
    << "  --outdir DIR            Output directory (default: out_connectivity)\n"
    << "  --montage SPEC          builtin:standard_1020_19 (default), builtin:standard_1010_61, or montage CSV (name,x,y)\n"
    << "  --metric NAME           Value column in edge list CSV (default: auto)\n"
    << "  --min X                 Drop edges below X (default: 0)\n"
    << "  --max X                 Drop edges above X\n"
    << "  --abs                   Use abs(weight) (useful if values can be negative)\n"
    << "  --max-edges N            Keep only the N strongest edges (0 = no limit)\n"
    << "  --vmin X --vmax Y        Fixed color/width scaling limits (otherwise auto from data)\n"
    << "  --min-width X            Stroke width for weakest edges (default: 0.5)\n"
    << "  --max-width X            Stroke width for strongest edges (default: 4.0)\n"
    << "  --size N                 SVG canvas size in px (default: 900)\n"
    << "  --labels                 Draw channel labels\n"
    << "  --title TEXT             Title text for the map\n"
    << "  --html-report            Write a simple HTML report that embeds the SVG\n"
    << "  -h, --help               Show this help\n";
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
    } else if (arg == "--metric" && i + 1 < argc) {
      a.metric = argv[++i];
    } else if (arg == "--min" && i + 1 < argc) {
      a.min_weight = to_double(argv[++i]);
    } else if (arg == "--max" && i + 1 < argc) {
      a.have_max_weight = true;
      a.max_weight = to_double(argv[++i]);
    } else if (arg == "--abs") {
      a.abs_weight = true;
    } else if (arg == "--max-edges" && i + 1 < argc) {
      a.max_edges = static_cast<size_t>(std::max(0, to_int(argv[++i])));
    } else if (arg == "--vmin" && i + 1 < argc) {
      a.vmin = to_double(argv[++i]);
      a.have_vlim = true;
    } else if (arg == "--vmax" && i + 1 < argc) {
      a.vmax = to_double(argv[++i]);
      a.have_vlim = true;
    } else if (arg == "--min-width" && i + 1 < argc) {
      a.min_width = to_double(argv[++i]);
    } else if (arg == "--max-width" && i + 1 < argc) {
      a.max_width = to_double(argv[++i]);
    } else if (arg == "--size" && i + 1 < argc) {
      a.size_px = to_int(argv[++i]);
    } else if (arg == "--labels") {
      a.labels = true;
    } else if (arg == "--title" && i + 1 < argc) {
      a.title = argv[++i];
    } else if (arg == "--html-report") {
      a.html_report = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static Montage load_montage(const std::string& spec) {
  std::string low = to_lower(spec);
  if (low == "builtin" || low == "default") {
    return Montage::builtin_standard_1020_19();
  }

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

struct Edge {
  std::string a;
  std::string b;
  double w{0.0};
};

struct EdgeTable {
  std::string metric;
  std::vector<Edge> edges;
  bool from_matrix{false};
};

static int find_col(const std::vector<std::string>& header, const std::string& key) {
  const std::string k = norm_key(key);
  for (size_t i = 0; i < header.size(); ++i) {
    if (norm_key(header[i]) == k) return static_cast<int>(i);
  }
  return -1;
}

static bool looks_like_pairs_header(const std::vector<std::string>& header) {
  return (find_col(header, "channel_a") >= 0 || find_col(header, "ch_a") >= 0) &&
         (find_col(header, "channel_b") >= 0 || find_col(header, "ch_b") >= 0);
}

static bool looks_like_matrix_header(const std::vector<std::string>& header) {
  if (header.size() < 3) return false;
  if (!trim(header[0]).empty()) return false;
  // Heuristic: at least 2 non-empty channel names.
  return !trim(header[1]).empty() && !trim(header[2]).empty();
}

static EdgeTable read_edges(const Args& args) {
  std::ifstream f(args.input_csv);
  if (!f) throw std::runtime_error("Failed to open input: " + args.input_csv);

  std::string line;
  bool saw_header = false;
  char delim = ',';
  std::vector<std::string> header;
  EdgeTable tab;
  size_t row_index = 0;

  // Columns for pairs mode
  int col_a = -1;
  int col_b = -1;
  int col_w = -1;

  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string raw = trim(line);
    if (!saw_header) {
      raw = strip_utf8_bom(raw);
    }
    if (is_comment_or_empty(raw)) continue;

    if (!saw_header) {
      delim = detect_delim(raw);
      header = parse_row(raw, delim);
      if (header.size() < 3) {
        throw std::runtime_error("Expected at least 3 columns in header: " + args.input_csv);
      }

      if (looks_like_pairs_header(header)) {
        col_a = find_col(header, "channel_a");
        if (col_a < 0) col_a = find_col(header, "ch_a");
        col_b = find_col(header, "channel_b");
        if (col_b < 0) col_b = find_col(header, "ch_b");

        if (col_a < 0 || col_b < 0) {
          throw std::runtime_error("Edge list CSV must have channel_a and channel_b columns.");
        }

        if (!args.metric.empty()) {
          col_w = find_col(header, args.metric);
          if (col_w < 0) {
            throw std::runtime_error("Metric column not found: '" + args.metric + "'");
          }
          tab.metric = header[static_cast<size_t>(col_w)];
        } else {
          // Prefer coherence/imcoh if present; else pick the first non-channel column.
          col_w = find_col(header, "coherence");
          if (col_w < 0) col_w = find_col(header, "imcoh");
          if (col_w < 0) {
            for (size_t i = 0; i < header.size(); ++i) {
              if (static_cast<int>(i) == col_a || static_cast<int>(i) == col_b) continue;
              if (!trim(header[i]).empty()) {
                col_w = static_cast<int>(i);
                break;
              }
            }
          }
          if (col_w < 0) {
            throw std::runtime_error("Could not determine metric column. Use --metric.");
          }
          tab.metric = header[static_cast<size_t>(col_w)];
        }

        saw_header = true;
        continue;
      }

      if (looks_like_matrix_header(header)) {
        tab.from_matrix = true;
        tab.metric = args.metric.empty() ? "weight" : args.metric;
        saw_header = true;
        continue;
      }

      throw std::runtime_error(
        "Unrecognized CSV header. Expected an edge list with channel_a/channel_b or a square matrix with blank first cell.");
    }

    const auto cols = parse_row(raw, delim);
    if (cols.empty()) continue;

    if (!tab.from_matrix) {
      const size_t need = static_cast<size_t>(std::max({col_a, col_b, col_w}) + 1);
      if (cols.size() < need) continue;
      const std::string a = trim(cols[static_cast<size_t>(col_a)]);
      const std::string b = trim(cols[static_cast<size_t>(col_b)]);
      if (a.empty() || b.empty()) continue;

      const std::string s = trim(cols[static_cast<size_t>(col_w)]);
      if (s.empty()) continue;
      double w = std::numeric_limits<double>::quiet_NaN();
      try {
        w = to_double(s);
      } catch (...) {
        w = std::numeric_limits<double>::quiet_NaN();
      }
      if (!std::isfinite(w)) continue;
      tab.edges.push_back({a, b, w});
      continue;
    }

    // Matrix mode.
    // Format from qeeg_coherence_cli: header is ["", ch0, ch1, ...]
    // rows are [ch_i, v_i0, v_i1, ...]
    if (cols.size() < header.size()) continue;
    const std::string row_ch = trim(cols[0]);
    if (row_ch.empty()) continue;
    const size_t irow = row_index;
    for (size_t j = 1; j < header.size(); ++j) {
      const std::string col_ch = trim(header[j]);
      if (col_ch.empty()) continue;
      const size_t icol = j - 1;
      if (icol <= irow) continue; // upper-triangle only (skip diagonal and lower)

      const std::string s = trim(cols[j]);
      if (s.empty()) continue;
      double w = std::numeric_limits<double>::quiet_NaN();
      try {
        w = to_double(s);
      } catch (...) {
        w = std::numeric_limits<double>::quiet_NaN();
      }
      if (!std::isfinite(w)) continue;
      tab.edges.push_back({row_ch, col_ch, w});
    }
    ++row_index;
  }

  if (!saw_header) {
    throw std::runtime_error("Input CSV appears empty: " + args.input_csv);
  }
  if (tab.edges.empty()) {
    throw std::runtime_error("No edges parsed from input. Check file format and --metric.");
  }
  return tab;
}

static double clamp01(double t) {
  if (t < 0.0) return 0.0;
  if (t > 1.0) return 1.0;
  return t;
}

static std::string rgb_hex(const RGB& c) {
  std::ostringstream oss;
  oss << '#'
      << std::hex << std::uppercase << std::setfill('0')
      << std::setw(2) << static_cast<int>(c.r)
      << std::setw(2) << static_cast<int>(c.g)
      << std::setw(2) << static_cast<int>(c.b);
  return oss.str();
}

static std::string fmt_double(double x, int digits = 6) {
  if (!std::isfinite(x)) return "nan";
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(digits) << x;
  return oss.str();
}

static void write_svg(const Args& args,
                      const std::vector<Edge>& edges,
                      const std::map<std::string, Vec2>& node_pos,
                      double vmin,
                      double vmax,
                      const std::string& svg_path) {
  std::ofstream out(svg_path);
  if (!out) throw std::runtime_error("Failed to write: " + svg_path);

  const int W = std::max(320, args.size_px);
  const int H = std::max(320, args.size_px);
  const double cx = W * 0.50;
  const double cy = H * 0.52;
  const double r = std::min(W, H) * 0.40;

  auto px = [&](const Vec2& p) -> std::pair<double, double> {
    const double x = cx + p.x * r;
    const double y = cy - p.y * r;
    return {x, y};
  };

  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
      << "\" viewBox=\"0 0 " << W << " " << H << "\">\n";

  // Styling + gradient for a small legend.
  out << "  <defs>\n";
  out << "    <linearGradient id=\"heat\" x1=\"0\" y1=\"1\" x2=\"0\" y2=\"0\">\n";
  out << "      <stop offset=\"0%\" stop-color=\"#0000FF\"/>\n";
  out << "      <stop offset=\"25%\" stop-color=\"#00FFFF\"/>\n";
  out << "      <stop offset=\"50%\" stop-color=\"#00FF00\"/>\n";
  out << "      <stop offset=\"75%\" stop-color=\"#FFFF00\"/>\n";
  out << "      <stop offset=\"100%\" stop-color=\"#FF0000\"/>\n";
  out << "    </linearGradient>\n";
  out << "  </defs>\n";

  // Background
  out << "  <rect x=\"0\" y=\"0\" width=\"" << W << "\" height=\"" << H
      << "\" fill=\"#0b1020\"/>\n";

  // Title
  {
    std::string title = args.title;
    if (title.empty()) {
      title = "Connectivity map (" + (args.metric.empty() ? "auto" : args.metric) + ")";
    }
    out << "  <text x=\"" << (W * 0.5) << "\" y=\"" << (H * 0.06)
        << "\" text-anchor=\"middle\" font-family=\"system-ui,Segoe UI,Roboto,Helvetica,Arial\""
        << " font-size=\"22\" fill=\"#E5E7EB\">" << svg_escape(title) << "</text>\n";
  }

  // Head outline
  out << "  <circle cx=\"" << cx << "\" cy=\"" << cy << "\" r=\"" << r
      << "\" fill=\"none\" stroke=\"#94A3B8\" stroke-width=\"2\"/>\n";
  // Nose (simple triangle)
  out << "  <path d=\"M " << (cx - 0.08 * r) << " " << (cy - 1.01 * r)
      << " L " << cx << " " << (cy - 1.12 * r)
      << " L " << (cx + 0.08 * r) << " " << (cy - 1.01 * r)
      << "\" fill=\"none\" stroke=\"#94A3B8\" stroke-width=\"2\"/>\n";

  // Legend (right side)
  {
    const double lw = 18.0;
    const double lh = 180.0;
    const double lx = W - 54.0;
    const double ly = H * 0.25;
    out << "  <rect x=\"" << lx << "\" y=\"" << ly << "\" width=\"" << lw << "\" height=\"" << lh
        << "\" fill=\"url(#heat)\" stroke=\"#94A3B8\" stroke-width=\"1\"/>\n";
    out << "  <text x=\"" << (lx + lw + 8.0) << "\" y=\"" << (ly + 10.0)
        << "\" font-size=\"12\" fill=\"#CBD5E1\" font-family=\"system-ui,Segoe UI,Roboto,Helvetica,Arial\">"
        << svg_escape(fmt_double(vmax, 3)) << "</text>\n";
    out << "  <text x=\"" << (lx + lw + 8.0) << "\" y=\"" << (ly + lh)
        << "\" font-size=\"12\" fill=\"#CBD5E1\" font-family=\"system-ui,Segoe UI,Roboto,Helvetica,Arial\">"
        << svg_escape(fmt_double(vmin, 3)) << "</text>\n";
  }

  // Edges: draw first so nodes are on top.
  out << "  <g id=\"edges\">\n";
  for (const auto& e : edges) {
    auto ita = node_pos.find(e.a);
    auto itb = node_pos.find(e.b);
    if (ita == node_pos.end() || itb == node_pos.end()) continue;

    const double w = e.w;
    const double t = clamp01((w - vmin) / (vmax - vmin));
    const RGB c = colormap_heat(t);
    const double sw = args.min_width + t * (args.max_width - args.min_width);
    const double op = 0.15 + 0.85 * t;

    const auto pa = px(ita->second);
    const auto pb = px(itb->second);
    out << "    <line x1=\"" << pa.first << "\" y1=\"" << pa.second
        << "\" x2=\"" << pb.first << "\" y2=\"" << pb.second
        << "\" stroke=\"" << rgb_hex(c) << "\" stroke-opacity=\"" << fmt_double(op, 3)
        << "\" stroke-width=\"" << fmt_double(sw, 3)
        << "\" stroke-linecap=\"round\"/>\n";
  }
  out << "  </g>\n";

  // Nodes
  out << "  <g id=\"nodes\">\n";
  for (const auto& kv : node_pos) {
    const auto p = px(kv.second);
    out << "    <circle cx=\"" << p.first << "\" cy=\"" << p.second
        << "\" r=\"6\" fill=\"#0b1020\" stroke=\"#E5E7EB\" stroke-width=\"2\"/>\n";
    if (args.labels) {
      out << "    <text x=\"" << (p.first + 8.0) << "\" y=\"" << (p.second + 4.0)
          << "\" font-size=\"12\" fill=\"#E5E7EB\" font-family=\"system-ui,Segoe UI,Roboto,Helvetica,Arial\">"
          << svg_escape(kv.first) << "</text>\n";
    }
  }
  out << "  </g>\n";

  // Footer
  out << "  <text x=\"" << (W * 0.5) << "\" y=\"" << (H * 0.97)
      << "\" text-anchor=\"middle\" font-size=\"12\" fill=\"#94A3B8\""
      << " font-family=\"system-ui,Segoe UI,Roboto,Helvetica,Arial\">"
      << "Montage: " << svg_escape(args.montage_spec)
      << " | Edges: " << edges.size()
      << " | Metric: " << svg_escape(args.metric.empty() ? "auto" : args.metric)
      << "</text>\n";

  out << "</svg>\n";
}

static void write_edges_used_csv(const std::string& outpath, const std::vector<Edge>& edges) {
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << "channel_a,channel_b,weight\n";
  out << std::setprecision(12);
  for (const auto& e : edges) {
    out << e.a << "," << e.b << "," << e.w << "\n";
  }
}

static void write_nodes_csv(const std::string& outpath,
                            const ConnectivityGraphMetrics& m,
                            const std::map<std::string, Vec2>& node_pos) {
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << "node,lobe,hemisphere,region,degree,strength,mean_weight,max_weight,x,y\n";
  out << std::setprecision(12);

  // For readability in spreadsheets, order by descending strength.
  std::vector<ConnectivityNodeMetrics> nodes = m.nodes;
  std::sort(nodes.begin(), nodes.end(), [](const ConnectivityNodeMetrics& a, const ConnectivityNodeMetrics& b) {
    if (a.strength != b.strength) return a.strength > b.strength;
    return a.node < b.node;
  });

  for (const auto& n : nodes) {
    double x = std::numeric_limits<double>::quiet_NaN();
    double y = std::numeric_limits<double>::quiet_NaN();
    const auto it = node_pos.find(n.node);
    if (it != node_pos.end()) {
      x = it->second.x;
      y = it->second.y;
    }

    out << n.node << ","
        << connectivity_lobe_name(n.lobe) << ","
        << connectivity_hemisphere_name(n.hemisphere) << ","
        << n.region << ","
        << n.degree << ","
        << n.strength << ","
        << n.mean_weight << ","
        << n.max_weight << ","
        << x << "," << y << "\n";
  }
}

static void write_region_pairs_csv(const std::string& outpath, const ConnectivityGraphMetrics& m) {
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << "region_a,region_b,edge_count,sum_weight,mean_weight\n";
  out << std::setprecision(12);

  std::vector<ConnectivityRegionPairMetrics> pairs = m.region_pairs;
  std::sort(pairs.begin(), pairs.end(), [](const ConnectivityRegionPairMetrics& a, const ConnectivityRegionPairMetrics& b) {
    // Descending mean weight, then stable names.
    if (a.mean_weight != b.mean_weight) return a.mean_weight > b.mean_weight;
    if (a.region_a != b.region_a) return a.region_a < b.region_a;
    return a.region_b < b.region_b;
  });

  for (const auto& p : pairs) {
    out << p.region_a << "," << p.region_b << "," << p.edge_count << "," << p.sum_weight << "," << p.mean_weight
        << "\n";
  }
}

static void write_html_report(const Args& args,
                              const std::string& svg_file,
                              const std::string& edges_csv,
                              const std::string& nodes_csv,
                              const std::string& region_csv,
                              const std::vector<Edge>& edges,
                              const ConnectivityGraphMetrics& metrics) {
  const std::string outpath = args.outdir + "/connectivity_report.html";
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);

  // Precompute small previews.
  std::vector<Edge> top_edges = edges;
  std::sort(top_edges.begin(), top_edges.end(), [](const Edge& a, const Edge& b) {
    if (a.w != b.w) return a.w > b.w;
    if (a.a != b.a) return a.a < b.a;
    return a.b < b.b;
  });
  const std::size_t max_preview_rows = 25;
  if (top_edges.size() > max_preview_rows) top_edges.resize(max_preview_rows);

  std::vector<ConnectivityNodeMetrics> top_nodes = metrics.nodes;
  std::sort(top_nodes.begin(), top_nodes.end(), [](const ConnectivityNodeMetrics& a, const ConnectivityNodeMetrics& b) {
    if (a.strength != b.strength) return a.strength > b.strength;
    return a.node < b.node;
  });
  if (top_nodes.size() > max_preview_rows) top_nodes.resize(max_preview_rows);

  std::vector<ConnectivityRegionPairMetrics> top_regions = metrics.region_pairs;
  std::sort(top_regions.begin(), top_regions.end(), [](const ConnectivityRegionPairMetrics& a,
                                                       const ConnectivityRegionPairMetrics& b) {
    if (a.mean_weight != b.mean_weight) return a.mean_weight > b.mean_weight;
    if (a.region_a != b.region_a) return a.region_a < b.region_a;
    return a.region_b < b.region_b;
  });
  if (top_regions.size() > max_preview_rows) top_regions.resize(max_preview_rows);

  out << "<!doctype html>\n"
      << "<html>\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>qEEG Connectivity Map</title>\n"
      << "  <style>\n"
      << "    html,body{margin:0;height:100%;background:#0b1020;color:#e5e7eb;font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;}\n"
      << "    .wrap{max-width:1100px;margin:0 auto;padding:18px;}\n"
      << "    a{color:#38bdf8;text-decoration:none;}a:hover{text-decoration:underline;}\n"
      << "    .card{background:rgba(17,26,51,0.6);border:1px solid rgba(255,255,255,0.10);border-radius:12px;padding:12px;}\n"
      << "    iframe{width:100%;height:900px;border:0;border-radius:12px;background:#0b1020;}\n"
      << "    .small{font-size:12px;color:#94a3b8;}\n"
      << "    table{border-collapse:collapse;width:100%;font-size:13px;}\n"
      << "    th,td{border-bottom:1px solid rgba(255,255,255,0.10);padding:6px 8px;text-align:left;}\n"
      << "    th{font-weight:600;color:#cbd5e1;}\n"
      << "    code{font-size:12px;}\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div class=\"wrap\">\n"
      << "    <h1 style=\"margin:0 0 6px 0;font-size:22px\">qEEG Connectivity Map</h1>\n"
      << "    <div class=\"small\">Generated by <code>qeeg_connectivity_map_cli</code></div>\n"
      << "    <div style=\"height:12px\"></div>\n"
      << "    <div class=\"card\">\n"
      << "      <div class=\"small\">Input: <code>" << svg_escape(args.input_csv) << "</code></div>\n"
      << "      <div class=\"small\" style=\"margin-top:6px\">Nodes: " << metrics.nodes.size() << " | Edges: "
      << edges.size() << "</div>\n"
      << "      <div style=\"height:10px\"></div>\n"
      << "      <iframe src=\"" << url_escape(svg_file) << "\"></iframe>\n"
      << "      <div class=\"small\" style=\"margin-top:10px\">Open the SVG directly: <a href=\"" << url_escape(svg_file) << "\">" << svg_escape(svg_file) << "</a></div>\n"
      << "      <div class=\"small\" style=\"margin-top:10px\">\n"
      << "        CSV outputs: "
      << "<a href=\"" << url_escape(edges_csv) << "\">" << svg_escape(edges_csv) << "</a> · "
      << "<a href=\"" << url_escape(nodes_csv) << "\">" << svg_escape(nodes_csv) << "</a> · "
      << "<a href=\"" << url_escape(region_csv) << "\">" << svg_escape(region_csv) << "</a>\n"
      << "      </div>\n"
      << "      <div style=\"height:12px\"></div>\n"

      << "      <details open>\n"
      << "        <summary style=\"cursor:pointer\">Top edges (preview)</summary>\n"
      << "        <div style=\"height:8px\"></div>\n"
      << "        <table>\n"
      << "          <thead><tr><th>Channel A</th><th>Channel B</th><th>Weight</th></tr></thead>\n"
      << "          <tbody>\n";
  for (const auto& e : top_edges) {
    out << "<tr><td><code>" << svg_escape(e.a) << "</code></td><td><code>" << svg_escape(e.b)
        << "</code></td><td>" << svg_escape(fmt_double(e.w, 6)) << "</td></tr>\n";
  }
  out << "          </tbody>\n"
      << "        </table>\n"
      << "      </details>\n"

      << "      <div style=\"height:12px\"></div>\n"
      << "      <details>\n"
      << "        <summary style=\"cursor:pointer\">Top nodes by strength (preview)</summary>\n"
      << "        <div style=\"height:8px\"></div>\n"
      << "        <table>\n"
      << "          <thead><tr><th>Node</th><th>Region</th><th>Degree</th><th>Strength</th><th>Mean</th></tr></thead>\n"
      << "          <tbody>\n";
  for (const auto& n : top_nodes) {
    out << "<tr><td><code>" << svg_escape(n.node) << "</code></td><td>" << svg_escape(n.region) << "</td><td>"
        << n.degree << "</td><td>" << svg_escape(fmt_double(n.strength, 6)) << "</td><td>"
        << svg_escape(fmt_double(n.mean_weight, 6)) << "</td></tr>\n";
  }
  out << "          </tbody>\n"
      << "        </table>\n"
      << "      </details>\n"

      << "      <div style=\"height:12px\"></div>\n"
      << "      <details>\n"
      << "        <summary style=\"cursor:pointer\">Top region pairs by mean weight (preview)</summary>\n"
      << "        <div style=\"height:8px\"></div>\n"
      << "        <table>\n"
      << "          <thead><tr><th>Region A</th><th>Region B</th><th>Edges</th><th>Mean</th><th>Sum</th></tr></thead>\n"
      << "          <tbody>\n";
  for (const auto& p : top_regions) {
    out << "<tr><td>" << svg_escape(p.region_a) << "</td><td>" << svg_escape(p.region_b) << "</td><td>"
        << p.edge_count << "</td><td>" << svg_escape(fmt_double(p.mean_weight, 6)) << "</td><td>"
        << svg_escape(fmt_double(p.sum_weight, 6)) << "</td></tr>\n";
  }
  out << "          </tbody>\n"
      << "        </table>\n"
      << "      </details>\n"
      << "    </div>\n"
      << "  </div>\n"
      << "</body>\n"
      << "</html>\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.input_csv.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.size_px < 320) {
      throw std::runtime_error("--size must be >= 320");
    }
    if (!(args.max_width >= args.min_width && args.min_width > 0.0)) {
      throw std::runtime_error("Invalid --min-width/--max-width");
    }
    if (args.have_vlim && !(args.vmax > args.vmin)) {
      throw std::runtime_error("Invalid --vmin/--vmax: require vmax > vmin");
    }

    // Allow chaining: --input can be a .csv/.tsv, a *_run_meta.json file, or an output directory.
    {
      ResolveInputTableOptions opt;
      if (!trim(args.metric).empty()) {
        const std::string m = to_lower(trim(args.metric));
        opt.preferred_filenames = {m + "_pairs.csv", m + "_pairs.tsv"};
        opt.preferred_contains = {m, "pairs"};
      } else {
        opt.preferred_filenames = {"coherence_pairs.csv", "coherence_pairs.tsv", "imcoh_pairs.csv", "imcoh_pairs.tsv"};
        opt.preferred_contains = {"pairs"};
      }
      ResolvedInputPath rp = resolve_input_table_path(args.input_csv, opt);
      if (!rp.note.empty()) std::cout << rp.note << "\n";
      args.input_csv = rp.path;
    }

    ensure_directory(args.outdir);

    const Montage montage = load_montage(args.montage_spec);
    EdgeTable tab = read_edges(args);

    // Apply filters + collect nodes.
    std::vector<Edge> edges;
    edges.reserve(tab.edges.size());
    for (const auto& e : tab.edges) {
      double w = e.w;
      if (args.abs_weight) w = std::fabs(w);
      if (!std::isfinite(w)) continue;
      if (w < args.min_weight) continue;
      if (args.have_max_weight && w > args.max_weight) continue;
      edges.push_back({e.a, e.b, w});
    }
    if (edges.empty()) {
      throw std::runtime_error("No edges left after filtering. Try lowering --min.");
    }

    // If requested, keep only the strongest edges.
    if (args.max_edges > 0 && edges.size() > args.max_edges) {
      std::nth_element(edges.begin(), edges.begin() + static_cast<std::ptrdiff_t>(args.max_edges), edges.end(),
                       [](const Edge& x, const Edge& y) { return x.w > y.w; });
      edges.resize(args.max_edges);
      std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.w > y.w; });
    }

    // Node positions (only nodes present in montage).
    std::map<std::string, Vec2> node_pos;
    for (const auto& e : edges) {
      Vec2 pa;
      if (montage.get(e.a, &pa)) node_pos.emplace(e.a, pa);
      Vec2 pb;
      if (montage.get(e.b, &pb)) node_pos.emplace(e.b, pb);
    }

    // Remove edges that reference unknown nodes (no montage position).
    std::vector<Edge> filtered;
    filtered.reserve(edges.size());
    for (const auto& e : edges) {
      if (node_pos.find(e.a) == node_pos.end()) continue;
      if (node_pos.find(e.b) == node_pos.end()) continue;
      filtered.push_back(e);
    }
    edges.swap(filtered);

    if (edges.empty() || node_pos.size() < 2) {
      throw std::runtime_error("No edges with known montage positions. Check --montage and channel names.");
    }

    // Determine scaling limits.
    double vmin = args.vmin;
    double vmax = args.vmax;
    if (!args.have_vlim) {
      vmin = std::numeric_limits<double>::infinity();
      vmax = -std::numeric_limits<double>::infinity();
      for (const auto& e : edges) {
        vmin = std::min(vmin, e.w);
        vmax = std::max(vmax, e.w);
      }
      if (!std::isfinite(vmin) || !std::isfinite(vmax) || !(vmax > vmin)) {
        vmin = 0.0;
        vmax = 1.0;
      }
    }
    if (!(vmax > vmin)) vmax = vmin + 1e-12;

    const std::string svg_file = "connectivity_map.svg";
    const std::string svg_path = args.outdir + "/" + svg_file;
    write_svg(args, edges, node_pos, vmin, vmax, svg_path);
    std::cout << "Wrote: " << svg_path << "\n";

    // Export filtered edge list + simple summaries to accompany the map.
    const std::string edges_csv_file = "connectivity_edges_used.csv";
    const std::string nodes_csv_file = "connectivity_nodes.csv";
    const std::string region_csv_file = "connectivity_region_pairs.csv";

    {
      write_edges_used_csv(args.outdir + "/" + edges_csv_file, edges);
      std::cout << "Wrote: " << args.outdir << "/" << edges_csv_file << "\n";
    }

    ConnectivityGraphMetrics metrics;
    {
      std::vector<ConnectivityEdge> g_edges;
      g_edges.reserve(edges.size());
      for (const auto& e : edges) g_edges.push_back({e.a, e.b, e.w});
      metrics = compute_connectivity_graph_metrics(g_edges);
      write_nodes_csv(args.outdir + "/" + nodes_csv_file, metrics, node_pos);
      write_region_pairs_csv(args.outdir + "/" + region_csv_file, metrics);
      std::cout << "Wrote: " << args.outdir << "/" << nodes_csv_file << "\n";
      std::cout << "Wrote: " << args.outdir << "/" << region_csv_file << "\n";
    }

    std::vector<std::string> outputs;
    outputs.push_back(svg_file);
    outputs.push_back(edges_csv_file);
    outputs.push_back(nodes_csv_file);
    outputs.push_back(region_csv_file);

    if (args.html_report) {
      write_html_report(args, svg_file, edges_csv_file, nodes_csv_file, region_csv_file, edges, metrics);
      outputs.push_back("connectivity_report.html");
      std::cout << "Wrote: " << args.outdir << "/connectivity_report.html\n";
    }

    // Run meta for UI discovery.
    {
      const std::string meta = args.outdir + "/connectivity_run_meta.json";
      outputs.push_back("connectivity_run_meta.json");
      (void)write_run_meta_json(meta, "qeeg_connectivity_map_cli", args.outdir, args.input_csv, outputs);
    }

    std::cout << "Done. Nodes: " << node_pos.size() << ", edges: " << edges.size() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
