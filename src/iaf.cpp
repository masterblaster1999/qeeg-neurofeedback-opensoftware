#include "qeeg/iaf.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace qeeg {

namespace {

static bool is_finite(double x) {
  return std::isfinite(x);
}

// Simple median helper (copy for safety).
static double median_copy(std::vector<double> v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = v.size();
  const size_t mid = n / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  double m = v[mid];
  if (n % 2 == 0) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid - 1), v.end());
    m = 0.5 * (m + v[mid - 1]);
  }
  return m;
}

// Fit y = a + b*x via least squares. Returns false if ill-conditioned.
static bool fit_line_ls(const std::vector<double>& x,
                        const std::vector<double>& y,
                        double* out_a,
                        double* out_b) {
  if (!out_a || !out_b) return false;
  if (x.size() != y.size() || x.size() < 2) return false;

  // Compute means
  double mx = 0.0, my = 0.0;
  size_t n = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!is_finite(x[i]) || !is_finite(y[i])) continue;
    mx += x[i];
    my += y[i];
    ++n;
  }
  if (n < 2) return false;
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);

  double sxx = 0.0;
  double sxy = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!is_finite(x[i]) || !is_finite(y[i])) continue;
    const double dx = x[i] - mx;
    sxx += dx * dx;
    sxy += dx * (y[i] - my);
  }

  if (sxx <= 0.0) return false;
  const double b = sxy / sxx;
  const double a = my - b * mx;
  *out_a = a;
  *out_b = b;
  return true;
}

// Moving-average smoothing in-place with an odd window size.
static std::vector<double> smooth_ma(const std::vector<double>& y, int win) {
  if (win <= 1 || y.size() < 3) return y;
  if (win % 2 == 0) win += 1;
  const int r = win / 2;
  std::vector<double> out(y.size(), 0.0);
  for (size_t i = 0; i < y.size(); ++i) {
    double s = 0.0;
    int cnt = 0;
    const int ii = static_cast<int>(i);
    for (int k = ii - r; k <= ii + r; ++k) {
      int kk = k;
      if (kk < 0) kk = 0;
      if (kk >= static_cast<int>(y.size())) kk = static_cast<int>(y.size()) - 1;
      double v = y[static_cast<size_t>(kk)];
      if (!is_finite(v)) continue;
      s += v;
      ++cnt;
    }
    if (cnt == 0) {
      out[i] = std::numeric_limits<double>::quiet_NaN();
    } else {
      out[i] = s / static_cast<double>(cnt);
    }
  }
  return out;
}

static int find_first_ge(const std::vector<double>& v, double x) {
  auto it = std::lower_bound(v.begin(), v.end(), x);
  if (it == v.end()) return -1;
  return static_cast<int>(std::distance(v.begin(), it));
}

static int find_last_le(const std::vector<double>& v, double x) {
  auto it = std::upper_bound(v.begin(), v.end(), x);
  if (it == v.begin()) return -1;
  --it;
  return static_cast<int>(std::distance(v.begin(), it));
}

static double parabolic_refine_hz(const std::vector<double>& freqs,
                                  const std::vector<double>& y,
                                  int i) {
  // Parabolic interpolation around i using i-1,i,i+1.
  if (i <= 0 || i + 1 >= static_cast<int>(y.size())) return freqs[static_cast<size_t>(i)];
  const double y1 = y[static_cast<size_t>(i - 1)];
  const double y2 = y[static_cast<size_t>(i)];
  const double y3 = y[static_cast<size_t>(i + 1)];
  if (!is_finite(y1) || !is_finite(y2) || !is_finite(y3)) return freqs[static_cast<size_t>(i)];
  const double denom = (y1 - 2.0 * y2 + y3);
  if (std::fabs(denom) < 1e-12) return freqs[static_cast<size_t>(i)];
  const double delta = 0.5 * (y1 - y3) / denom; // in bins
  if (!is_finite(delta) || std::fabs(delta) > 1.0) return freqs[static_cast<size_t>(i)];

  // Use local bin spacing for Hz conversion.
  const double f_im1 = freqs[static_cast<size_t>(i - 1)];
  const double f_ip1 = freqs[static_cast<size_t>(i + 1)];
  const double df = 0.5 * (f_ip1 - f_im1);
  return freqs[static_cast<size_t>(i)] + delta * df;
}

} // namespace

