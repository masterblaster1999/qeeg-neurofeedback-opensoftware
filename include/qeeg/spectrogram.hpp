#pragma once

#include <cstddef>
#include <vector>

namespace qeeg {

// Short-time Fourier transform (STFT) / spectrogram utilities.
//
// We compute a per-frame, one-sided power spectral density (PSD) using a Hann
// window and per-frame mean detrending. This is similar to Welch's method but
// without averaging across frames.

struct SpectrogramOptions {
  // Segment length in samples.
  // If 0, the implementation will choose a minimum reasonable value.
  size_t nperseg{0};

  // Hop size in samples (advance between successive frames).
  // If 0, defaults to nperseg/2.
  size_t hop{0};

  // FFT size.
  // If 0, uses the next power of two >= nperseg.
  size_t nfft{0};

  // If true, subtract the mean of each frame before windowing.
  bool detrend_mean{true};
};

struct SpectrogramResult {
  std::vector<double> times_sec;   // length = n_frames (center time of each frame)
  std::vector<double> freqs_hz;    // length = n_freq (one-sided)

  // Row-major [frame][freq] as a flat array of PSD values.
  // size = n_frames * n_freq
  std::vector<double> psd;

  size_t n_frames{0};
  size_t n_freq{0};

  double at(size_t frame, size_t freq) const {
    return psd[frame * n_freq + freq];
  }
};

// Compute a one-sided spectrogram (PSD per frame).
SpectrogramResult stft_spectrogram_psd(const std::vector<float>& x,
                                      double fs_hz,
                                      const SpectrogramOptions& opt);

} // namespace qeeg
