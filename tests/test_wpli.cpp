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

  const double pli_same = compute_pli(x, y_same, fs, alpha, opt);
  const double pli_shift = compute_pli(x, y_shift, fs, alpha, opt);
  const double pli_diff = compute_pli(x, y_diff_freq, fs, alpha, opt);

  const double wpli_same = compute_wpli(x, y_same, fs, alpha, opt);
  const double wpli_shift = compute_wpli(x, y_shift, fs, alpha, opt);
  const double wpli_diff = compute_wpli(x, y_diff_freq, fs, alpha, opt);

  std::cerr << "pli_same=" << pli_same << "\n";
  std::cerr << "pli_shift=" << pli_shift << "\n";
  std::cerr << "pli_diff=" << pli_diff << "\n";
  std::cerr << "wpli_same=" << wpli_same << "\n";
  std::cerr << "wpli_shift=" << wpli_shift << "\n";
  std::cerr << "wpli_diff=" << wpli_diff << "\n";

  expect(std::isfinite(pli_same), "pli_same should be finite");
  expect(std::isfinite(pli_shift), "pli_shift should be finite");
  expect(std::isfinite(pli_diff), "pli_diff should be finite");

  expect(std::isfinite(wpli_same), "wpli_same should be finite");
  expect(std::isfinite(wpli_shift), "wpli_shift should be finite");
  expect(std::isfinite(wpli_diff), "wpli_diff should be finite");

  // Same-phase oscillators should have near-zero lag measures.
  expect(pli_same < 0.30, "pli_same should be low (<0.30)");
  expect(wpli_same < 0.30, "wpli_same should be low (<0.30)");

  // Fixed non-zero phase lag should produce high PLI / wPLI.
  expect(pli_shift > 0.70, "pli_shift should be high (>0.70)");
  expect(wpli_shift > 0.70, "wpli_shift should be high (>0.70)");

  // Different frequency within the band => drifting phase relationship => lower lag measures.
  expect(pli_diff < 0.60, "pli_diff should be lower (<0.60)");
  expect(wpli_diff < 0.60, "wpli_diff should be lower (<0.60)");

  // Matrix sanity.
  {
    std::vector<std::vector<float>> chans;
    chans.push_back(x);
    chans.push_back(y_same);
    chans.push_back(y_shift);

    const auto m = compute_wpli_matrix(chans, fs, alpha, opt);
    expect(m.size() == 3, "matrix should be 3x3");
    expect(m[0].size() == 3, "matrix row should be size 3");

    for (size_t i = 0; i < 3; ++i) {
      expect(std::isfinite(m[i][i]) && std::fabs(m[i][i]) < 1e-9, "diagonal should be 0 for wPLI");
    }

    // x vs y_same (in-phase) should be low; x vs y_shift should be high.
    expect(m[0][1] < 0.35, "m[0][1] (in-phase) should be low");
    expect(m[0][2] > 0.70, "m[0][2] (lagged) should be high");
    expect(m[1][2] > 0.70, "m[1][2] (lagged) should be high");
  }

  std::cout << "wPLI/PLI tests passed.\n";
  return 0;
}
