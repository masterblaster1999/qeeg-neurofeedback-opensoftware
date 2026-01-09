#pragma once

#include "qeeg/coherence.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qeeg {

struct OnlineCoherenceOptions {
  // Sliding analysis window length.
  double window_seconds{2.0};

  // How often to emit a new frame.
  double update_seconds{0.25};

  // Welch parameters used for per-frame coherence estimation.
  WelchOptions welch;

  // Which coherence-like measure to compute.
  // Default matches historical behavior: magnitude-squared coherence.
  CoherenceMeasure measure{CoherenceMeasure::MagnitudeSquared};
};

struct OnlineCoherenceFrame {
  // Time (seconds) at the end of the analysis window (relative to start of stream).
  double t_end_sec{0.0};

  // Which coherence-like measure was computed for this frame.
  CoherenceMeasure measure{CoherenceMeasure::MagnitudeSquared};

  std::vector<std::string> channel_names;
  std::vector<BandDefinition> bands;

  // Pair metadata. pairs[i] refers to indices in channel_names.
  std::vector<std::pair<int,int>> pairs;
  std::vector<std::string> pair_names; // same length as pairs ("A-B")

  // coherences[band_index][pair_index] in [0,1]
  std::vector<std::vector<double>> coherences;
};

// Online coherence estimator:
// - maintains a ring buffer per channel
// - periodically computes Welch magnitude-squared coherence for selected pairs
// - reduces each coherence spectrum into band-averaged values
class OnlineWelchCoherence {
public:
  OnlineWelchCoherence(std::vector<std::string> channel_names,
                       double fs_hz,
                       std::vector<BandDefinition> bands,
                       std::vector<std::pair<int,int>> pairs,
                       OnlineCoherenceOptions opt = {});

  size_t n_channels() const { return channel_names_.size(); }
  size_t n_pairs() const { return pairs_.size(); }
  double fs_hz() const { return fs_hz_; }

  std::vector<OnlineCoherenceFrame> push_block(const std::vector<std::vector<float>>& block);

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

  OnlineCoherenceFrame compute_frame() const;

  std::vector<std::string> channel_names_;
  double fs_hz_{0.0};
  std::vector<BandDefinition> bands_;
  std::vector<std::pair<int,int>> pairs_;
  std::vector<std::string> pair_names_;
  OnlineCoherenceOptions opt_{};

  size_t window_samples_{0};
  size_t update_samples_{0};

  std::vector<Ring> rings_;

  size_t total_samples_{0};
  size_t since_last_update_{0};
};

} // namespace qeeg
