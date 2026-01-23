#include "qeeg/spectral_features.hpp"

#include "qeeg/robust_stats.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace qeeg {

namespace {

inline double lerp(double x0, double y0, double x1, double y1, double x) {
  if (x1 == x0) return y0;
  const double t = (x - x0) / (x1 - x0);
  return y0 + t * (y1 - y0);
}

struct Segment {
  double a{0.0};
  double b{0.0};
  double pa{0.0};
  double pb{0.0};
};

static void validate_psd(const PsdResult& psd) {
  if (psd.freqs_hz.size() != psd.psd.size() || psd.freqs_hz.size() < 2) {
    throw std::runtime_error("spectral_features: invalid psd input");
  }
  // Basic monotonicity check (best-effort).
  for (size_t i = 1; i < psd.freqs_hz.size(); ++i) {
    if (!(psd.freqs_hz[i] > psd.freqs_hz[i - 1])) {
      throw std::runtime_error("spectral_features: freqs_hz must be strictly increasing");
    }
  }
}

// Iterate over segments (piecewise-linear PSD) that overlap [fmin,fmax].
static void for_each_segment_in_range(const PsdResult& psd,
                                      double fmin_hz,
                                      double fmax_hz,
                                      const std::function<void(const Segment&)>& fn) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_features: fmax must be > fmin");
  }

  // Clamp to PSD support.
  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return; // empty overlap
  }

  for (size_t i = 0; i + 1 < psd.freqs_hz.size(); ++i) {
    const double f0 = psd.freqs_hz[i];
    const double f1 = psd.freqs_hz[i + 1];
    const double p0 = psd.psd[i];
    const double p1 = psd.psd[i + 1];

    const double a = std::max(f0, f_lo);
    const double b = std::min(f1, f_hi);
    if (b <= a) continue;

    Segment s;
    s.a = a;
    s.b = b;
    s.pa = lerp(f0, p0, f1, p1, a);
    s.pb = lerp(f0, p0, f1, p1, b);

    // Guard tiny negatives.
    if (!std::isfinite(s.pa)) s.pa = 0.0;
    if (!std::isfinite(s.pb)) s.pb = 0.0;
    if (s.pa < 0.0) s.pa = 0.0;
    if (s.pb < 0.0) s.pb = 0.0;

    fn(s);
  }
}

static double segment_area(const Segment& s) {
  return 0.5 * (s.pa + s.pb) * (s.b - s.a);
}

static double segment_integral_fP(const Segment& s) {
  // P(f) is linear between (a,pa) and (b,pb).
  const double len = (s.b - s.a);
  if (!(len > 0.0)) return 0.0;

  const double slope = (s.pb - s.pa) / len; // dP/df
  // P(f) = alpha + slope*f.
  const double alpha = s.pa - slope * s.a;

  const double a2 = s.a * s.a;
  const double b2 = s.b * s.b;
  const double a3 = a2 * s.a;
  const double b3 = b2 * s.b;

  // ∫ f*P(f) df = ∫ (alpha*f + slope*f^2) df
  //            = alpha*0.5*(b^2-a^2) + slope*(1/3)*(b^3-a^3)
  return alpha * 0.5 * (b2 - a2) + slope * (b3 - a3) / 3.0;
}

static double segment_integral_f2P(const Segment& s) {
  // P(f) is linear between (a,pa) and (b,pb).
  const double len = (s.b - s.a);
  if (!(len > 0.0)) return 0.0;

  const double slope = (s.pb - s.pa) / len; // dP/df
  // P(f) = alpha + slope*f.
  const double alpha = s.pa - slope * s.a;

  const double a2 = s.a * s.a;
  const double b2 = s.b * s.b;
  const double a3 = a2 * s.a;
  const double b3 = b2 * s.b;
  const double a4 = a3 * s.a;
  const double b4 = b3 * s.b;

  // ∫ f^2*P(f) df = ∫ (alpha*f^2 + slope*f^3) df
  //             = alpha*(1/3)*(b^3-a^3) + slope*(1/4)*(b^4-a^4)
  return alpha * (b3 - a3) / 3.0 + slope * (b4 - a4) / 4.0;
}


static double segment_integral_f3P(const Segment& s) {
  // P(f) is linear between (a,pa) and (b,pb).
  const double len = (s.b - s.a);
  if (!(len > 0.0)) return 0.0;

  const double slope = (s.pb - s.pa) / len; // dP/df
  // P(f) = alpha + slope*f.
  const double alpha = s.pa - slope * s.a;

  const double a2 = s.a * s.a;
  const double b2 = s.b * s.b;
  const double a3 = a2 * s.a;
  const double b3 = b2 * s.b;
  const double a4 = a3 * s.a;
  const double b4 = b3 * s.b;
  const double a5 = a4 * s.a;
  const double b5 = b4 * s.b;

  // ∫ f^3*P(f) df = ∫ (alpha*f^3 + slope*f^4) df
  //             = alpha*(1/4)*(b^4-a^4) + slope*(1/5)*(b^5-a^5)
  return alpha * (b4 - a4) / 4.0 + slope * (b5 - a5) / 5.0;
}

static double segment_integral_f4P(const Segment& s) {
  // P(f) is linear between (a,pa) and (b,pb).
  const double len = (s.b - s.a);
  if (!(len > 0.0)) return 0.0;

  const double slope = (s.pb - s.pa) / len; // dP/df
  // P(f) = alpha + slope*f.
  const double alpha = s.pa - slope * s.a;

  const double a2 = s.a * s.a;
  const double b2 = s.b * s.b;
  const double a3 = a2 * s.a;
  const double b3 = b2 * s.b;
  const double a4 = a3 * s.a;
  const double b4 = b3 * s.b;
  const double a5 = a4 * s.a;
  const double b5 = b4 * s.b;
  const double a6 = a5 * s.a;
  const double b6 = b5 * s.b;

  // ∫ f^4*P(f) df = ∫ (alpha*f^4 + slope*f^5) df
  //             = alpha*(1/5)*(b^5-a^5) + slope*(1/6)*(b^6-a^6)
  return alpha * (b5 - a5) / 5.0 + slope * (b6 - a6) / 6.0;
}


static double solve_freq_for_area_in_segment(const Segment& s, double area_target, double eps) {
  // Find x in [a,b] such that ∫_a^x P(f) df = area_target.
  // P is linear. Let dx = x-a, slope = dP/df.
  // area = pa*dx + 0.5*slope*dx^2.
  const double len = s.b - s.a;
  if (!(len > 0.0)) return s.a;

  const double slope = (s.pb - s.pa) / len;
  const double pa = std::max(0.0, s.pa);
  const double rem = std::max(0.0, area_target);

  if (std::fabs(slope) < 1e-15) {
    const double denom = std::max(pa, eps);
    double dx = rem / denom;
    if (dx < 0.0) dx = 0.0;
    if (dx > len) dx = len;
    return s.a + dx;
  }

  // Quadratic: 0.5*slope*dx^2 + pa*dx - rem = 0
  // => slope*dx^2 + 2*pa*dx - 2*rem = 0
  double disc = pa * pa + 2.0 * slope * rem;
  if (disc < 0.0) disc = 0.0; // numerical guard
  const double sqrt_disc = std::sqrt(disc);

  const double dx1 = (-pa + sqrt_disc) / slope;
  const double dx2 = (-pa - sqrt_disc) / slope;

  auto in_range = [&](double dx) {
    return dx >= -1e-12 && dx <= len + 1e-12;
  };

  double dx = 0.0;
  if (in_range(dx1)) {
    dx = dx1;
  } else if (in_range(dx2)) {
    dx = dx2;
  } else {
    // Fallback: clamp.
    dx = std::clamp(dx1, 0.0, len);
  }

  dx = std::clamp(dx, 0.0, len);
  return s.a + dx;
}

static double psd_at_freq_linear(const PsdResult& psd, double f_hz) {
  // Best-effort linear interpolation of sampled PSD at a given frequency.
  // Assumes validate_psd() already ran.
  if (psd.freqs_hz.empty()) return 0.0;
  if (f_hz <= psd.freqs_hz.front()) return psd.psd.front();
  if (f_hz >= psd.freqs_hz.back()) return psd.psd.back();

  auto it = std::lower_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_hz);
  if (it == psd.freqs_hz.begin()) return psd.psd.front();
  const size_t i1 = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it));
  if (i1 >= psd.freqs_hz.size()) return psd.psd.back();
  if (*it == f_hz) return psd.psd[i1];
  const size_t i0 = i1 - 1;
  return lerp(psd.freqs_hz[i0], psd.psd[i0], psd.freqs_hz[i1], psd.psd[i1], f_hz);
}

struct LinFit {
  double slope{0.0};
  double intercept{0.0};
};

static LinFit weighted_linear_fit(const std::vector<double>& x,
                                  const std::vector<double>& y,
                                  const std::vector<double>& w) {
  LinFit out;
  const size_t n = std::min(x.size(), std::min(y.size(), w.size()));
  if (n < 2) return out;

  double sw = 0.0;
  double swx = 0.0;
  double swy = 0.0;
  double swxx = 0.0;
  double swxy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    if (!(wi > 0.0) || !std::isfinite(wi)) continue;
    const double xi = x[i];
    const double yi = y[i];
    if (!std::isfinite(xi) || !std::isfinite(yi)) continue;
    sw += wi;
    swx += wi * xi;
    swy += wi * yi;
    swxx += wi * xi * xi;
    swxy += wi * xi * yi;
  }

  if (!(sw > 0.0)) return out;

  const double denom = sw * swxx - swx * swx;
  if (std::fabs(denom) <= 1e-24) {
    // Degenerate (all x equal). Fall back to mean.
    out.slope = 0.0;
    out.intercept = swy / sw;
    return out;
  }

  out.slope = (sw * swxy - swx * swy) / denom;
  out.intercept = (swy - out.slope * swx) / sw;
  return out;
}

struct Piecewise2SlopeFit {
  // a: value at the breakpoint x0 (in y-units)
  // b_lo / b_hi: slopes in the low/high segments
  double a{std::numeric_limits<double>::quiet_NaN()};
  double b_lo{std::numeric_limits<double>::quiet_NaN()};
  double b_hi{std::numeric_limits<double>::quiet_NaN()};
};

