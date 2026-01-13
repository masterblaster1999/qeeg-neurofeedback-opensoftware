#pragma once

#include "qeeg/nf_threshold.hpp"

#include <cmath>

namespace qeeg {

// Numeric hysteresis (Schmitt trigger) around a threshold.
//
// If hysteresis == 0, update() degenerates to is_reward(value, threshold, dir).
//
// For RewardDirection::Above:
//   - switch ON  when value > threshold + hysteresis
//   - switch OFF when value < threshold - hysteresis
//
// For RewardDirection::Below:
//   - switch ON  when value < threshold - hysteresis
//   - switch OFF when value > threshold + hysteresis
//
// Notes:
//   - Comparisons are strict ('>' and '<') to match is_reward() semantics.
//   - Non-finite value/threshold forces state=false.
class HysteresisGate {
 public:
  HysteresisGate() = default;

  HysteresisGate(double hysteresis, RewardDirection dir, bool initial_state = false)
      : hysteresis_(sanitize(hysteresis)), dir_(dir), state_(initial_state) {}

  void reset(bool state = false) { state_ = state; }

  void set_hysteresis(double h) { hysteresis_ = sanitize(h); }
  double hysteresis() const { return hysteresis_; }

  void set_direction(RewardDirection d) { dir_ = d; }
  RewardDirection direction() const { return dir_; }

  bool state() const { return state_; }

  bool update(double value, double threshold) {
    if (!std::isfinite(value) || !std::isfinite(threshold)) {
      state_ = false;
      return state_;
    }

    if (!(hysteresis_ > 0.0)) {
      state_ = is_reward(value, threshold, dir_);
      return state_;
    }

    const double h = hysteresis_;
    if (dir_ == RewardDirection::Above) {
      if (!state_) {
        if (value > threshold + h) state_ = true;
      } else {
        if (value < threshold - h) state_ = false;
      }
    } else { // Below
      if (!state_) {
        if (value < threshold - h) state_ = true;
      } else {
        if (value > threshold + h) state_ = false;
      }
    }

    return state_;
  }

 private:
  static double sanitize(double h) {
    return (std::isfinite(h) && h > 0.0) ? h : 0.0;
  }

  double hysteresis_{0.0};
  RewardDirection dir_{RewardDirection::Above};
  bool state_{false};
};

} // namespace qeeg
