#include "qeeg/signal.hpp"

#include "qeeg/biquad.hpp"
#include "qeeg/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace qeeg {

std::vector<float> bandpass_filter(const std::vector<float>& x,
                                  double fs_hz,
                                  double lo_hz,
                                  double hi_hz,
                                  bool zero_phase,
                                  double q) {
  if (fs_hz <= 0.0) throw std::runtime_error("bandpass_filter: fs_hz must be > 0");
  if (!std::isfinite(fs_hz)) throw std::runtime_error("bandpass_filter: fs_hz must be finite");
  if (!std::isfinite(lo_hz) || !std::isfinite(hi_hz)) {
    throw std::runtime_error("bandpass_filter: cutoffs must be finite");
  }
  if (lo_hz < 0.0 || hi_hz < 0.0) {
    throw std::runtime_error("bandpass_filter: cutoffs must be >= 0");
  }

  std::vector<float> y = x;
  if (y.empty()) return y;

  const double nyq = 0.5 * fs_hz;
  if (lo_hz > 0.0 && lo_hz >= nyq) {
    throw std::runtime_error("bandpass_filter: lo_hz must be < Nyquist");
  }
  if (hi_hz > 0.0 && hi_hz >= nyq) {
    throw std::runtime_error("bandpass_filter: hi_hz must be < Nyquist");
  }
  if (lo_hz > 0.0 && hi_hz > 0.0 && lo_hz >= hi_hz) {
    throw std::runtime_error("bandpass_filter: requires lo_hz < hi_hz");
  }

  std::vector<BiquadCoeffs> stages;
  stages.reserve(2);
  if (lo_hz > 0.0) stages.push_back(design_highpass(fs_hz, lo_hz, q));
  if (hi_hz > 0.0) stages.push_back(design_lowpass(fs_hz, hi_hz, q));
  if (stages.empty()) return y;

  if (zero_phase) {
    filtfilt_inplace(&y, stages);
  } else {
    BiquadChain chain(stages);
    chain.process_inplace(&y);
  }

  return y;
}

std::vector<std::complex<double>> analytic_signal_fft(const std::vector<float>& x) {
  const size_t n = x.size();
  if (n == 0) return {};

  // Zero-pad to a power of 2 to leverage the radix-2 FFT implementation.
  const size_t nfft = next_power_of_two(n);
  std::vector<std::complex<double>> a(nfft);
  for (size_t i = 0; i < n; ++i) a[i] = std::complex<double>(static_cast<double>(x[i]), 0.0);
  for (size_t i = n; i < nfft; ++i) a[i] = std::complex<double>(0.0, 0.0);

  fft_inplace(a, false);

  // Create an analytic signal by:
  // - keeping DC and Nyquist bins,
  // - doubling positive frequencies,
  // - zeroing negative frequencies (for even-length FFT).
  if (nfft > 1) {
    const size_t half = nfft / 2;
    for (size_t k = 1; k < half; ++k) a[k] *= 2.0;
    for (size_t k = half + 1; k < nfft; ++k) a[k] = 0.0;
  }

  fft_inplace(a, true);
  a.resize(n);
  return a;
}

size_t edge_trim_samples(size_t n, double frac) {
  if (n == 0) return 0;
  if (!std::isfinite(frac)) return 0;
  if (frac <= 0.0) return 0;
  if (frac >= 0.49) frac = 0.49;
  const size_t k = static_cast<size_t>(std::llround(frac * static_cast<double>(n)));
  return std::min(k, (n > 1 ? (n - 1) / 2 : size_t{0}));
}

} // namespace qeeg
