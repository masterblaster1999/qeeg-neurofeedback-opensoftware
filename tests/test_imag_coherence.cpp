#include "qeeg/coherence.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
  using namespace qeeg;

  const double fs = 256.0;
  const double seconds = 12.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  // A clean 10 Hz oscillator.
  std::vector<float> x(n, 0.0f);
  std::vector<float> y_inphase(n, 0.0f);
  std::vector<float> y_quarter(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double s = std::sin(2.0 * pi * 10.0 * t);
    x[i] = static_cast<float>(s);
    y_inphase[i] = static_cast<float>(s);
    y_quarter[i] = static_cast<float>(std::sin(2.0 * pi * 10.0 * t + 0.5 * pi));
  }

  WelchOptions opt;
  opt.nperseg = 512;
  opt.overlap_fraction = 0.5;

  const BandDefinition alpha{"alpha", 8.0, 12.0};

  const auto spec0 = welch_coherence_spectrum(x, y_inphase, fs, opt, CoherenceMeasure::ImaginaryCoherencyAbs);
  const auto spec90 = welch_coherence_spectrum(x, y_quarter, fs, opt, CoherenceMeasure::ImaginaryCoherencyAbs);

  const double im0 = average_band_value(spec0, alpha);
  const double im90 = average_band_value(spec90, alpha);

  std::cerr << "imcoh(alpha) in-phase=" << im0 << " quarter-cycle=" << im90 << "\n";

  assert(std::isfinite(im0));
  assert(std::isfinite(im90));

  // In-phase coupling has ~0 imaginary coherency.
  assert(im0 < 0.2);

  // Quarter-cycle lag should have a strong imaginary coherency component.
  assert(im90 > 0.5);
  assert(im90 > im0 + 0.3);

  std::cout << "Imaginary coherency test passed.\n";
  return 0;
}
