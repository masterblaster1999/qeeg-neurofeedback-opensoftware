#include "qeeg/pac.hpp"

#include "qeeg/biquad.hpp"
#include "qeeg/fft.hpp"
#include "qeeg/signal.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace qeeg {

static void validate_band(const BandDefinition& b, double fs_hz, const std::string& label) {
  if (fs_hz <= 0.0) throw std::runtime_error("compute_pac: fs_hz must be > 0");
  if (!(b.fmin_hz > 0.0) || !(b.fmax_hz > 0.0) || !(b.fmin_hz < b.fmax_hz)) {
    throw std::runtime_error("compute_pac: invalid " + label + " band (requires 0 < fmin < fmax)");
  }
  const double nyq = 0.5 * fs_hz;
  if (b.fmax_hz >= nyq) {
    throw std::runtime_error("compute_pac: " + label + " band fmax must be < Nyquist");
  }
}

// Note: shared implementations for bandpass filtering / analytic signal /
// edge trimming live in qeeg/signal.*.

static PacResult compute_pac_mi(const std::vector<double>& phase,
                               const std::vector<double>& amp,
                               size_t n_bins) {
  if (phase.size() != amp.size()) throw std::runtime_error("compute_pac_mi: size mismatch");
  if (phase.empty()) return {};
  if (n_bins < 2) throw std::runtime_error("compute_pac_mi: n_phase_bins must be >= 2");

  std::vector<double> sum_amp(n_bins, 0.0);
  std::vector<double> cnt(n_bins, 0.0);

  const double two_pi = 2.0 * std::acos(-1.0);
  const double pi = std::acos(-1.0);

  for (size_t i = 0; i < phase.size(); ++i) {
    const double ph = phase[i];
    const double a = amp[i];
    if (!std::isfinite(ph) || !std::isfinite(a)) continue;

    // Map [-pi, pi] -> [0, n_bins)
    double u = (ph + pi) / two_pi;
    if (u < 0.0) u = 0.0;
    if (u >= 1.0) u = std::nextafter(1.0, 0.0);
    size_t b = static_cast<size_t>(std::floor(u * static_cast<double>(n_bins)));
    if (b >= n_bins) b = n_bins - 1;
    sum_amp[b] += a;
    cnt[b] += 1.0;
  }

  std::vector<double> mean_amp(n_bins, 0.0);
  double total = 0.0;
  for (size_t b = 0; b < n_bins; ++b) {
    if (cnt[b] > 0.0) mean_amp[b] = sum_amp[b] / cnt[b];
    total += mean_amp[b];
  }

  PacResult r;
  r.mean_amp_by_phase_bin = mean_amp;

  if (!(total > 0.0) || !std::isfinite(total)) {
    r.value = std::numeric_limits<double>::quiet_NaN();
    return r;
  }

  // Normalize to a probability distribution.
  std::vector<double> p(n_bins, 0.0);
  for (size_t b = 0; b < n_bins; ++b) p[b] = mean_amp[b] / total;

  // Entropy of p
  double H = 0.0;
  for (size_t b = 0; b < n_bins; ++b) {
    const double pb = p[b];
    if (pb > 0.0) H -= pb * std::log(pb);
  }
  const double H0 = std::log(static_cast<double>(n_bins));
  double mi = 0.0;
  if (H0 > 0.0) mi = (H0 - H) / H0;
  if (!std::isfinite(mi)) mi = std::numeric_limits<double>::quiet_NaN();
  if (std::isfinite(mi)) {
    if (mi < 0.0) mi = 0.0;
    if (mi > 1.0) mi = 1.0;
  }
  r.value = mi;
  return r;
}

static PacResult compute_pac_mvl(const std::vector<double>& phase,
                                const std::vector<double>& amp) {
  if (phase.size() != amp.size()) throw std::runtime_error("compute_pac_mvl: size mismatch");
  if (phase.empty()) return {};

  std::complex<double> acc(0.0, 0.0);
  double sum_amp = 0.0;
  for (size_t i = 0; i < phase.size(); ++i) {
    const double ph = phase[i];
    const double a = amp[i];
    if (!std::isfinite(ph) || !std::isfinite(a)) continue;
    sum_amp += a;
    acc += std::polar(a, ph);
  }

  PacResult r;
  const double eps = 1e-12;
  if (sum_amp <= eps) {
    r.value = std::numeric_limits<double>::quiet_NaN();
    return r;
  }
  const double mvl = std::abs(acc) / (sum_amp + eps);
  r.value = mvl;
  return r;
}

PacResult compute_pac(const std::vector<float>& x,
                      double fs_hz,
                      const BandDefinition& phase_band,
                      const BandDefinition& amp_band,
                      const PacOptions& opt) {
  if (x.empty()) {
    PacResult r;
    r.value = std::numeric_limits<double>::quiet_NaN();
    return r;
  }

  validate_band(phase_band, fs_hz, "phase");
  validate_band(amp_band, fs_hz, "amplitude");

  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_pac: edge_trim_fraction must be in [0, 0.49]");
  }

  // 1) Bandpass for phase and amplitude components.
  const std::vector<float> x_phase = bandpass_filter(x, fs_hz, phase_band.fmin_hz, phase_band.fmax_hz, opt.zero_phase);
  const std::vector<float> x_amp   = bandpass_filter(x, fs_hz, amp_band.fmin_hz, amp_band.fmax_hz, opt.zero_phase);

  // 2) Hilbert / analytic signal.
  const auto z_phase = analytic_signal_fft(x_phase);
  const auto z_amp   = analytic_signal_fft(x_amp);

  const size_t n = std::min(z_phase.size(), z_amp.size());
  if (n == 0) {
    PacResult r;
    r.value = std::numeric_limits<double>::quiet_NaN();
    return r;
  }

  const size_t trim = edge_trim_samples(n, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (n > trim ? (n - trim) : 0);
  if (i1 <= i0 + 1) {
    PacResult r;
    r.value = std::numeric_limits<double>::quiet_NaN();
    return r;
  }

  std::vector<double> phase;
  std::vector<double> amp;
  phase.reserve(i1 - i0);
  amp.reserve(i1 - i0);

  for (size_t i = i0; i < i1; ++i) {
    const auto& zp = z_phase[i];
    const auto& za = z_amp[i];
    phase.push_back(std::atan2(zp.imag(), zp.real()));
    amp.push_back(std::abs(za));
  }

  if (opt.method == PacMethod::MeanVectorLength) {
    return compute_pac_mvl(phase, amp);
  }
  return compute_pac_mi(phase, amp, opt.n_phase_bins);
}

} // namespace qeeg
