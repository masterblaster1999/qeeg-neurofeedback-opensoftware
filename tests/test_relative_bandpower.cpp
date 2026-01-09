#include "qeeg/bandpower.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Constant PSD: relative power should reduce to width(band)/width(total).
  PsdResult psd;
  psd.freqs_hz = {0,1,2,3,4,5,6,7,8,9,10};
  psd.psd.assign(psd.freqs_hz.size(), 2.0);

  const double rel1 = compute_relative_bandpower(psd, 2.0, 4.0, 0.0, 10.0);
  assert(approx(rel1, 0.2));

  // Non-aligned boundaries (requires interpolation). With constant PSD:
  // band width = 2.0, total width = 9.0 => 2/9.
  const double rel2 = compute_relative_bandpower(psd, 2.5, 4.5, 0.5, 9.5);
  assert(approx(rel2, 2.0 / 9.0));

  // Near-zero total power should return 0.
  PsdResult zero;
  zero.freqs_hz = {0, 1, 2};
  zero.psd = {0.0, 0.0, 0.0};
  const double rel3 = compute_relative_bandpower(zero, 0.0, 1.0, 0.0, 2.0);
  assert(rel3 == 0.0);

  std::cout << "test_relative_bandpower OK\n";
  return 0;
}
