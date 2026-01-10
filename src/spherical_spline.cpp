#include "qeeg/spherical_spline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace qeeg {

static inline double dot3(const Vec3& a, const Vec3& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
}

Vec3 normalize_vec3(const Vec3& v) {
  const double n2 = v.x*v.x + v.y*v.y + v.z*v.z;
  if (n2 <= 0.0) return Vec3{0.0, 0.0, 0.0};
  const double inv = 1.0 / std::sqrt(n2);
  return Vec3{v.x * inv, v.y * inv, v.z * inv};
}

Vec3 project_to_unit_sphere(const Vec2& p) {
  double x = p.x;
  double y = p.y;
  double r2 = x*x + y*y;
  if (r2 > 1.0) {
    // Clamp to unit circle
    const double r = std::sqrt(r2);
    if (r > 0.0) {
      x /= r;
      y /= r;
    }
    r2 = 1.0;
  }
  const double z = std::sqrt(std::max(0.0, 1.0 - r2));
  return Vec3{x, y, z}; // already unit-length by construction
}

// Compute Legendre P_n(x) for n=0..N using recurrence, but streamed.
static inline double legendre_Pn(int n, double x, double& Pnm1, double& Pnm2) {
  // Caller provides Pnm1 = P_{n-1}, Pnm2 = P_{n-2}, and updates them.
  // For n==1, caller should pass Pnm1=P1, Pnm2=P0 and we return P1 without update.
  if (n == 0) return 1.0;
  if (n == 1) return x;
  const double Pn = ((2.0 * n - 1.0) * x * Pnm1 - (n - 1.0) * Pnm2) / static_cast<double>(n);
  Pnm2 = Pnm1;
  Pnm1 = Pn;
  return Pn;
}

// Perrin-style kernel g_m(x) = sum_{n=1..N} (2n+1) / (n(n+1))^m * P_n(x)
static double kernel_g(double x, int n_terms, int m) {
  if (n_terms < 1) return 0.0;
  if (m < 1) throw std::runtime_error("kernel_g: m must be >= 1");

  x = std::max(-1.0, std::min(1.0, x));

  double sum = 0.0;

  // P0, P1
  double Pnm2 = 1.0;
  double Pnm1 = x;

  for (int n = 1; n <= n_terms; ++n) {
    double Pn;
    if (n == 1) {
      Pn = Pnm1;
    } else {
      Pn = legendre_Pn(n, x, Pnm1, Pnm2);
    }

    const double nn1 = static_cast<double>(n) * static_cast<double>(n + 1);
    const double denom = std::pow(nn1, static_cast<double>(m));
    const double w = (2.0 * n + 1.0) / denom;
    sum += w * Pn;
  }

  return sum;
}

static std::vector<double> solve_linear_system_gauss(std::vector<double> A,
                                                     std::vector<double> b,
                                                     int n) {
  // A is row-major n*n. b length n.
  if (static_cast<int>(b.size()) != n) {
    throw std::runtime_error("solve_linear_system_gauss: b size mismatch");
  }
  if (static_cast<int>(A.size()) != n * n) {
    throw std::runtime_error("solve_linear_system_gauss: A size mismatch");
  }

  auto idx = [n](int r, int c) { return r * n + c; };

  for (int i = 0; i < n; ++i) {
    // Pivot
    int piv = i;
    double best = std::abs(A[idx(i, i)]);
    for (int r = i + 1; r < n; ++r) {
      const double v = std::abs(A[idx(r, i)]);
      if (v > best) {
        best = v;
        piv = r;
      }
    }

    if (best < 1e-14) {
      throw std::runtime_error("solve_linear_system_gauss: matrix is singular/ill-conditioned");
    }

    if (piv != i) {
      // swap rows i and piv
      for (int c = i; c < n; ++c) {
        std::swap(A[idx(i, c)], A[idx(piv, c)]);
      }
      std::swap(b[i], b[piv]);
    }

    const double diag = A[idx(i, i)];

    // Eliminate below
    for (int r = i + 1; r < n; ++r) {
      const double f = A[idx(r, i)] / diag;
      if (f == 0.0) continue;
      A[idx(r, i)] = 0.0;
      for (int c = i + 1; c < n; ++c) {
        A[idx(r, c)] -= f * A[idx(i, c)];
      }
      b[r] -= f * b[i];
    }
  }

  // Back-substitution
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  for (int i = n - 1; i >= 0; --i) {
    double s = b[i];
    for (int c = i + 1; c < n; ++c) {
      s -= A[idx(i, c)] * x[static_cast<size_t>(c)];
    }
    x[static_cast<size_t>(i)] = s / A[idx(i, i)];
  }
  return x;
}

