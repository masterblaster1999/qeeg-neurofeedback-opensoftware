#include "qeeg/biquad.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

static double rms(const std::vector<float>& x, size_t start = 0) {
  if (x.empty()) return 0.0;
  start = std::min(start, x.size());
  double s2 = 0.0;
  size_t n = 0;
  for (size_t i = start; i < x.size(); ++i) {
    const double v = static_cast<double>(x[i]);
    s2 += v * v;
    ++n;
  }
  return (n > 0) ? std::sqrt(s2 / static_cast<double>(n)) : 0.0;
}

static std::vector<float> sine(double fs_hz, double f_hz, double seconds, double amp = 1.0) {
  const size_t N = static_cast<size_t>(std::llround(seconds * fs_hz));
  std::vector<float> x;
  x.reserve(N);
  const double w = 2.0 * 3.141592653589793238462643383279502884 * f_hz;
  for (size_t n = 0; n < N; ++n) {
    const double t = static_cast<double>(n) / fs_hz;
    x.push_back(static_cast<float>(amp * std::sin(w * t)));
  }
  return x;
}

int main() {
  const double fs = 250.0;
  const double f_notch = 50.0;

  // 4 seconds gives time to settle; we'll discard the first second for RMS.
  auto x50 = sine(fs, 50.0, 4.0);
  auto x10 = sine(fs, 10.0, 4.0);

  const auto c = qeeg::design_notch(fs, f_notch, 30.0);
  qeeg::BiquadChain chain({c});

  auto y50 = x50;
  chain.reset();
  chain.process_inplace(&y50);

  auto y10 = x10;
  chain.reset();
  chain.process_inplace(&y10);

  const size_t discard = static_cast<size_t>(fs * 1.0);
  const double r50 = rms(y50, discard);
  const double r10 = rms(y10, discard);

  // Expect strong attenuation at the notch frequency.
  if (!(r50 < 0.35)) {
    std::cerr << "Notch filter insufficient attenuation at 50 Hz: rms=" << r50 << "\n";
    return 1;
  }

  // Expect little attenuation away from the notch frequency.
  if (!(r10 > 0.60)) {
    std::cerr << "Notch filter overly attenuated 10 Hz: rms=" << r10 << "\n";
    return 1;
  }

  std::cout << "OK: rms50=" << r50 << " rms10=" << r10 << "\n";
  return 0;
}
