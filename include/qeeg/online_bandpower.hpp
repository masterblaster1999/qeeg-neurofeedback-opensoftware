#pragma once

#include "qeeg/bandpower.hpp"
#include "qeeg/types.hpp"
#include "qeeg/welch_psd.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

struct OnlineBandpowerOptions {
  // Sliding analysis window length.
  double window_seconds{2.0};

  // How often to emit a new frame (granularity / feedback update rate).
  double update_seconds{0.25};

  // Welch PSD parameters used per frame.
  WelchOptions welch;

  // If enabled, output relative bandpower:
  //   band_power / total_power
  // where total_power is integrated over [relative_fmin_hz, relative_fmax_hz].
  //
  // If relative_power=true and both relative_fmin_hz and relative_fmax_hz are 0,
  // the integration range defaults to:
  //   [min(band.fmin_hz), max(band.fmax_hz)] across the requested bands.
  bool relative_power{false};
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};

  // If enabled, apply a log10 transform to the output values:
  //   log10(max(eps, value))
  // This matches the behavior used by qeeg_reference_cli for reference building.
  bool log10_power{false};
};

struct OnlineBandpowerFrame {
  // Time (seconds) at the end of the analysis window (relative to start of stream).
  double t_end_sec{0.0};

  // Fixed metadata per frame (copied for convenience).
  std::vector<std::string> channel_names;
  std::vector<BandDefinition> bands;

  // Normalization / transform metadata for interpreting `powers`.
  bool relative_power{false};
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};
  bool log10_power{false};

  // Bandpower matrix: powers[band_index][channel_index]
  std::vector<std::vector<double>> powers;
};

// A dependency-light, real-time-friendly bandpower estimator:
// - maintains a fixed-size ring buffer per channel
// - periodically computes Welch PSD over the most recent window
// - integrates bandpower for the requested bands
//
// Intended use:
// - file playback (simulate online neurofeedback)
// - live streaming integration later (e.g., LSL) by feeding samples in chunks
class OnlineWelchBandpower {
public:
  OnlineWelchBandpower(std::vector<std::string> channel_names,
                       double fs_hz,
                       std::vector<BandDefinition> bands,
                       OnlineBandpowerOptions opt = {});

  size_t n_channels() const { return channel_names_.size(); }
  double fs_hz() const { return fs_hz_; }

  // Push a block of samples for all channels.
  // block[ch][i] is sample i of channel ch. All channels must have the same length.
  // Returns 0 or more computed frames (depending on how many updates occurred).
  std::vector<OnlineBandpowerFrame> push_block(const std::vector<std::vector<float>>& block);

private:
  struct Ring {
    std::vector<float> buf;
    size_t head{0};
    size_t count{0};
    explicit Ring(size_t cap);
    void push(float x);
    bool full() const;
    void extract(std::vector<float>* out) const; // oldest->newest
  };

  OnlineBandpowerFrame compute_frame() const;

  std::vector<std::string> channel_names_;
  double fs_hz_{0.0};
  std::vector<BandDefinition> bands_;
  OnlineBandpowerOptions opt_{};

  size_t window_samples_{0};
  size_t update_samples_{0};

  std::vector<Ring> rings_;

  size_t total_samples_{0};
  size_t since_last_update_{0};
};

} // namespace qeeg
