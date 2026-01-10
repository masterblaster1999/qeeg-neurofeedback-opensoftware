#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

// Simple, dependency-light artifact detection for EEG.
//
// This module is intentionally pragmatic: it computes a few robust, time-domain
// features in sliding windows and flags outliers relative to a baseline period.
//
// ⚠️ Research / educational use only. Artifact detection is not a solved problem,
// and these heuristics should be validated and tuned for each dataset.

struct ArtifactDetectionOptions {
  // Sliding window parameters.
  double window_seconds{1.0};
  double step_seconds{0.5};

  // Baseline period for robust thresholds.
  // If <= 0, the entire recording is used to estimate the robust statistics.
  double baseline_seconds{10.0};

  // Robust z-score thresholds. If <= 0, that feature is disabled.
  //
  // - ptp: peak-to-peak amplitude (max-min)
  // - rms: root-mean-square energy
  // - kurtosis: excess kurtosis (kurtosis-3)
  double ptp_z{6.0};
  double rms_z{6.0};
  double kurtosis_z{6.0};

  // A window is flagged as "bad" if at least this many channels are flagged.
  size_t min_bad_channels{1};
};

struct ArtifactChannelStats {
  // Robust location/scale (median and MAD-derived scale, with std fallback).
  double ptp_median{0.0};
  double ptp_scale{1.0};
  double rms_median{0.0};
  double rms_scale{1.0};
  double kurtosis_median{0.0};
  double kurtosis_scale{1.0};
};

struct ArtifactChannelMetrics {
  // Raw features.
  double ptp{0.0};
  double rms{0.0};
  double kurtosis{0.0}; // excess kurtosis

  // Robust z-scores relative to baseline stats.
  double ptp_z{0.0};
  double rms_z{0.0};
  double kurtosis_z{0.0};

  bool bad{false};
};

struct ArtifactWindowResult {
  double t_start_sec{0.0};
  double t_end_sec{0.0};

  std::vector<ArtifactChannelMetrics> channels; // size = n_channels

  bool bad{false};
  size_t bad_channel_count{0};
};

struct ArtifactDetectionResult {
  ArtifactDetectionOptions opt;

  std::vector<std::string> channel_names;
  std::vector<ArtifactChannelStats> baseline_stats; // size = n_channels

  std::vector<ArtifactWindowResult> windows;
  size_t total_bad_windows{0};
};

// Merged contiguous artifact regions from a windowed detection run.
//
// A segment is formed by merging overlapping/adjacent *bad* windows.
// The per-channel counts tell you which channels drove the segment.
struct ArtifactSegment {
  double t_start_sec{0.0};
  double t_end_sec{0.0};

  // Indices into ArtifactDetectionResult::windows for the first/last bad window
  // that contributed to this segment.
  size_t first_window{0};
  size_t last_window{0};

  // Number of bad windows merged into this segment.
  size_t window_count{0};

  // Maximum number of bad channels among the windows in the segment.
  size_t max_bad_channels{0};

  // For each channel: number of windows in this segment where that channel was flagged.
  // Size matches ArtifactDetectionResult::channel_names.
  std::vector<size_t> bad_windows_per_channel;
};

// Count how many windows each channel was flagged in.
//
// Note: counts are based on per-channel flags (ArtifactChannelMetrics::bad),
// regardless of the global window flag (ArtifactWindowResult::bad).
std::vector<size_t> artifact_bad_counts_per_channel(const ArtifactDetectionResult& res);

// Merge overlapping/adjacent bad windows into contiguous segments.
//
// If merge_gap_seconds > 0, segments separated by a gap <= merge_gap_seconds
// are merged.
std::vector<ArtifactSegment> artifact_bad_segments(const ArtifactDetectionResult& res,
                                                   double merge_gap_seconds = 0.0);

// Detect artifact windows using robust z-score thresholding.
ArtifactDetectionResult detect_artifacts(const EEGRecording& rec, const ArtifactDetectionOptions& opt);

} // namespace qeeg
