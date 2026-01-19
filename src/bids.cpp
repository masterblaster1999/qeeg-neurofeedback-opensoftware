#include "qeeg/bids.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace qeeg {

static inline bool is_alnum(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

static std::string tsv_sanitize(std::string s) {
  // BIDS TSVs are plain tab-separated, without CSV-style quoting.
  // Replace tabs/newlines with spaces to keep the table well-formed.
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

bool is_valid_bids_label(const std::string& label) {
  if (label.empty()) return false;
  for (char c : label) {
    if (!is_alnum(c)) return false;
  }
  return true;
}

std::string format_bids_entity_chain(const BidsEntities& ent) {
  if (!is_valid_bids_label(ent.sub)) {
    throw std::runtime_error("BIDS: invalid subject label (use only letters/digits): '" + ent.sub + "'");
  }
  if (!is_valid_bids_label(ent.task)) {
    throw std::runtime_error("BIDS: invalid task label (use only letters/digits): '" + ent.task + "'");
  }
  if (!ent.ses.empty() && !is_valid_bids_label(ent.ses)) {
    throw std::runtime_error("BIDS: invalid session label (use only letters/digits): '" + ent.ses + "'");
  }
  if (!ent.acq.empty() && !is_valid_bids_label(ent.acq)) {
    throw std::runtime_error("BIDS: invalid acquisition label (use only letters/digits): '" + ent.acq + "'");
  }
  if (!ent.run.empty() && !is_valid_bids_label(ent.run)) {
    throw std::runtime_error("BIDS: invalid run label (use only letters/digits): '" + ent.run + "'");
  }

  // Ordering follows the EEG file templates in the BIDS specification:
  // sub-<label>[_ses-<label>]_task-<label>[_acq-<label>][_run-<index>]
  std::ostringstream oss;
  oss << "sub-" << ent.sub;
  if (!ent.ses.empty()) oss << "_ses-" << ent.ses;
  oss << "_task-" << ent.task;
  if (!ent.acq.empty()) oss << "_acq-" << ent.acq;
  if (!ent.run.empty()) oss << "_run-" << ent.run;
  return oss.str();
}

std::string format_bids_filename_stem(const BidsEntities& ent, const std::string& suffix) {
  if (suffix.empty()) throw std::runtime_error("BIDS: suffix must not be empty");
  return format_bids_entity_chain(ent) + "_" + suffix;
}

std::optional<BidsParsedFilename> parse_bids_filename(const std::string& filename_or_stem) {
  if (trim(filename_or_stem).empty()) return std::nullopt;

  // Accept either a path, a filename, or a bare stem.
  std::filesystem::path p = std::filesystem::u8path(filename_or_stem);
  std::string stem = p.stem().u8string();

  // Special-case double extensions like ".nii.gz" by stripping a second time.
  // (Not strictly needed for EEG, but makes the helper more general.)
  const std::string ext = to_lower(p.extension().u8string());
  if (ext == ".gz" || ext == ".bz2" || ext == ".xz") {
    stem = std::filesystem::path(stem).stem().u8string();
  }

  if (trim(stem).empty()) return std::nullopt;

  auto parts = split(stem, '_');
  if (parts.empty()) return std::nullopt;

  std::string suffix;
  if (!parts.empty() && parts.back().find('-') == std::string::npos) {
    suffix = parts.back();
    parts.pop_back();
  }

  BidsEntities ent;
  for (const auto& tok : parts) {
    const auto dash = tok.find('-');
    if (dash == std::string::npos) continue;
    const std::string key = tok.substr(0, dash);
    const std::string val = tok.substr(dash + 1);

    auto set_checked = [&](std::string* dst, const char* what) {
      if (val.empty()) return;
      if (!is_valid_bids_label(val)) {
        throw std::runtime_error(std::string("BIDS: invalid ") + what + " label in filename: '" + val + "'");
      }
      *dst = val;
    };

    if (key == "sub") {
      set_checked(&ent.sub, "subject");
    } else if (key == "task") {
      set_checked(&ent.task, "task");
    } else if (key == "ses") {
      set_checked(&ent.ses, "session");
    } else if (key == "acq") {
      set_checked(&ent.acq, "acquisition");
    } else if (key == "run") {
      set_checked(&ent.run, "run");
    } else {
      // Unknown entities are ignored (e.g., desc-*, rec-*, echo-*, ...).
    }
  }

  if (ent.sub.empty() || ent.task.empty()) return std::nullopt;

  BidsParsedFilename out;
  out.ent = ent;
  out.suffix = suffix;
  return out;
}

std::optional<std::string> find_bids_dataset_root(const std::string& path) {
  if (trim(path).empty()) return std::nullopt;

  std::filesystem::path p = std::filesystem::u8path(path);
  if (!std::filesystem::exists(p)) return std::nullopt;
  if (std::filesystem::is_regular_file(p)) {
    p = p.parent_path();
  }

  // Walk up until filesystem root.
  for (;;) {
    const auto dd = p / "dataset_description.json";
    if (std::filesystem::exists(dd) && std::filesystem::is_regular_file(dd)) {
      return p.u8string();
    }
    if (!p.has_parent_path()) break;
    const auto parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  return std::nullopt;
}

void write_bids_dataset_description(const std::string& dataset_root,
                                   const BidsDatasetDescription& desc,
                                   bool overwrite) {
  if (dataset_root.empty()) throw std::runtime_error("BIDS: dataset_root is empty");
  ensure_directory(dataset_root);

  const std::filesystem::path root = std::filesystem::u8path(dataset_root);
  const std::filesystem::path path = root / "dataset_description.json";

  if (!overwrite && std::filesystem::exists(path)) {
    return;
  }

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path.u8string());

  const std::string dtype = trim(desc.dataset_type);
  const bool is_derivative = (dtype == "derivative");
  if (is_derivative && desc.generated_by.empty()) {
    throw std::runtime_error(
        "BIDS: dataset_description.json for DatasetType=derivative must include GeneratedBy");
  }

  auto write_generated_by = [&]() {
    f << "  \"GeneratedBy\": [\n";
    for (size_t i = 0; i < desc.generated_by.size(); ++i) {
      const auto& g = desc.generated_by[i];
      if (trim(g.name).empty()) {
        throw std::runtime_error("BIDS: GeneratedBy entry is missing required Name");
      }

      f << "    {\n";
      f << "      \"Name\": \"" << json_escape(g.name) << "\"";
      if (!trim(g.version).empty()) {
        f << ",\n      \"Version\": \"" << json_escape(g.version) << "\"";
      }
      if (!trim(g.description).empty()) {
        f << ",\n      \"Description\": \"" << json_escape(g.description) << "\"";
      }
      if (!trim(g.code_url).empty()) {
        f << ",\n      \"CodeURL\": \"" << json_escape(g.code_url) << "\"";
      }

      const bool have_container = !trim(g.container_type).empty() || !trim(g.container_tag).empty() ||
                                  !trim(g.container_uri).empty();
      if (have_container) {
        f << ",\n      \"Container\": {\n";
        bool first = true;
        auto emit_kv = [&](const char* k, const std::string& v) {
          if (trim(v).empty()) return;
          f << (first ? "" : ",\n")
            << "        \"" << k << "\": \"" << json_escape(v) << "\"";
          first = false;
        };
        emit_kv("Type", g.container_type);
        emit_kv("Tag", g.container_tag);
        emit_kv("URI", g.container_uri);
        f << "\n      }";
      }

      f << "\n    }";
      if (i + 1 < desc.generated_by.size()) f << ",";
      f << "\n";
    }
    f << "  ]";
  };

  auto write_source_datasets = [&]() {
    f << "  \"SourceDatasets\": [\n";
    bool first_obj = true;
    for (const auto& s : desc.source_datasets) {
      const bool have_any = !trim(s.url).empty() || !trim(s.doi).empty() || !trim(s.version).empty();
      if (!have_any) continue;

      if (!first_obj) f << ",\n";
      first_obj = false;

      f << "    {";
      bool first = true;
      auto emit_kv = [&](const char* k, const std::string& v) {
        if (trim(v).empty()) return;
        f << (first ? "" : ", ")
          << "\"" << k << "\": \"" << json_escape(v) << "\"";
        first = false;
      };
      emit_kv("URL", s.url);
      emit_kv("DOI", s.doi);
      emit_kv("Version", s.version);
      f << "}";
    }
    f << "\n  ]";
  };

  // --- File ---
  f << "{\n";
  f << "  \"Name\": \"" << json_escape(desc.name) << "\",\n";
  f << "  \"BIDSVersion\": \"" << json_escape(desc.bids_version) << "\"";
  if (!dtype.empty()) {
    f << ",\n  \"DatasetType\": \"" << json_escape(dtype) << "\"";
  }

  if (!desc.generated_by.empty()) {
    f << ",\n";
    write_generated_by();
  }
  if (!desc.source_datasets.empty()) {
    f << ",\n";
    write_source_datasets();
  }

  f << "\n}\n";
}

static std::string units_for_type(const std::string& type_uc) {
  // Use common EEG conventions.
  if (type_uc == "EEG" || type_uc == "EOG" || type_uc == "HEOG" || type_uc == "VEOG" ||
      type_uc == "ECG" || type_uc == "EMG" || type_uc == "REF") {
    return "uV";
  }
  if (type_uc == "TRIG") {
    // Triggers are commonly stored as TTL in Volts.
    return "V";
  }
  return "n/a";
}

std::string guess_bids_channel_type(const std::string& channel_name) {
  // We use normalized_channel_name so it tolerates "EEG Fp1-REF" and similar.
  // normalize_channel_name returns a lowercase alnum-only key.
  const std::string key = normalize_channel_name(channel_name);

  if (key.empty()) return "MISC";

  // Reference channel (recorded reference).
  // Keep this conservative: match "REF"/"Reference" or "REF" followed by digits.
  if (key == "ref" || key == "reference") return "REF";
  if (starts_with(key, "ref") && key.size() > 3) {
    bool digits = true;
    for (size_t i = 3; i < key.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(key[i])) == 0) {
        digits = false;
        break;
      }
    }
    if (digits) return "REF";
  }

  // EOG variants.
  if (starts_with(key, "heog")) return "HEOG";
  if (starts_with(key, "veog")) return "VEOG";
  if (starts_with(key, "eog")) return "EOG";

  // Other common biosignals.
  if (starts_with(key, "ecg") || starts_with(key, "ekg")) return "ECG";
  if (starts_with(key, "emg")) return "EMG";
  if (starts_with(key, "gsr")) return "GSR";
  if (starts_with(key, "ppg")) return "PPG";
  if (starts_with(key, "resp")) return "RESP";
  if (starts_with(key, "temp")) return "TEMP";

  // Triggers / stim.
  // Common patterns:
  // - TRIG/TRIGGER
  // - STI 014 (MNE / Neuromag convention)
  // - Status (BioSemi / some BDF recordings)
  if (starts_with(key, "trig") || starts_with(key, "trigger") || starts_with(key, "stim") ||
      starts_with(key, "sti") || starts_with(key, "status")) {
    return "TRIG";
  }

  // Common auxiliary naming.
  if (starts_with(key, "aux") || starts_with(key, "misc")) return "MISC";

  // Default for scalp channels.
  return "EEG";
}

