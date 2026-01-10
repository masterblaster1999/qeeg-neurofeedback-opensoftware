#pragma once

#include "qeeg/artifacts.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

// Pragmatic channel-level quality checks.
//
// The goal of this module is to help with common EEG export issues:
// - disconnected electrodes / flat channels
// - extremely noisy channels
// - channels that are repeatedly flagged by simple artifact window heuristics
//
// ⚠️ Research/educational use only.
// These heuristics are intentionally simple and should be validated for each dataset.

struct ChannelQCOptions {
  // Flatline thresholds (physical units; typically microvolts).
  // If <= 0, that check is disabled.
  double flatline_ptp{1.0};
  double flatline_scale{0.0};

  // Relative flatline threshold: robust_scale < flatline_scale_factor * median_scale.
  // If <= 0, disabled.
  double flatline_scale_factor{0.02};

  // Noisy channel threshold: robust_scale > noisy_scale_factor * median_scale.
  // If <= 0, disabled.
  double noisy_scale_factor{10.0};

  // Artifact-based "often bad" channel threshold.
  // If <= 0, artifact-based channel badness is disabled.
  double artifact_bad_window_fraction{0.30};
  ArtifactDetectionOptions artifact_opt{};

  // Maximum number of samples used for robust statistics and optional correlation.
  // (Downsamples evenly if the recording is longer.)
  size_t max_samples_for_robust{50000};

  // Optional absolute correlation check against the global mean signal.
  // If <= 0, disabled.
  double min_abs_corr{0.0};
};

struct ChannelQCChannelResult {
  std::string channel;

  // Basic amplitude stats on raw samples.
  double min_value{0.0};
  double max_value{0.0};
  double ptp{0.0};

  double mean{0.0};
  double stddev{0.0};

  // Robust scale (MAD-based, consistent with std for Gaussian data).
  double robust_scale{0.0};

  // Fraction of sliding windows (from detect_artifacts) where this channel was flagged.
  // 0..1. If artifact scoring is disabled/unavailable, this will be 0.
  double artifact_bad_window_fraction{0.0};

  // |corr(channel, mean_over_channels)| computed on a downsampled set of points.
  // 0..1. If correlation check is disabled, this will be 0.
  double abs_corr_with_mean{0.0};

  bool flatline{false};
  bool noisy{false};
  bool artifact_often_bad{false};
  bool corr_low{false};

  bool bad{false};

  // Semi-colon separated reasons, e.g. "flatline;artifact_often_bad".
  std::string reasons;
};

struct ChannelQCResult {
  ChannelQCOptions opt;
  std::vector<ChannelQCChannelResult> channels;
  std::vector<size_t> bad_indices;
};

// Evaluate simple channel QC metrics and decide which channels are "bad".
ChannelQCResult evaluate_channel_qc(const EEGRecording& rec, const ChannelQCOptions& opt = {});

} // namespace qeeg
