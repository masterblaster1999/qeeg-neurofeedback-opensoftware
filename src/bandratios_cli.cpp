#include "qeeg/csv_io.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace qeeg;

namespace {

struct RatioSpec {
  // Output column name.
  std::string name;
  // Numerator/denominator band column names as referenced in bandpowers.csv.
  std::string num;
  std::string den;
};

struct Args {
  std::string bandpowers_path;
  std::string outdir{"out_bandratios"};

  // Repeated: NAME=NUM/DEN or NUM/DEN.
  std::vector<std::string> ratio_specs;

  bool log10{false};
  bool write_tsv{false};

  // Avoid division-by-zero and log10(0).
  double eps{1e-20};
};

static void print_help() {
  std::cout
      << "qeeg_bandratios_cli\n\n"
      << "Derive common neurofeedback band ratios from a bandpowers.csv table\n"
      << "(as produced by qeeg_map_cli or qeeg_bandpower_cli).\n\n"
      << "Usage:\n"
      << "  qeeg_bandratios_cli --bandpowers out_bp/bandpowers.csv --outdir out_ratios --ratio theta/beta\n"
      << "  qeeg_bandratios_cli --bandpowers out_bp --outdir out_ratios --ratio theta/beta\n"
      << "  qeeg_bandratios_cli --input out_bp/bandpower_run_meta.json --outdir out_ratios --ratio tbr=theta/beta --log10 --tsv\n\n"
      << "Options:\n"
      << "  --bandpowers SPEC        Input bandpowers table (CSV/TSV file, *_run_meta.json, or a directory containing bandpowers.*). Alias: --input\n"
      << "  --outdir DIR             Output directory (default: out_bandratios)\n"
      << "  --ratio SPEC             Ratio spec (repeatable).\n"
      << "                          Formats: NUM/DEN  or  NAME=NUM/DEN\n"
      << "                          Example: theta/beta  or  tbr=theta/beta\n"
      << "  --log10                  Apply log10(max(eps, ratio)) to ratio columns\n"
      << "  --eps X                  Small epsilon for den==0 and log10(0) (default: 1e-20)\n"
      << "  --tsv                    Also write a tab-delimited bandratios.tsv\n"
      << "  -h, --help               Show this help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if ((arg == "--bandpowers" || arg == "--input") && i + 1 < argc) {
      a.bandpowers_path = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--ratio" && i + 1 < argc) {
      a.ratio_specs.push_back(argv[++i]);
    } else if (arg == "--log10") {
      a.log10 = true;
    } else if (arg == "--tsv") {
      a.write_tsv = true;
    } else if (arg == "--eps" && i + 1 < argc) {
      a.eps = to_double(argv[++i]);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string sanitize_col(std::string s) {
  s = trim(s);
  if (s.empty()) return s;
  for (char& c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) c = '_';
  }
  // Avoid leading digits.
  if (!s.empty() && (s[0] >= '0' && s[0] <= '9')) s = std::string("r_") + s;
  return s;
}

static RatioSpec parse_ratio_spec(const std::string& spec_raw) {
  const std::string s = trim(spec_raw);
  if (s.empty()) throw std::runtime_error("Empty --ratio spec");

  std::string name_part;
  std::string expr = s;
  const size_t eq = s.find('=');
  if (eq != std::string::npos) {
    name_part = trim(s.substr(0, eq));
    expr = trim(s.substr(eq + 1));
  }

  const size_t slash = expr.find('/');
  if (slash == std::string::npos) {
    throw std::runtime_error("Invalid --ratio spec (expected NUM/DEN): " + s);
  }

  RatioSpec r;
  r.num = trim(expr.substr(0, slash));
  r.den = trim(expr.substr(slash + 1));
  if (r.num.empty() || r.den.empty()) {
    throw std::runtime_error("Invalid --ratio spec (empty numerator/denominator): " + s);
  }
  if (!name_part.empty()) {
    r.name = sanitize_col(name_part);
  } else {
    r.name = sanitize_col(r.num) + "_over_" + sanitize_col(r.den);
  }
  if (r.name.empty()) {
    throw std::runtime_error("Invalid --ratio spec (empty name): " + s);
  }
  return r;
}

static bool try_parse_double(const std::string& s, double* out) {
  if (!out) return false;
  const std::string t = trim(s);
  if (t.empty()) {
    *out = std::numeric_limits<double>::quiet_NaN();
    return false;
  }
  try {
    size_t idx = 0;
    const double v = std::stod(t, &idx);
    if (idx == 0) {
      *out = std::numeric_limits<double>::quiet_NaN();
      return false;
    }
    *out = v;
    return true;
  } catch (...) {
    *out = std::numeric_limits<double>::quiet_NaN();
    return false;
  }
}

static void write_bandratios_sidecar_json(const Args& args,
                                         const std::vector<RatioSpec>& ratios) {
  const std::string outpath = args.outdir + "/bandratios.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write bandratios.json: " + outpath);

  // Mirrors the BIDS *_events.json convention: top-level keys match CSV columns.
  const bool lg = args.log10;
  const std::string units = lg ? "log10(n/a)" : "n/a";