static bool solve_3x3(double A[3][3], double b[3], double x[3]) {
  // Gauss-Jordan elimination on the augmented matrix [A|b].
  double m[3][4] = {
      {A[0][0], A[0][1], A[0][2], b[0]},
      {A[1][0], A[1][1], A[1][2], b[1]},
      {A[2][0], A[2][1], A[2][2], b[2]},
  };

  for (int col = 0; col < 3; ++col) {
    int piv = col;
    double best = std::fabs(m[col][col]);
    for (int r = col + 1; r < 3; ++r) {
      const double v = std::fabs(m[r][col]);
      if (v > best) {
        best = v;
        piv = r;
      }
    }
    if (!(best > 1e-24) || !std::isfinite(best)) {
      return false;
    }
    if (piv != col) {
      for (int c = col; c < 4; ++c) {
        std::swap(m[col][c], m[piv][c]);
      }
    }

    const double diag = m[col][col];
    if (!std::isfinite(diag) || std::fabs(diag) <= 1e-24) return false;
    const double inv = 1.0 / diag;
    for (int c = col; c < 4; ++c) {
      m[col][c] *= inv;
    }

    for (int r = 0; r < 3; ++r) {
      if (r == col) continue;
      const double f = m[r][col];
      if (!std::isfinite(f) || std::fabs(f) <= 0.0) continue;
      for (int c = col; c < 4; ++c) {
        m[r][c] -= f * m[col][c];
      }
    }
  }

  x[0] = m[0][3];
  x[1] = m[1][3];
  x[2] = m[2][3];
  return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

static Piecewise2SlopeFit weighted_piecewise_continuous_fit(const std::vector<double>& x,
                                                           const std::vector<double>& y,
                                                           const std::vector<double>& w,
                                                           double x0) {
  Piecewise2SlopeFit out;
  const size_t n = std::min(x.size(), std::min(y.size(), w.size()));
  if (n < 3 || !std::isfinite(x0)) return out;

  double A[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
  double bvec[3] = {0.0, 0.0, 0.0};

  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    const double xi = x[i];
    const double yi = y[i];
    if (!(wi > 0.0) || !std::isfinite(wi) || !std::isfinite(xi) || !std::isfinite(yi)) continue;

    const double d = xi - x0;
    const double d1 = (xi <= x0) ? d : 0.0;
    const double d2 = (xi >= x0) ? d : 0.0;
    const double phi[3] = {1.0, d1, d2};

    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        A[r][c] += wi * phi[r] * phi[c];
      }
      bvec[r] += wi * phi[r] * yi;
    }
  }

  double sol[3] = {std::numeric_limits<double>::quiet_NaN(),
                  std::numeric_limits<double>::quiet_NaN(),
                  std::numeric_limits<double>::quiet_NaN()};
  if (!solve_3x3(A, bvec, sol)) {
    return out;
  }
  out.a = sol[0];
  out.b_lo = sol[1];
  out.b_hi = sol[2];
  return out;
}

static inline double predict_piecewise_continuous(double xi, double x0, const Piecewise2SlopeFit& fit) {
  const double d = xi - x0;
  if (xi <= x0) return fit.a + fit.b_lo * d;
  return fit.a + fit.b_hi * d;
}

static double weighted_r2(const std::vector<double>& y,
                          const std::vector<double>& yhat,
                          const std::vector<double>& w) {
  const size_t n = std::min(y.size(), std::min(yhat.size(), w.size()));
  if (n < 2) return std::numeric_limits<double>::quiet_NaN();

  double sw = 0.0;
  double swy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    const double yi = y[i];
    if (!(wi > 0.0) || !std::isfinite(wi) || !std::isfinite(yi)) continue;
    sw += wi;
    swy += wi * yi;
  }
  if (!(sw > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  const double ymean = swy / sw;

  double sse = 0.0;
  double sst = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    const double yi = y[i];
    const double yhi = yhat[i];
    if (!(wi > 0.0) || !std::isfinite(wi) || !std::isfinite(yi) || !std::isfinite(yhi)) continue;
    const double e = yi - yhi;
    const double d = yi - ymean;
    sse += wi * e * e;
    sst += wi * d * d;
  }

  if (!(sst > 1e-24)) {
    // y is (nearly) constant: define R^2 as 1 when the fit matches perfectly.
    return (sse <= 1e-24) ? 1.0 : 0.0;
  }
  double r2 = 1.0 - (sse / sst);
  if (!std::isfinite(r2)) return std::numeric_limits<double>::quiet_NaN();
  // Numerical guard; R^2 can be slightly negative due to rounding.
  if (r2 < -1.0) r2 = -1.0;
  if (r2 > 1.0) r2 = 1.0;
  return r2;
}

