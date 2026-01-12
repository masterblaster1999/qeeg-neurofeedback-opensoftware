#include "qeeg/online_plv.hpp"

#include "qeeg/bandpower.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

static int find_band_index(const std::vector<qeeg::BandDefinition>& bands, const std::string& name) {
  for (size_t i = 0; i < bands.size(); ++i) {
    if (bands[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

static std::vector<float> make_sine_with_noise(double fs,
                                               double seconds,
                                               double freq_hz,
                                               double phase_rad,
                                               double noise_std,
                                               std::mt19937* rng) {
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  std::normal_distribution<double> noise(0.0, noise_std);
  std::vector<float> x(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double s = std::sin(2.0 * pi * freq_hz * t + phase_rad);
    const double n0 = rng ? noise(*rng) : 0.0;
    x[i] = static_cast<float>(s + n0);
  }
  return x;
}

static double run_last_value(const std::vector<float>& a,
                             const std::vector<float>& b,
                             qeeg::PhaseConnectivityMeasure measure,
                             bool zero_phase_internal) {
  using namespace qeeg;

  const double fs = 256.0;
  const auto bands = default_eeg_bands();
  const int alpha_idx = find_band_index(bands, "alpha");
  assert(alpha_idx >= 0);

  OnlinePlvOptions opt;
  opt.window_seconds = 2.0;
  opt.update_seconds = 0.5;
  opt.measure = measure;
  opt.plv.zero_phase = zero_phase_internal;
  opt.plv.edge_trim_fraction = 0.10;

  OnlinePlvConnectivity eng({"A", "B"}, fs, bands, {{0, 1}}, opt);

  std::vector<std::vector<float>> block(2);
  block[0] = a;
  block[1] = b;

  const auto frames = eng.push_block(block);
  assert(!frames.empty());

  const auto& last = frames.back();
  return last.values[static_cast<size_t>(alpha_idx)][0];
}

int main() {
  using namespace qeeg;

  const double fs = 256.0;
  const double seconds = 10.0;

  std::mt19937 rng(123);

  // 10 Hz sinusoids, either in-phase or with a fixed pi/2 lag.
  const auto x = make_sine_with_noise(fs, seconds, 10.0, 0.0, 0.05, &rng);
  const auto y_same = make_sine_with_noise(fs, seconds, 10.0, 0.0, 0.05, &rng);
  const auto y_shift = make_sine_with_noise(fs, seconds, 10.0, (std::acos(-1.0) / 2.0), 0.05, &rng);

  // Use zero-phase internal filtering to keep the test stable.
  const bool zp = true;

  const double pli_same = run_last_value(x, y_same, PhaseConnectivityMeasure::PLI, zp);
  const double pli_shift = run_last_value(x, y_shift, PhaseConnectivityMeasure::PLI, zp);
  std::cerr << "online pli_same=" << pli_same << " pli_shift=" << pli_shift << "\n";
  assert(std::isfinite(pli_same));
  assert(std::isfinite(pli_shift));
  assert(pli_same < 0.35);
  assert(pli_shift > 0.70);

  const double wpli_same = run_last_value(x, y_same, PhaseConnectivityMeasure::WeightedPLI, zp);
  const double wpli_shift = run_last_value(x, y_shift, PhaseConnectivityMeasure::WeightedPLI, zp);
  std::cerr << "online wpli_same=" << wpli_same << " wpli_shift=" << wpli_shift << "\n";
  assert(std::isfinite(wpli_same));
  assert(std::isfinite(wpli_shift));
  assert(wpli_same < 0.35);
  assert(wpli_shift > 0.70);

  const double wpli2_same = run_last_value(x, y_same, PhaseConnectivityMeasure::WeightedPLI2Debiased, zp);
  const double wpli2_shift = run_last_value(x, y_shift, PhaseConnectivityMeasure::WeightedPLI2Debiased, zp);
  std::cerr << "online wpli2_same=" << wpli2_same << " wpli2_shift=" << wpli2_shift << "\n";
  assert(std::isfinite(wpli2_same));
  assert(std::isfinite(wpli2_shift));
  assert(wpli2_same < 0.35);
  assert(wpli2_shift > 0.70);

  std::cout << "Online PLV/PLI/wPLI tests passed.\n";
  return 0;
}
