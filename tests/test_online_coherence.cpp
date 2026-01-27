#include "qeeg/online_coherence.hpp"
#include "qeeg/bandpower.hpp"


#include "test_support.hpp"
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

  const double fs = 256.0;
  const double seconds = 6.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  // Two perfectly related 10 Hz sinusoids (phase shifted), coherence should be ~1.
  std::vector<float> a(n, 0.0f);
  std::vector<float> b(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    a[i] = static_cast<float>(std::sin(2.0 * pi * 10.0 * t));
    b[i] = static_cast<float>(std::sin(2.0 * pi * 10.0 * t + 0.7));
  }

  OnlineCoherenceOptions opt;
  opt.window_seconds = 2.0;
  opt.update_seconds = 0.5;
  opt.welch.nperseg = 256;
  opt.welch.overlap_fraction = 0.5;

  const auto bands = default_eeg_bands();
  const int alpha_idx = find_band_index(bands, "alpha");
  assert(alpha_idx >= 0);

  OnlineWelchCoherence eng({"A", "B"}, fs, bands, {{0, 1}}, opt);

  std::vector<std::vector<float>> block(2);
  block[0] = a;
  block[1] = b;

  const auto frames = eng.push_block(block);
  assert(!frames.empty());

  // Coherence is band-averaged; for a clean 10 Hz oscillator it should be high in alpha.
  const auto& last = frames.back();
  const double c = last.coherences[static_cast<size_t>(alpha_idx)][0];
  assert(c > 0.7);

  std::cout << "Online coherence test passed.\n";
  return 0;
}
