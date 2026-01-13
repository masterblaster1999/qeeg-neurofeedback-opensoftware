#pragma once

#include <algorithm>
#include <cmath>

#include "qeeg/nf_threshold.hpp"

namespace qeeg {

inline double clamp01_nonfinite_to0(double x) {
  if (!std::isfinite(x)) return 0.0;
  if (x < 0.0) return 0.0;
  if (x > 1.0) return 1.0;
  return x;
}

// Compute a continuous feedback value in [0, 1] from a metric and threshold.
//
// - reward_direction::Above: feedback increases as metric rises above threshold.
// - reward_direction::Below: feedback increases as metric falls below threshold.
// - span is the metric delta that maps to full-scale feedback (value==1).
//
// Notes:
// - If span is invalid, it is treated as 1.0.
// - If metric or threshold is non-finite, returns 0.
inline double feedback_value(double metric,
                             double threshold,
                             RewardDirection reward_direction,
                             double span) {
  if (!std::isfinite(metric) || !std::isfinite(threshold)) return 0.0;
  if (!(std::isfinite(span) && span > 0.0)) span = 1.0;

  const double delta = (reward_direction == RewardDirection::Above)
                         ? (metric - threshold)
                         : (threshold - metric);
  const double v = delta / span;
  return clamp01_nonfinite_to0(v);
}

} // namespace qeeg
