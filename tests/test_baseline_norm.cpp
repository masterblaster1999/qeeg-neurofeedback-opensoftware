#include "qeeg/baseline.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

static bool approx(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  BaselineNormMode m;
  assert(parse_baseline_norm_mode("ratio", &m) && m == BaselineNormMode::Ratio);
  assert(parse_baseline_norm_mode("rel", &m) && m == BaselineNormMode::RelativeChange);
  assert(parse_baseline_norm_mode("logratio", &m) && m == BaselineNormMode::Log10Ratio);
  assert(parse_baseline_norm_mode("db", &m) && m == BaselineNormMode::Decibel);

  // Simple sanity checks.
  assert(approx(baseline_normalize(2.0, 1.0, BaselineNormMode::Ratio), 2.0));
  assert(approx(baseline_normalize(2.0, 1.0, BaselineNormMode::RelativeChange), 1.0));
  assert(approx(baseline_normalize(2.0, 1.0, BaselineNormMode::Log10Ratio), std::log10(2.0)));
  assert(approx(baseline_normalize(2.0, 1.0, BaselineNormMode::Decibel), 10.0 * std::log10(2.0)));

  // Invalid cases should return NaN (baseline <= 0).
  const double x = baseline_normalize(1.0, 0.0, BaselineNormMode::Ratio);
  assert(std::isnan(x));

  // Invalid cases should return NaN (epoch <= 0 for log modes).
  const double y = baseline_normalize(0.0, 1.0, BaselineNormMode::Log10Ratio);
  assert(std::isnan(y));

  std::cout << "test_baseline_norm OK\n";
  return 0;
}
