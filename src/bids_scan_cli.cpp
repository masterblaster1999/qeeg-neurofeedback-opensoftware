#include "qeeg/bids.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string dataset_path;
  std::string outdir{"out_bids_scan"};

  // If true, also index files found under <dataset>/derivatives.
  bool include_derivatives{false};

  // If true, return a non-zero exit code when ANY warnings are found.
  // Errors always cause a non-zero exit code.
  bool strict{false};

  // 0 => unlimited.
  std::size_t max_files{0};
};

static void print_help() {
  std::cout
      << "qeeg_bids_scan_cli (lightweight BIDS-EEG index + sanity checks)\n\n"
      << "This tool scans a BIDS dataset for EEG recordings (EDF/BDF/BrainVision) and\n"
      << "writes a machine-readable index plus a small human-readable report.\n\n"
      << "This is NOT a full BIDS validator. It performs a few high-signal checks:\n"
      << "  - dataset_description.json exists and contains Name/BIDSVersion\n"
      << "  - EEG recordings follow sub-*/[ses-*]/eeg/*_eeg.<ext> layout\n"
      << "  - Sidecar files exist (eeg.json / channels.tsv / events.tsv/json)\n"
      << "  - eeg.json contains required EEG keys (best-effort string search)\n"
      << "  - channels.tsv has required columns (name, type, units) in order\n"
      << "  - events.tsv (if present) includes required columns (onset, duration)\n\n"
      << "Outputs (under --outdir):\n"
      << "  bids_index.json\n"
      << "  bids_index.csv\n"
      << "  bids_scan_report.txt\n"
      << "  bids_scan_run_meta.json\n\n"
      << "Usage:\n"
      << "  qeeg_bids_scan_cli --dataset /path/to/bids --outdir out_bids_scan\n"
      << "  qeeg_bids_scan_cli --dataset /path/to/bids/sub-01 --strict\n\n"
      << "Options:\n"
      << "  --dataset PATH             Dataset root (or any path inside the dataset)\n"
      << "  --outdir DIR               Output directory (default: out_bids_scan)\n"
      << "  --include-derivatives       Also scan <dataset>/derivatives\n"
      << "  --max-files N              Stop after indexing N recordings (0 = unlimited)\n"
      << "  --strict                   Exit non-zero if any warnings are found\n"
      << "  -h, --help                 Show help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--dataset" && i + 1 < argc) {
      a.dataset_path = argv[++i];
    } else if ((arg == "--outdir" || arg == "--out-dir") && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--include-derivatives") {
      a.include_derivatives = true;
    } else if (arg == "--strict") {
      a.strict = true;
    } else if (arg == "--max-files" && i + 1 < argc) {
      a.max_files = static_cast<std::size_t>(to_int(argv[++i]));
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  if (trim(a.dataset_path).empty()) {
    throw std::runtime_error("--dataset is required (use --help for usage)");
  }
  return a;
}

static std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream oss;
  oss << f.rdbuf();
  return oss.str();
}

static bool json_has_key(const std::string& text, const std::string& key) {
  if (text.empty() || key.empty()) return false;
  const std::string needle = "\"" + key + "\"";
  return text.find(needle) != std::string::npos;
}

