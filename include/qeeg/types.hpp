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

struct RGB {
  uint8_t r{0}, g{0}, b{0};
};

struct EEGRecording {
  std::vector<std::string> channel_names;        // size = n_channels
  double fs_hz{0.0};                             // sampling rate
  std::vector<std::vector<float>> data;          // data[ch][sample] in physical units (e.g. microvolts)

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
