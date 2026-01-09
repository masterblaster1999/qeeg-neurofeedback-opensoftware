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

static bool in_unit_interval(double v, double eps = 1e-9) {
  return v >= -eps && v <= 1.0 + eps;
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

  std::vector<std::vector<float>> block(1);
  block[0] = x;

  // 1) Absolute bandpower (default behavior).
  {
    OnlineWelchBandpower eng({"Pz"}, fs, default_eeg_bands(), opt);
    const auto frames = eng.push_block(block);
    assert(!frames.empty());

    const int alpha_idx = find_band_index(frames[0].bands, "alpha");
    const int theta_idx = find_band_index(frames[0].bands, "theta");
    const int beta_idx = find_band_index(frames[0].bands, "beta");
    assert(alpha_idx >= 0 && theta_idx >= 0 && beta_idx >= 0);

    // For a clean 10 Hz sine wave, alpha bandpower should dominate over theta/beta.
    for (const auto& fr : frames) {
      assert(!fr.relative_power);
      assert(!fr.log10_power);
      const double alpha = fr.powers[static_cast<size_t>(alpha_idx)][0];
      const double theta = fr.powers[static_cast<size_t>(theta_idx)][0];
      const double beta = fr.powers[static_cast<size_t>(beta_idx)][0];
      assert(alpha > theta);
      assert(alpha > beta);
    }
  }

  // 2) Relative bandpower (band / total within a range).
  {
    OnlineBandpowerOptions opt_rel = opt;
    opt_rel.relative_power = true;
    // Leave relative_fmin_hz/fmax_hz as (0,0) to use the default range derived from bands.

    OnlineWelchBandpower eng({"Pz"}, fs, default_eeg_bands(), opt_rel);
    const auto frames = eng.push_block(block);
    assert(!frames.empty());

    const int alpha_idx = find_band_index(frames[0].bands, "alpha");
    const int theta_idx = find_band_index(frames[0].bands, "theta");
    const int beta_idx = find_band_index(frames[0].bands, "beta");
    const int gamma_idx = find_band_index(frames[0].bands, "gamma");
    assert(alpha_idx >= 0 && theta_idx >= 0 && beta_idx >= 0 && gamma_idx >= 0);

    for (const auto& fr : frames) {
      assert(fr.relative_power);
      assert(!fr.log10_power);
      assert(fr.relative_fmax_hz > fr.relative_fmin_hz);

      const double alpha = fr.powers[static_cast<size_t>(alpha_idx)][0];
      const double theta = fr.powers[static_cast<size_t>(theta_idx)][0];
      const double beta = fr.powers[static_cast<size_t>(beta_idx)][0];
      const double gamma = fr.powers[static_cast<size_t>(gamma_idx)][0];

      // Still should be alpha-dominant.
      assert(alpha > theta);
      assert(alpha > beta);
      assert(alpha > gamma);

      // Relative powers should be within [0, 1] (up to tiny numerical slack).
      assert(in_unit_interval(alpha));
      assert(in_unit_interval(theta));
      assert(in_unit_interval(beta));
      assert(in_unit_interval(gamma));
    }
  }

  // 3) Relative + log10 transform.
  {
    OnlineBandpowerOptions opt_log = opt;
    opt_log.relative_power = true;
    opt_log.log10_power = true;

    OnlineWelchBandpower eng({"Pz"}, fs, default_eeg_bands(), opt_log);
    const auto frames = eng.push_block(block);
    assert(!frames.empty());

    const int alpha_idx = find_band_index(frames[0].bands, "alpha");
    const int theta_idx = find_band_index(frames[0].bands, "theta");
    const int beta_idx = find_band_index(frames[0].bands, "beta");
    assert(alpha_idx >= 0 && theta_idx >= 0 && beta_idx >= 0);

    for (const auto& fr : frames) {
      assert(fr.relative_power);
      assert(fr.log10_power);
      const double alpha = fr.powers[static_cast<size_t>(alpha_idx)][0];
      const double theta = fr.powers[static_cast<size_t>(theta_idx)][0];
      const double beta = fr.powers[static_cast<size_t>(beta_idx)][0];

      assert(std::isfinite(alpha));
      assert(std::isfinite(theta));
      assert(std::isfinite(beta));

      // log10 is monotonic, so ordering should be preserved.
      assert(alpha > theta);
      assert(alpha > beta);

      // For relative power, values should be <= 0 (since fraction <= 1), allowing a tiny slack.
      assert(alpha <= 1e-6);
      assert(theta <= 1e-6);
      assert(beta <= 1e-6);
    }
  }

  std::cout << "Online bandpower test passed.\n";
  return 0;
}
