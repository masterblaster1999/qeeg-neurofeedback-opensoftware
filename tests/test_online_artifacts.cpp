#include "qeeg/online_artifacts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

using namespace qeeg;

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  const double fs = 250.0;
  const double seconds = 12.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));
  const double pi = std::acos(-1.0);

  // Two mostly-clean sinusoids. Inject a large artifact burst later.
  std::vector<float> a(n, 0.0f);
  std::vector<float> b(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    a[i] = static_cast<float>(std::sin(2.0 * pi * 10.0 * t));
    b[i] = static_cast<float>(0.7 * std::sin(2.0 * pi * 10.0 * t + 0.3));
  }

  // Artifact: a big movement spike for ~0.5s.
  const size_t art_start = static_cast<size_t>(std::llround(8.0 * fs));
  const size_t art_len = static_cast<size_t>(std::llround(0.5 * fs));
  for (size_t i = art_start; i < std::min(n, art_start + art_len); ++i) {
    a[i] += 25.0f;
  }

  OnlineArtifactOptions opt;
  opt.window_seconds = 2.0;
  opt.update_seconds = 0.25;
  opt.baseline_seconds = 4.0;
  opt.ptp_z = 4.0;
  opt.rms_z = 4.0;
  opt.kurtosis_z = 4.0;
  opt.min_bad_channels = 1;

  OnlineArtifactGate gate({"A", "B"}, fs, opt);

  size_t flagged = 0;
  size_t ready_frames = 0;

  // Feed in uneven chunks to exercise remainder-stable update timing.
  const size_t chunk = 137;
  for (size_t pos = 0; pos < n; pos += chunk) {
    const size_t end = std::min(n, pos + chunk);
    std::vector<std::vector<float>> block(2);
    block[0].assign(a.begin() + static_cast<std::ptrdiff_t>(pos),
                    a.begin() + static_cast<std::ptrdiff_t>(end));
    block[1].assign(b.begin() + static_cast<std::ptrdiff_t>(pos),
                    b.begin() + static_cast<std::ptrdiff_t>(end));

    const auto frames = gate.push_block(block);
    for (const auto& fr : frames) {
      if (fr.baseline_ready) {
        ++ready_frames;
        if (fr.bad) ++flagged;
      }
    }
  }

  expect(ready_frames > 0, "OnlineArtifactGate should produce frames after baseline is ready");
  expect(flagged > 0, "OnlineArtifactGate should flag the injected artifact burst");

  std::cout << "test_online_artifacts OK\n";
  return 0;
}
