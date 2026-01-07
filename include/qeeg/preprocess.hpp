#pragma once

#include "qeeg/biquad.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// Basic qEEG preprocessing options.
//
// Notes:
// - All cutoffs are expressed in Hz.
// - Filtering here is intentionally dependency-light and meant as a *first pass*.
// - For offline analysis, enable zero_phase=true to reduce phase distortion.
struct PreprocessOptions {
  // Common Average Reference (CAR) across all channels.
  bool average_reference{false};

  // Line-noise notch filter.
  // 0 => disabled.
  double notch_hz{0.0};
  double notch_q{30.0};

  // Bandpass edges in Hz (implemented as a 2nd order Butterworth-like highpass
  // followed by a 2nd order Butterworth-like lowpass).
  // 0 => disabled for that edge.
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};

  // If true, use forward-backward (filtfilt-style) filtering for offline processing.
  // This is not available for streaming.
  bool zero_phase{false};
};

// Build a cascade of IIR stages matching the options.
std::vector<BiquadCoeffs> make_iir_stage_coeffs(double fs_hz, const PreprocessOptions& opt);

// Apply common average reference in-place.
void apply_average_reference_inplace(EEGRecording& rec);

// Offline preprocessing of the full recording (in-place).
//
// Steps:
// 1) optional average reference
// 2) optional notch + bandpass filtering
void preprocess_recording_inplace(EEGRecording& rec, const PreprocessOptions& opt);

// Streaming/online preprocessing (causal IIR only).
//
// Designed for chunked processing before feeding data into online estimators.
class StreamingPreprocessor {
public:
  StreamingPreprocessor(size_t n_channels, double fs_hz, const PreprocessOptions& opt);

  void reset();

  // Process a block in-place.
  // block[ch][i] is sample i for channel ch.
  void process_block(std::vector<std::vector<float>>* block);

private:
  size_t n_channels_{0};
  double fs_hz_{0.0};
  PreprocessOptions opt_{};
  std::vector<BiquadChain> chains_;
};

} // namespace qeeg
