#include "qeeg/spectral_features.hpp"

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

} // namespace

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

} // namespace qeeg
