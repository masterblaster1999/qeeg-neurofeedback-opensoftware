#pragma once

#include "qeeg/plv.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qeeg {

// Online/windowed phase connectivity (PLV / PLI / wPLI / wPLI2_debiased).
//
// This mirrors the structure of OnlineWelchCoherence:
// - maintains a fixed-size ring buffer per channel
// - periodically computes a phase-based connectivity measure over the most recent window
// - reduces the result into band-averaged values for selected channel pairs

enum class PhaseConnectivityMeasure {
  PLV,
  PLI,
  WeightedPLI,
  WeightedPLI2Debiased,
};

inline std::string phase_connectivity_measure_name(PhaseConnectivityMeasure m) {
  switch (m) {
    case PhaseConnectivityMeasure::PLV: return "plv";
    case PhaseConnectivityMeasure::PLI: return "pli";
    case PhaseConnectivityMeasure::WeightedPLI: return "wpli";
    case PhaseConnectivityMeasure::WeightedPLI2Debiased: return "wpli2_debiased";
    default: return "unknown";
  }
}

struct OnlinePlvOptions {
  // Sliding analysis window length.
  double window_seconds{2.0};

  // How often to emit a new frame.
  double update_seconds{0.25};

  // Which phase-based measure to compute.
  PhaseConnectivityMeasure measure{PhaseConnectivityMeasure::PLV};

  // Under-the-hood settings (bandpass + Hilbert + edge trim).
  PlvOptions plv;

  OnlinePlvOptions() {
    // Be a bit more "online-friendly" by default:
    // forward-backward (zero-phase) filtering is disabled unless explicitly enabled.
    plv.zero_phase = false;
  }
};

struct OnlinePlvFrame {
  // Time (seconds) at the end of the analysis window (relative to start of stream).
  double t_end_sec{0.0};

  // Which measure was computed.
  PhaseConnectivityMeasure measure{PhaseConnectivityMeasure::PLV};

  std::vector<std::string> channel_names;
  std::vector<BandDefinition> bands;

  // Pair metadata. pairs[i] refers to indices in channel_names.
  std::vector<std::pair<int, int>> pairs;
  std::vector<std::string> pair_names; // same length as pairs ("A-B")

  // values[band_index][pair_index] in [0,1] (wPLI2_debiased is also in [0,1]).
  std::vector<std::vector<double>> values;
};

// Online/windowed phase connectivity engine.
//
// Notes:
// - The per-window estimator is identical to the offline PLV/PLI/wPLI functions:
//   bandpass -> analytic signal -> accumulate phase-lag statistic.
// - For true real-time streaming, causal filtering (zero_phase=false) is more appropriate.
class OnlinePlvConnectivity {
public:
  OnlinePlvConnectivity(std::vector<std::string> channel_names,
                        double fs_hz,
                        std::vector<BandDefinition> bands,
                        std::vector<std::pair<int, int>> pairs,
                        OnlinePlvOptions opt = {});

  size_t n_channels() const { return channel_names_.size(); }
  size_t n_pairs() const { return pairs_.size(); }
  double fs_hz() const { return fs_hz_; }

  std::vector<OnlinePlvFrame> push_block(const std::vector<std::vector<float>>& block);

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

  OnlinePlvFrame compute_frame() const;

  std::vector<std::string> channel_names_;
  double fs_hz_{0.0};
  std::vector<BandDefinition> bands_;
  std::vector<std::pair<int, int>> pairs_;
  std::vector<std::string> pair_names_;
  OnlinePlvOptions opt_{};

  size_t window_samples_{0};
  size_t update_samples_{0};

  std::vector<Ring> rings_;

  size_t total_samples_{0};
  size_t since_last_update_{0};
};

} // namespace qeeg