bool is_valid_bids_coordinate_unit(const std::string& unit) {
  return unit == "m" || unit == "mm" || unit == "cm" || unit == "n/a";
}

static inline bool is_na_token(const std::string& s) {
  const std::string t = to_lower(trim(s));
  return t.empty() || t == "n/a" || t == "na";
}

static std::optional<double> parse_optional_double_strict(const std::string& s,
                                                          const std::string& context) {
  const std::string t = trim(s);
  if (is_na_token(t)) return std::nullopt;

  try {
    size_t pos = 0;
    const double v = std::stod(t, &pos);
    if (pos != t.size()) {
      throw std::runtime_error("Trailing characters");
    }
    if (!std::isfinite(v)) {
      throw std::runtime_error("Non-finite value");
    }
    return v;
  } catch (const std::exception&) {
    throw std::runtime_error("BIDS: failed to parse numeric value for " + context + ": '" + t + "'");
  }
}

static std::string format_double_or_na(const std::optional<double>& v, int precision = 6) {
  if (!v.has_value()) return "n/a";
  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << std::fixed << std::setprecision(precision) << v.value();
  return oss.str();
}

static std::string trim_trailing_zeros(std::string s) {
  const auto dot = s.find('.');
  if (dot == std::string::npos) {
    if (s == "-0") return "0";
    return s;
  }

  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") return "0";
  return s;
}

