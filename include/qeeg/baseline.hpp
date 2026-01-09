#pragma once

#include "qeeg/utils.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace qeeg {

// Baseline normalization modes (commonly used for event-related features).
//
// Given epoch_power (E) and baseline_power (B):
//  - ratio:       E / B
//  - rel:         (E - B) / B
//  - logratio:    log10(E / B)
//  - db:          10 * log10(E / B)
//
// The functions below return NaN when the requested transform is not
// numerically well-defined (e.g. baseline_power <= 0).
enum class BaselineNormMode {
  Ratio,
  RelativeChange,
  Log10Ratio,
  Decibel,
};

inline std::string baseline_mode_name(BaselineNormMode mode) {
  switch (mode) {
    case BaselineNormMode::Ratio:
      return "ratio";
    case BaselineNormMode::RelativeChange:
      return "rel";
    case BaselineNormMode::Log10Ratio:
      return "logratio";
    case BaselineNormMode::Decibel:
      return "db";
    default:
      return "unknown";
  }
}

inline bool parse_baseline_norm_mode(const std::string& s_in, BaselineNormMode* out_mode) {
  if (!out_mode) return false;
  std::string s = to_lower(trim(s_in));
  if (s.empty()) return false;

  if (s == "ratio" || s == "r") {
    *out_mode = BaselineNormMode::Ratio;
    return true;
  }
  if (s == "rel" || s == "relative" || s == "relative_change" || s == "relchange") {
    *out_mode = BaselineNormMode::RelativeChange;
    return true;
  }
  if (s == "logratio" || s == "log" || s == "log10" || s == "log10ratio") {
    *out_mode = BaselineNormMode::Log10Ratio;
    return true;
  }
  if (s == "db" || s == "decibel" || s == "dB") {
    *out_mode = BaselineNormMode::Decibel;
    return true;
  }
  return false;
}

inline double baseline_normalize(double epoch_power,
                                 double baseline_power,
                                 BaselineNormMode mode,
                                 double eps = 1e-20) {
  (void)eps;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  if (!std::isfinite(epoch_power) || !std::isfinite(baseline_power)) return nan;
  if (baseline_power <= 0.0) return nan;

  switch (mode) {
    case BaselineNormMode::Ratio:
      return epoch_power / baseline_power;
    case BaselineNormMode::RelativeChange:
      return (epoch_power - baseline_power) / baseline_power;
    case BaselineNormMode::Log10Ratio:
      if (epoch_power <= 0.0) return nan;
      return std::log10(epoch_power / baseline_power);
    case BaselineNormMode::Decibel:
      if (epoch_power <= 0.0) return nan;
      return 10.0 * std::log10(epoch_power / baseline_power);
    default:
      return nan;
  }
}

} // namespace qeeg