static std::vector<std::string> split_header_cols(std::string header) {
  header = strip_utf8_bom(header);
  // Prefer tab splitting if any tabs exist.
  if (header.find('\t') != std::string::npos) {
    return split(header, '\t');
  }
  // Fallback: split on runs of whitespace.
  std::vector<std::string> cols;
  std::string cur;
  for (char c : header) {
    if (c == ' ' || c == '\r' || c == '\n') {
      if (!cur.empty()) {
        cols.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) cols.push_back(cur);
  return cols;
}

static bool is_supported_eeg_data_file(const std::filesystem::path& p) {
  const std::string name = to_lower(p.filename().u8string());
  // Keep this intentionally small: the project supports EDF/BDF/BrainVision.
  const bool is_data = ends_with(name, ".edf") || ends_with(name, ".bdf") || ends_with(name, ".vhdr");
  if (!is_data) return false;

  // Basic suffix check: must contain "_eeg" in the stem.
  const std::string stem = to_lower(p.stem().u8string());
  return stem.size() >= 4 && ends_with(stem, "_eeg");
}

static std::string guess_format_from_extension(const std::filesystem::path& p) {
  const std::string ext = to_lower(p.extension().u8string());
  if (ext == ".edf") return "EDF";
  if (ext == ".bdf") return "BDF";
  if (ext == ".vhdr") return "BrainVision";
  return "Unknown";
}

static std::string safe_relative_u8(const std::filesystem::path& p,
                                   const std::filesystem::path& root) {
  std::error_code ec;
  auto rel = std::filesystem::relative(p, root, ec);
  if (ec) return p.u8string();
  return rel.u8string();
}

struct FoundRecording {
  std::string rel_path;
  BidsEntities ent;
  std::string format;
  bool has_eeg_json{false};
  bool has_channels_tsv{false};
  bool has_events_tsv{false};
  bool has_events_json{false};
  bool has_electrodes_tsv{false};
  bool has_coordsystem_json{false};
  bool has_brainvision_triplet{true};
  std::vector<std::string> issues; // warnings + errors (prefixed)
};

static void write_index_json(const std::string& path,
                            const std::string& dataset_root,
                            const std::vector<FoundRecording>& recs,
                            const std::vector<std::string>& warnings,
                            const std::vector<std::string>& errors) {
  std::ofstream o(path, std::ios::binary);
  if (!o) throw std::runtime_error("Failed to write: " + path);

  auto emit_string_array = [&](const char* key, const std::vector<std::string>& arr) {
    o << "  \"" << key << "\": [\n";
    for (size_t i = 0; i < arr.size(); ++i) {
      o << "    \"" << json_escape(arr[i]) << "\"";
      if (i + 1 < arr.size()) o << ",";
      o << "\n";
    }
    o << "  ]";
  };

  o << "{\n";
  o << "  \"DatasetRoot\": \"" << json_escape(dataset_root) << "\",\n";
  o << "  \"GeneratedAtUTC\": \"" << json_escape(now_string_utc()) << "\",\n";
  o << "  \"Recordings\": [\n";
  for (size_t i = 0; i < recs.size(); ++i) {
    const auto& r = recs[i];
    o << "    {\n";
    o << "      \"Path\": \"" << json_escape(r.rel_path) << "\",\n";
    o << "      \"Format\": \"" << json_escape(r.format) << "\",\n";
    o << "      \"Entities\": {\n";
    o << "        \"sub\": \"" << json_escape(r.ent.sub) << "\",\n";
    o << "        \"ses\": \"" << json_escape(r.ent.ses) << "\",\n";
    o << "        \"task\": \"" << json_escape(r.ent.task) << "\",\n";
    o << "        \"acq\": \"" << json_escape(r.ent.acq) << "\",\n";
    o << "        \"run\": \"" << json_escape(r.ent.run) << "\"\n";
    o << "      },\n";
    o << "      \"Sidecars\": {\n";
    o << "        \"eeg_json\": " << (r.has_eeg_json ? "true" : "false") << ",\n";
    o << "        \"channels_tsv\": " << (r.has_channels_tsv ? "true" : "false") << ",\n";
    o << "        \"events_tsv\": " << (r.has_events_tsv ? "true" : "false") << ",\n";
    o << "        \"events_json\": " << (r.has_events_json ? "true" : "false") << ",\n";
    o << "        \"electrodes_tsv\": " << (r.has_electrodes_tsv ? "true" : "false") << ",\n";
    o << "        \"coordsystem_json\": " << (r.has_coordsystem_json ? "true" : "false") << "\n";
    o << "      },\n";
    o << "      \"BrainVisionTripletOK\": " << (r.has_brainvision_triplet ? "true" : "false") << ",\n";
    o << "      \"Issues\": [\n";
    for (size_t j = 0; j < r.issues.size(); ++j) {
      o << "        \"" << json_escape(r.issues[j]) << "\"";
      if (j + 1 < r.issues.size()) o << ",";
      o << "\n";
    }
    o << "      ]\n";
    o << "    }";
    if (i + 1 < recs.size()) o << ",";
    o << "\n";
  }
  o << "  ],\n";
  emit_string_array("Warnings", warnings);
  o << ",\n";
  emit_string_array("Errors", errors);
  o << "\n";
  o << "}\n";
}

static void write_index_csv(const std::string& path,
                            const std::vector<FoundRecording>& recs) {
  std::ofstream o(path, std::ios::binary);
  if (!o) throw std::runtime_error("Failed to write: " + path);
  o << "path,format,sub,ses,task,acq,run,eeg_json,channels_tsv,events_tsv,events_json,electrodes_tsv,coordsystem_json,issues\n";
  for (const auto& r : recs) {
    auto b = [&](bool x) { return x ? "1" : "0"; };
    o << '"' << json_escape(r.rel_path) << '"' << ','
      << r.format << ','
      << r.ent.sub << ','
      << r.ent.ses << ','
      << r.ent.task << ','
      << r.ent.acq << ','
      << r.ent.run << ','
      << b(r.has_eeg_json) << ','
      << b(r.has_channels_tsv) << ','
      << b(r.has_events_tsv) << ','
      << b(r.has_events_json) << ','
      << b(r.has_electrodes_tsv) << ','
      << b(r.has_coordsystem_json) << ',';

    std::string joined;
    for (size_t i = 0; i < r.issues.size(); ++i) {
      if (i) joined += " | ";
      joined += r.issues[i];
    }
    o << '"' << json_escape(joined) << '"' << "\n";
  }
}

static void write_report_txt(const std::string& path,
                             const std::string& dataset_root,
                             const std::vector<FoundRecording>& recs,
                             const std::vector<std::string>& warnings,
                             const std::vector<std::string>& errors) {
  std::ofstream o(path, std::ios::binary);
  if (!o) throw std::runtime_error("Failed to write: " + path);

  o << "qeeg_bids_scan_cli report\n";
  o << "Generated (UTC): " << now_string_utc() << "\n";
  o << "Dataset root: " << dataset_root << "\n\n";

  o << "Found recordings: " << recs.size() << "\n";
  o << "Warnings: " << warnings.size() << "\n";
  o << "Errors: " << errors.size() << "\n\n";

  if (!errors.empty()) {
    o << "Errors:\n";
    for (const auto& e : errors) o << "  - " << e << "\n";
    o << "\n";
  }
  if (!warnings.empty()) {
    o << "Warnings:\n";
    for (const auto& w : warnings) o << "  - " << w << "\n";
    o << "\n";
  }

  o << "Per-recording details:\n";
  for (const auto& r : recs) {
    o << "\n== " << r.rel_path << " ==\n";
    o << "  format: " << r.format << "\n";
    o << "  sub=" << r.ent.sub;
    if (!r.ent.ses.empty()) o << " ses=" << r.ent.ses;
    o << " task=" << r.ent.task;
    if (!r.ent.acq.empty()) o << " acq=" << r.ent.acq;
    if (!r.ent.run.empty()) o << " run=" << r.ent.run;
    o << "\n";
    o << "  sidecars: eeg.json=" << (r.has_eeg_json ? "yes" : "no")
      << " channels.tsv=" << (r.has_channels_tsv ? "yes" : "no")
      << " events.tsv=" << (r.has_events_tsv ? "yes" : "no")
      << " events.json=" << (r.has_events_json ? "yes" : "no")
      << " electrodes.tsv=" << (r.has_electrodes_tsv ? "yes" : "no")
      << " coordsystem.json=" << (r.has_coordsystem_json ? "yes" : "no")
      << "\n";
    if (r.format == "BrainVision") {
      o << "  brainvision_triplet: " << (r.has_brainvision_triplet ? "ok" : "MISSING") << "\n";
    }

    if (!r.issues.empty()) {
      o << "  issues:\n";
      for (const auto& s : r.issues) o << "    - " << s << "\n";
    }
  }
}

static void push_issue(std::vector<std::string>* global_list,
                       std::vector<std::string>* per_file,
                       const std::string& prefix,
                       const std::string& msg) {
  const std::string full = prefix + msg;
  if (global_list) global_list->push_back(msg);
  if (per_file) per_file->push_back(full);
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    const auto maybe_root = find_bids_dataset_root(args.dataset_path);
    if (!maybe_root) {
      std::cerr << "Error: could not find dataset_description.json above --dataset path\n";
      return 2;
    }
    const std::string dataset_root = *maybe_root;
    const std::filesystem::path root = std::filesystem::u8path(dataset_root);

    ensure_directory(args.outdir);
    const std::filesystem::path outdir = std::filesystem::u8path(args.outdir);
    const std::string index_json = (outdir / "bids_index.json").u8string();
    const std::string index_csv = (outdir / "bids_index.csv").u8string();
    const std::string report_txt = (outdir / "bids_scan_report.txt").u8string();
    const std::string run_meta = (outdir / "bids_scan_run_meta.json").u8string();

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<FoundRecording> found;

    // ---- dataset_description.json sanity ----
    const std::filesystem::path dd = root / "dataset_description.json";
    if (!std::filesystem::exists(dd)) {
      errors.push_back("dataset_description.json is missing at dataset root");
    } else {
      const std::string dd_text = read_text_file(dd);
      if (!json_has_key(dd_text, "Name")) {
        warnings.push_back("dataset_description.json is missing required key: Name");
      }
      if (!json_has_key(dd_text, "BIDSVersion")) {
        warnings.push_back("dataset_description.json is missing required key: BIDSVersion");
      }
    }

    // ---- scan for EEG recordings ----
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied);
    for (const auto& ent : it) {
      if (!ent.is_regular_file()) continue;
      const auto p = ent.path();

      // Skip derivatives unless explicitly included.
      if (!args.include_derivatives) {
        auto rel = safe_relative_u8(p, root);
        if (starts_with(to_lower(rel), "derivatives/")) continue;
      }

      if (!is_supported_eeg_data_file(p)) continue;

      FoundRecording rec;
      rec.rel_path = safe_relative_u8(p, root);
      rec.format = guess_format_from_extension(p);

      // Extension case sensitivity note (BIDS discourages .EDF/.BDF).
      const std::string ext_raw = p.extension().u8string();
      if (!ext_raw.empty() && std::any_of(ext_raw.begin(), ext_raw.end(),
                                          [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; })) {
        push_issue(&warnings, &rec.issues, "[WARN] ", "Uppercase extension used: '" + ext_raw + "'");
      }

      // Parse entities from the filename.
      std::optional<BidsParsedFilename> parsed;
      try {
        parsed = parse_bids_filename(p.filename().u8string());
      } catch (const std::exception& e) {
        push_issue(&errors, &rec.issues, "[ERROR] ",
                   std::string("Invalid BIDS entities in filename: ") + e.what());
      }

      if (!parsed) {
        push_issue(&errors, &rec.issues, "[ERROR] ",
                   "Could not parse required BIDS entities (sub/task) from filename");
      } else {
        rec.ent = parsed->ent;
        if (parsed->suffix != "eeg") {
          push_issue(&warnings, &rec.issues, "[WARN] ",
                     "Filename suffix is not 'eeg' (parsed suffix='" + parsed->suffix + "')");
        }
      }

      // Check expected directory placement: sub-*/[ses-*]/eeg/*.ext
      {
        std::filesystem::path relp = std::filesystem::u8path(rec.rel_path);
        std::vector<std::string> parts;
        for (const auto& comp : relp) {
          parts.push_back(comp.u8string());
        }

        // Expect at least: sub-xx / eeg / file
        if (parts.size() >= 3) {
          const std::string d0 = parts[0];
          if (!starts_with(d0, "sub-")) {
            push_issue(&warnings, &rec.issues, "[WARN] ", "File is not under a sub-* directory");
          } else if (parsed) {
            const std::string sub_dir = d0.substr(4);
            if (!sub_dir.empty() && sub_dir != rec.ent.sub) {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "Subject label in directory ('" + sub_dir + "') does not match filename ('" + rec.ent.sub + "')");
            }
          }

          // If a ses-* directory exists, it should match.
          std::size_t idx = 1;
          if (parts.size() >= 4 && starts_with(parts[1], "ses-")) {
            const std::string ses_dir = parts[1].substr(4);
            if (parsed && !ses_dir.empty() && !rec.ent.ses.empty() && ses_dir != rec.ent.ses) {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "Session label in directory ('" + ses_dir + "') does not match filename ('" + rec.ent.ses + "')");
            }
            idx = 2;
          }
          if (idx < parts.size() - 1 && parts[idx] != "eeg") {
            push_issue(&warnings, &rec.issues, "[WARN] ",
                       "Expected file under an 'eeg/' folder (found under '" + parts[idx] + "')");
          }
        } else {
          push_issue(&warnings, &rec.issues, "[WARN] ", "Unexpected shallow path (not under sub-*/eeg)");
        }
      }

      // Sidecar checks (best-effort).
      if (parsed) {
        const std::string eeg_stem = format_bids_filename_stem(rec.ent, "eeg");
        const std::string channels_stem = format_bids_filename_stem(rec.ent, "channels");
        const std::string events_stem = format_bids_filename_stem(rec.ent, "events");
        const std::string electrodes_stem = format_bids_filename_stem(rec.ent, "electrodes");
        const std::string coordsystem_stem = format_bids_filename_stem(rec.ent, "coordsystem");

        const auto eeg_json_path = p.parent_path() / (eeg_stem + ".json");
        const auto channels_tsv_path = p.parent_path() / (channels_stem + ".tsv");
        const auto events_tsv_path = p.parent_path() / (events_stem + ".tsv");
        const auto events_json_path = p.parent_path() / (events_stem + ".json");
        const auto electrodes_tsv_path = p.parent_path() / (electrodes_stem + ".tsv");
        const auto coordsystem_json_path = p.parent_path() / (coordsystem_stem + ".json");

        rec.has_eeg_json = std::filesystem::exists(eeg_json_path);
        rec.has_channels_tsv = std::filesystem::exists(channels_tsv_path);
        rec.has_events_tsv = std::filesystem::exists(events_tsv_path);
        rec.has_events_json = std::filesystem::exists(events_json_path);
        rec.has_electrodes_tsv = std::filesystem::exists(electrodes_tsv_path);
        rec.has_coordsystem_json = std::filesystem::exists(coordsystem_json_path);

        if (!rec.has_eeg_json) {
          push_issue(&warnings, &rec.issues, "[WARN] ", "Missing required sidecar: " + eeg_json_path.filename().u8string());
        } else {
          const std::string eeg_text = read_text_file(eeg_json_path);
          const std::vector<std::string> required_keys = {
              "EEGReference", "SamplingFrequency", "PowerLineFrequency", "SoftwareFilters"};
          for (const auto& k : required_keys) {
            if (!json_has_key(eeg_text, k)) {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "eeg.json appears to be missing required key: " + k);
            }
          }
        }

        if (!rec.has_channels_tsv) {
          // channels.tsv is RECOMMENDED by the BIDS EEG spec, but many tools rely on it.
          push_issue(&warnings, &rec.issues, "[WARN] ", "Missing recommended sidecar: " + channels_tsv_path.filename().u8string());
        } else {
          std::ifstream f(channels_tsv_path, std::ios::binary);
          std::string header;
          std::getline(f, header);
          const auto cols = split_header_cols(header);
          if (cols.size() < 3) {
            push_issue(&warnings, &rec.issues, "[WARN] ",
                       "channels.tsv header has fewer than 3 columns (expected name,type,units)");
          } else {
            if (cols[0] != "name" || cols[1] != "type" || cols[2] != "units") {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "channels.tsv first columns should be: name<TAB>type<TAB>units");
            }
          }
        }

        if (rec.has_events_tsv) {
          std::ifstream f(events_tsv_path, std::ios::binary);
          std::string header;
          std::getline(f, header);
          const auto cols = split_header_cols(header);
          auto find_col = [&](const std::string& name) -> int {
            for (size_t ci = 0; ci < cols.size(); ++ci) {
              if (cols[ci] == name) return static_cast<int>(ci);
            }
            return -1;
          };
          const int i_onset = find_col("onset");
          const int i_duration = find_col("duration");
          if (cols.size() < 2) {
            push_issue(&warnings, &rec.issues, "[WARN] ",
                       "events.tsv header has fewer than 2 columns (expected onset\t duration)");
          } else {
            if (i_onset < 0 || i_duration < 0) {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "events.tsv is missing required columns: onset and/or duration");
            } else if (!(i_onset == 0 && i_duration == 1)) {
              push_issue(&warnings, &rec.issues, "[WARN] ",
                         "events.tsv recommended first columns are: onset\t duration");
            }
          }

          bool has_extra_cols = false;
          for (const auto& c : cols) {
            if (c.empty()) continue;
            if (c == "onset" || c == "duration") continue;
            has_extra_cols = true;
            break;
          }
          if (has_extra_cols && !rec.has_events_json) {
            // events.json is not strictly required by BIDS, but it is recommended when additional columns exist.
            push_issue(&warnings, &rec.issues, "[WARN] ",
                       "events.tsv has additional columns but events.json is missing (consider adding column descriptions)");
          }
        }

        if (rec.has_electrodes_tsv && !rec.has_coordsystem_json) {
          // The EEG spec states coordsystem.json MUST accompany electrodes.tsv.
          push_issue(&errors, &rec.issues, "[ERROR] ",
                     "electrodes.tsv exists but coordsystem.json is missing (required by BIDS EEG)");
        }
      }

      // BrainVision triplet check (.vhdr/.vmrk/.eeg)
      if (rec.format == "BrainVision") {
        const std::filesystem::path base = p;
        const std::filesystem::path vmrk = base;
        const std::filesystem::path eeg = base;
        const auto vmrk_path = vmrk.parent_path() / (vmrk.stem().u8string() + ".vmrk");
        const auto eeg_path = eeg.parent_path() / (eeg.stem().u8string() + ".eeg");
        if (!std::filesystem::exists(vmrk_path) || !std::filesystem::exists(eeg_path)) {
          rec.has_brainvision_triplet = false;
          push_issue(&warnings, &rec.issues, "[WARN] ",
                     "BrainVision .vhdr found but .vmrk/.eeg file is missing");
        }
      }

      found.push_back(rec);
      if (args.max_files > 0 && found.size() >= args.max_files) {
        warnings.push_back("Stopped early due to --max-files");
        break;
      }
    }

    // Sort results for stability.
    std::sort(found.begin(), found.end(), [](const FoundRecording& a, const FoundRecording& b) {
      return a.rel_path < b.rel_path;
    });

    write_index_json(index_json, dataset_root, found, warnings, errors);
    write_index_csv(index_csv, found);
    write_report_txt(report_txt, dataset_root, found, warnings, errors);

    // Run meta for UI.
    write_run_meta_json(run_meta,
                        "qeeg_bids_scan_cli",
                        args.outdir,
                        dataset_root,
                        {"bids_index.json", "bids_index.csv", "bids_scan_report.txt"});

    std::cout << "Dataset root: " << dataset_root << "\n";
    std::cout << "Found recordings: " << found.size() << "\n";
    std::cout << "Warnings: " << warnings.size() << "\n";
    std::cout << "Errors: " << errors.size() << "\n";
    std::cout << "Wrote: " << index_json << "\n";
    std::cout << "Wrote: " << report_txt << "\n";

    if (!errors.empty()) return 2;
    if (args.strict && !warnings.empty()) return 1;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return 2;
  }
}