static std::string format_double_compact(double v, int precision = 12) {
  if (!std::isfinite(v)) {
    throw std::runtime_error("BIDS: non-finite numeric value");
  }

  // Normalize negative zero for nicer output.
  if (v == 0.0) v = 0.0;

  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << std::fixed << std::setprecision(precision) << v;
  return trim_trailing_zeros(oss.str());
}

static char detect_delim(const std::string& header_line) {
  // Prefer TSV if tabs are present.
  if (header_line.find('\t') != std::string::npos) return '\t';
  return ',';
}

std::vector<BidsElectrode> load_bids_electrodes_table(const std::string& path) {
  if (path.empty()) throw std::runtime_error("BIDS: electrodes table path is empty");

  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("BIDS: failed to open electrodes table: " + path);

  std::string line;
  bool have_header = false;
  char delim = ',';
  std::vector<std::string> header_fields;
  std::unordered_map<std::string, size_t> col;

  std::vector<BidsElectrode> out;

  while (std::getline(f, line)) {
    // Handle UTF-8 BOM on first line (best-effort).
    if (!have_header) {
      line = strip_utf8_bom(line);
    }

    const std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    if (!trimmed.empty() && trimmed[0] == '#') continue;

    if (!have_header) {
      delim = detect_delim(trimmed);
      header_fields = (delim == '\t') ? split(trimmed, delim) : split_csv_row(trimmed, delim);

      col.clear();
      for (size_t i = 0; i < header_fields.size(); ++i) {
        std::string key = to_lower(trim(header_fields[i]));
        if (key.empty()) continue;
        // First occurrence wins.
        if (col.find(key) == col.end()) col[key] = i;
      }

      // Required columns.
      //
      // BIDS electrodes.tsv requires x/y/z columns on output, but allows "n/a" values.
      // For convenience we allow input tables to omit the z column (commonly used 2D
      // montages with name,x,y). In that case, z is treated as missing (n/a).
      if (col.find("name") == col.end() || col.find("x") == col.end() ||
          col.find("y") == col.end()) {
        throw std::runtime_error("BIDS: electrodes table must include header columns: name,x,y (z optional)");
      }

      have_header = true;
      continue;
    }

    const auto fields = (delim == '\t') ? split(trimmed, delim) : split_csv_row(trimmed, delim);

    auto get_field = [&](const char* key) -> std::string {
      auto it = col.find(key);
      if (it == col.end()) return {};
      const size_t idx = it->second;
      if (idx >= fields.size()) return {};
      return trim(fields[idx]);
    };

    BidsElectrode e;
    e.name = get_field("name");
    if (e.name.empty()) {
      throw std::runtime_error("BIDS: electrodes table row is missing a 'name' value");
    }

    e.x = parse_optional_double_strict(get_field("x"), "electrodes.x");
    e.y = parse_optional_double_strict(get_field("y"), "electrodes.y");
    if (col.find("z") != col.end()) {
      e.z = parse_optional_double_strict(get_field("z"), "electrodes.z");
    } else {
      e.z.reset();
    }

    // Optional columns.
    e.type = is_na_token(get_field("type")) ? std::string() : get_field("type");
    e.material = is_na_token(get_field("material")) ? std::string() : get_field("material");
    if (col.find("impedance") != col.end()) {
      e.impedance_kohm = parse_optional_double_strict(get_field("impedance"), "electrodes.impedance");
    }

    out.push_back(std::move(e));
  }

  if (!have_header) {
    throw std::runtime_error("BIDS: electrodes table is empty or missing a header row");
  }

  return out;
}

