#include "qeeg/plv.hpp"

#include "qeeg/signal.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
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

static std::vector<std::complex<double>> analytic_band(const std::vector<float>& x,
                                                       double fs_hz,
                                                       const BandDefinition& band,
                                                       bool zero_phase) {
  const std::vector<float> xf = bandpass_filter(x, fs_hz, band, zero_phase);
  return analytic_signal_fft(xf);
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

double compute_pli(const std::vector<float>& x,
                   const std::vector<float>& y,
                   double fs_hz,
                   const BandDefinition& band,
                   const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_pli: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n = std::min(x.size(), y.size());
  if (n < 4) return std::numeric_limits<double>::quiet_NaN();

  std::vector<float> x0(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
  std::vector<float> y0(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));

  const auto zx = analytic_band(x0, fs_hz, band, opt.zero_phase);
  const auto zy = analytic_band(y0, fs_hz, band, opt.zero_phase);
  const size_t m = std::min(zx.size(), zy.size());
  if (m < 4) return std::numeric_limits<double>::quiet_NaN();

  const size_t trim = edge_trim_samples(m, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (m > trim ? (m - trim) : 0);
  if (i1 <= i0 + 1) return std::numeric_limits<double>::quiet_NaN();

  double acc = 0.0;
  size_t cnt = 0;
  for (size_t i = i0; i < i1; ++i) {
    const auto& a = zx[i];
    const auto& b = zy[i];
    if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
      continue;
    }
    const std::complex<double> c = a * std::conj(b);
    const double im = c.imag();
    if (!std::isfinite(im)) continue;

    // sign(Im(cross)) in {-1, 0, +1}; treat exact zero as 0.
    double s = 0.0;
    if (im > 0.0) s = 1.0;
    else if (im < 0.0) s = -1.0;

    acc += s;
    ++cnt;
  }

  if (cnt == 0) return std::numeric_limits<double>::quiet_NaN();
  double pli = std::fabs(acc) / static_cast<double>(cnt);
  if (!std::isfinite(pli)) return std::numeric_limits<double>::quiet_NaN();
  if (pli < 0.0) pli = 0.0;
  if (pli > 1.0) pli = 1.0;
  return pli;
}

double compute_wpli(const std::vector<float>& x,
                    const std::vector<float>& y,
                    double fs_hz,
                    const BandDefinition& band,
                    const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_wpli: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n = std::min(x.size(), y.size());
  if (n < 4) return std::numeric_limits<double>::quiet_NaN();

  std::vector<float> x0(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
  std::vector<float> y0(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));

  const auto zx = analytic_band(x0, fs_hz, band, opt.zero_phase);
  const auto zy = analytic_band(y0, fs_hz, band, opt.zero_phase);
  const size_t m = std::min(zx.size(), zy.size());
  if (m < 4) return std::numeric_limits<double>::quiet_NaN();

  const size_t trim = edge_trim_samples(m, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (m > trim ? (m - trim) : 0);
  if (i1 <= i0 + 1) return std::numeric_limits<double>::quiet_NaN();

  double sum_im = 0.0;
  double sum_abs = 0.0;

  for (size_t i = i0; i < i1; ++i) {
    const auto& a = zx[i];
    const auto& b = zy[i];
    if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
      continue;
    }
    const std::complex<double> c = a * std::conj(b);
    const double im = c.imag();
    if (!std::isfinite(im)) continue;
    sum_im += im;
    sum_abs += std::fabs(im);
  }

  // If the imaginary component is ~0 for all samples (purely zero-lag), treat as 0.
  const double eps = 1e-20;
  if (!(sum_abs > eps)) return 0.0;

  double wpli = std::fabs(sum_im) / sum_abs;
  if (!std::isfinite(wpli)) return std::numeric_limits<double>::quiet_NaN();
  if (wpli < 0.0) wpli = 0.0;
  if (wpli > 1.0) wpli = 1.0;
  return wpli;
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

static void build_analytic_channels(const std::vector<std::vector<float>>& channels,
                                    double fs_hz,
                                    const BandDefinition& band,
                                    const PlvOptions& opt,
                                    std::vector<std::vector<std::complex<double>>>* out_z,
                                    size_t* out_i0,
                                    size_t* out_L) {
  const size_t n_ch = channels.size();
  out_z->assign(n_ch, {});
  *out_i0 = 0;
  *out_L = 0;
  if (n_ch == 0) return;

  size_t n = std::numeric_limits<size_t>::max();
  for (const auto& ch : channels) n = std::min(n, ch.size());
  if (n < 4) return;

  for (size_t c = 0; c < n_ch; ++c) {
    std::vector<float> xc(channels[c].begin(), channels[c].begin() + static_cast<std::ptrdiff_t>(n));
    (*out_z)[c] = analytic_band(xc, fs_hz, band, opt.zero_phase);
  }

  size_t m = n;
  for (const auto& zi : *out_z) m = std::min(m, zi.size());
  if (m < 4) return;

  const size_t trim = edge_trim_samples(m, opt.edge_trim_fraction);
  const size_t i0 = trim;
  const size_t i1 = (m > trim ? (m - trim) : 0);
  if (i1 <= i0 + 1) return;

  *out_i0 = i0;
  *out_L = i1 - i0;
}

std::vector<std::vector<double>> compute_pli_matrix(const std::vector<std::vector<float>>& channels,
                                                    double fs_hz,
                                                    const BandDefinition& band,
                                                    const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_pli_matrix: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n_ch = channels.size();
  std::vector<std::vector<double>> out(n_ch, std::vector<double>(n_ch, std::numeric_limits<double>::quiet_NaN()));
  if (n_ch == 0) return out;

  std::vector<std::vector<std::complex<double>>> z;
  size_t i0 = 0;
  size_t L = 0;
  build_analytic_channels(channels, fs_hz, band, opt, &z, &i0, &L);
  if (L < 2 || z.size() != n_ch) return out;

  for (size_t i = 0; i < n_ch; ++i) {
    out[i][i] = 0.0;
  }

  for (size_t i = 0; i < n_ch; ++i) {
    for (size_t j = i + 1; j < n_ch; ++j) {
      double acc = 0.0;
      size_t cnt = 0;
      for (size_t k = 0; k < L; ++k) {
        const auto& a = z[i][i0 + k];
        const auto& b = z[j][i0 + k];
        if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
          continue;
        }
        const double im = (a * std::conj(b)).imag();
        if (!std::isfinite(im)) continue;
        double s = 0.0;
        if (im > 0.0) s = 1.0;
        else if (im < 0.0) s = -1.0;
        acc += s;
        ++cnt;
      }

      double pli = std::numeric_limits<double>::quiet_NaN();
      if (cnt > 0) {
        pli = std::fabs(acc) / static_cast<double>(cnt);
        if (!std::isfinite(pli)) pli = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(pli)) {
          if (pli < 0.0) pli = 0.0;
          if (pli > 1.0) pli = 1.0;
        }
      }

      out[i][j] = pli;
      out[j][i] = pli;
    }
  }

  return out;
}

