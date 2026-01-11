#include "qeeg/bids.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace qeeg {

static inline bool is_alnum(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char uc : s) {
    char c = static_cast<char>(uc);
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (uc < 0x20) {
        // Control chars -> \u00XX
        std::ostringstream oss;
        oss << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
            << static_cast<int>(uc);
        out += oss.str();
      } else {
        out.push_back(c);
      }
      break;
    }
  }
  return out;
}

static std::string tsv_sanitize(std::string s) {
  // BIDS TSVs are plain tab-separated, without CSV-style quoting.
  // Replace tabs/newlines with spaces to keep the table well-formed.
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

static std::string to_upper_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
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

  f << "{\n";
  f << "  \"Name\": \"" << json_escape(desc.name) << "\",\n";
  f << "  \"BIDSVersion\": \"" << json_escape(desc.bids_version) << "\"";
  if (!desc.dataset_type.empty()) {
    f << ",\n  \"DatasetType\": \"" << json_escape(desc.dataset_type) << "\"\n";
  } else {
    f << "\n";
  }
  f << "}\n";
}

static std::string units_for_type(const std::string& type_uc) {
  // Use common EEG conventions.
  if (type_uc == "EEG" || type_uc == "EOG" || type_uc == "HEOG" || type_uc == "VEOG" ||
      type_uc == "ECG" || type_uc == "EMG") {
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

void write_bids_channels_tsv(const std::string& path,
                            const EEGRecording& rec,
                            const std::vector<std::string>& channel_status,
                            const std::vector<std::string>& channel_status_desc) {
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

  // Required columns: name, type, units (in this order).
  // We also include status/status_description for compatibility with QC tooling.
  f << "name\ttype\tunits\tstatus\tstatus_description\n";

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

    f << tsv_sanitize(name) << "\t";
    f << tsv_sanitize(type_uc) << "\t";
    f << tsv_sanitize(units) << "\t";
    f << tsv_sanitize(status) << "\t";
    f << tsv_sanitize(status_desc) << "\n";
  }
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

  // Required columns: onset, duration (in this order).
  // We optionally include trial_type/sample/value after that.
  f << "onset\tduration";
  if (opts.include_trial_type) f << "\ttrial_type";
  if (opts.include_sample) f << "\tsample";
  if (opts.include_value) f << "\tvalue";
  f << "\n";

  f << std::setprecision(12);

  // BIDS recommends sorting by onset.
  std::vector<AnnotationEvent> sorted = events;
  std::sort(sorted.begin(), sorted.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    return a.onset_sec < b.onset_sec;
  });

  for (const auto& ev : sorted) {
    const double onset = ev.onset_sec;
    const double dur = (ev.duration_sec < 0.0) ? 0.0 : ev.duration_sec;
    const std::string label = ev.text.empty() ? "n/a" : ev.text;

    f << onset << "\t" << dur;

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
                "Annotation text carried over from the source recording.",
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

  f << std::setprecision(12);

  // REQUIRED fields for EEG (_eeg.json):
  // - EEGReference (string)
  // - SamplingFrequency (number)
  // - PowerLineFrequency (number or "n/a")
  // - SoftwareFilters (object or "n/a")
  f << "{\n";
  f << "  \"EEGReference\": \"" << json_escape(meta.eeg_reference.empty() ? "n/a" : meta.eeg_reference)
    << "\",\n";
  f << "  \"SamplingFrequency\": " << fs << ",\n";

  if (meta.power_line_frequency_hz.has_value()) {
    f << "  \"PowerLineFrequency\": " << meta.power_line_frequency_hz.value() << ",\n";
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
  f << "  \"RecordingDuration\": " << duration << ",\n";

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
