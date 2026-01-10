#pragma once

#include "qeeg/types.hpp"
#include "qeeg/welch_psd.hpp"

#include <cstddef>

namespace qeeg {

// Detect power-line interference (50/60 Hz) from a recording.
//
// This is a pragmatic helper for choosing a notch filter frequency when working
// with unknown export settings (common when exchanging EDF/BDF/ASCII exports).
//
// The detector compares the mean PSD density in a narrow band around 50 Hz and
// 60 Hz to the mean PSD density in nearby sidebands (a local baseline). It then
// aggregates per-channel evidence using the median ratio.

struct LineNoiseCandidate {
  // Center frequency for this candidate (e.g., 50 or 60).
  double freq_hz{0.0};

  // Mean(PSD) in the peak band divided by Mean(PSD) in the baseline sidebands.
  // Values >~ 1 indicate an elevated peak; larger is stronger.
  double ratio{0.0};

  // Mean PSD density in the peak band (units: signal_unit^2/Hz).
  double peak_mean{0.0};

  // Mean PSD density in the baseline sidebands (units: signal_unit^2/Hz).
  double baseline_mean{0.0};
};

struct LineNoiseEstimate {
  // Recommended notch frequency (0 => no strong evidence for 50 or 60).
  double recommended_hz{0.0};

  // Strength of the recommendation: median ratio for the recommended candidate.
  double strength_ratio{0.0};

  // Median ratios for each candidate.
  LineNoiseCandidate cand50{};
  LineNoiseCandidate cand60{};

  // Number of channels evaluated.
  size_t n_channels_used{0};
};

// Estimate the strength of a single line-noise candidate from a PSD.
//
// peak band: [center_hz-peak_half_width_hz, center_hz+peak_half_width_hz]
// baseline bands:
//   left:  [center_hz-baseline_half_width_hz, center_hz-guard_hz]
//   right: [center_hz+guard_hz, center_hz+baseline_half_width_hz]
//
// If the requested bands do not fit within the PSD frequency range, the
// returned ratio is 0.
LineNoiseCandidate estimate_line_noise_candidate(const PsdResult& psd,
                                                 double center_hz,
                                                 double peak_half_width_hz = 0.5,
                                                 double guard_hz = 1.5,
                                                 double baseline_half_width_hz = 5.0);

// Detect whether 50 Hz or 60 Hz line noise is more prominent.
//
// - Computes Welch PSD for up to max_channels channels.
// - Computes candidate ratios for 50 and 60 (if below Nyquist).
// - Aggregates per-channel ratios using the median.
// - If the best median ratio is < min_ratio, returns recommended_hz=0.
LineNoiseEstimate detect_line_noise_50_60(const EEGRecording& rec,
                                          const WelchOptions& opt = {},
                                          size_t max_channels = 8,
                                          double min_ratio = 3.0);

} // namespace qeeg