std::vector<std::vector<double>> compute_wpli_matrix(const std::vector<std::vector<float>>& channels,
                                                     double fs_hz,
                                                     const BandDefinition& band,
                                                     const PlvOptions& opt) {
  validate_band(band, fs_hz);
  if (opt.edge_trim_fraction < 0.0 || opt.edge_trim_fraction >= 0.5) {
    throw std::runtime_error("compute_wpli_matrix: edge_trim_fraction must be in [0, 0.49]");
  }

  const size_t n_ch = channels.size();
  std::vector<std::vector<double>> out(n_ch, std::vector<double>(n_ch, std::numeric_limits<double>::quiet_NaN()));
  if (n_ch == 0) return out;

  std::vector<std::vector<std::complex<double>>> z;
  size_t i0 = 0;
  size_t L = 0;
  build_analytic_channels(channels, fs_hz, band, opt, &z, &i0, &L);
  if (L < 2 || z.size() != n_ch) return out;

  for (size_t i = 0; i < n_ch; ++i) {
    out[i][i] = 0.0;
  }

  const double eps = 1e-20;

  for (size_t i = 0; i < n_ch; ++i) {
    for (size_t j = i + 1; j < n_ch; ++j) {
      double sum_im = 0.0;
      double sum_abs = 0.0;
      for (size_t k = 0; k < L; ++k) {
        const auto& a = z[i][i0 + k];
        const auto& b = z[j][i0 + k];
        if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) || !std::isfinite(b.real()) || !std::isfinite(b.imag())) {
          continue;
        }
        const double im = (a * std::conj(b)).imag();
        if (!std::isfinite(im)) continue;
        sum_im += im;
        sum_abs += std::fabs(im);
      }

      double wpli = 0.0;
      if (sum_abs > eps) {
        wpli = std::fabs(sum_im) / sum_abs;
        if (!std::isfinite(wpli)) wpli = std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(wpli)) {
          if (wpli < 0.0) wpli = 0.0;
          if (wpli > 1.0) wpli = 1.0;
        }
      }

      out[i][j] = wpli;
      out[j][i] = wpli;
    }
  }

  return out;
}

} // namespace qeeg