void write_bids_electrodes_tsv(const std::string& path,
                               const std::vector<BidsElectrode>& electrodes) {
  if (path.empty()) throw std::runtime_error("BIDS: electrodes.tsv path is empty");

  // Validate uniqueness of electrode names.
  std::unordered_set<std::string> seen;
  seen.reserve(electrodes.size());
  for (const auto& e : electrodes) {
    if (e.name.empty()) throw std::runtime_error("BIDS: electrode name must not be empty");
    if (!seen.insert(e.name).second) {
      throw std::runtime_error("BIDS: duplicate electrode name: '" + e.name + "'");
    }
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  // Required columns first in mandated order, plus recommended columns.
  f << "name\tx\ty\tz\ttype\tmaterial\timpedance\n";

  for (const auto& e : electrodes) {
    const std::string type = e.type.empty() ? "n/a" : e.type;
    const std::string material = e.material.empty() ? "n/a" : e.material;

    f << tsv_sanitize(e.name) << "\t";
    f << tsv_sanitize(format_double_or_na(e.x)) << "\t";
    f << tsv_sanitize(format_double_or_na(e.y)) << "\t";
    f << tsv_sanitize(format_double_or_na(e.z)) << "\t";
    f << tsv_sanitize(type) << "\t";
    f << tsv_sanitize(material) << "\t";
    f << tsv_sanitize(format_double_or_na(e.impedance_kohm, /*precision=*/6)) << "\n";
  }
}

void write_bids_coordsystem_json(const std::string& path,
                                 const BidsCoordsystemJsonEegMetadata& meta) {
  if (path.empty()) throw std::runtime_error("BIDS: coordsystem.json path is empty");
  if (meta.eeg_coordinate_system.empty()) {
    throw std::runtime_error("BIDS: EEGCoordinateSystem is empty");
  }
  if (meta.eeg_coordinate_units.empty()) {
    throw std::runtime_error("BIDS: EEGCoordinateUnits is empty");
  }
  if (!is_valid_bids_coordinate_unit(meta.eeg_coordinate_units)) {
    throw std::runtime_error("BIDS: invalid EEGCoordinateUnits (use m/mm/cm/n/a): '" + meta.eeg_coordinate_units + "'");
  }
  if (meta.eeg_coordinate_system == "Other" && meta.eeg_coordinate_system_description.empty()) {
    throw std::runtime_error("BIDS: EEGCoordinateSystemDescription is required when EEGCoordinateSystem is 'Other'");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  f << "{\n";
  f << "  \"EEGCoordinateSystem\": \"" << json_escape(meta.eeg_coordinate_system) << "\",\n";
  f << "  \"EEGCoordinateUnits\": \"" << json_escape(meta.eeg_coordinate_units) << "\"";

  if (!meta.eeg_coordinate_system_description.empty()) {
    f << ",\n  \"EEGCoordinateSystemDescription\": \""
      << json_escape(meta.eeg_coordinate_system_description) << "\"";
  }

  f << "\n}\n";
}

void write_bids_channels_tsv(const std::string& path,
                            const EEGRecording& rec,
                            const std::vector<std::string>& channel_status,
                            const std::vector<std::string>& channel_status_desc,
                            const std::string& common_reference) {
  if (path.empty()) throw std::runtime_error("BIDS: channels.tsv path is empty");

  if (rec.channel_names.size() != rec.data.size()) {
    throw std::runtime_error("BIDS: recording has mismatched channel_names/data sizes");
  }

  // Validate uniqueness of channel names (BIDS requirement).
  std::unordered_set<std::string> seen;
  seen.reserve(rec.channel_names.size());
  for (const auto& n : rec.channel_names) {
    if (!seen.insert(n).second) {
      throw std::runtime_error("BIDS: duplicate channel name in recording: '" + n + "'");
    }
  }

  if (!channel_status.empty() && channel_status.size() != rec.channel_names.size()) {
    throw std::runtime_error("BIDS: channel_status size must match channel count");
  }
  if (!channel_status_desc.empty() && channel_status_desc.size() != rec.channel_names.size()) {
    throw std::runtime_error("BIDS: channel_status_desc size must match channel count");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  // Required columns: name, type, units (in this order).
  // qeeg also writes several OPTIONAL columns (in the order they are introduced
  // in the BIDS specification) for better interoperability with downstream tools.
  // See: https://bids-specification.readthedocs.io/en/stable/modality-specific-files/electroencephalography.html
  f << "name\ttype\tunits\tdescription\tsampling_frequency\treference\tlow_cutoff\thigh_cutoff\tnotch\tstatus\tstatus_description\n";

  const std::string ref = trim(common_reference).empty() ? "n/a" : common_reference;
  const std::optional<double> fs = (rec.fs_hz > 0.0) ? std::optional<double>(rec.fs_hz) : std::nullopt;

  for (size_t i = 0; i < rec.channel_names.size(); ++i) {
    const std::string& name = rec.channel_names[i];
    const std::string type_uc = guess_bids_channel_type(name);
    const std::string units = units_for_type(type_uc);

    std::string status = "good";
    std::string status_desc = "n/a";

    if (!channel_status.empty()) {
      status = channel_status[i];
      if (status != "good" && status != "bad" && status != "n/a") {
        // Keep it strict-ish.
        throw std::runtime_error("BIDS: invalid channel status (use good/bad/n/a): '" + status + "'");
      }
    }
    if (!channel_status_desc.empty()) {
      status_desc = channel_status_desc[i].empty() ? "n/a" : channel_status_desc[i];
    }

    // Required.
    f << tsv_sanitize(name) << "\t";
    f << tsv_sanitize(type_uc) << "\t";
    f << tsv_sanitize(units) << "\t";

    // Optional columns (we always emit them):
    f << "n/a\t";  // description
    f << tsv_sanitize(format_double_or_na(fs, /*precision=*/6)) << "\t";
    f << tsv_sanitize(ref) << "\t";
    f << "n/a\t";  // low_cutoff
    f << "n/a\t";  // high_cutoff
    f << "n/a\t";  // notch

    // QC columns.
    f << tsv_sanitize(status) << "\t";
    f << tsv_sanitize(status_desc) << "\n";
  }
}

void write_bids_channels_tsv(const std::string& path,
                            const std::vector<std::string>& channel_names,
                            const std::vector<std::string>& channel_status,
                            const std::vector<std::string>& channel_status_desc,
                            const std::string& common_reference) {
  if (path.empty()) throw std::runtime_error("BIDS: channels.tsv path is empty");

  // Validate uniqueness of channel names (BIDS requirement).
  std::unordered_set<std::string> seen;
  seen.reserve(channel_names.size());
  for (const auto& n : channel_names) {
    if (n.empty()) {
      throw std::runtime_error("BIDS: empty channel name in channel list");
    }
    if (!seen.insert(n).second) {
      throw std::runtime_error("BIDS: duplicate channel name in channel list: '" + n + "'");
    }
  }

  if (!channel_status.empty() && channel_status.size() != channel_names.size()) {
    throw std::runtime_error("BIDS: channel_status size must match channel count");
  }
  if (!channel_status_desc.empty() && channel_status_desc.size() != channel_names.size()) {
    throw std::runtime_error("BIDS: channel_status_desc size must match channel count");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  // Required columns: name, type, units (in this order).
  // qeeg also writes several OPTIONAL columns for interoperability.
  f << "name\ttype\tunits\tdescription\tsampling_frequency\treference\tlow_cutoff\thigh_cutoff\tnotch\tstatus\tstatus_description\n";

  const std::string ref = trim(common_reference).empty() ? "n/a" : common_reference;

  for (size_t i = 0; i < channel_names.size(); ++i) {
    const std::string& name = channel_names[i];
    const std::string type_uc = guess_bids_channel_type(name);
    const std::string units = units_for_type(type_uc);

    std::string status = "good";
    std::string status_desc = "n/a";

    if (!channel_status.empty()) {
      status = channel_status[i];
      if (status != "good" && status != "bad" && status != "n/a") {
        // Keep it strict-ish.
        throw std::runtime_error("BIDS: invalid channel status (use good/bad/n/a): '" + status + "'");
      }
    }
    if (!channel_status_desc.empty()) {
      status_desc = channel_status_desc[i].empty() ? "n/a" : channel_status_desc[i];
    }

    // Required.
    f << tsv_sanitize(name) << "\t";
    f << tsv_sanitize(type_uc) << "\t";
    f << tsv_sanitize(units) << "\t";

    // Optional columns.
    f << "n/a\t";  // description
    f << "n/a\t";  // sampling_frequency (unknown)
    f << tsv_sanitize(ref) << "\t";
    f << "n/a\t";  // low_cutoff
    f << "n/a\t";  // high_cutoff
    f << "n/a\t";  // notch

    // QC columns.
    f << tsv_sanitize(status) << "\t";
    f << tsv_sanitize(status_desc) << "\n";
  }
}

std::vector<std::string> load_bids_channels_tsv_names(const std::string& path) {
  if (path.empty()) throw std::runtime_error("BIDS: channels.tsv path is empty");

  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("BIDS: failed to open channels.tsv: " + path);

  std::string line;
  bool have_header = false;
  char delim = '\t';
  int col_name = -1;

  std::vector<std::string> out;
  std::unordered_set<std::string> seen;

  while (std::getline(f, line)) {
    if (!have_header) {
      line = strip_utf8_bom(line);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim(line);
    if (t.empty() || (!t.empty() && t[0] == '#')) continue;

    if (!have_header) {
      // BIDS uses TSV, but tolerate CSV for convenience.
      delim = (t.find('\t') != std::string::npos) ? '\t' : ',';
      const auto header_fields = (delim == '\t') ? split(t, delim) : split_csv_row(t, delim);
      for (size_t i = 0; i < header_fields.size(); ++i) {
        const std::string h = to_lower(trim(header_fields[i]));
        if (h == "name" || h == "channel") {
          col_name = static_cast<int>(i);
          break;
        }
      }
      if (col_name < 0) {
        throw std::runtime_error("BIDS: channels.tsv missing required column: name");
      }
      have_header = true;
      continue;
    }

    const auto fields = (delim == '\t') ? split(t, delim) : split_csv_row(t, delim);
    if (static_cast<size_t>(col_name) >= fields.size()) continue;
    const std::string name = trim(fields[static_cast<size_t>(col_name)]);
    if (name.empty()) continue;
    const std::string name_lc = to_lower(name);
    if (name_lc == "n/a" || name_lc == "na") continue;

    if (!seen.insert(name).second) {
      throw std::runtime_error("BIDS: duplicate channel name in channels.tsv: '" + name + "'");
    }
    out.push_back(name);
  }

  if (!have_header) {
    throw std::runtime_error("BIDS: channels.tsv missing header row: " + path);
  }

  return out;
}

static bool parse_int64_strict(const std::string& s, long long* out) {
  const std::string t = trim(s);
  if (t.empty()) return false;

  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') {
    i = 1;
    if (i >= t.size()) return false;
  }

  for (; i < t.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(t[i])) == 0) return false;
  }

  try {
    *out = std::stoll(t);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void write_bids_events_tsv(const std::string& path,
                          const std::vector<AnnotationEvent>& events,
                          const BidsEventsTsvOptions& opts,
                          double fs_hz) {
  if (path.empty()) throw std::runtime_error("BIDS: events.tsv path is empty");

  if (opts.include_sample && !(opts.sample_index_base == 0 || opts.sample_index_base == 1)) {
    throw std::runtime_error("BIDS: sample_index_base must be 0 or 1");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  // Required columns: onset, duration (in this order).
  // We optionally include trial_type/sample/value after that.
  f << "onset\tduration";
  if (opts.include_trial_type) f << "\ttrial_type";
  if (opts.include_sample) f << "\tsample";
  if (opts.include_value) f << "\tvalue";
  f << "\n";

  // BIDS recommends sorting by onset.
  std::vector<AnnotationEvent> sorted = events;
  std::sort(sorted.begin(), sorted.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    return a.onset_sec < b.onset_sec;
  });

  for (const auto& ev : sorted) {
    const double onset = ev.onset_sec;
    const double dur = (ev.duration_sec < 0.0) ? 0.0 : ev.duration_sec;
    const std::string label = ev.text.empty() ? "n/a" : ev.text;

    f << format_double_compact(onset) << "\t" << format_double_compact(dur);

    if (opts.include_trial_type) {
      f << "\t" << tsv_sanitize(label);
    }

    if (opts.include_sample) {
      if (fs_hz > 0.0) {
        const long long sample0 = static_cast<long long>(std::llround(onset * fs_hz));
        f << "\t" << (sample0 + static_cast<long long>(opts.sample_index_base));
      } else {
        f << "\t" << "n/a";
      }
    }

    if (opts.include_value) {
      long long v = 0;
      if (parse_int64_strict(label, &v)) {
        f << "\t" << v;
      } else {
        f << "\t" << "n/a";
      }
    }

    f << "\n";
  }
}

void write_bids_events_json(const std::string& path, const BidsEventsTsvOptions& opts) {
  if (path.empty()) throw std::runtime_error("BIDS: events.json path is empty");

  if (opts.include_sample && !(opts.sample_index_base == 0 || opts.sample_index_base == 1)) {
    throw std::runtime_error("BIDS: sample_index_base must be 0 or 1");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  auto write_entry = [&](const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units,
                         bool& wrote_any) {
    if (wrote_any) f << ",\n";
    f << "  \"" << json_escape(key) << "\": {\n";
    f << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    f << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units.empty()) {
      f << ",\n    \"Units\": \"" << json_escape(units) << "\"";
    }
    f << "\n  }";
    wrote_any = true;
  };

  f << "{\n";
  bool wrote_any = false;

  if (opts.include_trial_type) {
    write_entry("trial_type",
                "Event label",
                "Event label, typically derived from the recording annotations or produced by a processing pipeline.",
                "",
                wrote_any);
  }

  if (opts.include_sample) {
    const std::string base_desc = (opts.sample_index_base == 0)
        ? "Sample indices are 0-based (sample 0 is the first stored sample)."
        : "Sample indices are 1-based (sample 1 is the first stored sample).";

    write_entry("sample",
                "Event onset sample",
                "Event onset expressed in samples of the accompanying raw data file. "
                "Computed as round(onset * SamplingFrequency). " + base_desc,
                "samples",
                wrote_any);
  }

  if (opts.include_value) {
    write_entry("value",
                "Event code",
                "Integer event code parsed from the annotation text when it is a plain integer; otherwise 'n/a'.",
                "",
                wrote_any);
  }

  f << "\n}\n";
}

static std::string trial_type_level_description(const std::string& level) {
  // Provide slightly nicer descriptions for common qeeg-derived categories.
  if (level == "NF:Reward") return "Neurofeedback reward active.";
  if (level == "NF:Artifact") return "Artifact gate active (data considered contaminated).";
  if (level == "NF:Baseline") return "Baseline estimation segment.";
  if (level == "NF:Train") return "Neurofeedback training block.";
  if (level == "NF:Rest") return "Neurofeedback rest block (reinforcement paused).";
  if (level.rfind("NF:", 0) == 0) return "Neurofeedback derived event.";
  if (level.rfind("MS:", 0) == 0) {
    const std::string s = level.substr(3);
    if (!s.empty()) return "Microstate " + s + " segment.";
    return "Microstate segment.";
  }
  if (level == "n/a") return "Not applicable / missing label.";
  return "Event label.";
}

static std::vector<std::string> collect_unique_trial_types_limited(
    const std::vector<AnnotationEvent>& events,
    size_t max_unique,
    bool* too_many) {
  if (too_many) *too_many = false;

  // Collect up to max_unique+1 so we can detect overflow.
  std::set<std::string> uniq;
  for (const auto& ev : events) {
    const std::string label = ev.text.empty() ? "n/a" : ev.text;
    uniq.insert(label);
    if (uniq.size() > max_unique) {
      if (too_many) *too_many = true;
      break;
    }
  }

  // Deterministic ordering.
  std::vector<std::string> out;
  out.reserve(std::min(max_unique, uniq.size()));
  for (const auto& s : uniq) {
    if (out.size() >= max_unique) break;
    out.push_back(s);
  }
  return out;
}

void write_bids_events_json(const std::string& path,
                            const BidsEventsTsvOptions& opts,
                            const std::vector<AnnotationEvent>& events) {
  if (path.empty()) throw std::runtime_error("BIDS: events.json path is empty");

  if (opts.include_sample && !(opts.sample_index_base == 0 || opts.sample_index_base == 1)) {
    throw std::runtime_error("BIDS: sample_index_base must be 0 or 1");
  }

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  auto write_entry = [&](const std::string& key,
                         const std::string& long_name,
                         const std::string& desc,
                         const std::string& units,
                         bool& wrote_any) {
    if (wrote_any) f << ",\n";
    f << "  \"" << json_escape(key) << "\": {\n";
    f << "    \"LongName\": \"" << json_escape(long_name) << "\",\n";
    f << "    \"Description\": \"" << json_escape(desc) << "\"";
    if (!units.empty()) {
      f << ",\n    \"Units\": \"" << json_escape(units) << "\"";
    }
    f << "\n  }";
    wrote_any = true;
  };

  f << "{\n";
  bool wrote_any = false;

  if (opts.include_trial_type) {
    if (wrote_any) f << ",\n";

    bool too_many = false;
    const auto levels = (opts.include_trial_type_levels)
        ? collect_unique_trial_types_limited(events, opts.trial_type_levels_max, &too_many)
        : std::vector<std::string>{};

    std::string desc = "Event label, typically derived from the recording annotations or produced by a processing pipeline.";
    if (opts.include_trial_type_levels) {
      if (too_many) {
        desc += " Levels omitted because the number of unique values exceeded the configured maximum.";
      } else {
        desc += " Levels are listed under Levels for convenience.";
      }
    }

    f << "  \"trial_type\": {\n";
    f << "    \"LongName\": \"Event label\",\n";
    f << "    \"Description\": \"" << json_escape(desc) << "\"";

    if (opts.include_trial_type_levels && !too_many) {
      f << ",\n    \"Levels\": {\n";
      for (size_t i = 0; i < levels.size(); ++i) {
        const auto& k = levels[i];
        const auto v = trial_type_level_description(k);
        f << "      \"" << json_escape(k) << "\": \"" << json_escape(v) << "\"";
        if (i + 1 < levels.size()) f << ",";
        f << "\n";
      }
      f << "    }";
    }

    f << "\n  }";
    wrote_any = true;
  }

  if (opts.include_sample) {
    const std::string base_desc = (opts.sample_index_base == 0)
        ? "Sample indices are 0-based (sample 0 is the first stored sample)."
        : "Sample indices are 1-based (sample 1 is the first stored sample).";

    write_entry("sample",
                "Event onset sample",
                "Event onset expressed in samples of the accompanying raw data file. "
                "Computed as round(onset * SamplingFrequency). " + base_desc,
                "samples",
                wrote_any);
  }

  if (opts.include_value) {
    write_entry("value",
                "Event code",
                "Integer event code parsed from the annotation text when it is a plain integer; otherwise 'n/a'.",
                "",
                wrote_any);
  }

  f << "\n}\n";
}

void write_bids_eeg_json(const std::string& path,
                        const EEGRecording& rec,
                        const BidsEegJsonMetadata& meta) {
  if (path.empty()) throw std::runtime_error("BIDS: eeg.json path is empty");

  // Channel counts (recommended by spec).
  int eeg_count = 0;
  int ecg_count = 0;
  int emg_count = 0;
  int eog_count = 0;
  int misc_count = 0;
  int trig_count = 0;

  for (const auto& ch : rec.channel_names) {
    const std::string t = guess_bids_channel_type(ch);
    if (t == "EEG") eeg_count++;
    else if (t == "ECG") ecg_count++;
    else if (t == "EMG") emg_count++;
    else if (t == "EOG" || t == "HEOG" || t == "VEOG") eog_count++;
    else if (t == "TRIG") trig_count++;
    else misc_count++;
  }

  const double fs = rec.fs_hz;
  const double duration = (fs > 0.0) ? (static_cast<double>(rec.n_samples()) / fs) : 0.0;

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to write: " + path);

  f.imbue(std::locale::classic());

  // REQUIRED fields for EEG (_eeg.json):
  // - EEGReference (string)
  // - SamplingFrequency (number)
  // - PowerLineFrequency (number or "n/a")
  // - SoftwareFilters (object or "n/a")
  f << "{\n";
  f << "  \"EEGReference\": \"" << json_escape(meta.eeg_reference.empty() ? "n/a" : meta.eeg_reference)
    << "\",\n";
  f << "  \"SamplingFrequency\": " << format_double_compact(fs) << ",\n";

  if (meta.power_line_frequency_hz.has_value()) {
    f << "  \"PowerLineFrequency\": " << format_double_compact(meta.power_line_frequency_hz.value()) << ",\n";
  } else {
    f << "  \"PowerLineFrequency\": \"n/a\",\n";
  }

  if (!meta.software_filters_raw_json.empty()) {
    // Assume it's a raw JSON object (caller responsibility).
    f << "  \"SoftwareFilters\": " << meta.software_filters_raw_json << ",\n";
  } else {
    f << "  \"SoftwareFilters\": \"n/a\",\n";
  }

  // Helpful, spec-recommended fields.
  if (!meta.task_name.empty()) {
    f << "  \"TaskName\": \"" << json_escape(meta.task_name) << "\",\n";
  }

  f << "  \"RecordingType\": \"continuous\",\n";
  f << "  \"RecordingDuration\": " << format_double_compact(duration) << ",\n";

  if (!meta.eeg_ground.empty()) {
    f << "  \"EEGGround\": \"" << json_escape(meta.eeg_ground) << "\",\n";
  }
  if (!meta.cap_manufacturer.empty()) {
    f << "  \"CapManufacturer\": \"" << json_escape(meta.cap_manufacturer) << "\",\n";
  }
  if (!meta.cap_model.empty()) {
    f << "  \"CapManufacturersModelName\": \"" << json_escape(meta.cap_model) << "\",\n";
  }

  // Channel counts.
  f << "  \"EEGChannelCount\": " << eeg_count << ",\n";
  f << "  \"ECGChannelCount\": " << ecg_count << ",\n";
  f << "  \"EMGChannelCount\": " << emg_count << ",\n";
  f << "  \"EOGChannelCount\": " << eog_count << ",\n";
  f << "  \"MISCChannelCount\": " << misc_count << ",\n";
  f << "  \"TriggerChannelCount\": " << trig_count << "\n";

  f << "}\n";
}

} // namespace qeeg
