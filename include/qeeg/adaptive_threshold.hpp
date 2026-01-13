#pragma once

#include "qeeg/nf_threshold.hpp"
#include "qeeg/robust_stats.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qeeg {

// Adaptive threshold controller for neurofeedback.
//
// Supported modes:
//   - Exponential: multiplicative controller based on reward-rate error
//       thr <- thr * exp(eta * (rr - target))
//     (implemented by adapt_threshold() in nf_threshold.hpp)
//
//   - Quantile: maintain a rolling window of recent metric values and set the
//     threshold to the empirical quantile implied by the desired reward-rate.
//     For reward-above:  thr <- F^{-1}(1 - target)
//     For reward-below:  thr <- F^{-1}(target)
//
// Quantile mode optionally blends toward the desired threshold using eta:
//   thr <- thr + eta * (thr_desired - thr)
//
// This implementation is intentionally dependency-free and fast enough for
// interactive offline playback.

enum class AdaptMode {
  Exponential,
  Quantile
};

inline std::string adapt_mode_name(AdaptMode m) {
  switch (m) {
    case AdaptMode::Exponential: return "exp";
    case AdaptMode::Quantile: return "quantile";
  }
  return "exp";
}

inline AdaptMode parse_adapt_mode(std::string s) {
  s = trim(s);
  std::string t;
  t.reserve(s.size());
  for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (t.empty() || t == "exp" || t == "exponential" || t == "mul" || t == "multiplicative") {
    return AdaptMode::Exponential;
  }
  if (t == "quantile" || t == "pct" || t == "percentile" || t == "q") {
    return AdaptMode::Quantile;
  }
  throw std::runtime_error("Invalid adapt mode: '" + s + "' (expected 'exp' or 'quantile')");
}

struct AdaptiveThresholdConfig {
  AdaptMode mode{AdaptMode::Exponential};
  RewardDirection reward_direction{RewardDirection::Above};
  double target_reward_rate{0.6};

  // Meaning depends on mode:
  //  - Exponential: eta is the multiplicative controller gain (see nf_threshold.hpp)
  //  - Quantile: eta is the blend factor in (0,1] used to smooth threshold changes
  double eta{0.10};

  // Only update the threshold if at least this many seconds elapsed since
  // the last update. 0 disables the interval gate (updates every frame).
  double update_interval_seconds{0.0};

  // Quantile mode: rolling window size in seconds.
  double quantile_window_seconds{30.0};
  // Quantile mode: require at least this many samples in the rolling window.
  size_t quantile_min_samples{20};
};

class AdaptiveThresholdController {
 public:
  explicit AdaptiveThresholdController(AdaptiveThresholdConfig cfg)
      : cfg_(std::move(cfg)) {}

  void reset() {
    hist_.clear();
    last_update_t_ = std::numeric_limits<double>::quiet_NaN();
    update_count_ = 0;
    last_desired_threshold_ = std::numeric_limits<double>::quiet_NaN();
  }

  const AdaptiveThresholdConfig& config() const { return cfg_; }

  AdaptMode mode() const { return cfg_.mode; }

  // Adds an observation to the rolling window (Quantile mode only).
  // Safe to call for all modes (noop for non-quantile).
  void observe(double t_end_sec, double metric_value) {
    if (cfg_.mode != AdaptMode::Quantile) return;
    if (!std::isfinite(t_end_sec) || !std::isfinite(metric_value)) return;

    prune(t_end_sec);
    hist_.push_back({t_end_sec, metric_value});
    prune(t_end_sec);
  }

  // Removes old samples from the rolling window.
  void prune(double t_end_sec) {
    if (cfg_.mode != AdaptMode::Quantile) return;
    if (!std::isfinite(t_end_sec)) return;
    if (!(cfg_.quantile_window_seconds > 0.0)) return;

    while (!hist_.empty()) {
      const double age = t_end_sec - hist_.front().first;
      if (std::isfinite(age) && age > cfg_.quantile_window_seconds) {
        hist_.pop_front();
        continue;
      }
      break;
    }
  }

  size_t history_size() const { return hist_.size(); }
  size_t update_count() const { return update_count_; }
  double last_desired_threshold() const { return last_desired_threshold_; }

  // The target quantile implied by (reward_direction, target_reward_rate).
  double target_quantile() const {
    const double r = cfg_.target_reward_rate;
    const double q = (cfg_.reward_direction == RewardDirection::Above) ? (1.0 - r) : r;
    return clamp01(q);
  }

  // Update the threshold based on the configured mode.
  //
  // - current_threshold: the current threshold (must be finite to update)
  // - reward_rate: recent reward rate in [0,1] (used by Exponential mode)
  // - t_end_sec: current time (seconds) used for the update interval gate
  double update(double current_threshold, double reward_rate, double t_end_sec) {
    if (!std::isfinite(current_threshold)) return current_threshold;
    if (!(cfg_.eta > 0.0) || !std::isfinite(cfg_.eta)) return current_threshold;

    // Optional interval gate.
    if (cfg_.update_interval_seconds > 0.0 && std::isfinite(last_update_t_) && std::isfinite(t_end_sec)) {
      const double dt = t_end_sec - last_update_t_;
      if (std::isfinite(dt) && dt >= 0.0 && dt < cfg_.update_interval_seconds) {
        return current_threshold;
      }
    }

    double next = current_threshold;
    if (cfg_.mode == AdaptMode::Exponential) {
      next = adapt_threshold(current_threshold,
                            reward_rate,
                            cfg_.target_reward_rate,
                            cfg_.eta,
                            cfg_.reward_direction);
      if (next != current_threshold) {
        ++update_count_;
        if (std::isfinite(t_end_sec)) last_update_t_ = t_end_sec;
      }
      return next;
    }

    // Quantile mode.
    prune(t_end_sec);
    if (hist_.size() < std::max<size_t>(1, cfg_.quantile_min_samples)) {
      return current_threshold;
    }

    std::vector<double> values;
    values.reserve(hist_.size());
    for (const auto& p : hist_) values.push_back(p.second);

    const double q = target_quantile();
    const double desired = quantile_inplace(&values, q);
    last_desired_threshold_ = desired;

    if (!std::isfinite(desired)) return current_threshold;

    const double a = clamp01(cfg_.eta);
    next = current_threshold + a * (desired - current_threshold);

    ++update_count_;
    if (std::isfinite(t_end_sec)) last_update_t_ = t_end_sec;
    return next;
  }

 private:
  static double clamp01(double x) {
    if (!std::isfinite(x)) return 0.5;
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
  }

  AdaptiveThresholdConfig cfg_;

  // Only used in Quantile mode.
  std::deque<std::pair<double, double>> hist_;

  // Common bookkeeping.
  double last_update_t_{std::numeric_limits<double>::quiet_NaN()};
  size_t update_count_{0};
  double last_desired_threshold_{std::numeric_limits<double>::quiet_NaN()};
};

} // namespace qeeg