static double weighted_rmse(const std::vector<double>& y,
                           const std::vector<double>& yhat,
                           const std::vector<double>& w) {
  const size_t n = std::min(y.size(), std::min(yhat.size(), w.size()));
  if (n < 1) return std::numeric_limits<double>::quiet_NaN();

  double sw = 0.0;
  double sse = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    const double yi = y[i];
    const double yhi = yhat[i];
    if (!(wi > 0.0) || !std::isfinite(wi) || !std::isfinite(yi) || !std::isfinite(yhi)) continue;
    const double e = yi - yhi;
    sw += wi;
    sse += wi * e * e;
  }
  if (!(sw > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  return std::sqrt(sse / sw);
}

static double unweighted_rmse(const std::vector<double>& y,
                             const std::vector<double>& yhat) {
  const size_t n = std::min(y.size(), yhat.size());
  if (n < 1) return std::numeric_limits<double>::quiet_NaN();

  double sse = 0.0;
  double cnt = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double yi = y[i];
    const double yhi = yhat[i];
    if (!std::isfinite(yi) || !std::isfinite(yhi)) continue;
    const double e = yi - yhi;
    sse += e * e;
    cnt += 1.0;
  }
  if (!(cnt > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  return std::sqrt(sse / cnt);
}

static double mad_scale(const std::vector<double>& r) {
  if (r.empty()) return 0.0;
  std::vector<double> tmp = r;
  const double med = median_inplace(&tmp);
  std::vector<double> absdev;
  absdev.reserve(r.size());
  for (double v : r) absdev.push_back(std::fabs(v - med));
  const double mad = median_inplace(&absdev);
  // Gaussian-consistent MAD.
  return mad * 1.4826;
}

} // namespace


double spectral_psd_at_frequency(const PsdResult& psd, double freq_hz) {
  validate_psd(psd);
  if (!std::isfinite(freq_hz)) return 0.0;
  double v = psd_at_freq_linear(psd, freq_hz);
  if (!std::isfinite(v) || v < 0.0) v = 0.0;
  return v;
}

double spectral_prominence_db_from_loglog_fit(const PsdResult& psd,
                                             double freq_hz,
                                             const SpectralLogLogFit& fit,
                                             double eps) {
  validate_psd(psd);
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double p = spectral_psd_at_frequency(psd, freq_hz);
  if (!std::isfinite(p) || !(p > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double y = std::log10(std::max(p, eps));
  const double yhat = fit.intercept + fit.slope * std::log10(freq_hz);
  if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();

  const double prom_db = 10.0 * (y - yhat);
  return std::isfinite(prom_db) ? prom_db : std::numeric_limits<double>::quiet_NaN();
}



double spectral_aperiodic_log10_psd_from_loglog_fit(const SpectralLogLogFit& fit,
                                                    double freq_hz,
                                                    double eps) {
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double yhat = fit.intercept + fit.slope * std::log10(freq_hz);
  return std::isfinite(yhat) ? yhat : std::numeric_limits<double>::quiet_NaN();
}

double spectral_aperiodic_log10_psd_from_two_slope_fit(const SpectralLogLogTwoSlopeFit& fit,
                                                       double freq_hz,
                                                       double eps) {
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  (void)eps;
  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(fit.knee_hz) || !(fit.knee_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double x = std::log10(freq_hz);
  double yhat = std::numeric_limits<double>::quiet_NaN();
  if (freq_hz <= fit.knee_hz) {
    if (!std::isfinite(fit.slope_low) || !std::isfinite(fit.intercept_low)) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    yhat = fit.intercept_low + fit.slope_low * x;
  } else {
    if (!std::isfinite(fit.slope_high) || !std::isfinite(fit.intercept_high)) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    yhat = fit.intercept_high + fit.slope_high * x;
  }
  return std::isfinite(yhat) ? yhat : std::numeric_limits<double>::quiet_NaN();
}

double spectral_aperiodic_log10_psd_from_knee_fit(const SpectralAperiodicKneeFit& fit,
                                                  double freq_hz,
                                                  double eps) {
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(fit.offset) || !std::isfinite(fit.exponent) || !std::isfinite(fit.knee)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double fterm = std::pow(freq_hz, fit.exponent);
  if (!std::isfinite(fterm)) return std::numeric_limits<double>::quiet_NaN();

  const double denom = fit.knee + fterm;
  if (!std::isfinite(denom) || !(denom > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double yhat = fit.offset - std::log10(std::max(denom, eps));
  return std::isfinite(yhat) ? yhat : std::numeric_limits<double>::quiet_NaN();
}

double spectral_prominence_db_from_two_slope_fit(const PsdResult& psd,
                                                 double freq_hz,
                                                 const SpectralLogLogTwoSlopeFit& fit,
                                                 double eps) {
  validate_psd(psd);
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double p = spectral_psd_at_frequency(psd, freq_hz);
  if (!std::isfinite(p) || !(p > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double y = std::log10(std::max(p, eps));
  const double yhat = spectral_aperiodic_log10_psd_from_two_slope_fit(fit, freq_hz, eps);
  if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();

  const double prom_db = 10.0 * (y - yhat);
  return std::isfinite(prom_db) ? prom_db : std::numeric_limits<double>::quiet_NaN();
}

double spectral_prominence_db_from_knee_fit(const PsdResult& psd,
                                            double freq_hz,
                                            const SpectralAperiodicKneeFit& fit,
                                            double eps) {
  validate_psd(psd);
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(freq_hz) || !(freq_hz > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double p = spectral_psd_at_frequency(psd, freq_hz);
  if (!std::isfinite(p) || !(p > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double y = std::log10(std::max(p, eps));
  const double yhat = spectral_aperiodic_log10_psd_from_knee_fit(fit, freq_hz, eps);
  if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();

  const double prom_db = 10.0 * (y - yhat);
  return std::isfinite(prom_db) ? prom_db : std::numeric_limits<double>::quiet_NaN();
}

double spectral_periodic_power_from_loglog_fit(const PsdResult& psd,
                                              double fmin_hz,
                                              double fmax_hz,
                                              const SpectralLogLogFit& fit,
                                              bool positive_only,
                                              double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_power_from_loglog_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; require positive f for log10.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  // Collect sample points across the range (include interpolated boundaries).
  std::vector<double> f;
  std::vector<double> r; // residual power (linear domain)
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    const double x = std::log10(fhz);
    const double yhat = fit.intercept + fit.slope * x;
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (positive_only && res < 0.0) res = 0.0;
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return 0.0;
  }

  double area = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) area += a;
  }
  return area;
}


double spectral_periodic_power_fraction_from_loglog_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       const SpectralLogLogFit& fit,
                                                       bool positive_only,
                                                       double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error(
        "spectral_periodic_power_fraction_from_loglog_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Match the effective range used by the log-domain background (positive f only).
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return 0.0;

  const double periodic = spectral_periodic_power_from_loglog_fit(psd, f_lo, f_hi, fit, positive_only, eps);
  const double total = spectral_total_power(psd, f_lo, f_hi);
  if (!std::isfinite(periodic) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!(total > eps)) return 0.0;
  return periodic / total;
}


double spectral_periodic_edge_frequency_from_loglog_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       const SpectralLogLogFit& fit,
                                                       double edge,
                                                       double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_loglog_fit: fmax must be > fmin");
  }
  if (!(edge > 0.0 && edge <= 1.0) || !std::isfinite(edge)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_loglog_fit: edge must be in (0,1]");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; background uses log10(f), so require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Sample periodic residual on the same support as spectral_periodic_power_from_loglog_fit
  // (include interpolated boundaries + original PSD bin centers inside the range).
  std::vector<double> f;
  std::vector<double> r;
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    const double x = std::log10(fhz);
    const double yhat = fit.intercept + fit.slope * x;
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (res < 0.0) res = 0.0; // edge frequency requires a non-negative distribution
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Total periodic residual power.
  double total = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) total += a;
  }

  if (!(total > eps) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double target = edge * total;
  double cum = 0.0;
  double out_f = f.front();

  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (!std::isfinite(a) || a < 0.0) continue;
    if (cum + a >= target) {
      const double rem = target - cum;
      Segment s;
      s.a = f[i];
      s.b = f[i + 1];
      s.pa = r[i];
      s.pb = r[i + 1];
      out_f = solve_freq_for_area_in_segment(s, rem, eps);
      return out_f;
    }
    cum += a;
    out_f = f[i + 1];
  }

  return out_f;
}




double spectral_periodic_power_from_two_slope_fit(const PsdResult& psd,
                                                  double fmin_hz,
                                                  double fmax_hz,
                                                  const SpectralLogLogTwoSlopeFit& fit,
                                                  bool positive_only,
                                                  double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_power_from_two_slope_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; require positive f for log10.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  // Collect sample points across the range (include interpolated boundaries).
  std::vector<double> f;
  std::vector<double> r; // residual power (linear domain)
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    const double yhat = spectral_aperiodic_log10_psd_from_two_slope_fit(fit, fhz, eps);
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (positive_only && res < 0.0) res = 0.0;
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return 0.0;
  }

  double area = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) area += a;
  }
  return area;
}

double spectral_periodic_power_fraction_from_two_slope_fit(const PsdResult& psd,
                                                           double fmin_hz,
                                                           double fmax_hz,
                                                           const SpectralLogLogTwoSlopeFit& fit,
                                                           bool positive_only,
                                                           double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error(
        "spectral_periodic_power_fraction_from_two_slope_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Match the effective range used by the log-domain background (positive f only).
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return 0.0;

  const double periodic = spectral_periodic_power_from_two_slope_fit(psd, f_lo, f_hi, fit, positive_only, eps);
  const double total = spectral_total_power(psd, f_lo, f_hi);
  if (!std::isfinite(periodic) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!(total > eps)) return 0.0;
  return periodic / total;
}

double spectral_periodic_edge_frequency_from_two_slope_fit(const PsdResult& psd,
                                                           double fmin_hz,
                                                           double fmax_hz,
                                                           const SpectralLogLogTwoSlopeFit& fit,
                                                           double edge,
                                                           double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_two_slope_fit: fmax must be > fmin");
  }
  if (!(edge > 0.0 && edge <= 1.0) || !std::isfinite(edge)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_two_slope_fit: edge must be in (0,1]");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; background uses log10(f), so require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Sample periodic residual (include interpolated boundaries + original PSD bin centers).
  std::vector<double> f;
  std::vector<double> r;
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    const double yhat = spectral_aperiodic_log10_psd_from_two_slope_fit(fit, fhz, eps);
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (res < 0.0) res = 0.0; // edge frequency requires a non-negative distribution
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Total periodic residual power.
  double total = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) total += a;
  }

  if (!(total > eps) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double target = edge * total;
  double cum = 0.0;
  double out_f = f.front();

  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (!std::isfinite(a) || a < 0.0) continue;
    if (cum + a >= target) {
      const double rem = target - cum;
      Segment s;
      s.a = f[i];
      s.b = f[i + 1];
      s.pa = r[i];
      s.pb = r[i + 1];
      out_f = solve_freq_for_area_in_segment(s, rem, eps);
      return out_f;
    }
    cum += a;
    out_f = f[i + 1];
  }

  return out_f;
}



double spectral_periodic_power_from_knee_fit(const PsdResult& psd,
                                             double fmin_hz,
                                             double fmax_hz,
                                             const SpectralAperiodicKneeFit& fit,
                                             bool positive_only,
                                             double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_power_from_knee_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  // Collect sample points across the range (include interpolated boundaries).
  std::vector<double> f;
  std::vector<double> r; // residual power (linear domain)
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    const double yhat = spectral_aperiodic_log10_psd_from_knee_fit(fit, fhz, eps);
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (positive_only && res < 0.0) res = 0.0;
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return 0.0;
  }

  double area = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) area += a;
  }
  return area;
}

double spectral_periodic_power_fraction_from_knee_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      const SpectralAperiodicKneeFit& fit,
                                                      bool positive_only,
                                                      double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error(
        "spectral_periodic_power_fraction_from_knee_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Match the effective range used by the background (positive f only).
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return 0.0;

  const double periodic = spectral_periodic_power_from_knee_fit(psd, f_lo, f_hi, fit, positive_only, eps);
  const double total = spectral_total_power(psd, f_lo, f_hi);
  if (!std::isfinite(periodic) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!(total > eps)) return 0.0;
  return periodic / total;
}

double spectral_periodic_edge_frequency_from_knee_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      const SpectralAperiodicKneeFit& fit,
                                                      double edge,
                                                      double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_knee_fit: fmax must be > fmin");
  }
  if (!(edge > 0.0 && edge <= 1.0) || !std::isfinite(edge)) {
    throw std::runtime_error("spectral_periodic_edge_frequency_from_knee_fit: edge must be in (0,1]");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Clamp to PSD support; require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Sample periodic residual (include interpolated boundaries + original PSD bin centers).
  std::vector<double> f;
  std::vector<double> r;
  f.reserve(psd.freqs_hz.size() + 2);
  r.reserve(psd.freqs_hz.size() + 2);

  auto background_at = [&](double fhz) -> double {
    const double yhat = spectral_aperiodic_log10_psd_from_knee_fit(fit, fhz, eps);
    if (!std::isfinite(yhat)) return std::numeric_limits<double>::quiet_NaN();
    const double bg = std::pow(10.0, yhat);
    if (!std::isfinite(bg) || bg < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return bg;
  };

  auto push_point = [&](double fhz) {
    if (!std::isfinite(fhz) || !(fhz > 0.0)) return;
    double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv) || pv < 0.0) pv = 0.0;
    double bg = background_at(fhz);
    if (!std::isfinite(bg) || bg < 0.0) bg = 0.0;
    double res = pv - bg;
    if (res < 0.0) res = 0.0; // edge frequency requires a non-negative distribution
    if (!std::isfinite(res)) res = 0.0;
    f.push_back(fhz);
    r.push_back(res);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Total periodic residual power.
  double total = 0.0;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (std::isfinite(a)) total += a;
  }

  if (!(total > eps) || !std::isfinite(total)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double target = edge * total;
  double cum = 0.0;
  double out_f = f.front();

  for (size_t i = 0; i + 1 < f.size(); ++i) {
    const double df = f[i + 1] - f[i];
    if (!(df > 0.0) || !std::isfinite(df)) continue;
    const double a = 0.5 * (r[i] + r[i + 1]) * df;
    if (!std::isfinite(a) || a < 0.0) continue;
    if (cum + a >= target) {
      const double rem = target - cum;
      Segment s;
      s.a = f[i];
      s.b = f[i + 1];
      s.pa = r[i];
      s.pb = r[i + 1];
      out_f = solve_freq_for_area_in_segment(s, rem, eps);
      return out_f;
    }
    cum += a;
    out_f = f[i + 1];
  }

  return out_f;
}

SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralLogLogFit& fit,
                                                   bool require_local_max,
                                                   double min_prominence_db,
                                                   double eps) {
  validate_psd(psd);
  SpectralProminentPeak out;

  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_max_prominence_peak: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!std::isfinite(fit.slope) || !std::isfinite(fit.intercept)) {
    return out;
  }

  // Clamp to PSD support. Prominence uses log10(f), so require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return out;

  // Find index range within [f_lo,f_hi] (inclusive).
  auto it0 = std::lower_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_lo);
  auto it1 = std::upper_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_hi);
  if (it0 == psd.freqs_hz.end() || it0 == it1) return out;
  const size_t i0 = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it0));
  const size_t i1_excl = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it1));
  if (i1_excl == 0) return out;
  const size_t i1 = i1_excl - 1;
  if (i0 >= psd.freqs_hz.size() || i1 >= psd.freqs_hz.size() || i0 > i1) return out;

  // Compute per-bin prominence in dB (log10 domain residual).
  std::vector<double> prom(psd.psd.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = i0; i <= i1; ++i) {
    const double f = psd.freqs_hz[i];
    double p = psd.psd[i];
    if (!std::isfinite(f) || !(f > 0.0)) continue;
    if (!std::isfinite(p)) continue;
    if (p < eps) p = eps;

    const double y = std::log10(p);
    const double yhat = fit.intercept + fit.slope * std::log10(f);
    if (!std::isfinite(y) || !std::isfinite(yhat)) continue;
    prom[i] = 10.0 * (y - yhat);
  }

  // Select the best candidate.
  double best_prom = -std::numeric_limits<double>::infinity();
  size_t best_idx = std::numeric_limits<size_t>::max();
  double best_f = std::numeric_limits<double>::quiet_NaN();

  for (size_t i = i0; i <= i1; ++i) {
    const double v = prom[i];
    if (!std::isfinite(v)) continue;

    if (require_local_max) {
      if (i == i0 || i == i1) continue;
      const double vL = prom[i - 1];
      const double vR = prom[i + 1];
      if (!std::isfinite(vL) || !std::isfinite(vR)) continue;
      if (!(v >= vL && v >= vR)) continue;
    }

    const double f = psd.freqs_hz[i];
    if (!std::isfinite(f)) continue;

    if (v > best_prom ||
        (v == best_prom && std::isfinite(best_f) && f < best_f) ||
        (v == best_prom && !std::isfinite(best_f))) {
      best_prom = v;
      best_idx = i;
      best_f = f;
    }
  }

  if (best_idx == std::numeric_limits<size_t>::max()) return out;
  if (!std::isfinite(best_prom)) return out;
  const double tol = 1e-12 * (1.0 + std::fabs(min_prominence_db));
  if (!(best_prom > min_prominence_db + tol)) return out;

  out.found = true;
  out.peak_bin = best_idx;
  out.peak_hz = best_f;
  out.prominence_db = best_prom;
  out.peak_hz_refined = out.peak_hz;

  // Parabolic refinement on the prominence curve (in dB).
  if (best_idx > i0 && best_idx < i1) {
    const double y1 = prom[best_idx - 1];
    const double y2 = prom[best_idx];
    const double y3 = prom[best_idx + 1];
    if (std::isfinite(y1) && std::isfinite(y2) && std::isfinite(y3)) {
      const double denom = (y1 - 2.0 * y2 + y3);
      if (std::fabs(denom) > 1e-12) {
        const double delta = 0.5 * (y1 - y3) / denom; // in bins
        if (std::isfinite(delta) && std::fabs(delta) <= 1.0) {
          const double f_im1 = psd.freqs_hz[best_idx - 1];
          const double f_ip1 = psd.freqs_hz[best_idx + 1];
          const double df = 0.5 * (f_ip1 - f_im1);
          if (std::isfinite(df) && df > 0.0) {
            double f_ref = out.peak_hz + delta * df;
            if (f_ref < f_lo) f_ref = f_lo;
            if (f_ref > f_hi) f_ref = f_hi;
            out.peak_hz_refined = f_ref;
          }
        }
      }
    }
  }

  return out;
}





SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralLogLogTwoSlopeFit& fit,
                                                   bool require_local_max,
                                                   double min_prominence_db,
                                                   double eps) {
  validate_psd(psd);
  SpectralProminentPeak out;

  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_max_prominence_peak(two_slope): fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return out;
  }
  if (!std::isfinite(fit.knee_hz) || !(fit.knee_hz > 0.0)) {
    return out;
  }
  if (!std::isfinite(fit.slope_low) || !std::isfinite(fit.intercept_low) ||
      !std::isfinite(fit.slope_high) || !std::isfinite(fit.intercept_high)) {
    return out;
  }

  // Clamp to PSD support. Prominence uses log10(f), so require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return out;

  // Find index range within [f_lo,f_hi] (inclusive).
  auto it0 = std::lower_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_lo);
  auto it1 = std::upper_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_hi);
  if (it0 == psd.freqs_hz.end() || it0 == it1) return out;
  const size_t i0 = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it0));
  const size_t i1_excl = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it1));
  if (i1_excl == 0) return out;
  const size_t i1 = i1_excl - 1;
  if (i0 >= psd.freqs_hz.size() || i1 >= psd.freqs_hz.size() || i0 > i1) return out;

  // Compute per-bin prominence in dB (log10 domain residual).
  std::vector<double> prom(psd.psd.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = i0; i <= i1; ++i) {
    const double f = psd.freqs_hz[i];
    double p = psd.psd[i];
    if (!std::isfinite(f) || !(f > 0.0)) continue;
    if (!std::isfinite(p)) continue;
    if (p < eps) p = eps;

    const double y = std::log10(p);
    const double yhat = spectral_aperiodic_log10_psd_from_two_slope_fit(fit, f, eps);
    if (!std::isfinite(y) || !std::isfinite(yhat)) continue;
    prom[i] = 10.0 * (y - yhat);
  }

  // Select the best candidate.
  double best_prom = -std::numeric_limits<double>::infinity();
  size_t best_idx = std::numeric_limits<size_t>::max();
  double best_f = std::numeric_limits<double>::quiet_NaN();

  for (size_t i = i0; i <= i1; ++i) {
    const double v = prom[i];
    if (!std::isfinite(v)) continue;

    if (require_local_max) {
      if (i == i0 || i == i1) continue;
      const double vL = prom[i - 1];
      const double vR = prom[i + 1];
      if (!std::isfinite(vL) || !std::isfinite(vR)) continue;
      if (!(v >= vL && v >= vR)) continue;
    }

    const double f = psd.freqs_hz[i];
    if (!std::isfinite(f)) continue;

    if (v > best_prom ||
        (v == best_prom && std::isfinite(best_f) && f < best_f) ||
        (v == best_prom && !std::isfinite(best_f))) {
      best_prom = v;
      best_idx = i;
      best_f = f;
    }
  }

  if (best_idx == std::numeric_limits<size_t>::max()) return out;
  if (!std::isfinite(best_prom)) return out;
  const double tol = 1e-12 * (1.0 + std::fabs(min_prominence_db));
  if (!(best_prom > min_prominence_db + tol)) return out;

  out.found = true;
  out.peak_bin = best_idx;
  out.peak_hz = best_f;
  out.prominence_db = best_prom;
  out.peak_hz_refined = out.peak_hz;

  // Parabolic refinement on the prominence curve (in dB).
  if (best_idx > i0 && best_idx < i1) {
    const double y1 = prom[best_idx - 1];
    const double y2 = prom[best_idx];
    const double y3 = prom[best_idx + 1];
    if (std::isfinite(y1) && std::isfinite(y2) && std::isfinite(y3)) {
      const double denom = (y1 - 2.0 * y2 + y3);
      if (std::fabs(denom) > 1e-12) {
        const double delta = 0.5 * (y1 - y3) / denom; // in bins
        if (std::isfinite(delta) && std::fabs(delta) <= 1.0) {
          const double f_im1 = psd.freqs_hz[best_idx - 1];
          const double f_ip1 = psd.freqs_hz[best_idx + 1];
          const double df = 0.5 * (f_ip1 - f_im1);
          if (std::isfinite(df) && df > 0.0) {
            double f_ref = out.peak_hz + delta * df;
            if (f_ref < f_lo) f_ref = f_lo;
            if (f_ref > f_hi) f_ref = f_hi;
            out.peak_hz_refined = f_ref;
          }
        }
      }
    }
  }

  return out;
}



SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralAperiodicKneeFit& fit,
                                                   bool require_local_max,
                                                   double min_prominence_db,
                                                   double eps) {
  validate_psd(psd);
  SpectralProminentPeak out;

  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_max_prominence_peak(knee): fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  if (!fit.found) {
    return out;
  }
  if (!std::isfinite(fit.offset) || !std::isfinite(fit.exponent) || !std::isfinite(fit.knee)) {
    return out;
  }

  // Clamp to PSD support. Require positive f.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) return out;

  // Find index range within [f_lo,f_hi] (inclusive).
  auto it0 = std::lower_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_lo);
  auto it1 = std::upper_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f_hi);
  if (it0 == psd.freqs_hz.end() || it0 == it1) return out;
  const size_t i0 = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it0));
  const size_t i1_excl = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it1));
  if (i1_excl == 0) return out;
  const size_t i1 = i1_excl - 1;
  if (i0 >= psd.freqs_hz.size() || i1 >= psd.freqs_hz.size() || i0 > i1) return out;

  // Compute per-bin prominence in dB (log10 domain residual).
  std::vector<double> prom(psd.psd.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = i0; i <= i1; ++i) {
    const double f = psd.freqs_hz[i];
    double p = psd.psd[i];
    if (!std::isfinite(f) || !(f > 0.0)) continue;
    if (!std::isfinite(p)) continue;
    if (p < eps) p = eps;

    const double y = std::log10(p);
    const double yhat = spectral_aperiodic_log10_psd_from_knee_fit(fit, f, eps);
    if (!std::isfinite(y) || !std::isfinite(yhat)) continue;
    prom[i] = 10.0 * (y - yhat);
  }

  // Select the best candidate.
  double best_prom = -std::numeric_limits<double>::infinity();
  size_t best_idx = std::numeric_limits<size_t>::max();
  double best_f = std::numeric_limits<double>::quiet_NaN();

  for (size_t i = i0; i <= i1; ++i) {
    const double v = prom[i];
    if (!std::isfinite(v)) continue;

    if (require_local_max) {
      if (i == i0 || i == i1) continue;
      const double vL = prom[i - 1];
      const double vR = prom[i + 1];
      if (!std::isfinite(vL) || !std::isfinite(vR)) continue;
      if (!(v >= vL && v >= vR)) continue;
    }

    const double f = psd.freqs_hz[i];
    if (!std::isfinite(f)) continue;

    if (v > best_prom ||
        (v == best_prom && std::isfinite(best_f) && f < best_f) ||
        (v == best_prom && !std::isfinite(best_f))) {
      best_prom = v;
      best_idx = i;
      best_f = f;
    }
  }

  if (best_idx == std::numeric_limits<size_t>::max()) return out;
  if (!std::isfinite(best_prom)) return out;
  const double tol = 1e-12 * (1.0 + std::fabs(min_prominence_db));
  if (!(best_prom > min_prominence_db + tol)) return out;

  out.found = true;
  out.peak_bin = best_idx;
  out.peak_hz = best_f;
  out.prominence_db = best_prom;
  out.peak_hz_refined = out.peak_hz;

  // Parabolic refinement on the prominence curve (in dB).
  if (best_idx > i0 && best_idx < i1) {
    const double y1 = prom[best_idx - 1];
    const double y2 = prom[best_idx];
    const double y3 = prom[best_idx + 1];
    if (std::isfinite(y1) && std::isfinite(y2) && std::isfinite(y3)) {
      const double denom = (y1 - 2.0 * y2 + y3);
      if (std::fabs(denom) > 1e-12) {
        const double delta = 0.5 * (y1 - y3) / denom; // in bins
        if (std::isfinite(delta) && std::fabs(delta) <= 1.0) {
          const double f_im1 = psd.freqs_hz[best_idx - 1];
          const double f_ip1 = psd.freqs_hz[best_idx + 1];
          const double df = 0.5 * (f_ip1 - f_im1);
          if (std::isfinite(df) && df > 0.0) {
            double f_ref = out.peak_hz + delta * df;
            if (f_ref < f_lo) f_ref = f_lo;
            if (f_ref > f_hi) f_ref = f_hi;
            out.peak_hz_refined = f_ref;
          }
        }
      }
    }
  }

  return out;
}

