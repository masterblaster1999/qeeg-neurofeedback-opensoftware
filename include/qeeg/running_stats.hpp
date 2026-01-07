#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

namespace qeeg {

// Numerically-stable running mean/variance accumulator (Welford's algorithm).
//
// Notes:
// - add(x) ignores non-finite values.
// - variance_sample() uses (n-1) in the denominator and returns NaN if n < 2.
// - variance_population() uses n in the denominator and returns NaN if n < 1.
//
// This is useful when accumulating reference distributions over a dataset
// (e.g., qEEG bandpower means/stds per channel).
class RunningStats {
public:
  void clear() {
    n_ = 0;
    mean_ = 0.0;
    m2_ = 0.0;
  }

  void add(double x) {
    if (!std::isfinite(x)) return;
    ++n_;
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(n_);
    const double delta2 = x - mean_;
    m2_ += delta * delta2;
  }

  size_t n() const { return n_; }
  double mean() const { return (n_ == 0) ? std::numeric_limits<double>::quiet_NaN() : mean_; }

  double variance_population() const {
    if (n_ < 1) return std::numeric_limits<double>::quiet_NaN();
    return m2_ / static_cast<double>(n_);
  }

  double variance_sample() const {
    if (n_ < 2) return std::numeric_limits<double>::quiet_NaN();
    return m2_ / static_cast<double>(n_ - 1);
  }

  double stddev_population() const {
    const double v = variance_population();
    return std::isfinite(v) ? std::sqrt(v) : v;
  }

  double stddev_sample() const {
    const double v = variance_sample();
    return std::isfinite(v) ? std::sqrt(v) : v;
  }

private:
  size_t n_{0};
  double mean_{0.0};
  double m2_{0.0};
};

} // namespace qeeg
