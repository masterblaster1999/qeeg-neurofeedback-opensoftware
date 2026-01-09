#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace qeeg {

// Small robust statistics helpers used across the project.
//
// - median_inplace(): O(n) average time via nth_element (modifies the input vector)
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
