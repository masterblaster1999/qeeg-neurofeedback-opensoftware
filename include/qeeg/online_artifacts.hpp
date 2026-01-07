#pragma once

#include "qeeg/artifacts.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

// Online artifact gating for neurofeedback-style loops.
//
// This is intentionally pragmatic and dependency-light: it computes a few
// time-domain features on a sliding window and compares them to robust
// baseline statistics (median + MAD-derived scale).
//
// Use-case:
// - suppress reward / adaptive threshold updates during gross artifacts
// - optionally export an artifact time series aligned with NF updates
//
// ⚠️ Research / educational use only.

struct OnlineArtifactOptions {
  // Sliding window parameters. Use the same values as your online metric
  // engine so frames align (e.g., qeeg_nf_cli).
  double window_seconds{2.0};
  double update_seconds{0.25};

  // Baseline period used to estimate robust per-channel thresholds.
  double baseline_seconds{10.0};

  // Robust z-score thresholds. If <= 0, that feature is disabled.
  double ptp_z{6.0};
  double rms_z{6.0};
  double kurtosis_z{6.0};

  // Frame is "bad" if at least this many channels are flagged.
  size_t min_bad_channels{1};
};

struct OnlineArtifactFrame {
  // Time (seconds) at the end of the analysis window.
  double t_end_sec{0.0};

  // Whether baseline stats have been computed.
  bool baseline_ready{false};

  bool bad{false};
  size_t bad_channel_count{0};

  // Debug summaries (max z-score across channels for each feature).
  double max_ptp_z{0.0};
  double max_rms_z{0.0};
  double max_kurtosis_z{0.0};
};

class OnlineArtifactGate {
public:
  OnlineArtifactGate(std::vector<std::string> channel_names,
                     double fs_hz,
                     OnlineArtifactOptions opt = {});

  size_t n_channels() const { return channel_names_.size(); }
  double fs_hz() const { return fs_hz_; }
  bool baseline_ready() const { return baseline_ready_; }
  const std::vector<ArtifactChannelStats>& baseline_stats() const { return baseline_stats_; }

  // Push a block of samples for all channels.
  // block[ch][i] is sample i of channel ch. All channels must have the same length.
  // Returns 0 or more frames (depending on how many updates occurred).
  std::vector<OnlineArtifactFrame> push_block(const std::vector<std::vector<float>>& block);

private:
  struct Ring {
    std::vector<float> buf;
    size_t head{0};
    size_t count{0};
    explicit Ring(size_t cap);
    void push(float x);
    bool full() const;
  };

  struct RawFeatures {
    std::vector<double> ptp;
    std::vector<double> rms;
    std::vector<double> kurtosis;
  };

  RawFeatures compute_raw_features() const;
  void ensure_baseline_stats_built(double t_end_sec);

  std::vector<std::string> channel_names_;
  double fs_hz_{0.0};
  OnlineArtifactOptions opt_;

  size_t window_samples_{0};
  size_t update_samples_{0};
  size_t baseline_end_samples_{0};

  std::vector<Ring> rings_;
  size_t total_samples_{0};
  size_t since_last_update_{0};

  // Baseline accumulation (per-channel).
  bool baseline_ready_{false};
  std::vector<std::vector<double>> base_ptp_;
  std::vector<std::vector<double>> base_rms_;
  std::vector<std::vector<double>> base_kurt_;
  std::vector<ArtifactChannelStats> baseline_stats_;
};

} // namespace qeeg
