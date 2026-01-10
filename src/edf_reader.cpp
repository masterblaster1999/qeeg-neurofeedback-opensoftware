#include "qeeg/edf_reader.hpp"

#include "qeeg/annotations.hpp"
#include "qeeg/resample.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace qeeg {

static std::string read_fixed(std::ifstream& f, size_t n) {
  std::string s(n, '\0');
  f.read(&s[0], static_cast<std::streamsize>(n));
  if (!f) throw std::runtime_error("EDF parse error: unexpected EOF");
  return s;
}

static int16_t read_i16_le(std::ifstream& f) {
  uint8_t b0 = 0, b1 = 0;
  f.read(reinterpret_cast<char*>(&b0), 1);
  f.read(reinterpret_cast<char*>(&b1), 1);
  if (!f) throw std::runtime_error("EDF parse error: unexpected EOF while reading samples");
  return static_cast<int16_t>(static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8));
}

static std::string sanitize_label(std::string label) {
  label = trim(label);
  if (label.empty()) return label;

  // Common variants in EDF labels:
  //   "EEG Fp1", "EEG-Fp1", "Fp1-REF", "EEG1", "ExG 1", ...
  const std::string low0 = to_lower(label);

  // Strip leading "EEG" only when it's clearly a modality token (followed by whitespace/separator
  // or a letter). Do NOT strip in cases like "EEG1" where the digit is part of the channel name.
  if (starts_with(low0, "eeg") && label.size() > 3) {
    const char next = low0[3];
    if (next == ' ' || next == '-' || next == '_' ||
        (std::isalpha(static_cast<unsigned char>(next)) != 0)) {
      label = trim(label.substr(3));
      while (!label.empty() && (label[0] == ' ' || label[0] == '-' || label[0] == '_')) {
        label.erase(label.begin());
      }
    }
  }

  // Strip trailing "-ref" or " ref"
  std::string low = to_lower(label);
  auto strip_suffix = [&](const std::string& suf) {
    if (ends_with(low, suf)) {
      label = trim(label.substr(0, label.size() - suf.size()));
      low = to_lower(label);
    }
  };
  strip_suffix("-ref");
  strip_suffix(" ref");

  // Remove whitespace to make CLI channel arguments less painful (no quoting needed).
  label.erase(std::remove_if(label.begin(), label.end(),
                            [](unsigned char c) { return std::isspace(c) != 0; }),
              label.end());

  return label;
}

