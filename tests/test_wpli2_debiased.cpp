#include "qeeg/plv.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  using namespace qeeg;

  const double fs = 256.0;
  const double seconds = 10.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  const BandDefinition alpha{"alpha", 8.0, 12.0};

  std::vector<float> x(n, 0.0f);
  std::vector<float> y_same(n, 0.0f);
  std::vector<float> y_shift(n, 0.0f);
  std::vector<float> y_diff_freq(n, 0.0f);

  // Deterministic noise helps keep this test stable.
  std::mt19937 rng(123);
  std::normal_distribution<double> noise(0.0, 0.05);

  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double s10 = std::sin(2.0 * pi * 10.0 * t);
    const double s10_shift = std::sin(2.0 * pi * 10.0 * t + (pi / 2.0));
    const double s12 = std::sin(2.0 * pi * 12.0 * t);

    const double nx = noise(rng);
    const double ny = noise(rng);

    x[i] = static_cast<float>(s10 + nx);
    y_same[i] = static_cast<float>(s10 + ny);
    y_shift[i] = static_cast<float>(s10_shift + ny);
    y_diff_freq[i] = static_cast<float>(s12 + ny);
  }

  PlvOptions opt;
  opt.zero_phase = true;
  opt.edge_trim_fraction = 0.10;

  const double wpli2_same = compute_wpli2_debiased(x, y_same, fs, alpha, opt);
  const double wpli2_shift = compute_wpli2_debiased(x, y_shift, fs, alpha, opt);
  const double wpli2_diff = compute_wpli2_debiased(x, y_diff_freq, fs, alpha, opt);

  std::cerr << "wpli2_same=" << wpli2_same << "\n";
  std::cerr << "wpli2_shift=" << wpli2_shift << "\n";
  std::cerr << "wpli2_diff=" << wpli2_diff << "\n";

  expect(std::isfinite(wpli2_same), "wpli2_same should be finite");
  expect(std::isfinite(wpli2_shift), "wpli2_shift should be finite");
  expect(std::isfinite(wpli2_diff), "wpli2_diff should be finite");

  // In-phase oscillators should have near-zero lag metrics.
  expect(wpli2_same < 0.25, "wpli2_same should be low (<0.25)");

  // Fixed non-zero phase lag should produce a high lag metric.
  // Note: this is an estimator of wPLI^2, so values are typically a bit more conservative than wPLI.
  expect(wpli2_shift > 0.50, "wpli2_shift should be high (>0.50)");

  // Different frequency (still within the band) => drifting phase relationship => lower metric.
  expect(wpli2_diff < 0.60, "wpli2_diff should be lower (<0.60)");

  // Matrix sanity.
  {
    std::vector<std::vector<float>> chans;
    chans.push_back(x);
    chans.push_back(y_same);
    chans.push_back(y_shift);

    const auto m = compute_wpli2_debiased_matrix(chans, fs, alpha, opt);
    expect(m.size() == 3, "matrix should be 3x3");
    expect(m[0].size() == 3, "matrix row should be size 3");

    for (size_t i = 0; i < 3; ++i) {
      expect(std::isfinite(m[i][i]) && std::fabs(m[i][i]) < 1e-9, "diagonal should be 0 for wPLI2_debiased");
    }

    // x vs y_same (in-phase) should be low; x vs y_shift should be high.
    expect(m[0][1] < 0.30, "m[0][1] (in-phase) should be low");
    expect(m[0][2] > 0.50, "m[0][2] (lagged) should be high");
    expect(m[1][2] > 0.50, "m[1][2] (lagged) should be high");
  }

  std::cout << "wPLI2_debiased tests passed.\n";
  return 0;
}
