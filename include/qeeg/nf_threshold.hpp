#pragma once

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>

namespace qeeg {

enum class RewardDirection {
  Above, // reward when value > threshold
  Below  // reward when value < threshold
};

inline std::string reward_direction_name(RewardDirection d) {
  return (d == RewardDirection::Above) ? "above" : "below";
}

inline RewardDirection parse_reward_direction(std::string s) {
  s = trim(s);
  std::string t;
  t.reserve(s.size());
  for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (t == "above" || t == "gt" || t == ">" || t == "higher" || t == "high" || t == "up") {
    return RewardDirection::Above;
  }
  if (t == "below" || t == "lt" || t == "<" || t == "lower" || t == "low" || t == "down") {
    return RewardDirection::Below;
  }

  throw std::runtime_error("Invalid reward direction: '" + s + "' (expected 'above' or 'below')");
}

inline bool is_reward(double value, double threshold, RewardDirection dir) {
  return (dir == RewardDirection::Above) ? (value > threshold) : (value < threshold);
}

// Adaptive threshold update intended for NF-style "keep reward-rate near target".
//
// Uses a simple exponential adjustment:
//   thr *= exp( eta * (reward_rate - target_rate) )
//
// For RewardDirection::Below, the sign is inverted so the controller behavior
// remains intuitive (too many rewards => lower threshold; too few => raise threshold).
inline double adapt_threshold(double threshold,
                              double reward_rate,
                              double target_rate,
                              double eta,
                              RewardDirection dir) {
  if (!std::isfinite(threshold) || !std::isfinite(reward_rate) || !std::isfinite(target_rate) ||
      !std::isfinite(eta) || eta <= 0.0) {
    return threshold;
  }

  double exponent = eta * (reward_rate - target_rate);
  if (dir == RewardDirection::Below) exponent = -exponent;

  // Avoid "stuck at zero" (multiplying 0 by exp(...) stays 0 forever).
  constexpr double kMinAbsThreshold = 1e-12;
  if (std::fabs(threshold) < kMinAbsThreshold) {
    threshold = (threshold >= 0.0) ? kMinAbsThreshold : -kMinAbsThreshold;
  }

  return threshold * std::exp(exponent);
}

} // namespace qeeg
