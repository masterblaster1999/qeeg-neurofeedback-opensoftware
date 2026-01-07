#include "qeeg/pac.hpp"

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
    if (coupled) {
      // Envelope strongly depends on the low-frequency oscillation.
      env = 1.0 + mod * low;
    }
    const double high = env * std::sin(2.0 * pi * f_carrier * t);
    // Mix low + high.
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

  // Classic theta-gamma style PAC toy signal.
  const double f_phase = 6.0;
  const double f_carrier = 80.0;

  const auto x_coupled = make_signal(n, fs, f_phase, f_carrier, /*mod=*/0.9, /*coupled=*/true);
  const auto x_control = make_signal(n, fs, f_phase, f_carrier, /*mod=*/0.0, /*coupled=*/false);

  const BandDefinition phase_band{"theta", 4.0, 8.0};
  const BandDefinition amp_band{"gamma", 70.0, 90.0};

  PacOptions opt;
  opt.method = PacMethod::ModulationIndex;
  opt.n_phase_bins = 18;
  opt.zero_phase = true;
  opt.edge_trim_fraction = 0.10;

  const double mi_coupled = compute_pac(x_coupled, fs, phase_band, amp_band, opt).value;
  const double mi_control = compute_pac(x_control, fs, phase_band, amp_band, opt).value;

  expect(std::isfinite(mi_coupled), "MI coupled should be finite");
  expect(std::isfinite(mi_control), "MI control should be finite");
  // The absolute thresholds here are intentionally loose.
  expect(mi_coupled > mi_control + 0.02, "MI coupled should exceed control by a margin");
  expect(mi_coupled > 0.03, "MI coupled should be meaningfully > 0");

  opt.method = PacMethod::MeanVectorLength;
  const double mvl_coupled = compute_pac(x_coupled, fs, phase_band, amp_band, opt).value;
  const double mvl_control = compute_pac(x_control, fs, phase_band, amp_band, opt).value;

  expect(std::isfinite(mvl_coupled), "MVL coupled should be finite");
  expect(std::isfinite(mvl_control), "MVL control should be finite");
  expect(mvl_coupled > mvl_control + 0.05, "MVL coupled should exceed control by a margin");
  expect(mvl_coupled > 0.10, "MVL coupled should be meaningfully > 0");

  std::cout << "test_pac OK\n";
  return 0;
}
