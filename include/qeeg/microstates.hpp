#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// A first-pass EEG microstate analysis implementation.
//
// Microstates are quasi-stable scalp topographies that can be estimated by
// clustering channel topographies at peaks of Global Field Power (GFP).
//
// This implementation is intentionally dependency-light and designed to be
// "good enough" for experimentation and education. It follows a common recipe:
//  1) compute GFP over time
//  2) pick the strongest local maxima (GFP peaks)
//  3) build normalized topographies at those peaks
//  4) k-means cluster those peak topographies with optional polarity invariance
//  5) assign every sample to the closest template and compute basic stats

struct MicrostatesOptions {
  // Number of microstates to estimate.
  int k{4};

  // Peak selection (clustering is performed on topographies extracted at these peaks).
  //
  // We find all local maxima of GFP, sort them by GFP amplitude descending, then keep
  // the top `peak_pick_fraction` (clamped to `max_peaks`).
  //
  // Typical values used in the literature are around 0.05-0.15.
  double peak_pick_fraction{0.10};

  // Cap the number of GFP peaks used for clustering (runtime control).
  size_t max_peaks{1000};

  // Enforce a minimum spacing between selected peaks (in samples). 0 disables.
  size_t min_peak_distance_samples{0};

  // If true, subtract the channel-wise mean from each topography before normalizing.
  // When common-average reference (CAR) is applied, this is often redundant but still
  // helpful as a safeguard.
  bool demean_topography{true};

  // If true, treat topographies as equivalent up to sign (polarity invariant).
  // This is common in microstate analysis since maps are often defined modulo polarity.
  bool polarity_invariant{true};

  // K-means settings.
  int max_iterations{100};
  double convergence_tol{1e-6};
  unsigned seed{12345};

  // Optional temporal smoothing on the final sample-wise labels.
  // Segments shorter than this will be merged into neighbors.
  // 0 disables.
  int min_segment_samples{0};
};

struct MicrostatesResult {
  // Templates: k x n_channels. Each template is unit-norm (L2) and optionally demeaned.
  std::vector<std::vector<double>> templates;

  // Sample-wise labels, length = n_samples, values in [0,k). A label of -1 indicates
  // an undefined sample (e.g., zero-norm topography).
  std::vector<int> labels;

  // Global Field Power time series, length = n_samples.
  std::vector<double> gfp;

  // Per-sample absolute correlation (cosine similarity) to the assigned template,
  // length = n_samples. In [0,1] for polarity-invariant mode.
  std::vector<double> corr;

  // Global Explained Variance (GEV), a common microstate summary measure.
  // Computed as sum_t (GFP(t)^2 * corr(t)^2) / sum_t GFP(t)^2.
  double gev{0.0};

  // Basic per-state stats (length = k).
  std::vector<double> coverage;            // fraction of samples assigned to each state
  std::vector<double> mean_duration_sec;   // mean segment duration
  std::vector<double> occurrence_per_sec;  // segments per second

  // Transition counts between consecutive segments (k x k).
  std::vector<std::vector<int>> transition_counts;
};

// Compute Global Field Power (GFP) over time for a recording.
// GFP is implemented as the per-sample standard deviation across channels.
std::vector<double> compute_gfp(const EEGRecording& rec);

// Estimate microstates on a full recording.
//
// Notes:
// - For best results, apply common-average reference and light bandpass beforehand.
// - The result's `templates` are ordered by cluster index; you can rename them A,B,C...
//   at the presentation layer.
MicrostatesResult estimate_microstates(const EEGRecording& rec, const MicrostatesOptions& opt);

} // namespace qeeg