static bool is_microvolt_dimension(std::string dim) {
  dim = to_lower(trim(dim));
  dim.erase(std::remove_if(dim.begin(), dim.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            dim.end());
  if (dim.empty()) return false;

  // Common encodings seen in EDF: "uV", "µV", "μV", sometimes "mV"
  if (dim.find("uv") != std::string::npos) return true;
  if (dim.find("µv") != std::string::npos) return true; // micro sign
  if (dim.find("μv") != std::string::npos) return true; // Greek mu
  if (dim.find("mv") != std::string::npos) return true;

  return false;
}

static double voltage_to_microvolts_multiplier(std::string dim) {
  // EDF/BDF store physical dimensions as free-form 8-char strings.
  // Standardize voltage-like channels to microvolts for downstream QEEG code.
  dim = to_lower(trim(dim));
  dim.erase(std::remove_if(dim.begin(), dim.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            dim.end());
  if (dim.empty()) return 1.0;

  // Microvolt spellings.
  if (dim.find("uv") != std::string::npos) return 1.0;
  if (dim.find("µv") != std::string::npos) return 1.0;
  if (dim.find("μv") != std::string::npos) return 1.0;

  // Millivolts.
  if (dim.find("mv") != std::string::npos) return 1000.0;

  // Volts (best-effort).
  if (dim.find('v') != std::string::npos || dim.find("volt") != std::string::npos) return 1e6;

  return 1.0;
}

static bool is_known_eeg_electrode_key(const std::string& key) {
  // Minimal 10-20 set (19-ch), plus common reference electrodes.
  static const std::unordered_set<std::string> k = {
      "fp1","fp2","f7","f3","fz","f4","f8",
      "t7","t8","c3","cz","c4",
      "p7","p3","pz","p4","p8",
      "o1","o2",
      "a1","a2","m1","m2"
  };
  return k.find(key) != k.end();
}

static bool looks_like_exg_or_eeg_channel(const std::string& sanitized_label,
                                         const std::string& phys_dim,
                                         const std::string& raw_label) {
  // Physical dimension is a strong indicator.
  if (is_microvolt_dimension(phys_dim)) return true;

  // Fall back to label heuristics.
  const std::string raw_low = to_lower(trim(raw_label));
  if (raw_low.find("eeg") != std::string::npos) return true;
  if (raw_low.find("exg") != std::string::npos) return true;

  const std::string key = normalize_channel_name(sanitized_label);
  if (key.empty()) return false;

  if (starts_with(key, "exg")) return true;
  if (starts_with(key, "eeg")) return true; // e.g. "EEG1"
  if (starts_with(key, "ch")) return true;  // generic channel IDs (some exports)
  if (is_known_eeg_electrode_key(key)) return true;

  return false;
}

EEGRecording EDFReader::read(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to open EDF: " + path);

  // Header (fixed part)
  std::string version = read_fixed(f, 8);
  (void)version;
  std::string patient_id = read_fixed(f, 80);
  std::string recording_id = read_fixed(f, 80);
  std::string start_date = read_fixed(f, 8);
  std::string start_time = read_fixed(f, 8);
  std::string header_bytes_s = read_fixed(f, 8);
  std::string reserved = read_fixed(f, 44);
  std::string num_records_s = read_fixed(f, 8);
  std::string record_duration_s = read_fixed(f, 8);
  std::string num_signals_s = read_fixed(f, 4);

  const int header_bytes = to_int(header_bytes_s);
  int num_records = 0;
  try { num_records = to_int(num_records_s); }
  catch (...) { num_records = 0; }

  double record_duration = 0.0;
  try { record_duration = to_double(record_duration_s); }
  catch (...) { record_duration = 0.0; }

  const int num_signals = to_int(num_signals_s);
  if (num_signals <= 0) throw std::runtime_error("EDF: invalid number of signals");

  // Per-signal fields (each field is an array of num_signals entries)
  std::vector<std::string> labels(num_signals);
  for (int i = 0; i < num_signals; ++i) labels[i] = read_fixed(f, 16);

  // transducer type (80), physical dimension (8)
  for (int i = 0; i < num_signals; ++i) (void)read_fixed(f, 80);
  std::vector<std::string> phys_dim(num_signals);
  for (int i = 0; i < num_signals; ++i) phys_dim[i] = read_fixed(f, 8);

  std::vector<double> phys_min(num_signals), phys_max(num_signals);
  std::vector<int> dig_min(num_signals), dig_max(num_signals);
  for (int i = 0; i < num_signals; ++i) phys_min[i] = to_double(read_fixed(f, 8));
  for (int i = 0; i < num_signals; ++i) phys_max[i] = to_double(read_fixed(f, 8));
  for (int i = 0; i < num_signals; ++i) dig_min[i] = to_int(read_fixed(f, 8));
  for (int i = 0; i < num_signals; ++i) dig_max[i] = to_int(read_fixed(f, 8));

  // prefiltering (80)
  for (int i = 0; i < num_signals; ++i) (void)read_fixed(f, 80);

  std::vector<int> samples_per_record(num_signals);
  for (int i = 0; i < num_signals; ++i) samples_per_record[i] = to_int(read_fixed(f, 8));

  // reserved (32)
  for (int i = 0; i < num_signals; ++i) (void)read_fixed(f, 32);

  if (record_duration <= 0.0) {
    throw std::runtime_error("EDF: record_duration must be > 0 (got " + std::to_string(record_duration) + ")");
  }

  // Infer num_records if unknown (<= 0)
  const std::uintmax_t file_size = std::filesystem::file_size(std::filesystem::u8path(path));
  const size_t header_size = static_cast<size_t>(header_bytes);

  // EDF samples are 16-bit signed per spec.
  size_t bytes_per_record = 0;
  for (int i = 0; i < num_signals; ++i) {
    if (samples_per_record[i] < 0) throw std::runtime_error("EDF: negative samples_per_record");
    bytes_per_record += static_cast<size_t>(samples_per_record[i]) * 2u;
  }
  if (bytes_per_record == 0) throw std::runtime_error("EDF: bytes_per_record computed as 0");

  if (num_records <= 0) {
    if (file_size < header_size) throw std::runtime_error("EDF: file smaller than header");
    const size_t data_bytes = static_cast<size_t>(file_size - header_size);
    num_records = static_cast<int>(data_bytes / bytes_per_record);
  }
  if (num_records <= 0) throw std::runtime_error("EDF: could not determine number of data records");

  // Per-signal sampling rates.
  std::vector<double> fs_per_signal(num_signals, 0.0);
  for (int i = 0; i < num_signals; ++i) {
    fs_per_signal[i] = static_cast<double>(samples_per_record[i]) / record_duration;
  }

  // Keep data channels; parse annotations into rec.events.
  std::vector<bool> keep(num_signals, false);
  std::vector<bool> is_annotation(num_signals, false);
  std::vector<bool> eeg_candidate(num_signals, false);
  std::vector<std::string> sig_name(num_signals);

  for (int i = 0; i < num_signals; ++i) {
    const std::string raw = trim(labels[i]);
    const std::string low = to_lower(raw);

    if (low.find("edf annotations") != std::string::npos ||
        low.find("annotations") != std::string::npos) {
      is_annotation[i] = true;
      keep[i] = false;
      continue;
    }

    if (samples_per_record[i] <= 0) {
      keep[i] = false;
      continue;
    }

    const std::string name = sanitize_label(raw);
    if (name.empty()) {
      keep[i] = false;
      continue;
    }

    sig_name[i] = name;
    keep[i] = true;
    eeg_candidate[i] = looks_like_exg_or_eeg_channel(name, phys_dim[i], raw);
  }

  // Choose a global sampling rate. Mixed sampling rates are common for NeXus exports
  // (EEG/ExG at high rate + peripherals at lower rates). We select the highest
  // EEG/ExG-like sampling rate as the target and resample other kept channels.
  bool have_candidates = false;
  double fs_target = -1.0;
  for (int i = 0; i < num_signals; ++i) {
    if (!keep[i] || !eeg_candidate[i]) continue;
    have_candidates = true;
    fs_target = std::max(fs_target, fs_per_signal[i]);
  }
  if (!have_candidates) {
    for (int i = 0; i < num_signals; ++i) {
      if (!keep[i]) continue;
      fs_target = std::max(fs_target, fs_per_signal[i]);
    }
  }
  if (!(fs_target > 0.0)) throw std::runtime_error("EDF: invalid sampling rate");

  int target_spr = 0;
  for (int i = 0; i < num_signals; ++i) {
    if (!keep[i]) continue;
    if (std::abs(fs_per_signal[i] - fs_target) <= 1e-6) {
      target_spr = std::max(target_spr, samples_per_record[i]);
    }
  }
  if (target_spr <= 0) {
    target_spr = static_cast<int>(std::llround(fs_target * record_duration));
  }
  if (target_spr <= 0) throw std::runtime_error("EDF: failed to determine target samples_per_record");

  const size_t target_total = static_cast<size_t>(num_records) * static_cast<size_t>(target_spr);

  // Build the final channel list.
  std::vector<std::string> kept_names;
  kept_names.reserve(static_cast<size_t>(num_signals));
  std::vector<int> kept_index(num_signals, -1);

  int ki = 0;
  for (int i = 0; i < num_signals; ++i) {
    if (keep[i]) {
      kept_index[i] = ki++;
      kept_names.push_back(sig_name[i]);
    }
  }
  if (kept_names.empty()) {
    throw std::runtime_error("EDF: no signals found");
  }

  EEGRecording rec;
  rec.fs_hz = fs_target;
  rec.channel_names = kept_names;
  rec.data.resize(rec.channel_names.size());

  // Precompute scale + offset + unit conversion per signal.
  std::vector<double> scale(num_signals, 1.0);
  std::vector<double> offset(num_signals, 0.0);
  std::vector<double> unit_mult(num_signals, 1.0);
  for (int i = 0; i < num_signals; ++i) {
    const int dmin = dig_min[i];
    const int dmax = dig_max[i];
    const double pmin = phys_min[i];
    const double pmax = phys_max[i];

    if (dmax == dmin) {
      scale[i] = 0.0;
      offset[i] = 0.0;
    } else {
      scale[i] = (pmax - pmin) / static_cast<double>(dmax - dmin);
      offset[i] = pmin - static_cast<double>(dmin) * scale[i];
    }

    unit_mult[i] = voltage_to_microvolts_multiplier(phys_dim[i]);
  }

  // Temp storage (raw, pre-resample) for each kept channel.
  std::vector<std::vector<float>> tmp;
  tmp.resize(rec.channel_names.size());
  for (int i = 0; i < num_signals; ++i) {
    if (!keep[i]) continue;
    const size_t total = static_cast<size_t>(num_records) * static_cast<size_t>(samples_per_record[i]);
    tmp[static_cast<size_t>(kept_index[i])].reserve(total);
  }

  // Seek to start of data (header_bytes)
  f.seekg(static_cast<std::streamoff>(header_bytes), std::ios::beg);
  if (!f) throw std::runtime_error("EDF: failed to seek to data");

  // Read records
  for (int r = 0; r < num_records; ++r) {
    (void)r;
    for (int s = 0; s < num_signals; ++s) {
      const int n = samples_per_record[s];
      if (is_annotation[s]) {
        // EDF+ annotation channel: samples are stored as 16-bit integers but only
        // the low 8 bits are used as 8-bit bytes for the TAL text.
        std::vector<uint8_t> bytes;
        bytes.reserve(static_cast<size_t>(n));
        for (int j = 0; j < n; ++j) {
          const int16_t dig = read_i16_le(f);
          const uint16_t u = static_cast<uint16_t>(dig);
          bytes.push_back(static_cast<uint8_t>(u & 0xFFu));
        }
        auto ev = parse_edfplus_annotations_record(bytes);
        rec.events.insert(rec.events.end(), ev.begin(), ev.end());
      } else {
        for (int j = 0; j < n; ++j) {
          const int16_t dig = read_i16_le(f);
          if (keep[s]) {
            const double phys = (static_cast<double>(dig) * scale[s] + offset[s]) * unit_mult[s];
            tmp[static_cast<size_t>(kept_index[s])].push_back(static_cast<float>(phys));
          }
        }
      }
    }
  }

  // Resample kept channels to the target length.
  for (size_t ch = 0; ch < rec.data.size(); ++ch) {
    if (tmp[ch].size() == target_total) {
      rec.data[ch] = std::move(tmp[ch]);
    } else {
      rec.data[ch] = resample_linear(tmp[ch], target_total);
      if (rec.data[ch].size() != target_total) {
        throw std::runtime_error("EDF: resample failed to produce the expected length");
      }
    }
  }

  // Sanity: ensure all kept channels have same length
  const size_t nsamp = rec.n_samples();
  if (nsamp != target_total) {
    throw std::runtime_error("EDF: internal length mismatch (nsamp != target_total)");
  }
  for (const auto& ch : rec.data) {
    if (ch.size() != nsamp) {
      throw std::runtime_error("EDF: channel length mismatch after resample");
    }
  }

  (void)patient_id;
  (void)recording_id;
  (void)start_date;
  (void)start_time;
  (void)reserved;

  // Sort merged events across records/signals.
  std::sort(rec.events.begin(), rec.events.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
    if (a.duration_sec != b.duration_sec) return a.duration_sec < b.duration_sec;
    return a.text < b.text;
  });

  return rec;
}

} // namespace qeeg
