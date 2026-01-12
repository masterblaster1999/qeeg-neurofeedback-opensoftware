#pragma once

#include <cstddef>

namespace qeeg {

// Simple boolean debouncer / hysteresis latch.
//
// Motivation:
// In real-time pipelines, thresholded boolean decisions can flicker due to noise.
// A common mitigation is to require N consecutive frames of a condition before
// changing the output state.
//
// Behavior:
// - If the current state is OFF, it will only turn ON after `on_count` consecutive
//   true inputs.
// - If the current state is ON, it will only turn OFF after `off_count` consecutive
//   false inputs.
class BoolDebouncer {
public:
  explicit BoolDebouncer(size_t on_count = 1, size_t off_count = 1, bool initial_state = false)
      : on_count_(on_count ? on_count : 1),
        off_count_(off_count ? off_count : 1),
        state_(initial_state) {}

  size_t on_count() const { return on_count_; }
  size_t off_count() const { return off_count_; }
  bool state() const { return state_; }

  // Reset internal counters and set the current output state.
  void reset(bool state = false) {
    state_ = state;
    on_run_ = 0;
    off_run_ = 0;
  }

  // Update using the new input and return the current output state.
  bool update(bool in) {
    if (in) {
      ++on_run_;
      off_run_ = 0;
      if (!state_ && on_run_ >= on_count_) {
        state_ = true;
        // Keep runs bounded (not strictly necessary, but avoids growth).
        on_run_ = on_count_;
      }
    } else {
      ++off_run_;
      on_run_ = 0;
      if (state_ && off_run_ >= off_count_) {
        state_ = false;
        off_run_ = off_count_;
      }
    }
    return state_;
  }

private:
  size_t on_count_{1};
  size_t off_count_{1};
  bool state_{false};
  size_t on_run_{0};
  size_t off_run_{0};
};

} // namespace qeeg
