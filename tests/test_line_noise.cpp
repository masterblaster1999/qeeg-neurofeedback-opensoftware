#include "qeeg/line_noise.hpp"

#include <cassert>
#include <cmath>
#include <random>
#include <vector>

using namespace qeeg;

static EEGRecording make_synth(double fs, double sine_hz, double sine_amp, double noise_std, size_t n_samples) {
  EEGRecording rec;
  rec.fs_hz = fs;
  rec.channel_names = {"Ch1", "Ch2"};
  rec.data.resize(2);
  rec.data[0].resize(n_samples);
  rec.data[1].resize(n_samples);

  std::mt19937 rng(123);
  std::normal_distribution<double> nd(0.0, noise_std);

  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n_samples; ++i) {
    const double t = static_cast<double>(i) / fs;
    const double s = sine_amp * std::sin(2.0 * pi * sine_hz * t);
    const double n1 = nd(rng);
    const double n2 = nd(rng);
    rec.data[0][i] = static_cast<float>(s + n1);
    rec.data[1][i] = static_cast<float>(s + n2);
  }
  return rec;
}

int main() {
  // Strong 50 Hz interference.
  {
    EEGRecording rec = make_synth(256.0, 50.0, 20.0, 0.5, 4096);
    WelchOptions w;
    w.nperseg = 1024;
    w.overlap_fraction = 0.5;
    const LineNoiseEstimate ln = detect_line_noise_50_60(rec, w, /*max_channels=*/2, /*min_ratio=*/3.0);
    assert(ln.recommended_hz == 50.0);
    assert(ln.cand50.ratio >= ln.cand60.ratio);
    assert(ln.strength_ratio >= 3.0);
  }

  // Strong 60 Hz interference.
  {
    EEGRecording rec = make_synth(256.0, 60.0, 20.0, 0.5, 4096);
    WelchOptions w;
    w.nperseg = 1024;
    w.overlap_fraction = 0.5;
    const LineNoiseEstimate ln = detect_line_noise_50_60(rec, w, /*max_channels=*/2, /*min_ratio=*/3.0);
    assert(ln.recommended_hz == 60.0);
    assert(ln.cand60.ratio >= ln.cand50.ratio);
    assert(ln.strength_ratio >= 3.0);
  }

  // No line noise: a 10 Hz tone + noise should not trigger a strong 50/60 recommendation.
  {
    EEGRecording rec = make_synth(256.0, 10.0, 5.0, 0.5, 4096);
    WelchOptions w;
    w.nperseg = 1024;
    w.overlap_fraction = 0.5;
    const LineNoiseEstimate ln = detect_line_noise_50_60(rec, w, /*max_channels=*/2, /*min_ratio=*/10.0);
    assert(ln.recommended_hz == 0.0);
  }

  return 0;
}
