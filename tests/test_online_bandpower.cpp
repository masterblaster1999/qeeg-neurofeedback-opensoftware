#include "qeeg/online_bandpower.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static int find_band_index(const std::vector<qeeg::BandDefinition>& bands, const std::string& name) {
  for (size_t i = 0; i < bands.size(); ++i) {
    if (bands[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

int main() {
  using namespace qeeg;

  const double fs = 250.0;
  const double seconds = 4.0;
  const double f = 10.0; // alpha-ish
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));

  std::vector<float> x(n, 0.0f);
  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    x[i] = static_cast<float>(std::sin(2.0 * pi * f * t));
  }

  OnlineBandpowerOptions opt;
  opt.window_seconds = 2.0;
  opt.update_seconds = 0.5;
  opt.welch.nperseg = 256;
  opt.welch.overlap_fraction = 0.5;

  OnlineWelchBandpower eng({"Pz"}, fs, default_eeg_bands(), opt);

  std::vector<std::vector<float>> block(1);
  block[0] = x;
  const auto frames = eng.push_block(block);
  assert(!frames.empty());

  const int alpha_idx = find_band_index(frames[0].bands, "alpha");
  const int theta_idx = find_band_index(frames[0].bands, "theta");
  const int beta_idx = find_band_index(frames[0].bands, "beta");
  assert(alpha_idx >= 0 && theta_idx >= 0 && beta_idx >= 0);

  // For a clean 10 Hz sine wave, alpha bandpower should dominate over theta/beta.
  for (const auto& fr : frames) {
    const double alpha = fr.powers[static_cast<size_t>(alpha_idx)][0];
    const double theta = fr.powers[static_cast<size_t>(theta_idx)][0];
    const double beta = fr.powers[static_cast<size_t>(beta_idx)][0];
    assert(alpha > theta);
    assert(alpha > beta);
  }

  std::cout << "Online bandpower test passed.\n";
  return 0;
}