IafEstimate estimate_iaf(const PsdResult& psd, const IafOptions& opt) {
  IafEstimate out;

  if (psd.freqs_hz.empty() || psd.psd.empty() || psd.freqs_hz.size() != psd.psd.size()) {
    return out;
  }

  if (!(opt.alpha_max_hz > opt.alpha_min_hz) || opt.alpha_min_hz <= 0.0) {
    return out;
  }

  // Convert to dB scale.
  std::vector<double> y_db(psd.psd.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < psd.psd.size(); ++i) {
    const double p = psd.psd[i];
    if (!is_finite(p) || p <= 0.0) continue;
    y_db[i] = 10.0 * std::log10(p);
  }

  // Optional 1/f detrend: y = a + b*log10(f)
  std::vector<double> y_work = y_db;
  if (opt.detrend_1_f) {
    std::vector<double> x_fit;
    std::vector<double> y_fit;
    x_fit.reserve(psd.freqs_hz.size());
    y_fit.reserve(psd.freqs_hz.size());
    for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
      const double f = psd.freqs_hz[i];
      if (!is_finite(f) || f <= 0.0) continue;
      if (f < opt.detrend_min_hz || f > opt.detrend_max_hz) continue;
      // Exclude alpha search region from fit.
      if (f >= opt.alpha_min_hz && f <= opt.alpha_max_hz) continue;
      const double yv = y_db[i];
      if (!is_finite(yv)) continue;
      x_fit.push_back(std::log10(f));
      y_fit.push_back(yv);
    }

    double a = 0.0, b = 0.0;
    if (fit_line_ls(x_fit, y_fit, &a, &b)) {
      for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
        const double f = psd.freqs_hz[i];
        if (!is_finite(f) || f <= 0.0) continue;
        const double yv = y_db[i];
        if (!is_finite(yv)) continue;
        const double x = std::log10(f);
        y_work[i] = yv - (a + b * x);
      }
    }
  }

  // Frequency smoothing.
  int win = 1;
  if (opt.smooth_hz > 0.0 && psd.freqs_hz.size() >= 3) {
    // Estimate typical bin spacing from the median of differences.
    std::vector<double> dfs;
    dfs.reserve(psd.freqs_hz.size() - 1);
    for (size_t i = 1; i < psd.freqs_hz.size(); ++i) {
      const double df = psd.freqs_hz[i] - psd.freqs_hz[i - 1];
      if (is_finite(df) && df > 0.0) dfs.push_back(df);
    }
    double df_med = median_copy(dfs);
    if (is_finite(df_med) && df_med > 0.0) {
      int radius = static_cast<int>(std::llround(opt.smooth_hz / df_med));
      if (radius < 0) radius = 0;
      win = 2 * radius + 1;
      if (win < 1) win = 1;
    }
  }
  const std::vector<double> y_smooth = smooth_ma(y_work, win);

  // Find alpha band indices.
  const int i0 = find_first_ge(psd.freqs_hz, opt.alpha_min_hz);
  const int i1 = find_last_le(psd.freqs_hz, opt.alpha_max_hz);
  if (i0 < 0 || i1 < 0 || i1 - i0 < 2) {
    return out;
  }

  // Collect values in alpha band for baseline (median).
  std::vector<double> band_vals;
  band_vals.reserve(static_cast<size_t>(i1 - i0 + 1));
  for (int i = i0; i <= i1; ++i) {
    const double v = y_smooth[static_cast<size_t>(i)];
    if (is_finite(v)) band_vals.push_back(v);
  }
  const double band_med = median_copy(band_vals);

  // Find max in band.
  int i_peak = -1;
  double best = -std::numeric_limits<double>::infinity();
  for (int i = i0; i <= i1; ++i) {
    const double v = y_smooth[static_cast<size_t>(i)];
    if (!is_finite(v)) continue;
    if (v > best) {
      best = v;
      i_peak = i;
    }
  }
  if (i_peak < 0) return out;

  if (opt.require_local_max) {
    if (i_peak <= 0 || i_peak + 1 >= static_cast<int>(y_smooth.size())) return out;
    const double yL = y_smooth[static_cast<size_t>(i_peak - 1)];
    const double yC = y_smooth[static_cast<size_t>(i_peak)];
    const double yR = y_smooth[static_cast<size_t>(i_peak + 1)];
    if (!is_finite(yL) || !is_finite(yC) || !is_finite(yR)) return out;
    if (!(yC >= yL && yC >= yR)) return out;
  }

  const double prom = best - band_med;
  if (opt.min_prominence_db > 0.0 && is_finite(prom) && prom < opt.min_prominence_db) {
    return out;
  }

  out.found = true;
  out.peak_bin = i_peak;
  out.peak_value_db = best;
  out.prominence_db = prom;
  out.iaf_hz = parabolic_refine_hz(psd.freqs_hz, y_smooth, i_peak);
  // Clamp refined estimate to alpha range.
  if (out.iaf_hz < opt.alpha_min_hz) out.iaf_hz = opt.alpha_min_hz;
  if (out.iaf_hz > opt.alpha_max_hz) out.iaf_hz = opt.alpha_max_hz;
  return out;
}

IafEstimate estimate_iaf_from_signal(const std::vector<float>& x,
                                    double fs_hz,
                                    const WelchOptions& wopt,
                                    const IafOptions& opt) {
  PsdResult psd = welch_psd(x, fs_hz, wopt);
  return estimate_iaf(psd, opt);
}

std::vector<BandDefinition> individualized_bands_from_iaf(
    double iaf_hz, const IndividualizedBandsOptions& opt) {
  std::vector<BandDefinition> bands;
  if (!is_finite(iaf_hz) || iaf_hz <= 0.0) {
    return bands;
  }

  const double dmin = opt.delta_min_hz;
  const double dmax = std::max(dmin, iaf_hz - opt.delta_theta_split_below_iaf);
  const double tmax = std::max(dmax, iaf_hz - opt.theta_alpha_split_below_iaf);
  const double amax = std::max(tmax, iaf_hz + opt.alpha_beta_split_above_iaf);
  const double bmax = std::max(amax, opt.beta_max_hz);
  const double gmax = std::max(bmax, opt.gamma_max_hz);

  bands.push_back({"delta", dmin, dmax});
  bands.push_back({"theta", dmax, tmax});
  bands.push_back({"alpha", tmax, amax});
  bands.push_back({"beta", amax, bmax});
  bands.push_back({"gamma", bmax, gmax});
  return bands;
}

std::string bands_to_spec_string(const std::vector<BandDefinition>& bands) {
  std::string out;
  for (size_t i = 0; i < bands.size(); ++i) {
    const auto& b = bands[i];
    if (b.name.empty()) continue;
    if (!out.empty()) out += ",";
    out += b.name;
    out += ":";
    out += std::to_string(b.fmin_hz);
    out += "-";
    out += std::to_string(b.fmax_hz);
  }
  return out;
}

} // namespace qeeg
