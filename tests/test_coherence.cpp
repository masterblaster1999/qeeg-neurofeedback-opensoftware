#include "qeeg/coherence.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static bool in_range(double v, double lo, double hi) {
  return v >= lo && v <= hi;
}

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  using namespace qeeg;

  const double fs = 256.0;
  const double seconds = 20.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  std::vector<float> x(n, 0.0f);
  std::vector<float> y_same(n, 0.0f);
  std::vector<float> y_noise(n, 0.0f);

  std::mt19937 rng(123);
  std::normal_distribution<double> noise(0.0, 1.0);

  // 10 Hz alpha-ish oscillator plus small noise.
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double s = std::sin(2.0 * pi * 10.0 * t);
    const double nx = 0.2 * noise(rng);
    const double ny = 0.2 * noise(rng);
    x[i] = static_cast<float>(s + nx);
    y_same[i] = static_cast<float>(s + ny);            // highly coherent with x
    y_noise[i] = static_cast<float>(noise(rng));       // mostly incoherent with x
  }

  WelchOptions opt;
  opt.nperseg = 512;
  opt.overlap_fraction = 0.5;

  const BandDefinition alpha{"alpha", 8.0, 12.0};

  {
    const auto coh = welch_coherence(x, y_same, fs, opt);
    expect(!coh.freqs_hz.empty(), "coherence spectrum should not be empty");
    expect(coh.freqs_hz.size() == coh.coherence.size(), "freq/coherence size mismatch");

    // Coherence spectrum values should be within [0,1].
    for (double c : coh.coherence) {
      expect(in_range(c, 0.0, 1.0), "coherence value out of range");
    }

    const double mean_alpha_same = average_band_coherence(coh, alpha);
    std::cerr << "mean_alpha (same)=" << mean_alpha_same << "\n";

    // With a strong shared 10 Hz component, alpha-band coherence should be higher
    // than a noise-only control.
    expect(std::isfinite(mean_alpha_same), "mean_alpha (same) should be finite");

    // We'll check the relative separation in the second block once we have the
    // noise control estimate.
  }

  {
    const auto coh = welch_coherence(x, y_noise, fs, opt);
    const double mean_alpha_noise = average_band_coherence(coh, alpha);
    std::cerr << "mean_alpha (noise)=" << mean_alpha_noise << "\n";
    expect(std::isfinite(mean_alpha_noise), "mean_alpha (noise) should be finite");

    // Recompute same-coherence quickly for comparison (small overhead, keeps
    // the test self-contained).
    const auto coh_same = welch_coherence(x, y_same, fs, opt);
    const double mean_alpha_same = average_band_coherence(coh_same, alpha);

    // We expect a clear separation between the two conditions.
    expect(mean_alpha_same > mean_alpha_noise + 0.10,
           "alpha coherence should be higher for the shared-sine signal than for noise");

    // Additional loose absolute sanity checks.
    expect(mean_alpha_same > 0.25, "alpha coherence for shared-sine should be > 0.25");
    expect(mean_alpha_noise < 0.35, "alpha coherence for noise should be < 0.35");
  }

  std::cout << "Coherence tests passed.\n";
  return 0;
}
