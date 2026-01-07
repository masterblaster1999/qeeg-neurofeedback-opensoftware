#include "qeeg/welch_psd.hpp"

#include "qeeg/fft.hpp"

#include <cmath>
#include <complex>
#include <stdexcept>

namespace qeeg {

static std::vector<double> hann_window(size_t n) {
  std::vector<double> w(n, 1.0);
  if (n <= 1) return w;
  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n; ++i) {
    w[i] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(n - 1));
  }
  return w;
}

static double mean_of_segment(const std::vector<float>& x, size_t start, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[start + i]);
  return s / static_cast<double>(n);
}

PsdResult welch_psd(const std::vector<float>& x, double fs_hz, const WelchOptions& opt) {
  if (fs_hz <= 0.0) throw std::runtime_error("welch_psd: fs_hz must be > 0");
  if (x.empty()) throw std::runtime_error("welch_psd: input signal is empty");
  if (opt.overlap_fraction < 0.0 || opt.overlap_fraction >= 1.0) {
    throw std::runtime_error("welch_psd: overlap_fraction must be in [0,1)");
  }

  size_t nperseg = opt.nperseg;
  if (nperseg < 8) nperseg = 8;
  if (nperseg > x.size()) nperseg = x.size();

  const size_t noverlap = static_cast<size_t>(std::floor(static_cast<double>(nperseg) * opt.overlap_fraction));
  const size_t hop = (nperseg > noverlap) ? (nperseg - noverlap) : 1;

  // Choose FFT size: power of two >= nperseg
  const size_t nfft = next_power_of_two(nperseg);
  const size_t nfreq = nfft / 2 + 1;

  std::vector<double> window = hann_window(nperseg);
  double U = 0.0;
  for (double wi : window) U += wi * wi;
  if (U <= 0.0) throw std::runtime_error("welch_psd: invalid window normalization");

  std::vector<double> pxx_acc(nfreq, 0.0);
  size_t nsegments = 0;

  for (size_t start = 0; start + nperseg <= x.size(); start += hop) {
    double m = mean_of_segment(x, start, nperseg);

    std::vector<std::complex<double>> buf(nfft);
    for (size_t i = 0; i < nperseg; ++i) {
      double v = (static_cast<double>(x[start + i]) - m) * window[i];
      buf[i] = std::complex<double>(v, 0.0);
    }
    for (size_t i = nperseg; i < nfft; ++i) buf[i] = std::complex<double>(0.0, 0.0);

    fft_inplace(buf, /*inverse=*/false);

    // Scale to PSD density (roughly following common Welch definitions):
    // Pxx = (1/(fs * U)) * |X|^2
    const double scale = 1.0 / (fs_hz * U);

    for (size_t k = 0; k < nfreq; ++k) {
      double mag2 = std::norm(buf[k]);
      double p = mag2 * scale;

      // Convert to one-sided PSD (double non-DC/non-Nyquist)
      if (k != 0 && k != nfft / 2) p *= 2.0;
      pxx_acc[k] += p;
    }

    ++nsegments;
  }

  if (nsegments == 0) {
    throw std::runtime_error("welch_psd: not enough samples for one segment");
  }

  for (double& v : pxx_acc) v /= static_cast<double>(nsegments);

  PsdResult out;
  out.freqs_hz.resize(nfreq);
  out.psd.resize(nfreq);
  for (size_t k = 0; k < nfreq; ++k) {
    out.freqs_hz[k] = static_cast<double>(k) * fs_hz / static_cast<double>(nfft);
    out.psd[k] = pxx_acc[k];
  }
  return out;
}

} // namespace qeeg
