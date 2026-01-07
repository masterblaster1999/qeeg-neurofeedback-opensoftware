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

  // Optional event/annotation list (EDF+/BDF+ "Annotations" signal). Empty for CSV inputs
  // and for EDF/BDF that contain no annotations.
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
};

} // namespace qeeg
