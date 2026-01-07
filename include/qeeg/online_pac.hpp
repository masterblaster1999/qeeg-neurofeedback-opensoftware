#pragma once

#include "qeeg/pac.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

struct OnlinePacOptions {
  // Sliding analysis window.
  double window_seconds{4.0};

  // How often to emit a new PAC estimate.
  double update_seconds{0.25};

  // PAC estimator settings.
  PacOptions pac;

  OnlinePacOptions() {
    // By default, keep this more "online-friendly":
    // internal zero-phase filtering is disabled unless explicitly enabled.
    pac.zero_phase = false;
  }
};

struct OnlinePacFrame {
  double t_end_sec{0.0};
  double value{0.0};
};

// Online/windowed PAC estimator for a single channel.
//
// This mirrors the structure of OnlineWelchBandpower:
// - maintains a fixed-size ring buffer
// - periodically computes PAC on the most recent window
class OnlinePAC {
public:
  OnlinePAC(double fs_hz,
            BandDefinition phase_band,
            BandDefinition amp_band,
            OnlinePacOptions opt = {});

  double fs_hz() const { return fs_hz_; }

  // Push a block of samples for one channel.
  std::vector<OnlinePacFrame> push_block(const std::vector<float>& x);

private:
  struct Ring {
    std::vector<float> buf;
    size_t head{0};
    size_t count{0};
    explicit Ring(size_t cap);
    void push(float v);
    bool full() const;
    void extract(std::vector<float>* out) const; // oldest->newest
  };

  OnlinePacFrame compute_frame() const;

  double fs_hz_{0.0};
  BandDefinition phase_band_{};
  BandDefinition amp_band_{};
  OnlinePacOptions opt_{};

  size_t window_samples_{0};
  size_t update_samples_{0};

  Ring ring_;
  size_t total_samples_{0};
  size_t since_last_update_{0};
};

} // namespace qeeg
