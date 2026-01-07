#include "qeeg/spectrogram.hpp"

#include <cmath>
#include <iostream>
#include <vector>

using namespace qeeg;

static void expect_true(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "TEST FAIL: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  // A clean sine wave should produce a spectrogram whose peak frequency is
  // near the sinusoid frequency in every frame.
  const double fs = 200.0;
  const double f0 = 10.0;
  const double seconds = 8.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * fs));

  std::vector<float> x(n, 0.0f);
  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n; ++i) {
    double t = static_cast<double>(i) / fs;
    x[i] = static_cast<float>(std::sin(2.0 * pi * f0 * t));
  }

  SpectrogramOptions opt;
  opt.nperseg = static_cast<size_t>(fs * 1.0); // 1 second
  opt.hop = static_cast<size_t>(fs * 0.25);    // 250 ms
  opt.nfft = 256;

  SpectrogramResult s = stft_spectrogram_psd(x, fs, opt);
  expect_true(s.n_frames > 5, "expected multiple frames");
  expect_true(s.n_freq > 10, "expected multiple frequency bins");

  // Find the max bin per frame up to 40 Hz
  double mean_peak_hz = 0.0;
  size_t counted = 0;
  for (size_t t = 0; t < s.n_frames; ++t) {
    double best_p = -1.0;
    size_t best_k = 0;
    for (size_t k = 0; k < s.n_freq; ++k) {
      if (s.freqs_hz[k] > 40.0) break;
      double p = s.at(t, k);
      if (p > best_p) {
        best_p = p;
        best_k = k;
      }
    }
    mean_peak_hz += s.freqs_hz[best_k];
    ++counted;
  }
  mean_peak_hz /= static_cast<double>(counted);

  // FFT bin width is fs/nfft ~= 0.78125 Hz; allow some wiggle due to windowing.
  expect_true(std::fabs(mean_peak_hz - f0) < 2.0, "peak frequency not near 10 Hz");

  std::cout << "test_spectrogram OK (mean_peak_hz=" << mean_peak_hz << ")\n";
  return 0;
}