double spectral_peak_frequency_parabolic(const PsdResult& psd,
                                        double fmin_hz,
                                        double fmax_hz,
                                        bool log_domain,
                                        double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_peak_frequency_parabolic: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) return f_lo;

  // Start from the argmax-based peak estimate.
  const double f0 = spectral_peak_frequency(psd, f_lo, f_hi);
  if (!std::isfinite(f0)) return f0;

  // Only refine if f0 lands exactly on a sampled bin with neighbors.
  auto it = std::lower_bound(psd.freqs_hz.begin(), psd.freqs_hz.end(), f0);
  if (it == psd.freqs_hz.end() || *it != f0) {
    return f0;
  }
  const size_t i = static_cast<size_t>(std::distance(psd.freqs_hz.begin(), it));
  if (i == 0 || i + 1 >= psd.freqs_hz.size()) {
    return f0;
  }

  // Require neighbors to be within the analysis range.
  if (psd.freqs_hz[i - 1] < f_lo || psd.freqs_hz[i + 1] > f_hi) {
    return f0;
  }

  auto y_of = [&](double p) -> double {
    if (!std::isfinite(p) || p < 0.0) p = 0.0;
    if (log_domain) {
      return std::log10(std::max(p, eps));
    }
    return p;
  };

  const double y1 = y_of(psd.psd[i - 1]);
  const double y2 = y_of(psd.psd[i]);
  const double y3 = y_of(psd.psd[i + 1]);
  if (!std::isfinite(y1) || !std::isfinite(y2) || !std::isfinite(y3)) {
    return f0;
  }

  const double denom = (y1 - 2.0 * y2 + y3);
  if (std::fabs(denom) < 1e-18) {
    return f0;
  }

  const double delta = 0.5 * (y1 - y3) / denom; // in bins
  if (!std::isfinite(delta) || std::fabs(delta) > 1.0) {
    return f0;
  }

  // Convert bin delta to Hz using local spacing.
  const double f_im1 = psd.freqs_hz[i - 1];
  const double f_ip1 = psd.freqs_hz[i + 1];
  const double df = 0.5 * (f_ip1 - f_im1);
  if (!(df > 0.0) || !std::isfinite(df)) {
    return f0;
  }

  double f_ref = psd.freqs_hz[i] + delta * df;
  if (!std::isfinite(f_ref)) return f0;
  if (f_ref < f_lo) f_ref = f_lo;
  if (f_ref > f_hi) f_ref = f_hi;
  return f_ref;
}

