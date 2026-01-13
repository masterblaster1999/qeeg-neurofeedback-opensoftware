#pragma once

#include <cmath>
#include <limits>

namespace qeeg {

// Reward shaping for online / pseudo-realtime neurofeedback.
//
// Operates on a per-update boolean "raw_reward" signal (e.g., metric above/below
// threshold). Two optional behaviors can be enabled:
//   - Dwell: require raw_reward to remain true for dwell_seconds before reward turns on.
//   - Refractory: after reward turns off, require refractory_seconds to elapse before
//                 it can turn on again.
//
// The shaper is intentionally simple: it gates a boolean stream, independent of the
// underlying metric value.
class RewardShaper {
public:
  RewardShaper() = default;
  RewardShaper(double dwell_seconds, double refractory_seconds) {
    set_dwell_seconds(dwell_seconds);
    set_refractory_seconds(refractory_seconds);
  }

  void reset() {
    dwell_accum_sec_ = 0.0;
    out_prev_ = false;
    last_reward_off_time_sec_ = std::numeric_limits<double>::quiet_NaN();
  }

  void set_dwell_seconds(double s) {
    dwell_seconds_ = (std::isfinite(s) && s > 0.0) ? s : 0.0;
    if (dwell_seconds_ == 0.0) dwell_accum_sec_ = 0.0;
  }

  void set_refractory_seconds(double s) {
    refractory_seconds_ = (std::isfinite(s) && s > 0.0) ? s : 0.0;
  }

  double dwell_seconds() const { return dwell_seconds_; }
  double refractory_seconds() const { return refractory_seconds_; }

  // Update and return the shaped reward.
  //
  // - raw_reward: the instantaneous reward condition.
  // - dt_seconds: time since previous update (seconds); used to accumulate dwell time.
  // - t_end_sec:  current update timestamp (seconds); used for refractory.
  // - freeze: if true, reward is forced off and dwell accumulation resets.
  bool update(bool raw_reward, double dt_seconds, double t_end_sec, bool freeze) {
    const double dt = (std::isfinite(dt_seconds) && dt_seconds > 0.0) ? dt_seconds : 0.0;

    if (freeze) {
      dwell_accum_sec_ = 0.0;
      if (out_prev_ && std::isfinite(t_end_sec)) {
        last_reward_off_time_sec_ = t_end_sec;
      }
      out_prev_ = false;
      return false;
    }

    if (!raw_reward) {
      dwell_accum_sec_ = 0.0;
      if (out_prev_ && std::isfinite(t_end_sec)) {
        last_reward_off_time_sec_ = t_end_sec;
      }
      out_prev_ = false;
      return false;
    }

    // raw_reward is true.
    if (dwell_seconds_ > 0.0) {
      dwell_accum_sec_ += dt;
      if (dwell_accum_sec_ < dwell_seconds_) {
        out_prev_ = false;
        return false;
      }
    }

    // Already rewarding: remain ON until raw_reward turns off.
    if (out_prev_) {
      return true;
    }

    // Turning ON: enforce refractory since the last time reward turned off.
    if (refractory_seconds_ > 0.0 && std::isfinite(last_reward_off_time_sec_) && std::isfinite(t_end_sec)) {
      const double since = t_end_sec - last_reward_off_time_sec_;
      if (!std::isfinite(since) || since < refractory_seconds_) {
        return false;
      }
    }

    out_prev_ = true;
    return true;
  }

private:
  double dwell_seconds_{0.0};
  double refractory_seconds_{0.0};
  double dwell_accum_sec_{0.0};
  bool out_prev_{false};
  double last_reward_off_time_sec_{std::numeric_limits<double>::quiet_NaN()};
};

} // namespace qeeg
