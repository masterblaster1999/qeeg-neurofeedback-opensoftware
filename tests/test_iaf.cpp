#include "qeeg/iaf.hpp"

#include <cmath>
#include <iostream>
#include <random>
#include <vector>

using namespace qeeg;

static void expect_true(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "TEST FAIL: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  // Synthetic EEG-like signal with a dominant 10 Hz alpha component.
  const double fs = 250.0;
  const double f_alpha = 10.0;
  const double seconds = 30.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));

  std::vector<float> x(n, 0.0f);
  const double pi = std::acos(-1.0);

  std::mt19937 rng(123);
  std::normal_distribution<double> noise(0.0, 0.5);

  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double sig =
        2.0 * std::sin(2.0 * pi * f_alpha * t) +
        0.6 * std::sin(2.0 * pi * 20.0 * t) +
        noise(rng);
    x[i] = static_cast<float>(sig);
  }

  WelchOptions wopt;
  wopt.nperseg = 1024;
  wopt.overlap_fraction = 0.5;

  IafOptions iopt;
  iopt.alpha_min_hz = 7.0;
  iopt.alpha_max_hz = 13.0;
  iopt.smooth_hz = 1.0;
  iopt.min_prominence_db = 0.1;

  IafEstimate est = estimate_iaf_from_signal(x, fs, wopt, iopt);
  expect_true(est.found, "expected to find an alpha peak");
  expect_true(std::fabs(est.iaf_hz - f_alpha) < 1.0, "IAF not near 10 Hz");
  expect_true(std::isfinite(est.cog_hz), "expected a finite CoG estimate");
  expect_true(std::fabs(est.cog_hz - f_alpha) < 1.0, "CoG not near 10 Hz");

  std::cout << "test_iaf OK (iaf_hz=" << est.iaf_hz << ", cog_hz=" << est.cog_hz << ")\n";
  return 0;
}