double spectral_value_db(const PsdResult& psd, double freq_hz, double eps) {
  validate_psd(psd);
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (!std::isfinite(freq_hz) || freq_hz < 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double p = spectral_psd_at_frequency(psd, freq_hz);
  if (!std::isfinite(p) || !(p > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double v = 10.0 * std::log10(std::max(p, eps));
  return std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN();
}

double spectral_peak_fwhm_hz(const PsdResult& psd,
                             double peak_freq_hz,
                             double fmin_hz,
                             double fmax_hz,
                             double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_peak_fwhm_hz: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (!std::isfinite(peak_freq_hz) || !(peak_freq_hz > f_lo) || !(peak_freq_hz < f_hi)) {
    // Require the peak to be strictly inside the range so both sides exist.
    return std::numeric_limits<double>::quiet_NaN();
  }

  const double p_peak = spectral_psd_at_frequency(psd, peak_freq_hz);
  if (!std::isfinite(p_peak) || !(p_peak > eps)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double target = 0.5 * p_peak;

  // Left search: collect segments in [f_lo, peak_freq_hz] and walk backwards.
  std::vector<Segment> left;
  left.reserve(psd.freqs_hz.size());
  for_each_segment_in_range(psd, f_lo, peak_freq_hz, [&](const Segment& s) { left.push_back(s); });
  double f_left = std::numeric_limits<double>::quiet_NaN();
  for (auto it = left.rbegin(); it != left.rend(); ++it) {
    const Segment& s = *it;
    const double pA = s.pa;
    const double pB = s.pb;
    const double da = pA - target;
    const double db = pB - target;
    if (!std::isfinite(da) || !std::isfinite(db)) continue;
    if (db == 0.0) {
      f_left = s.b;
      break;
    }
    if (da == 0.0) {
      f_left = s.a;
      break;
    }
    if (da * db <= 0.0) {
      const double denom = (pB - pA);
      if (std::fabs(denom) < 1e-30) {
        f_left = s.a;
      } else {
        const double t = (target - pA) / denom;
        f_left = s.a + t * (s.b - s.a);
      }
      break;
    }
  }

  // Right search: collect segments in [peak_freq_hz, f_hi] and walk forwards.
  std::vector<Segment> right;
  right.reserve(psd.freqs_hz.size());
  for_each_segment_in_range(psd, peak_freq_hz, f_hi, [&](const Segment& s) { right.push_back(s); });
  double f_right = std::numeric_limits<double>::quiet_NaN();
  for (const Segment& s : right) {
    const double pA = s.pa;
    const double pB = s.pb;
    const double da = pA - target;
    const double db = pB - target;
    if (!std::isfinite(da) || !std::isfinite(db)) continue;
    if (da == 0.0) {
      f_right = s.a;
      break;
    }
    if (db == 0.0) {
      f_right = s.b;
      break;
    }
    if (da * db <= 0.0) {
      const double denom = (pB - pA);
      if (std::fabs(denom) < 1e-30) {
        f_right = s.b;
      } else {
        const double t = (target - pA) / denom;
        f_right = s.a + t * (s.b - s.a);
      }
      break;
    }
  }

  if (!std::isfinite(f_left) || !std::isfinite(f_right)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double width = f_right - f_left;
  if (!std::isfinite(width) || !(width > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return width;
}

double spectral_total_power(const PsdResult& psd, double fmin_hz, double fmax_hz) {
  double total = 0.0;
  for_each_segment_in_range(psd, fmin_hz, fmax_hz, [&](const Segment& s) {
    total += segment_area(s);
  });
  return total;
}

double spectral_entropy(const PsdResult& psd,
                        double fmin_hz,
                        double fmax_hz,
                        bool normalize,
                        double eps) {
  std::vector<double> areas;
  areas.reserve(psd.freqs_hz.size());

  double total = 0.0;
  for_each_segment_in_range(psd, fmin_hz, fmax_hz, [&](const Segment& s) {
    const double a = segment_area(s);
    if (a > 0.0) {
      areas.push_back(a);
      total += a;
    }
  });

  if (!(total > eps) || areas.empty()) {
    return 0.0;
  }

  double H = 0.0;
  for (double a : areas) {
    const double p = a / total;
    if (!(p > 0.0)) continue;
    H -= p * std::log(p);
  }

  if (!normalize) return H;
  if (areas.size() <= 1) return 0.0;
  const double denom = std::log(static_cast<double>(areas.size()));
  if (!(denom > 0.0)) return 0.0;
  return H / denom;
}

double spectral_mean_frequency(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double eps) {
  double total = 0.0;
  double mom1 = 0.0;
  for_each_segment_in_range(psd, fmin_hz, fmax_hz, [&](const Segment& s) {
    const double a = segment_area(s);
    total += a;
    mom1 += segment_integral_fP(s);
  });
  if (!(total > eps)) return 0.0;
  return mom1 / total;
}

double spectral_bandwidth(const PsdResult& psd,
                          double fmin_hz,
                          double fmax_hz,
                          double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_bandwidth: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  double total = 0.0;
  double fP = 0.0;
  double f2P = 0.0;

  for_each_segment_in_range(psd, f_lo, f_hi, [&](const Segment& s) {
    const double a = segment_area(s);
    if (a <= 0.0) return;
    total += a;
    fP += segment_integral_fP(s);
    f2P += segment_integral_f2P(s);
  });

  if (!(total > eps) || !std::isfinite(total)) return 0.0;

  const double mean = fP / total;
  const double e2 = f2P / total;
  if (!std::isfinite(mean) || !std::isfinite(e2)) return 0.0;
  const double var = std::max(0.0, e2 - mean * mean);
  return std::sqrt(var);
}



double spectral_skewness(const PsdResult& psd,
                         double fmin_hz,
                         double fmax_hz,
                         double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_skewness: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  double m0 = 0.0;
  double m1 = 0.0;
  double m2 = 0.0;
  double m3 = 0.0;

  for_each_segment_in_range(psd, f_lo, f_hi, [&](const Segment& s) {
    const double a = segment_area(s);
    if (a <= 0.0) return;
    m0 += a;
    m1 += segment_integral_fP(s);
    m2 += segment_integral_f2P(s);
    m3 += segment_integral_f3P(s);
  });

  if (!(m0 > eps) || !std::isfinite(m0)) return 0.0;

  const double mu = m1 / m0;
  const double e2 = m2 / m0;
  const double var = e2 - mu * mu;
  if (!(var > eps) || !std::isfinite(var)) return 0.0;

  const double e3 = m3 / m0;
  const double c3 = e3 - 3.0 * mu * e2 + 2.0 * mu * mu * mu;

  const double denom = std::sqrt(var) * var; // var^(3/2)
  if (!(denom > eps) || !std::isfinite(denom)) return 0.0;

  const double skew = c3 / denom;
  if (!std::isfinite(skew)) return 0.0;
  return skew;
}

double spectral_kurtosis_excess(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_kurtosis_excess: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  double m0 = 0.0;
  double m1 = 0.0;
  double m2 = 0.0;
  double m3 = 0.0;
  double m4 = 0.0;

  for_each_segment_in_range(psd, f_lo, f_hi, [&](const Segment& s) {
    const double a = segment_area(s);
    if (a <= 0.0) return;
    m0 += a;
    m1 += segment_integral_fP(s);
    m2 += segment_integral_f2P(s);
    m3 += segment_integral_f3P(s);
    m4 += segment_integral_f4P(s);
  });

  if (!(m0 > eps) || !std::isfinite(m0)) return 0.0;

  const double mu = m1 / m0;
  const double e2 = m2 / m0;
  const double var = e2 - mu * mu;
  if (!(var > eps) || !std::isfinite(var)) return 0.0;

  const double e3 = m3 / m0;
  const double e4 = m4 / m0;

  const double mu2 = mu * mu;
  const double mu4 = mu2 * mu2;

  // Central 4th moment: E[(X-mu)^4] = E[X^4] - 4*mu*E[X^3] + 6*mu^2*E[X^2] - 3*mu^4
  const double c4 = e4 - 4.0 * mu * e3 + 6.0 * mu2 * e2 - 3.0 * mu4;

  const double denom = var * var;
  if (!(denom > eps) || !std::isfinite(denom)) return 0.0;

  const double kurt = c4 / denom;
  if (!std::isfinite(kurt)) return 0.0;

  // Excess kurtosis = kurtosis - 3.
  return kurt - 3.0;
}



double spectral_flatness(const PsdResult& psd,
                         double fmin_hz,
                         double fmax_hz,
                         double eps) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_flatness: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return 0.0;
  }

  const double width = f_hi - f_lo;
  if (!(width > 0.0)) return 0.0;

  // Arithmetic mean of PSD density over the band.
  const double total = spectral_total_power(psd, f_lo, f_hi);
  const double arith_mean = total / width;
  if (!(arith_mean > eps) || !std::isfinite(arith_mean)) return 0.0;

  // Geometric mean of PSD density over the band, approximating
  //   exp( (1/width) * ∫ log(P(f)) df )
  double int_log = 0.0;
  for_each_segment_in_range(psd, f_lo, f_hi, [&](const Segment& s) {
    double pa = s.pa;
    double pb = s.pb;
    if (!std::isfinite(pa) || pa < eps) pa = eps;
    if (!std::isfinite(pb) || pb < eps) pb = eps;
    const double la = std::log(pa);
    const double lb = std::log(pb);
    const double segw = (s.b - s.a);
    if (!(segw > 0.0)) return;
    // Trapezoidal rule in the log domain.
    int_log += 0.5 * (la + lb) * segw;
  });

  const double geom_mean = std::exp(int_log / width);
  if (!std::isfinite(geom_mean) || geom_mean <= 0.0) return 0.0;

  const double flat = geom_mean / arith_mean;
  if (!std::isfinite(flat) || flat < 0.0) return 0.0;
  // Numerical guard: flatness should not exceed 1 for non-negative PSD,
  // but allow tiny overshoot due to approximation.
  return (flat > 1.0) ? 1.0 : flat;
}


double spectral_edge_frequency(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double edge,
                               double eps) {
  if (!(edge > 0.0 && edge <= 1.0)) {
    throw std::runtime_error("spectral_edge_frequency: edge must be in (0,1]");
  }

  // First pass: total.
  double total = spectral_total_power(psd, fmin_hz, fmax_hz);
  if (!(total > eps)) {
    // Best-effort return for degenerate PSD.
    return std::max(fmin_hz, psd.freqs_hz.front());
  }

  const double target = edge * total;
  double cum = 0.0;
  double out_f = std::max(fmin_hz, psd.freqs_hz.front());

  for_each_segment_in_range(psd, fmin_hz, fmax_hz, [&](const Segment& s) {
    if (cum >= target) return;
    const double a = segment_area(s);
    if (cum + a >= target) {
      const double rem = target - cum;
      out_f = solve_freq_for_area_in_segment(s, rem, eps);
      cum = target;
      return;
    }
    cum += a;
    out_f = s.b;
  });

  return out_f;
}

double spectral_peak_frequency(const PsdResult& psd, double fmin_hz, double fmax_hz) {
  validate_psd(psd);
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_peak_frequency: fmax must be > fmin");
  }

  const double f_lo = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi > f_lo)) {
    return f_lo;
  }

  double best_f = f_lo;
  double best_p = -std::numeric_limits<double>::infinity();

  // Include interpolated boundaries.
  for (size_t i = 0; i + 1 < psd.freqs_hz.size(); ++i) {
    const double f0 = psd.freqs_hz[i];
    const double f1 = psd.freqs_hz[i + 1];
    const double p0 = psd.psd[i];
    const double p1 = psd.psd[i + 1];

    if (f0 <= f_lo && f_lo <= f1) {
      const double p = lerp(f0, p0, f1, p1, f_lo);
      if (p > best_p) {
        best_p = p;
        best_f = f_lo;
      }
    }
    if (f0 <= f_hi && f_hi <= f1) {
      const double p = lerp(f0, p0, f1, p1, f_hi);
      if (p > best_p) {
        best_p = p;
        best_f = f_hi;
      }
    }
  }

  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double f = psd.freqs_hz[i];
    if (f < f_lo || f > f_hi) continue;
    const double p = psd.psd[i];
    if (p > best_p) {
      best_p = p;
      best_f = f;
    }
  }

  return best_f;
}

SpectralLogLogFit spectral_loglog_fit(const PsdResult& psd,
                                     double fmin_hz,
                                     double fmax_hz,
                                     const std::vector<FrequencyRange>& exclude_ranges_hz,
                                     bool robust,
                                     int max_iter,
                                     double eps) {
  validate_psd(psd);
  SpectralLogLogFit out;
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_loglog_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (max_iter < 0) max_iter = 0;

  // Clamp to PSD support.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi0 > f_lo0)) {
    return out; // empty overlap
  }

  // log10 requires positive frequencies.
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return out;
  }

  auto is_excluded = [&](double fhz) {
    if (exclude_ranges_hz.empty()) return false;
    for (const auto& r : exclude_ranges_hz) {
      const double lo = r.fmin_hz;
      const double hi = r.fmax_hz;
      if (!std::isfinite(lo) || !std::isfinite(hi)) continue;
      if (!(hi > lo)) continue;
      if (fhz >= lo && fhz <= hi) return true;
    }
    return false;
  };

  // Collect sample points across the range. Include interpolated boundaries.
  std::vector<double> f;
  std::vector<double> p;
  f.reserve(psd.freqs_hz.size() + 2);
  p.reserve(psd.freqs_hz.size() + 2);

  auto push_point = [&](double fhz) {
    if (!(fhz > 0.0) || !std::isfinite(fhz)) return;
    if (is_excluded(fhz)) return;
    const double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv)) return;
    f.push_back(fhz);
    p.push_back(pv);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 2) {
    out.n_points = f.size();
    return out;
  }

  // Transform into log-log space.
  std::vector<double> x;
  std::vector<double> y;
  x.reserve(f.size());
  y.reserve(f.size());
  for (size_t i = 0; i < f.size(); ++i) {
    const double fi = f[i];
    double pi = p[i];
    if (!std::isfinite(fi) || !(fi > 0.0)) continue;
    if (!std::isfinite(pi) || pi < 0.0) continue;
    if (pi < eps) pi = eps;
    const double xi = std::log10(fi);
    const double yi = std::log10(pi);
    if (!std::isfinite(xi) || !std::isfinite(yi)) continue;
    x.push_back(xi);
    y.push_back(yi);
  }

  out.n_points = x.size();
  if (x.size() < 2) {
    return out;
  }

  std::vector<double> w(x.size(), 1.0);
  LinFit fit = weighted_linear_fit(x, y, w);

  if (robust && max_iter > 0) {
    // A small Huber IRLS loop to reduce the influence of narrowband peaks.
    // This is intentionally lightweight and deterministic.
    for (int it = 0; it < max_iter; ++it) {
      std::vector<double> r;
      r.reserve(x.size());
      for (size_t i = 0; i < x.size(); ++i) {
        const double yhat = fit.intercept + fit.slope * x[i];
        r.push_back(y[i] - yhat);
      }

      const double s = mad_scale(r);
      if (!(s > 1e-12) || !std::isfinite(s)) break;

      const double k = 1.345 * s;
      for (size_t i = 0; i < x.size(); ++i) {
        const double a = std::fabs(r[i]);
        if (!(a > k)) {
          w[i] = 1.0;
        } else {
          w[i] = k / a;
        }
        if (!(w[i] > 0.0) || !std::isfinite(w[i])) w[i] = 1.0;
      }

      const LinFit next = weighted_linear_fit(x, y, w);
      const double ds = std::fabs(next.slope - fit.slope);
      const double di = std::fabs(next.intercept - fit.intercept);
      fit = next;
      if (ds < 1e-10 && di < 1e-10) break;
    }
  }

  // Compute R^2 in the log-log domain (weighted if robust).
  std::vector<double> yhat;
  yhat.reserve(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    yhat.push_back(fit.intercept + fit.slope * x[i]);
  }

  out.slope = fit.slope;
  out.intercept = fit.intercept;
  out.r2 = weighted_r2(y, yhat, w);
  out.rmse = weighted_rmse(y, yhat, w);
  out.rmse_unweighted = unweighted_rmse(y, yhat);
  return out;
}

