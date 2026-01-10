#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace qeeg {

struct Vec2 {
  double x{0.0};
  double y{0.0};
};

struct Vec3 {
  double x{0.0};
  double y{0.0};
  double z{0.0};
};

struct RGB {
  uint8_t r{0}, g{0}, b{0};
};

// EDF+/BDF+ can contain annotations/events ("TAL" - Time-stamped Annotation Lists).
// We expose them as a simple list of (onset, duration, text) relative to the
// start time of the file.
//
// Notes:
// - duration_sec may be 0 for point events or when the duration is not present.
// - onset_sec is in seconds since the start time of the file (can be fractional).
struct AnnotationEvent {
  double onset_sec{0.0};
  double duration_sec{0.0};
  std::string text;
};

struct EEGRecording {
  std::vector<std::string> channel_names;        // size = n_channels
  double fs_hz{0.0};                             // sampling rate
  std::vector<std::vector<float>> data;          // data[ch][sample] in physical units (e.g. microvolts)

  // Optional event/annotation list (EDF+/BDF+ "Annotations" signal).
  //
  // For CSV/ASCII inputs, this is typically empty unless the CSV contains a
  // marker/event column that is detected by CSVReader.
  //
  // For EDF/BDF inputs, this list is empty when the file contains no
  // annotations.
  std::vector<AnnotationEvent> events;

  size_t n_channels() const { return data.size(); }
  size_t n_samples() const { return data.empty() ? 0 : data[0].size(); }
};

struct PsdResult {
  std::vector<double> freqs_hz;  // length = n_freq_bins
  std::vector<double> psd;       // same length, units ~ (signal_unit^2 / Hz)
};

struct BandDefinition {
  std::string name;
  double fmin_hz{0.0};
  double fmax_hz{0.0};
};

using BandPowerByChannel = std::unordered_map<std::string, double>; // channel -> power
using BandPowers = std::unordered_map<std::string, BandPowerByChannel>; // band -> (channel -> power)

struct ReferenceStats {
  // key: band|channel (lowercased)
  std::unordered_map<std::string, double> mean;
  std::unordered_map<std::string, double> stdev;

  // Optional metadata parsed from comment lines ("# key=value") if present in
  // the reference CSV.
  //
  // qeeg_reference_cli writes (among others):
  //   # log10_power=0/1
  //   # relative_power=0/1
  //   # relative_fmin_hz=LO
  //   # relative_fmax_hz=HI
  //   # robust=0/1
  //   # n_files=N
  bool meta_log10_power_present{false};
  bool meta_log10_power{false};

  bool meta_relative_power_present{false};
  bool meta_relative_power{false};

  bool meta_relative_fmin_hz_present{false};
  double meta_relative_fmin_hz{0.0};

  bool meta_relative_fmax_hz_present{false};
  double meta_relative_fmax_hz{0.0};

  bool meta_robust_present{false};
  bool meta_robust{false};

  bool meta_n_files_present{false};
  std::size_t meta_n_files{0};
};

} // namespace qeeg
