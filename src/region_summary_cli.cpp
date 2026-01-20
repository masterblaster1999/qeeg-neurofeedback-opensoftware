#include "qeeg/run_meta.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// qEEG "brain mapping" often starts with per-channel quantitative metrics
// (bandpowers, ratios, z-scores). This helper CLI summarizes those metrics into
// coarse scalp "regions" (lobe x hemisphere) to make reports easier to read.
//
// The input format matches many qeeg tools:
//   channel,<metric1>,<metric2>,...
//
// Output:
//   - region_summary.csv (wide)
//   - region_summary_long.csv (long)
//   - region_report.html (optional)
//   - region_summary_run_meta.json

using namespace qeeg;

namespace {

struct Args {
  std::string input_csv;
  std::string outdir{"out_regions"};

  bool html_report{false};

  // Column selection
  std::vector<std::string> metrics;  // if empty: include all numeric columns
  std::vector<std::string> exclude;  // remove specific metrics
};

static void print_help() {
  std::cout
    << "qeeg_region_summary_cli\n\n"
    << "Summarize per-channel qEEG metrics into coarse brain regions (lobe x hemisphere).\n\n"
    << "Input:\n"
    << "  A CSV with a channel column + one or more numeric columns, e.g.:\n"
    << "    channel,alpha,alpha_z,theta_beta\n\n"
    << "Typical sources:\n"
    << "  - out_map/bandpowers.csv          (qeeg_map_cli or qeeg_bandpower_cli)\n"
    << "  - out_ratios/bandratios.csv       (qeeg_bandratios_cli)\n"
    << "  - any custom table: channel,<metric1>,<metric2>,...\n\n"
    << "Outputs (in --outdir):\n"
    << "  - region_summary.csv              Wide format (one row per group)\n"
    << "  - region_summary_long.csv         Long format (group,metric,mean,n)\n"
    << "  - region_report.html              Optional HTML table report\n"
    << "  - region_summary_run_meta.json    UI discovery metadata\n\n"
    << "Usage:\n"
    << "  qeeg_region_summary_cli --input out_map/bandpowers.csv --outdir out_regions --html-report\n"
    << "  qeeg_region_summary_cli --input out_ratios/bandratios.csv --metric theta_beta\n"
    << "  qeeg_region_summary_cli --input out_bandpower --metric alpha --html-report\n"
    << "  qeeg_region_summary_cli --input out_bandpower/bandpower_run_meta.json --metric alpha\n\n"
    << "Required:\n"
    << "  --input PATH            CSV/TSV file, *_run_meta.json, or an output directory containing a per-channel table\n\n"
    << "Options:\n"
    << "  --outdir DIR            Output directory (default: out_regions)\n"
    << "  --metric NAME           Include only this metric column (repeatable)\n"
    << "  --exclude NAME          Exclude a metric column (repeatable)\n"
    << "  --html-report           Write region_report.html\n"
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
    } else if (arg == "--metric" && i + 1 < argc) {
      a.metrics.push_back(argv[++i]);
    } else if (arg == "--exclude" && i + 1 < argc) {
      a.exclude.push_back(argv[++i]);
    } else if (arg == "--html-report") {
      a.html_report = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
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

enum class Lobe {
  Frontal,
  Central,
  Parietal,
  Occipital,
  Temporal,
  Unknown,
};

enum class Hemisphere {
  Left,
  Right,
  Midline,
  Unknown,
};

static Hemisphere hemisphere_of(const std::string& norm_ch) {
  // norm_ch is expected to be lowercase.
  if (norm_ch.empty()) return Hemisphere::Unknown;
  if (ends_with(norm_ch, "z")) return Hemisphere::Midline;

  // Find a trailing integer (10-20 / 10-10 style).
  int value = 0;
  int place = 1;
  bool saw_digit = false;
  for (int i = static_cast<int>(norm_ch.size()) - 1; i >= 0; --i) {
    const unsigned char c = static_cast<unsigned char>(norm_ch[static_cast<size_t>(i)]);
    if (std::isdigit(c) == 0) break;
    saw_digit = true;
    value += (static_cast<int>(c - '0')) * place;
    place *= 10;
  }
  if (!saw_digit) return Hemisphere::Unknown;
  return (value % 2) ? Hemisphere::Left : Hemisphere::Right;
}

static Lobe lobe_of(const std::string& norm_ch) {
  // Very lightweight heuristics, intended for common 10-20 / 10-10 labels.
  //
  // Order matters: handle two-letter prefixes before single-letter buckets.
  if (norm_ch.empty()) return Lobe::Unknown;

  if (starts_with(norm_ch, "fp")) return Lobe::Frontal;
  if (starts_with(norm_ch, "af")) return Lobe::Frontal;
  if (starts_with(norm_ch, "ft")) return Lobe::Temporal;     // fronto-temporal
  if (starts_with(norm_ch, "tp")) return Lobe::Temporal;     // temporo-parietal
  if (starts_with(norm_ch, "po")) return Lobe::Occipital;    // parieto-occipital
  if (starts_with(norm_ch, "fc")) return Lobe::Central;      // fronto-central
  if (starts_with(norm_ch, "cp")) return Lobe::Parietal;     // centro-parietal

  // Single-letter prefixes
  if (starts_with(norm_ch, "f")) return Lobe::Frontal;
  if (starts_with(norm_ch, "c")) return Lobe::Central;
  if (starts_with(norm_ch, "p")) return Lobe::Parietal;
  if (starts_with(norm_ch, "o")) return Lobe::Occipital;
  if (starts_with(norm_ch, "t")) return Lobe::Temporal;

  return Lobe::Unknown;
}

static std::string lobe_name(Lobe l) {
  switch (l) {
    case Lobe::Frontal: return "Frontal";
    case Lobe::Central: return "Central";
    case Lobe::Parietal: return "Parietal";
    case Lobe::Occipital: return "Occipital";
    case Lobe::Temporal: return "Temporal";
    default: return "Other";
  }
}

static std::string hemi_name(Hemisphere h) {
  switch (h) {
    case Hemisphere::Left: return "Left";
    case Hemisphere::Right: return "Right";
    case Hemisphere::Midline: return "Midline";
    default: return "Unknown";
  }
}

static std::string hemi_short(Hemisphere h) {
  switch (h) {
    case Hemisphere::Left: return "L";
    case Hemisphere::Right: return "R";
    case Hemisphere::Midline: return "Z";
    default: return "U";
  }
}

static std::string normalize_for_region(const std::string& ch) {
  // normalize_channel_name does several useful cleanups (strip -REF, map T3->T7, ...)
  // but returns a canonical label in a human-friendly case. For our lightweight
  // prefix matching, we just lowercase it.
  return to_lower(normalize_channel_name(ch));
}

static std::vector<std::pair<std::string, std::string>> groups_for_channel(const std::string& ch_norm) {
  const Hemisphere h = hemisphere_of(ch_norm);
  const Lobe l = lobe_of(ch_norm);
  const std::string ln = lobe_name(l);
  const std::string hn = hemi_name(h);
  const std::string lh = ln + "_" + hemi_short(h);

  std::vector<std::pair<std::string, std::string>> g;
  g.reserve(4);
  g.push_back({"all", "All"});
  g.push_back({"lobe", ln});
  g.push_back({"hemisphere", hn});
  g.push_back({"lobe_hemi", lh});
  return g;
}

struct Agg {
  int n_channels{0};
  std::vector<double> sum;
  std::vector<int> n_valid;
};

static void write_csv_wide(const std::string& outpath,
                           const ChannelTable& t,
                           const std::map<std::pair<std::string, std::string>, Agg>& aggs) {
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << std::setprecision(12);

  out << "group_type,group,n_channels";
  for (const auto& m : t.metrics) {
    out << "," << m;
  }
  out << "\n";

  for (const auto& kv : aggs) {
    const auto& key = kv.first;
    const auto& a = kv.second;
    out << key.first << "," << key.second << "," << a.n_channels;
    for (size_t i = 0; i < t.metrics.size(); ++i) {
      double mean = std::numeric_limits<double>::quiet_NaN();
      if (i < a.sum.size() && i < a.n_valid.size() && a.n_valid[i] > 0) {
        mean = a.sum[i] / static_cast<double>(a.n_valid[i]);
      }
      if (std::isfinite(mean)) out << "," << mean;
      else out << ",";
    }
    out << "\n";
  }
}

static void write_csv_long(const std::string& outpath,
                           const ChannelTable& t,
                           const std::map<std::pair<std::string, std::string>, Agg>& aggs) {
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << std::setprecision(12);

  out << "group_type,group,n_channels,metric,mean,n_valid\n";
  for (const auto& kv : aggs) {
    const auto& key = kv.first;
    const auto& a = kv.second;
    for (size_t i = 0; i < t.metrics.size(); ++i) {
      double mean = std::numeric_limits<double>::quiet_NaN();
      int n_valid = 0;
      if (i < a.sum.size() && i < a.n_valid.size()) {
        n_valid = a.n_valid[i];
        if (n_valid > 0) {
          mean = a.sum[i] / static_cast<double>(n_valid);
        }
      }
      out << key.first << "," << key.second << "," << a.n_channels << "," << t.metrics[i] << ",";
      if (std::isfinite(mean)) out << mean;
      out << "," << n_valid << "\n";
    }
  }
}

static void write_html_report(const Args& args,
                              const ChannelTable& t,
                              const std::map<std::pair<std::string, std::string>, Agg>& aggs) {
  const std::string outpath = args.outdir + "/region_report.html";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write: " + outpath);
  out << std::setprecision(12);

  out << "<!doctype html>\n"
      << "<html>\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>qEEG region summary</title>\n"
      << "  <style>\n"
      << "    body{font-family:system-ui,Arial,sans-serif;margin:16px;line-height:1.35}\n"
      << "    code{background:#f5f5f5;padding:2px 4px;border-radius:4px}\n"
      << "    table{border-collapse:collapse;width:100%;margin:12px 0}\n"
      << "    th,td{border:1px solid #ddd;padding:6px 8px;text-align:left;font-size:14px}\n"
      << "    th{background:#fafafa;position:sticky;top:0}\n"
      << "    .mono{font-family:ui-monospace,Menlo,monospace}\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <h1>qEEG region summary</h1>\n"
      << "  <p>Input: <span class=\"mono\">" << svg_escape(args.input_csv) << "</span></p>\n"
      << "  <p>Groups are heuristic (lobe + hemisphere) based on standard 10-20 / 10-10 channel naming.</p>\n";

  out << "  <p>Downloads: "
      << "<a href=\"" << url_escape("region_summary.csv") << "\">region_summary.csv</a> | "
      << "<a href=\"" << url_escape("region_summary_long.csv") << "\">region_summary_long.csv</a>"
      << "</p>\n";

  out << "  <table>\n";
  out << "    <thead><tr><th>Group type</th><th>Group</th><th>Channels</th>";
  for (const auto& m : t.metrics) {
    out << "<th>" << svg_escape(m) << "</th>";
  }
  out << "</tr></thead>\n";
  out << "    <tbody>\n";

  for (const auto& kv : aggs) {
    const auto& key = kv.first;
    const auto& a = kv.second;
    out << "      <tr><td>" << svg_escape(key.first) << "</td><td>" << svg_escape(key.second) << "</td><td>" << a.n_channels << "</td>";
    for (size_t i = 0; i < t.metrics.size(); ++i) {
      double mean = std::numeric_limits<double>::quiet_NaN();
      if (i < a.sum.size() && i < a.n_valid.size() && a.n_valid[i] > 0) {
        mean = a.sum[i] / static_cast<double>(a.n_valid[i]);
      }
      out << "<td>";
      if (std::isfinite(mean)) out << mean;
      out << "</td>";
    }
    out << "</tr>\n";
  }

  out << "    </tbody>\n";
  out << "  </table>\n";
  out << "</body>\n</html>\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.input_csv.empty()) {
      print_help();
      return 2;
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

    const ChannelTable t = read_channel_table(args);
    if (t.channels.size() != t.values.front().size()) {
      throw std::runtime_error("Internal error: channel/value row mismatch");
    }

    // Aggregate per group.
    std::map<std::pair<std::string, std::string>, Agg> aggs;

    for (size_t row = 0; row < t.channels.size(); ++row) {
      const std::string ch_norm = normalize_for_region(t.channels[row]);
      const auto groups = groups_for_channel(ch_norm);

      for (const auto& g : groups) {
        const auto key = std::make_pair(g.first, g.second);
        auto it = aggs.find(key);
        if (it == aggs.end()) {
          Agg a;
          a.sum.assign(t.metrics.size(), 0.0);
          a.n_valid.assign(t.metrics.size(), 0);
          it = aggs.emplace(key, std::move(a)).first;
        }
        Agg& a = it->second;
        a.n_channels += 1;

        for (size_t mi = 0; mi < t.metrics.size(); ++mi) {
          const double v = t.values[mi][row];
          if (!std::isfinite(v)) continue;
          a.sum[mi] += v;
          a.n_valid[mi] += 1;
        }
      }
    }

    const std::string wide_csv = args.outdir + "/region_summary.csv";
    const std::string long_csv = args.outdir + "/region_summary_long.csv";
    write_csv_wide(wide_csv, t, aggs);
    write_csv_long(long_csv, t, aggs);

    if (args.html_report) {
      write_html_report(args, t, aggs);
    }

    // Run meta (for qeeg_ui_* discovery).
    {
      std::vector<std::string> outs;
      outs.push_back("region_summary.csv");
      outs.push_back("region_summary_long.csv");
      if (args.html_report) outs.push_back("region_report.html");
      const std::string meta = args.outdir + "/region_summary_run_meta.json";
      outs.push_back("region_summary_run_meta.json");
      (void)write_run_meta_json(meta, "qeeg_region_summary_cli", args.outdir, args.input_csv, outs);
    }

    std::cout << "Wrote: " << wide_csv << "\n";
    std::cout << "Wrote: " << long_csv << "\n";
    if (args.html_report) {
      std::cout << "Wrote: " << (args.outdir + "/region_report.html") << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
