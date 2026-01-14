#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace qeeg {

// Small robust statistics helpers used across the project.
//
// - median_inplace(): O(n) average time via nth_element (modifies the input vector)
// - quantile_inplace(): O(n) average time via nth_element (modifies the input vector)
// - robust_scale(): median absolute deviation (MAD) scaled to be consistent with
//   the standard deviation for Gaussian data, with a fallback to sample standard
//   deviation when the MAD is ~0.

inline double median_inplace(std::vector<double>* v) {
  if (!v || v->empty()) return 0.0;
  const size_t n = v->size();
  const size_t mid = n / 2;
  std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid), v->end());
  double med = (*v)[mid];
  if (n % 2 == 0) {
    // Need the lower middle as well.
    auto max_it = std::max_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid));
    med = 0.5 * (med + *max_it);
  }
  return med;
}

// Convenience overload: accepts a non-null vector by reference.
inline double median_inplace(std::vector<double>& v) { return median_inplace(&v); }


// Linearly-interpolated empirical quantile.
//
// - q is clamped to [0,1].
// - q=0 returns min, q=1 returns max.
// - For 0<q<1, uses linear interpolation between the two nearest order statistics
//   at index q*(n-1).
//
// This is intended for robust threshold initialization and other lightweight uses.
// It is not meant to be a full featured statistics package.
inline double quantile_inplace(std::vector<double>* v, double q) {
  if (!v || v->empty()) return 0.0;

  if (!std::isfinite(q)) q = 0.5;
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;

  const size_t n = v->size();
  if (n == 1) return (*v)[0];

  const double idx = q * static_cast<double>(n - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = static_cast<size_t>(std::ceil(idx));

  std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(lo), v->end());
  const double a = (*v)[lo];
  if (hi == lo) return a;

  std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(hi), v->end());
  const double b = (*v)[hi];

  const double t = idx - static_cast<double>(lo);
  return a + (b - a) * t;
}

// Convenience overload: accepts a non-null vector by reference.
inline double quantile_inplace(std::vector<double>& v, double q) { return quantile_inplace(&v, q); }


inline double quantile(const std::vector<double>& values, double q) {
  std::vector<double> tmp = values;
  return quantile_inplace(&tmp, q);
}

inline double robust_scale(const std::vector<double>& values, double med) {
  if (values.empty()) return 1.0;

  std::vector<double> absdev;
  absdev.reserve(values.size());
  for (double x : values) absdev.push_back(std::fabs(x - med));

  const double mad = median_inplace(&absdev);

  // 1.4826 is 1 / Phi^{-1}(0.75), which makes MAD consistent with std for normal data.
  double scale = mad * 1.4826;

  // If MAD is ~0 (constant-ish data), fall back to sample stddev.
  if (!(scale > 1e-12)) {
    if (values.size() >= 2) {
      double sum = 0.0;
      for (double x : values) sum += x;
      const double mean = sum / static_cast<double>(values.size());
      double acc = 0.0;
      for (double x : values) {
        const double d = x - mean;
        acc += d * d;
      }
      const double var = acc / static_cast<double>(values.size() - 1);
      scale = std::sqrt(std::max(0.0, var));
    }
  }

  if (!(scale > 1e-12)) scale = 1.0;
  return scale;
}

} // namespace qeeg
