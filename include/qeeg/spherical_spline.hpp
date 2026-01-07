#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// Parameters for Perrin-style spherical spline interpolation on the unit sphere.
// Typical EEG topomap settings use m=4 and n_terms ~ 50.
struct SphericalSplineOptions {
  int n_terms{50};       // number of Legendre terms in the kernel expansion (>= 5 recommended)
  int m{4};              // spline order (Perrin uses m=4 for scalp potentials)
  double lambda{1e-5};   // diagonal regularization for numerical stability (>= 0)
};

// Utility: normalize a 3D vector to unit length (returns {0,0,0} if input is zero).
Vec3 normalize_vec3(const Vec3& v);

// Utility: project a 2D montage point on the unit disk to the unit sphere (z >= 0 hemisphere).
// If x^2+y^2 > 1, the point is clamped to the unit circle.
Vec3 project_to_unit_sphere(const Vec2& p);

// A fitted spherical spline interpolator that can be evaluated at arbitrary points on the unit sphere.
// Fit solves:
//   [G  1][c] = [v]
//   [1ᵀ 0][d]   [0]
// and evaluates f(q) = sum_i c_i g(dot(q, p_i)) + d
class SphericalSplineInterpolator {
public:
  SphericalSplineInterpolator() = default;

  static SphericalSplineInterpolator fit(const std::vector<Vec3>& positions_unit,
                                         const std::vector<double>& values,
                                         const SphericalSplineOptions& opt = {});

  double evaluate(const Vec3& q_unit) const;

  size_t n_points() const { return pos_.size(); }
  const SphericalSplineOptions& options() const { return opt_; }

private:
  SphericalSplineOptions opt_{};
  std::vector<Vec3> pos_;      // unit sphere points
  std::vector<double> coeff_;  // spline coefficients c_i
  double constant_{0.0};       // constant term d
};

} // namespace qeeg
