#include "qeeg/coherence.hpp"

#include "qeeg/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
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

CoherenceResult welch_coherence(const std::vector<float>& x,
                               const std::vector<float>& y,
                               double fs_hz,
                               const WelchOptions& opt) {
  if (fs_hz <= 0.0) throw std::runtime_error("welch_coherence: fs_hz must be > 0");
  if (x.empty() || y.empty()) throw std::runtime_error("welch_coherence: input signal is empty");
  if (x.size() != y.size()) throw std::runtime_error("welch_coherence: x and y must have same length");
  if (opt.overlap_fraction < 0.0 || opt.overlap_fraction >= 1.0) {
    throw std::runtime_error("welch_coherence: overlap_fraction must be in [0,1)");
  }

  size_t nperseg = opt.nperseg;
  if (nperseg < 8) nperseg = 8;
  if (nperseg > x.size()) nperseg = x.size();

  const size_t noverlap = static_cast<size_t>(std::floor(static_cast<double>(nperseg) * opt.overlap_fraction));
  const size_t hop = (nperseg > noverlap) ? (nperseg - noverlap) : 1;

  const size_t nfft = next_power_of_two(nperseg);
  const size_t nfreq = nfft / 2 + 1;

  std::vector<double> window = hann_window(nperseg);
  double U = 0.0;
  for (double wi : window) U += wi * wi;
  if (U <= 0.0) throw std::runtime_error("welch_coherence: invalid window normalization");

  std::vector<double> pxx_acc(nfreq, 0.0);
  std::vector<double> pyy_acc(nfreq, 0.0);
  std::vector<std::complex<double>> pxy_acc(nfreq, std::complex<double>(0.0, 0.0));
  size_t nsegments = 0;

  for (size_t start = 0; start + nperseg <= x.size(); start += hop) {
    const double mx = mean_of_segment(x, start, nperseg);
    const double my = mean_of_segment(y, start, nperseg);

    std::vector<std::complex<double>> bx(nfft);
    std::vector<std::complex<double>> by(nfft);
    for (size_t i = 0; i < nperseg; ++i) {
      const double vx = (static_cast<double>(x[start + i]) - mx) * window[i];
      const double vy = (static_cast<double>(y[start + i]) - my) * window[i];
      bx[i] = std::complex<double>(vx, 0.0);
      by[i] = std::complex<double>(vy, 0.0);
    }
    for (size_t i = nperseg; i < nfft; ++i) {
      bx[i] = std::complex<double>(0.0, 0.0);
      by[i] = std::complex<double>(0.0, 0.0);
    }

    fft_inplace(bx, /*inverse=*/false);
    fft_inplace(by, /*inverse=*/false);

    // Scale matches welch_psd: (1/(fs * U)) * |X|^2 (one-sided factor handled below).
    const double scale = 1.0 / (fs_hz * U);

    for (size_t k = 0; k < nfreq; ++k) {
      double pxx = std::norm(bx[k]) * scale;
      double pyy = std::norm(by[k]) * scale;
      std::complex<double> pxy = bx[k] * std::conj(by[k]) * scale;

      // One-sided scaling (double non-DC/non-Nyquist bins)
      if (k != 0 && k != nfft / 2) {
        pxx *= 2.0;
        pyy *= 2.0;
        pxy *= 2.0;
      }

      pxx_acc[k] += pxx;
      pyy_acc[k] += pyy;
      pxy_acc[k] += pxy;
    }

    ++nsegments;
  }

  if (nsegments == 0) {
    throw std::runtime_error("welch_coherence: not enough samples for one segment");
  }

  for (size_t k = 0; k < nfreq; ++k) {
    pxx_acc[k] /= static_cast<double>(nsegments);
    pyy_acc[k] /= static_cast<double>(nsegments);
    pxy_acc[k] /= static_cast<double>(nsegments);
  }

  CoherenceResult out;
  out.freqs_hz.resize(nfreq);
  out.coherence.resize(nfreq);

  const double eps = 1e-24;
  for (size_t k = 0; k < nfreq; ++k) {
    out.freqs_hz[k] = static_cast<double>(k) * fs_hz / static_cast<double>(nfft);

    const double denom = std::max(eps, pxx_acc[k] * pyy_acc[k]);
    const double num = std::norm(pxy_acc[k]);
    double c = num / denom;

    // Numerical guard rails.
    if (!std::isfinite(c)) c = 0.0;
    if (c < 0.0) c = 0.0;
    if (c > 1.0) c = 1.0;

    out.coherence[k] = c;
  }

  return out;
}

double average_band_coherence(const CoherenceResult& coh,
                             double fmin_hz,
                             double fmax_hz) {
  if (coh.freqs_hz.size() != coh.coherence.size() || coh.freqs_hz.size() < 2) {
    throw std::runtime_error("average_band_coherence: invalid spectrum input");
  }
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("average_band_coherence: fmax must be > fmin");
  }

  double area = 0.0;
  double covered = 0.0;

  for (size_t i = 0; i + 1 < coh.freqs_hz.size(); ++i) {
    const double f0 = coh.freqs_hz[i];
    const double f1 = coh.freqs_hz[i + 1];
    const double c0 = coh.coherence[i];
    const double c1 = coh.coherence[i + 1];

    const double a = std::max(f0, fmin_hz);
    const double b = std::min(f1, fmax_hz);
    if (b <= a) continue;

    auto lerp = [](double x0, double y0, double x1, double y1, double x) -> double {
      if (x1 == x0) return y0;
      const double t = (x - x0) / (x1 - x0);
      return y0 + t * (y1 - y0);
    };

    const double ca = lerp(f0, c0, f1, c1, a);
    const double cb = lerp(f0, c0, f1, c1, b);

    area += 0.5 * (ca + cb) * (b - a);
    covered += (b - a);
  }

  if (covered <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Return the mean coherence across the band.
  return area / covered;
}

} // namespace qeeg
