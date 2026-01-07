#include "qeeg/online_pac.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace qeeg;

static std::vector<float> make_signal(size_t n,
                                      double fs,
                                      double f_phase,
                                      double f_carrier,
                                      double mod,
                                      bool coupled) {
  const double pi = std::acos(-1.0);
  std::vector<float> x(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double low = std::sin(2.0 * pi * f_phase * t);
    double env = 1.0;
    if (coupled) env = 1.0 + mod * low;
    const double high = env * std::sin(2.0 * pi * f_carrier * t);
    x[i] = static_cast<float>(0.5 * low + high);
  }
  return x;
}

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  const double fs = 500.0;
  const double seconds = 12.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));

  const double f_phase = 6.0;
  const double f_carrier = 80.0;

  const auto x = make_signal(n, fs, f_phase, f_carrier, 0.9, true);

  const BandDefinition phase_band{"theta", 4.0, 8.0};
  const BandDefinition amp_band{"gamma", 70.0, 90.0};

  OnlinePacOptions opt;
  opt.window_seconds = 4.0;
  opt.update_seconds = 0.25;
  opt.pac.method = PacMethod::ModulationIndex;
  opt.pac.n_phase_bins = 18;
  opt.pac.edge_trim_fraction = 0.10;
  // For test stability, match offline: enable zero-phase for internal bandpass.
  opt.pac.zero_phase = true;

  OnlinePAC eng(fs, phase_band, amp_band, opt);

  // Feed in a few chunks.
  std::vector<OnlinePacFrame> frames;
  const size_t chunk = 123;
  for (size_t pos = 0; pos < x.size(); pos += chunk) {
    const size_t end = std::min(x.size(), pos + chunk);
    std::vector<float> block(x.begin() + static_cast<std::ptrdiff_t>(pos),
                             x.begin() + static_cast<std::ptrdiff_t>(end));
    const auto out = eng.push_block(block);
    frames.insert(frames.end(), out.begin(), out.end());
  }

  expect(!frames.empty(), "OnlinePAC should emit frames");
  const double last = frames.back().value;
  expect(std::isfinite(last), "Last PAC value should be finite");
  expect(last > 0.03, "PAC value should be > 0 for the coupled toy signal");

  std::cout << "test_online_pac OK\n";
  return 0;
}
