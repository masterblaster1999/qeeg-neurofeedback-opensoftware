#include "qeeg/spectral_features.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-8) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Case 1: constant PSD on [0,10].
  // - Total power should be width.
  // - Normalized entropy should be 1 (uniform distribution across frequency).
  // - SEF95 should be 9.5.
  PsdResult flat;
  for (int i = 0; i <= 10; ++i) {
    flat.freqs_hz.push_back(static_cast<double>(i));
    flat.psd.push_back(1.0);
  }

  const double total = spectral_total_power(flat, 0.0, 10.0);
  assert(approx(total, 10.0));

  const double H = spectral_entropy(flat, 0.0, 10.0, /*normalize=*/true);
  assert(approx(H, 1.0, 1e-12));

  const double sef95 = spectral_edge_frequency(flat, 0.0, 10.0, 0.95);
  assert(approx(sef95, 9.5, 1e-12));

  const double med = spectral_edge_frequency(flat, 0.0, 10.0, 0.5);
  assert(approx(med, 5.0, 1e-12));

  // Case 2: PSD proportional to frequency: P(f)=f.
  // Total power = ∫_0^10 f df = 50.
  // Median frequency solves ∫_0^x f df = 25 => x = sqrt(50).
  PsdResult ramp;
  for (int i = 0; i <= 10; ++i) {
    ramp.freqs_hz.push_back(static_cast<double>(i));
    ramp.psd.push_back(static_cast<double>(i));
  }
  const double total2 = spectral_total_power(ramp, 0.0, 10.0);
  assert(approx(total2, 50.0, 1e-12));

  const double med2 = spectral_edge_frequency(ramp, 0.0, 10.0, 0.5);
  const double expected_med2 = std::sqrt(50.0);
  assert(approx(med2, expected_med2, 1e-9));

  std::cout << "test_spectral_features OK\n";
  return 0;
}