SphericalSplineInterpolator SphericalSplineInterpolator::fit(const std::vector<Vec3>& positions_unit,
                                                             const std::vector<double>& values,
                                                             const SphericalSplineOptions& opt) {
  if (positions_unit.size() != values.size()) {
    throw std::runtime_error("SphericalSplineInterpolator::fit: positions and values size mismatch");
  }
  if (positions_unit.size() < 3) {
    throw std::runtime_error("SphericalSplineInterpolator::fit: need at least 3 points");
  }
  if (opt.n_terms < 5) {
    throw std::runtime_error("SphericalSplineInterpolator::fit: n_terms too small (>=5 recommended)");
  }
  if (opt.m < 1) {
    throw std::runtime_error("SphericalSplineInterpolator::fit: m must be >= 1");
  }
  if (opt.lambda < 0.0) {
    throw std::runtime_error("SphericalSplineInterpolator::fit: lambda must be >= 0");
  }

  SphericalSplineInterpolator out;
  out.opt_ = opt;

  const size_t K = positions_unit.size();
  out.pos_.reserve(K);
  for (const auto& p : positions_unit) {
    const Vec3 u = normalize_vec3(p);
    if (u.x == 0.0 && u.y == 0.0 && u.z == 0.0) {
      throw std::runtime_error("SphericalSplineInterpolator::fit: zero-length position vector");
    }
    out.pos_.push_back(u);
  }

  // Build augmented system (K+1)x(K+1)
  const int N = static_cast<int>(K) + 1;
  std::vector<double> A(static_cast<size_t>(N) * static_cast<size_t>(N), 0.0);
  std::vector<double> b(static_cast<size_t>(N), 0.0);

  auto idx = [N](int r, int c) { return r * N + c; };

  for (int i = 0; i < static_cast<int>(K); ++i) {
    b[static_cast<size_t>(i)] = values[static_cast<size_t>(i)];
    for (int j = 0; j < static_cast<int>(K); ++j) {
      const double x = dot3(out.pos_[static_cast<size_t>(i)], out.pos_[static_cast<size_t>(j)]);
      double gij = kernel_g(x, opt.n_terms, opt.m);
      if (i == j) gij += opt.lambda;
      A[static_cast<size_t>(idx(i, j))] = gij;
    }
    // last column = 1
    A[static_cast<size_t>(idx(i, static_cast<int>(K)))] = 1.0;
  }

  // constraint row
  for (int j = 0; j < static_cast<int>(K); ++j) {
    A[static_cast<size_t>(idx(static_cast<int>(K), j))] = 1.0;
  }
  A[static_cast<size_t>(idx(static_cast<int>(K), static_cast<int>(K)))] = 0.0;
  b[static_cast<size_t>(K)] = 0.0;

  std::vector<double> x = solve_linear_system_gauss(std::move(A), std::move(b), N);

  out.coeff_.assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(K));
  out.constant_ = x[static_cast<size_t>(K)];
  return out;
}

double SphericalSplineInterpolator::evaluate(const Vec3& q_unit) const {
  if (pos_.empty()) return std::numeric_limits<double>::quiet_NaN();
  const Vec3 q = normalize_vec3(q_unit);
  double s = constant_;
  for (size_t i = 0; i < pos_.size(); ++i) {
    const double x = dot3(q, pos_[i]);
    s += coeff_[i] * kernel_g(x, opt_.n_terms, opt_.m);
  }
  return s;
}

std::vector<double> spherical_spline_weights(const std::vector<Vec3>& positions_unit,
                                             const Vec3& q_unit,
                                             const SphericalSplineOptions& opt) {
  if (positions_unit.size() < 3) {
    throw std::runtime_error("spherical_spline_weights: need at least 3 points");
  }
  if (opt.n_terms < 5) {
    throw std::runtime_error("spherical_spline_weights: n_terms too small (>=5 recommended)");
  }
  if (opt.m < 1) {
    throw std::runtime_error("spherical_spline_weights: m must be >= 1");
  }
  if (opt.lambda < 0.0) {
    throw std::runtime_error("spherical_spline_weights: lambda must be >= 0");
  }

  // Normalize sample positions.
  const size_t K = positions_unit.size();
  std::vector<Vec3> pos;
  pos.reserve(K);
  for (const auto& p : positions_unit) {
    const Vec3 u = normalize_vec3(p);
    if (u.x == 0.0 && u.y == 0.0 && u.z == 0.0) {
      throw std::runtime_error("spherical_spline_weights: zero-length position vector");
    }
    pos.push_back(u);
  }
  const Vec3 q = normalize_vec3(q_unit);
  if (q.x == 0.0 && q.y == 0.0 && q.z == 0.0) {
    throw std::runtime_error("spherical_spline_weights: zero-length query vector");
  }

  const int N = static_cast<int>(K) + 1; // augmented system size
  std::vector<double> M(static_cast<size_t>(N) * static_cast<size_t>(N), 0.0);

  auto idx = [N](int r, int c) { return r * N + c; };

  // Build M = [G 1; 1^T 0]
  for (int i = 0; i < static_cast<int>(K); ++i) {
    for (int j = 0; j < static_cast<int>(K); ++j) {
      const double x = dot3(pos[static_cast<size_t>(i)], pos[static_cast<size_t>(j)]);
      double gij = kernel_g(x, opt.n_terms, opt.m);
      if (i == j) gij += opt.lambda;
      M[static_cast<size_t>(idx(i, j))] = gij;
    }
    M[static_cast<size_t>(idx(i, static_cast<int>(K)))] = 1.0;
  }
  for (int j = 0; j < static_cast<int>(K); ++j) {
    M[static_cast<size_t>(idx(static_cast<int>(K), j))] = 1.0;
  }
  M[static_cast<size_t>(idx(static_cast<int>(K), static_cast<int>(K)))] = 0.0;

  // Weights satisfy:
  //   w_full^T = [g(q,p_i), 1] * M^{-1}
  // Equivalently, solve M^T x = u where u = [g(q,p_i), 1]^T,
  // then w = x[0:K].

  // Build A = M^T
  std::vector<double> A(static_cast<size_t>(N) * static_cast<size_t>(N), 0.0);
  for (int r = 0; r < N; ++r) {
    for (int c = 0; c < N; ++c) {
      A[static_cast<size_t>(idx(r, c))] = M[static_cast<size_t>(idx(c, r))];
    }
  }

  // RHS u
  std::vector<double> u(static_cast<size_t>(N), 0.0);
  for (size_t i = 0; i < K; ++i) {
    const double x = dot3(q, pos[i]);
    u[i] = kernel_g(x, opt.n_terms, opt.m);
  }
  u[K] = 1.0;

  std::vector<double> x = solve_linear_system_gauss(std::move(A), std::move(u), N);
  x.resize(K); // drop the extra coefficient
  return x;
}

} // namespace qeeg
