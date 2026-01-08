#include "qeeg/plv.hpp"

#include "qeeg/signal.hpp"

#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace qeeg {

static void validate_band(const BandDefinition& b, double fs_hz) {
  if (fs_hz <= 0.0) throw std::runtime_error("compute_plv: fs_hz must be > 0");
  if (!(b.fmin_hz > 0.0) || !(b.fmax_hz > 0.0) || !(b.fmin_hz < b.fmax_hz)) {
    throw std::runtime_error("compute_plv: invalid band (requires 0 < fmin < fmax)");
  }
  const double nyq = 0.5 * fs_hz;
  if (b.fmax_hz >= nyq) {
    throw std::runtime_error("compute_plv: band fmax must be < Nyquist");
  }
}

static std::vector<std::complex<double>> unit_phasor(const std::vector<float>& x,
                                                    double fs_hz,
                                                    const BandDefinition& band,
                                                    bool zero_phase) {
  const std::vector<float> xf = bandpass_filter(x, fs_hz, band, zero_phase);
  const auto z = analytic_signal_fft(xf);
  std::vector<std::complex<double>> u;
  u.reserve(z.size());
  for (const auto& zi : z) {
    const double ph = std::atan2(zi.imag(), zi.real());
    u.emplace_back(std::cos(ph), std::sin(ph));
  }
  return u;
}

double compute_plv(const std::vector<float>& x,
                   const std::vector<float>& y,
                   double fs_hz,
                   const BandDefinition& band,
                   const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_plv: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n = std::min(x.size(), y.size());
  if (n < 4) return std::numeric_limits<double>::quiet_NaN();

  std::vector<float> x0(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
  std::vector<float> y0(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));

  const auto ux = unit_phasor(x0, fs_hz, band, opt.zero_phase);
  const auto uy = unit_phasor(y0, fs_hz, band, opt.zero_phase);
  const size_t m = std::min(ux.size(), uy.size());
  if (m < 4) return std::numeric_limits<double>::quiet_NaN();

  const size_t trim = edge_trim_samples(m, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (m > trim ? (m - trim) : 0);
  if (i1 <= i0 + 1) return std::numeric_limits<double>::quiet_NaN();

  std::complex<double> acc(0.0, 0.0);
  size_t cnt = 0;
  for (size_t i = i0; i < i1; ++i) {
    const auto& a = ux[i];
    const auto& b = uy[i];
    if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
      continue;
    }
    // exp(i*(phi_x - phi_y)) = exp(i*phi_x) * conj(exp(i*phi_y))
    acc += a * std::conj(b);
    ++cnt;
  }

  if (cnt == 0) return std::numeric_limits<double>::quiet_NaN();
  const double plv = std::abs(acc) / static_cast<double>(cnt);
  // Clamp for numerical noise.
  if (!std::isfinite(plv)) return std::numeric_limits<double>::quiet_NaN();
  if (plv < 0.0) return 0.0;
  if (plv > 1.0) return 1.0;
  return plv;
}

std::vector<std::vector<double>> compute_plv_matrix(const std::vector<std::vector<float>>& channels,
                                                    double fs_hz,
                                                    const BandDefinition& band,
                                                    const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_plv_matrix: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n_ch = channels.size();
  std::vector<std::vector<double>> out(n_ch, std::vector<double>(n_ch, std::numeric_limits<double>::quiet_NaN()));
  if (n_ch == 0) return out;

  // Build unit phasors for each channel after trimming to common length.
  size_t n = std::numeric_limits<size_t>::max();
  for (const auto& ch : channels) n = std::min(n, ch.size());
  if (n < 4) return out;

  std::vector<std::vector<std::complex<double>>> u(n_ch);
  for (size_t c = 0; c < n_ch; ++c) {
    std::vector<float> xc(channels[c].begin(), channels[c].begin() + static_cast<std::ptrdiff_t>(n));
    u[c] = unit_phasor(xc, fs_hz, band, opt.zero_phase);
  }

  size_t m = n;
  for (const auto& ui : u) m = std::min(m, ui.size());
  if (m < 4) return out;

  const size_t trim = edge_trim_samples(m, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (m > trim ? (m - trim) : 0);
  if (i1 <= i0 + 1) return out;

  const size_t L = i1 - i0;

  for (size_t i = 0; i < n_ch; ++i) {
    out[i][i] = 1.0;
  }

  for (size_t i = 0; i < n_ch; ++i) {
    for (size_t j = i + 1; j < n_ch; ++j) {
      std::complex<double> acc(0.0, 0.0);
      size_t cnt = 0;
      for (size_t k = 0; k < L; ++k) {
        const auto& a = u[i][i0 + k];
        const auto& b = u[j][i0 + k];
        if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
          continue;
        }
        acc += a * std::conj(b);
        ++cnt;
      }

      double plv = std::numeric_limits<double>::quiet_NaN();
      if (cnt > 0) {
        plv = std::abs(acc) / static_cast<double>(cnt);
        if (!std::isfinite(plv)) plv = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(plv)) {
          if (plv < 0.0) plv = 0.0;
          if (plv > 1.0) plv = 1.0;
        }
      }
      out[i][j] = plv;
      out[j][i] = plv;
    }
  }

  return out;
}

} // namespace qeeg
