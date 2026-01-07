#include "qeeg/spectrogram.hpp"

#include "qeeg/fft.hpp"

#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

namespace qeeg {

static std::vector<double> hann_window(size_t n) {
  std::vector<double> w(n, 1.0);
  if (n <= 1) return w;
  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n; ++i) {
    w[i] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) /
                               static_cast<double>(n - 1));
  }
  return w;
}

static double mean_of_segment(const std::vector<float>& x, size_t start, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[start + i]);
  return s / static_cast<double>(n);
}

SpectrogramResult stft_spectrogram_psd(const std::vector<float>& x,
                                      double fs_hz,
                                      const SpectrogramOptions& opt_in) {
  if (fs_hz <= 0.0) throw std::runtime_error("stft_spectrogram_psd: fs_hz must be > 0");
  if (x.empty()) throw std::runtime_error("stft_spectrogram_psd: input signal is empty");

  SpectrogramOptions opt = opt_in;
  if (opt.nperseg == 0) opt.nperseg = 256;
  if (opt.nperseg < 8) opt.nperseg = 8;
  if (opt.nperseg > x.size()) opt.nperseg = x.size();
  if (opt.hop == 0) opt.hop = opt.nperseg / 2;
  if (opt.hop == 0) opt.hop = 1;

  const size_t nfft = (opt.nfft == 0) ? next_power_of_two(opt.nperseg) : opt.nfft;
  if (!is_power_of_two(nfft)) {
    throw std::runtime_error("stft_spectrogram_psd: nfft must be a power of two");
  }
  if (nfft < opt.nperseg) {
    throw std::runtime_error("stft_spectrogram_psd: nfft must be >= nperseg");
  }
  const size_t nfreq = nfft / 2 + 1;

  std::vector<double> window = hann_window(opt.nperseg);
  double U = 0.0;
  for (double wi : window) U += wi * wi;
  if (U <= 0.0) throw std::runtime_error("stft_spectrogram_psd: invalid window normalization");

  // Number of frames
  size_t nframes = 0;
  for (size_t start = 0; start + opt.nperseg <= x.size(); start += opt.hop) {
    ++nframes;
  }
  if (nframes == 0) throw std::runtime_error("stft_spectrogram_psd: not enough samples for one frame");

  SpectrogramResult out;
  out.n_frames = nframes;
  out.n_freq = nfreq;
  out.times_sec.resize(nframes);
  out.freqs_hz.resize(nfreq);
  out.psd.assign(nframes * nfreq, 0.0);

  for (size_t k = 0; k < nfreq; ++k) {
    out.freqs_hz[k] = static_cast<double>(k) * fs_hz / static_cast<double>(nfft);
  }

  const double scale = 1.0 / (fs_hz * U);

  size_t frame = 0;
  for (size_t start = 0; start + opt.nperseg <= x.size(); start += opt.hop) {
    double m = 0.0;
    if (opt.detrend_mean) m = mean_of_segment(x, start, opt.nperseg);

    std::vector<std::complex<double>> buf(nfft);
    for (size_t i = 0; i < opt.nperseg; ++i) {
      double v = static_cast<double>(x[start + i]) - m;
      v *= window[i];
      buf[i] = std::complex<double>(v, 0.0);
    }
    for (size_t i = opt.nperseg; i < nfft; ++i) buf[i] = std::complex<double>(0.0, 0.0);

    fft_inplace(buf, /*inverse=*/false);

    for (size_t k = 0; k < nfreq; ++k) {
      double p = std::norm(buf[k]) * scale;
      // One-sided PSD correction
      if (k != 0 && k != nfft / 2) p *= 2.0;
      out.psd[frame * nfreq + k] = p;
    }

    // Center time (seconds)
    out.times_sec[frame] = (static_cast<double>(start) + 0.5 * static_cast<double>(opt.nperseg)) / fs_hz;
    ++frame;
  }

  return out;
}

} // namespace qeeg