SpectralLogLogFit spectral_loglog_fit(const PsdResult& psd,
                                     double fmin_hz,
                                     double fmax_hz,
                                     bool robust,
                                     int max_iter,
                                     double eps) {
  const std::vector<FrequencyRange> none;
  return spectral_loglog_fit(psd, fmin_hz, fmax_hz, none, robust, max_iter, eps);
}

SpectralLogLogTwoSlopeFit spectral_loglog_two_slope_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      const std::vector<FrequencyRange>& exclude_ranges_hz,
                                                      bool robust,
                                                      int max_iter,
                                                      std::size_t min_points_per_side,
                                                      double eps) {
  validate_psd(psd);
  SpectralLogLogTwoSlopeFit out;
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_loglog_two_slope_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (max_iter < 0) max_iter = 0;
  if (min_points_per_side < 2) min_points_per_side = 2;

  // Clamp to PSD support.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi0 > f_lo0)) {
    return out; // empty overlap
  }

  // log10 requires positive frequencies.
  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return out;
  }

  auto is_excluded = [&](double fhz) {
    if (exclude_ranges_hz.empty()) return false;
    for (const auto& r : exclude_ranges_hz) {
      const double lo = r.fmin_hz;
      const double hi = r.fmax_hz;
      if (!std::isfinite(lo) || !std::isfinite(hi)) continue;
      if (!(hi > lo)) continue;
      if (fhz >= lo && fhz <= hi) return true;
    }
    return false;
  };

  // Collect sample points (include interpolated boundaries).
  std::vector<double> f;
  std::vector<double> p;
  f.reserve(psd.freqs_hz.size() + 2);
  p.reserve(psd.freqs_hz.size() + 2);

  auto push_point = [&](double fhz) {
    if (!(fhz > 0.0) || !std::isfinite(fhz)) return;
    if (is_excluded(fhz)) return;
    const double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv)) return;
    f.push_back(fhz);
    p.push_back(pv);
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  if (f.size() < 3) {
    out.n_points = f.size();
    return out;
  }

  // Transform to log10-log10 space.
  std::vector<double> x;
  std::vector<double> y;
  x.reserve(f.size());
  y.reserve(f.size());
  for (size_t i = 0; i < f.size(); ++i) {
    const double fi = f[i];
    double pi = p[i];
    if (!std::isfinite(fi) || !(fi > 0.0)) continue;
    if (!std::isfinite(pi) || pi < 0.0) continue;
    if (pi < eps) pi = eps;
    const double xi = std::log10(fi);
    const double yi = std::log10(pi);
    if (!std::isfinite(xi) || !std::isfinite(yi)) continue;
    x.push_back(xi);
    y.push_back(yi);
  }

  out.n_points = x.size();
  const size_t n = x.size();
  // Each side includes the knee point, so the minimum total points is
  // 2*min_points_per_side - 1.
  const std::size_t min_total = (min_points_per_side * 2u) - 1u;
  if (n < min_total) {
    return out;
  }

  // Candidate knee search (OLS on the full set; robust IRLS is applied afterwards).
  double best_x0 = std::numeric_limits<double>::quiet_NaN();
  double best_rmse = std::numeric_limits<double>::infinity();
  Piecewise2SlopeFit best_fit;

  std::vector<double> w_ones(n, 1.0);

  const size_t start = min_points_per_side - 1;
  const size_t end = n - min_points_per_side;
  for (size_t k = start; k <= end; ++k) {
    const double x0 = x[k];
    if (!std::isfinite(x0)) continue;

    const Piecewise2SlopeFit fit0 = weighted_piecewise_continuous_fit(x, y, w_ones, x0);
    if (!std::isfinite(fit0.a) || !std::isfinite(fit0.b_lo) || !std::isfinite(fit0.b_hi)) continue;

    double sw = 0.0;
    double sse = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double yi = y[i];
      const double yhi = predict_piecewise_continuous(x[i], x0, fit0);
      if (!std::isfinite(yi) || !std::isfinite(yhi)) continue;
      const double e = yi - yhi;
      sw += 1.0;
      sse += e * e;
    }
    if (!(sw > 0.0)) continue;
    const double rmse = std::sqrt(sse / sw);
    if (rmse < best_rmse) {
      best_rmse = rmse;
      best_x0 = x0;
      best_fit = fit0;
    }
  }

  if (!std::isfinite(best_x0) || !std::isfinite(best_fit.a)) {
    return out;
  }

  // Optionally refine with Huber IRLS at the selected knee.
  std::vector<double> w(n, 1.0);
  Piecewise2SlopeFit fit = best_fit;
  if (robust && max_iter > 0) {
    for (int it = 0; it < max_iter; ++it) {
      std::vector<double> r;
      r.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        const double yhi = predict_piecewise_continuous(x[i], best_x0, fit);
        r.push_back(y[i] - yhi);
      }

      const double s = mad_scale(r);
      if (!(s > 1e-12) || !std::isfinite(s)) break;

      const double k = 1.345 * s;
      for (size_t i = 0; i < n; ++i) {
        const double a = std::fabs(r[i]);
        if (!(a > k)) {
          w[i] = 1.0;
        } else {
          w[i] = k / a;
        }
        if (!(w[i] > 0.0) || !std::isfinite(w[i])) w[i] = 1.0;
      }

      const Piecewise2SlopeFit next = weighted_piecewise_continuous_fit(x, y, w, best_x0);
      const double d0 = std::fabs(next.a - fit.a);
      const double d1 = std::fabs(next.b_lo - fit.b_lo);
      const double d2 = std::fabs(next.b_hi - fit.b_hi);
      fit = next;
      if (d0 < 1e-10 && d1 < 1e-10 && d2 < 1e-10) break;
    }
  }

  // Final predictions.
  std::vector<double> yhat;
  yhat.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    yhat.push_back(predict_piecewise_continuous(x[i], best_x0, fit));
  }

  out.found = true;
  out.knee_hz = std::pow(10.0, best_x0);
  out.slope_low = fit.b_lo;
  out.slope_high = fit.b_hi;
  out.intercept_low = fit.a - fit.b_lo * best_x0;
  out.intercept_high = fit.a - fit.b_hi * best_x0;
  out.r2 = weighted_r2(y, yhat, w);
  out.rmse = weighted_rmse(y, yhat, w);
  out.rmse_unweighted = unweighted_rmse(y, yhat);
  return out;
}

SpectralLogLogTwoSlopeFit spectral_loglog_two_slope_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      bool robust,
                                                      int max_iter,
                                                      std::size_t min_points_per_side,
                                                      double eps) {
  const std::vector<FrequencyRange> none;
  return spectral_loglog_two_slope_fit(psd, fmin_hz, fmax_hz, none, robust, max_iter, min_points_per_side, eps);
}




namespace {

static inline double log10_safe(double v, double eps) {
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (!std::isfinite(v) || v <= eps) v = eps;
  return std::log10(v);
}

struct KneeModelParams {
  double offset{std::numeric_limits<double>::quiet_NaN()};
  double exponent{std::numeric_limits<double>::quiet_NaN()};
  double knee_freq_hz{std::numeric_limits<double>::quiet_NaN()};
  double knee_param{std::numeric_limits<double>::quiet_NaN()};
  double sse{std::numeric_limits<double>::infinity()};
};

static inline double knee_model_log10_term(double f_hz, double knee_freq_hz, double exponent, double eps) {
  // term = knee + f^exponent, where knee = knee_freq^exponent.
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (!std::isfinite(f_hz) || f_hz < 0.0) f_hz = 0.0;
  if (!std::isfinite(exponent) || exponent <= 0.0) exponent = 1.0;

  double f_term = 0.0;
  if (f_hz > 0.0) {
    f_term = std::pow(f_hz, exponent);
    if (!std::isfinite(f_term)) f_term = 0.0;
  }

  double k_term = 0.0;
  if (std::isfinite(knee_freq_hz) && knee_freq_hz > 0.0) {
    k_term = std::pow(knee_freq_hz, exponent);
    if (!std::isfinite(k_term)) k_term = 0.0;
  }

  double term = k_term + f_term;
  if (!std::isfinite(term) || term <= eps) term = eps;
  return std::log10(term);
}

static inline double knee_model_offset_from_terms(const std::vector<double>& y,
                                                  const std::vector<double>& log10_terms,
                                                  const std::vector<double>& w) {
  // For model yhat = offset - log10(term), optimal offset (weighted LS) is:
  //   offset = mean_w( y + log10(term) ).
  const size_t n = std::min(y.size(), std::min(log10_terms.size(), w.size()));
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  double sw = 0.0;
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double wi = w[i];
    if (!(wi > 0.0) || !std::isfinite(wi)) continue;
    const double yi = y[i];
    const double ti = log10_terms[i];
    if (!std::isfinite(yi) || !std::isfinite(ti)) continue;
    sw += wi;
    s += wi * (yi + ti);
  }
  if (!(sw > 0.0) || !std::isfinite(sw) || !std::isfinite(s)) return std::numeric_limits<double>::quiet_NaN();
  return s / sw;
}

