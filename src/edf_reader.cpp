#include "qeeg/edf_reader.hpp"

#include "qeeg/utils.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
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

static std::string normalize_label(std::string label) {
  label = trim(label);

  // Common variants in EDF labels
  // e.g., "EEG Fp1", "EEG-Fp1", "Fp1-REF", etc.
  std::string low = to_lower(label);

  // Strip leading "eeg" token
  if (starts_with(low, "eeg")) {
    // remove first 3 chars and any separators/spaces
    label = trim(label.substr(3));
    while (!label.empty() && (label[0] == ' ' || label[0] == '-' || label[0] == '_')) {
      label.erase(label.begin());
    }
  }

  // Strip trailing "-ref" or "ref"
  low = to_lower(label);
  auto strip_suffix = [&](const std::string& suf) {
    if (ends_with(low, suf)) {
      label = trim(label.substr(0, label.size() - suf.size()));
      low = to_lower(label);
    }
  };
  strip_suffix("-ref");
  strip_suffix(" ref");

  // If label contains spaces, take last token (often channel name)
  auto parts = split(label, ' ');
  if (parts.size() >= 2) {
    label = trim(parts.back());
  }

  return label;
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

  // Skip unused fields we don't need right now:
  // transducer type (80), physical dimension (8)
  for (int i = 0; i < num_signals; ++i) (void)read_fixed(f, 80);
  for (int i = 0; i < num_signals; ++i) (void)read_fixed(f, 8);

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

  // EDF samples are 16-bit signed per spec (this reader only supports EDF, not BDF 24-bit).
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

  // Determine per-signal sampling rates and ensure they match
  std::vector<double> fs_per_signal(num_signals, 0.0);
  for (int i = 0; i < num_signals; ++i) {
    fs_per_signal[i] = static_cast<double>(samples_per_record[i]) / record_duration;
  }

  // Keep channels that are not annotations
  std::vector<bool> keep(num_signals, true);
  std::vector<std::string> kept_names;
  kept_names.reserve(num_signals);

  for (int i = 0; i < num_signals; ++i) {
    std::string label = trim(labels[i]);
    std::string low = to_lower(label);
    if (low.find("edf annotations") != std::string::npos ||
        low.find("annotations") != std::string::npos) {
      keep[i] = false;
      continue;
    }
    std::string ch = normalize_label(label);
    if (ch.empty()) {
      keep[i] = false;
      continue;
    }
    kept_names.push_back(ch);
  }

  if (kept_names.empty()) {
    throw std::runtime_error("EDF: no EEG channels found (only annotations?)");
  }

  // Choose a global fs_hz: require all kept channels to match (within tolerance)
  double fs0 = -1.0;
  for (int i = 0; i < num_signals; ++i) {
    if (!keep[i]) continue;
    if (fs0 < 0.0) fs0 = fs_per_signal[i];
    else {
      double diff = std::abs(fs_per_signal[i] - fs0);
      if (diff > 1e-6) {
        throw std::runtime_error("EDF: channels have different sampling rates; this first pass requires equal fs");
      }
    }
  }
  if (fs0 <= 0.0) throw std::runtime_error("EDF: invalid sampling rate");

  // Allocate output recording
  EEGRecording rec;
  rec.fs_hz = fs0;
  rec.channel_names = kept_names;
  rec.data.resize(rec.channel_names.size());

  // Precompute scale + offset per signal
  std::vector<double> scale(num_signals, 1.0);
  std::vector<double> offset(num_signals, 0.0);
  for (int i = 0; i < num_signals; ++i) {
    int dmin = dig_min[i];
    int dmax = dig_max[i];
    double pmin = phys_min[i];
    double pmax = phys_max[i];
    if (dmax == dmin) {
      scale[i] = 0.0;
      offset[i] = 0.0;
    } else {
      scale[i] = (pmax - pmin) / static_cast<double>(dmax - dmin);
      offset[i] = pmin - static_cast<double>(dmin) * scale[i];
    }
  }

  // Seek to start of data (header_bytes)
  f.seekg(static_cast<std::streamoff>(header_bytes), std::ios::beg);
  if (!f) throw std::runtime_error("EDF: failed to seek to data");

  // Map original signal index -> kept channel index
  std::vector<int> kept_index(num_signals, -1);
  int ki = 0;
  for (int i = 0; i < num_signals; ++i) {
    if (keep[i]) {
      kept_index[i] = ki;
      ++ki;
    }
  }

  // Pre-reserve
  for (int i = 0; i < num_signals; ++i) {
    if (!keep[i]) continue;
    int out_idx = kept_index[i];
    size_t total = static_cast<size_t>(num_records) * static_cast<size_t>(samples_per_record[i]);
    rec.data[static_cast<size_t>(out_idx)].reserve(total);
  }

  // Read records
  for (int r = 0; r < num_records; ++r) {
    for (int s = 0; s < num_signals; ++s) {
      const int n = samples_per_record[s];
      for (int j = 0; j < n; ++j) {
        int16_t dig = read_i16_le(f);
        if (keep[s]) {
          double phys = static_cast<double>(dig) * scale[s] + offset[s];
          rec.data[static_cast<size_t>(kept_index[s])].push_back(static_cast<float>(phys));
        }
      }
    }
  }

  // Sanity: ensure all kept channels have same length
  const size_t nsamp = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != nsamp) {
      throw std::runtime_error("EDF: channel length mismatch after read");
    }
  }

  (void)patient_id; (void)recording_id; (void)start_date; (void)start_time; (void)reserved;

  return rec;
}

} // namespace qeeg
