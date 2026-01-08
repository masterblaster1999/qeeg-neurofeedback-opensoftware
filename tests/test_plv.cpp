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
  std::mt19937 rng(42);
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

  const double plv_same = compute_plv(x, y_same, fs, alpha, opt);
  const double plv_shift = compute_plv(x, y_shift, fs, alpha, opt);
  const double plv_diff = compute_plv(x, y_diff_freq, fs, alpha, opt);

  std::cerr << "plv_same=" << plv_same << "\n";
  std::cerr << "plv_shift=" << plv_shift << "\n";
  std::cerr << "plv_diff=" << plv_diff << "\n";

  expect(std::isfinite(plv_same), "plv_same should be finite");
  expect(std::isfinite(plv_shift), "plv_shift should be finite");
  expect(std::isfinite(plv_diff), "plv_diff should be finite");

  // Same-frequency oscillator should be highly phase-locked.
  expect(plv_same > 0.85, "plv_same should be high (>0.85)");

  // A fixed phase offset is still perfectly locked.
  expect(plv_shift > 0.85, "plv_shift should be high (>0.85)");

  // A different frequency within the band should have a drifting phase
  // relationship => lower PLV.
  expect(plv_diff < 0.65, "plv_diff should be lower (<0.65)");

  // Matrix sanity: identical signals => PLV close to 1 off-diagonal.
  {
    std::vector<std::vector<float>> chans;
    chans.push_back(x);
    chans.push_back(y_same);
    chans.push_back(y_shift);

    const auto m = compute_plv_matrix(chans, fs, alpha, opt);
    expect(m.size() == 3, "matrix should be 3x3");
    expect(m[0].size() == 3, "matrix row should be size 3");

    for (size_t i = 0; i < 3; ++i) {
      expect(std::isfinite(m[i][i]) && std::fabs(m[i][i] - 1.0) < 1e-9,
             "diagonal should be 1");
    }

    expect(m[0][1] > 0.80, "m[0][1] should be high");
    expect(m[0][2] > 0.80, "m[0][2] should be high");
    expect(m[1][2] > 0.80, "m[1][2] should be high");
  }

  std::cout << "PLV tests passed.\n";
  return 0;
}