static KneeModelParams knee_model_fit_grid(const std::vector<double>& f,
                                           const std::vector<double>& y,
                                           const std::vector<double>& w,
                                           double f_lo,
                                           double f_hi,
                                           double exponent_center,
                                           double exponent_span,
                                           double exponent_step,
                                           int n_fk_log,
                                           bool include_zero_knee,
                                           double eps) {
  KneeModelParams best;
  const size_t n = std::min(f.size(), std::min(y.size(), w.size()));
  if (n < 2) return best;

  if (!(f_hi > f_lo)) return best;
  if (!std::isfinite(exponent_center)) exponent_center = 2.0;
  if (!(exponent_center > 0.0)) exponent_center = 2.0;
  if (!(exponent_span > 0.0) || !std::isfinite(exponent_span)) exponent_span = 1.5;
  if (!(exponent_step > 0.0) || !std::isfinite(exponent_step)) exponent_step = 0.1;
  if (n_fk_log < 8) n_fk_log = 8;
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;

  const double e_min = std::max(0.05, exponent_center - exponent_span);
  const double e_max = std::min(8.0, exponent_center + exponent_span);

  // Knee frequency candidates: 0 + log-spaced between f_lo and f_hi.
  std::vector<double> fk_list;
  fk_list.reserve(static_cast<size_t>(n_fk_log) + 1);
  if (include_zero_knee) fk_list.push_back(0.0);
  const double fk_lo = std::max(f_lo, 1e-6);
  const double fk_hi = std::max(f_hi, fk_lo * 1.000001);
  for (int i = 0; i < n_fk_log; ++i) {
    const double t = (n_fk_log == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(n_fk_log - 1);
    const double fk = fk_lo * std::pow(fk_hi / fk_lo, t);
    fk_list.push_back(fk);
  }

  std::vector<double> log10_terms(n);
  std::vector<double> yhat(n);

  for (double exponent = e_min; exponent <= e_max + 0.5 * exponent_step; exponent += exponent_step) {
    if (!(exponent > 0.0) || !std::isfinite(exponent)) continue;

    for (double fk : fk_list) {
      // Compute per-point log10(k + f^exponent).
      for (size_t i = 0; i < n; ++i) {
        log10_terms[i] = knee_model_log10_term(f[i], fk, exponent, eps);
      }
      const double offset = knee_model_offset_from_terms(y, log10_terms, w);
      if (!std::isfinite(offset)) continue;

      double sse = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const double wi = w[i];
        if (!(wi > 0.0) || !std::isfinite(wi)) continue;
        const double yi = y[i];
        const double ti = log10_terms[i];
        if (!std::isfinite(yi) || !std::isfinite(ti)) continue;
        const double yhi = offset - ti;
        const double r = yi - yhi;
        sse += wi * r * r;
      }

      if (sse < best.sse) {
        best.sse = sse;
        best.offset = offset;
        best.exponent = exponent;
        best.knee_freq_hz = fk;
        if (fk > 0.0 && std::isfinite(fk)) {
          const double kp = std::pow(fk, exponent);
          best.knee_param = std::isfinite(kp) ? kp : std::numeric_limits<double>::quiet_NaN();
        } else {
          best.knee_param = 0.0;
        }
      }
    }
  }

  return best;
}

} // namespace

SpectralAperiodicKneeFit spectral_aperiodic_knee_fit(const PsdResult& psd,
                                                    double fmin_hz,
                                                    double fmax_hz,
                                                    const std::vector<FrequencyRange>& exclude_ranges_hz,
                                                    bool robust,
                                                    int max_iter,
                                                    double eps) {
  validate_psd(psd);
  SpectralAperiodicKneeFit out;
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("spectral_aperiodic_knee_fit: fmax must be > fmin");
  }
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  if (max_iter < 0) max_iter = 0;

  // Clamp to PSD support.
  const double f_lo0 = std::max(fmin_hz, psd.freqs_hz.front());
  const double f_hi0 = std::min(fmax_hz, psd.freqs_hz.back());
  if (!(f_hi0 > f_lo0)) {
    return out;
  }

  const double f_lo = std::max(f_lo0, 1e-9);
  const double f_hi = f_hi0;
  if (!(f_hi > f_lo)) {
    return out;
  }

  auto is_excluded = [&](double fhz) {
    if (exclude_ranges_hz.empty()) return false;
    for (const auto& r : exclude_ranges_hz) {
      const double lo = r.fmin_hz;
      const double hi = r.fmax_hz;
      if (!std::isfinite(lo) || !std::isfinite(hi)) continue;
      if (!(hi > lo)) continue;
      if (fhz >= lo && fhz <= hi) return true;
    }
    return false;
  };

  // Collect points (include interpolated boundaries).
  std::vector<double> f;
  std::vector<double> y;
  f.reserve(psd.freqs_hz.size() + 2);
  y.reserve(psd.freqs_hz.size() + 2);

  auto push_point = [&](double fhz) {
    if (!(fhz > 0.0) || !std::isfinite(fhz)) return;
    if (is_excluded(fhz)) return;
    const double pv = psd_at_freq_linear(psd, fhz);
    if (!std::isfinite(pv)) return;
    f.push_back(fhz);
    y.push_back(log10_safe(pv, eps));
  };

  push_point(f_lo);
  for (size_t i = 0; i < psd.freqs_hz.size(); ++i) {
    const double fi = psd.freqs_hz[i];
    if (fi <= f_lo || fi >= f_hi) continue;
    push_point(fi);
  }
  if (f_hi != f_lo) push_point(f_hi);

  out.n_points = f.size();
  if (f.size() < 2) {
    return out;
  }

  std::vector<double> w(f.size(), 1.0);

  // Use the (non-knee) log-log fit to pick a reasonable exponent search center.
  double exponent0 = 2.0;
  {
    const SpectralLogLogFit ll = spectral_loglog_fit(psd, f_lo, f_hi, exclude_ranges_hz, /*robust=*/false, /*max_iter=*/0, eps);
    if (std::isfinite(ll.slope)) {
      const double e0 = -ll.slope;
      if (std::isfinite(e0) && e0 > 0.0) exponent0 = std::clamp(e0, 0.05, 8.0);
    }
  }

  KneeModelParams best;
  double prev_offset = std::numeric_limits<double>::quiet_NaN();
  double prev_exponent = std::numeric_limits<double>::quiet_NaN();
  double prev_fk = std::numeric_limits<double>::quiet_NaN();

  const int iters = (robust ? std::max(1, max_iter) : 1);
  for (int it = 0; it < iters; ++it) {
    // Coarse search around exponent0.
    best = knee_model_fit_grid(f, y, w, f_lo, f_hi, exponent0, /*span=*/1.5, /*step=*/0.1, /*n_fk_log=*/28,
                               /*include_zero_knee=*/true, eps);

    // Refine around best.
    if (std::isfinite(best.exponent) && std::isfinite(best.knee_freq_hz)) {
      const double e_center = best.exponent;
      double fk_center = best.knee_freq_hz;
      if (!(fk_center >= 0.0) || !std::isfinite(fk_center)) fk_center = 0.0;

      double span = 0.25;
      double step = 0.02;

      // If we landed on the boundary of the exponent span, broaden slightly.
      if (std::fabs(e_center - exponent0) > 1.25) {
        span = 0.5;
      }

      // Build a custom refinement grid for knee frequency.
      // We'll call knee_model_fit_grid with a narrowed exponent window and a tighter fk log grid.
      // To focus fk around the candidate, we temporarily adjust f_lo/f_hi.
      double fk_lo = f_lo;
      double fk_hi = f_hi;
      if (fk_center > 0.0) {
        fk_lo = std::max(f_lo, fk_center / 2.0);
        fk_hi = std::min(f_hi, fk_center * 2.0);
        if (!(fk_hi > fk_lo)) {
          fk_lo = f_lo;
          fk_hi = f_hi;
        }
      } else {
        // Focus on small knees if the best knee is 0.
        fk_lo = f_lo;
        fk_hi = std::min(f_hi, std::max(f_lo * 4.0, f_lo + 1e-6));
      }

      // We can't pass fk_lo/fk_hi directly into knee_model_fit_grid (it uses f_lo/f_hi for fk range).
      // So we call it with the fk-focused bounds.
      KneeModelParams refined = knee_model_fit_grid(f, y, w, fk_lo, fk_hi, e_center, span, step, /*n_fk_log=*/32,
                                                    /*include_zero_knee=*/true, eps);
      if (refined.sse < best.sse) best = refined;
    }

    if (!robust || max_iter <= 0) break;

    // Compute residuals for robust weighting.
    std::vector<double> r;
    r.reserve(f.size());
    for (size_t i = 0; i < f.size(); ++i) {
      const double ti = knee_model_log10_term(f[i], best.knee_freq_hz, best.exponent, eps);
      const double yhat = best.offset - ti;
      r.push_back(y[i] - yhat);
    }

    const double s = mad_scale(r);
    if (!(s > 1e-12) || !std::isfinite(s)) break;

    const double k = 1.345 * s;
    for (size_t i = 0; i < f.size(); ++i) {
      const double a = std::fabs(r[i]);
      if (!(a > k)) {
        w[i] = 1.0;
      } else {
        w[i] = k / a;
      }
      if (!(w[i] > 0.0) || !std::isfinite(w[i])) w[i] = 1.0;
    }

    // Convergence check.
    const double d_off = std::isfinite(prev_offset) && std::isfinite(best.offset) ? std::fabs(best.offset - prev_offset) : std::numeric_limits<double>::infinity();
    const double d_exp = std::isfinite(prev_exponent) && std::isfinite(best.exponent) ? std::fabs(best.exponent - prev_exponent) : std::numeric_limits<double>::infinity();
    const double d_fk = std::isfinite(prev_fk) && std::isfinite(best.knee_freq_hz) ? std::fabs(best.knee_freq_hz - prev_fk) : std::numeric_limits<double>::infinity();

    prev_offset = best.offset;
    prev_exponent = best.exponent;
    prev_fk = best.knee_freq_hz;

    if (d_off < 1e-6 && d_exp < 1e-3 && d_fk < 1e-3) {
      break;
    }
  }

  if (!std::isfinite(best.offset) || !std::isfinite(best.exponent) || !std::isfinite(best.sse)) {
    return out;
  }

  // Final predictions for R^2 / RMSE in the log10(PSD) domain.
  std::vector<double> yhat;
  yhat.reserve(f.size());
  for (size_t i = 0; i < f.size(); ++i) {
    const double ti = knee_model_log10_term(f[i], best.knee_freq_hz, best.exponent, eps);
    yhat.push_back(best.offset - ti);
  }

  out.found = true;
  out.offset = best.offset;
  out.exponent = best.exponent;
  out.knee_freq_hz = (std::isfinite(best.knee_freq_hz) ? best.knee_freq_hz : std::numeric_limits<double>::quiet_NaN());
  out.knee = best.knee_param;
  out.r2 = weighted_r2(y, yhat, w);
  out.rmse = weighted_rmse(y, yhat, w);
  out.rmse_unweighted = unweighted_rmse(y, yhat);
  return out;
}

SpectralAperiodicKneeFit spectral_aperiodic_knee_fit(const PsdResult& psd,
                                                    double fmin_hz,
                                                    double fmax_hz,
                                                    bool robust,
                                                    int max_iter,
                                                    double eps) {
  const std::vector<FrequencyRange> none;
  return spectral_aperiodic_knee_fit(psd, fmin_hz, fmax_hz, none, robust, max_iter, eps);
}
} // namespace qeeg