  auto write_entry = [&](bool* first,
                         const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units_field) {
    if (!*first) out << ",\n";
    *first = false;
    out << "  \"" << json_escape(key) << "\": {\n";
    out << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    out << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units_field.empty()) {
      out << ",\n    \"Units\": \"" << json_escape(units_field) << "\"";
    }
    out << "\n  }";
  };

  out << "{\n";
  bool first = true;
  write_entry(&first,
              "channel",
              "Channel label",
              "EEG channel label (one row per channel).",
              "");

  for (const auto& r : ratios) {
    std::string desc = "Ratio computed from bandpowers.csv columns: (" + r.num + ") / (" + r.den + ").";
    if (lg) desc += " Values are log10-transformed.";
    write_entry(&first,
                r.name,
                r.name + " band ratio",
                desc,
                units);
  }

  out << "\n}\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.bandpowers_path.empty()) {
      print_help();
      throw std::runtime_error("--bandpowers is required");
    }

    // Allow --bandpowers/--input to be either a file, a *_run_meta.json, or a directory
    // containing a bandpowers table (for easy CLI chaining).
    ResolveInputTableOptions opt;
    opt.preferred_filenames = {"bandpowers.csv", "bandpowers.tsv"};
    opt.preferred_contains = {"bandpower", "bandpowers"};
    const ResolvedInputPath resolved = resolve_input_table_path(args.bandpowers_path, opt);
    if (!resolved.note.empty()) {
      std::cerr << resolved.note << "\n";
    }
    const std::string bandpowers_path = resolved.path;
    if (args.eps <= 0.0) {
      throw std::runtime_error("--eps must be > 0");
    }

    std::vector<RatioSpec> ratios;
    if (args.ratio_specs.empty()) {
      // Safe, common defaults.
      ratios.push_back(parse_ratio_spec("theta/beta"));
      ratios.push_back(parse_ratio_spec("alpha/theta"));
    } else {
      for (const auto& s : args.ratio_specs) {
        ratios.push_back(parse_ratio_spec(s));
      }
    }

    // De-duplicate names if needed (user may repeat specs).
    {
      std::unordered_map<std::string, int> seen;
      for (auto& r : ratios) {
        const std::string base = r.name;
        int& count = seen[base];
        ++count;
        if (count > 1) {
          r.name = base + "_" + std::to_string(count);
        }
      }
    }

    ensure_directory(args.outdir);

    std::ifstream in(std::filesystem::u8path(bandpowers_path), std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open bandpowers CSV: " + bandpowers_path);

    std::string header_line;
    if (!std::getline(in, header_line)) {
      throw std::runtime_error("Empty bandpowers CSV: " + bandpowers_path);
    }
    if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
    const std::vector<std::string> header_raw = split_csv_row(header_line, ',');
    if (header_raw.empty()) {
      throw std::runtime_error("Failed to parse header row in: " + bandpowers_path);
    }

    std::vector<std::string> header;
    header.reserve(header_raw.size());
    for (const auto& f : header_raw) header.push_back(strip_utf8_bom(trim(f)));

    std::unordered_map<std::string, size_t> col_index;
    col_index.reserve(header.size());
    for (size_t i = 0; i < header.size(); ++i) {
      const std::string key = to_lower(trim(header[i]));
      if (key.empty()) continue;
      // Keep first occurrence.
      if (col_index.find(key) == col_index.end()) col_index[key] = i;
    }

    auto find_col = [&](const std::string& name) -> size_t {
      const std::string key = to_lower(trim(name));
      auto it = col_index.find(key);
      if (it == col_index.end()) {
        throw std::runtime_error("Missing required column in bandpowers.csv: '" + name + "'");
      }
      return it->second;
    };

    const size_t channel_idx = find_col("channel");

    struct RatioCols {
      RatioSpec spec;
      size_t num_idx{0};
      size_t den_idx{0};
    };
    std::vector<RatioCols> rcols;
    rcols.reserve(ratios.size());
    for (const auto& r : ratios) {
      RatioCols rc;
      rc.spec = r;
      rc.num_idx = find_col(r.num);
      rc.den_idx = find_col(r.den);
      rcols.push_back(rc);
    }

    const std::string out_csv = args.outdir + "/bandratios.csv";
    std::ofstream out(std::filesystem::u8path(out_csv), std::ios::binary);
    if (!out) throw std::runtime_error("Failed to write: " + out_csv);
    out << std::setprecision(12);

    // Header
    out << "channel";
    for (const auto& rc : rcols) {
      out << "," << rc.spec.name;
    }
    out << "\n";

    // Rows
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim(line);
      if (t.empty()) continue;
      if (starts_with(t, "#") || starts_with(t, "//")) continue;

      std::vector<std::string> fields = split_csv_row(line, ',');
      if (fields.size() < header.size()) fields.resize(header.size());

      const std::string channel = (channel_idx < fields.size()) ? fields[channel_idx] : std::string();
      out << csv_escape(channel);

      for (const auto& rc : rcols) {
        double num = std::numeric_limits<double>::quiet_NaN();
        double den = std::numeric_limits<double>::quiet_NaN();
        if (rc.num_idx < fields.size()) try_parse_double(fields[rc.num_idx], &num);
        if (rc.den_idx < fields.size()) try_parse_double(fields[rc.den_idx], &den);

        double r = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(num) && std::isfinite(den) && std::fabs(den) > args.eps) {
          r = num / den;
          if (args.log10) {
            r = std::log10(std::max(args.eps, r));
          }
        }
        out << "," << r;
      }
      out << "\n";
    }

    // Sidecar JSON
    write_bandratios_sidecar_json(args, ratios);

    // Optional TSV (BIDS-friendly)
    std::vector<std::string> outs;
    outs.push_back("bandratios.csv");
    outs.push_back("bandratios.json");

    if (args.write_tsv) {
      const std::string out_tsv = args.outdir + "/bandratios.tsv";
      convert_csv_file_to_tsv(out_csv, out_tsv);
      outs.push_back("bandratios.tsv");
    }

    // Run manifest for qeeg_ui_cli / qeeg_ui_server_cli
    {
      const std::string meta_path = args.outdir + "/bandratios_run_meta.json";
      outs.push_back("bandratios_run_meta.json");
      if (!write_run_meta_json(meta_path, "qeeg_bandratios_cli", args.outdir, bandpowers_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    }

    std::cout << "Wrote: " << out_csv << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
