#include "qeeg/spherical_spline.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

static bool approx(double a, double b, double eps) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Fixed point set on the sphere (non-degenerate).
  std::vector<Vec3> pos = {
    normalize_vec3(Vec3{1, 0, 0}),
    normalize_vec3(Vec3{-1, 0, 0}),
    normalize_vec3(Vec3{0, 1, 0}),
    normalize_vec3(Vec3{0, -1, 0}),
    normalize_vec3(Vec3{0, 0, 1}),
    normalize_vec3(Vec3{0.3, 0.6, 0.7}),
  };

  const Vec3 q = normalize_vec3(Vec3{0.2, -0.4, 0.9});

  SphericalSplineOptions opt;
  opt.n_terms = 60;
  opt.m = 4;
  opt.lambda = 1e-8;

  const std::vector<double> w = spherical_spline_weights(pos, q, opt);
  assert(w.size() == pos.size());

  std::mt19937 rng(123);
  std::uniform_real_distribution<double> U(-10.0, 10.0);

  // For a spherical spline interpolator, evaluating at q must be a linear
  // function of the input values v. The precomputed weights should match the
  // interpolator evaluation.
  for (int it = 0; it < 25; ++it) {
    std::vector<double> v;
    v.reserve(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) v.push_back(U(rng));

    const auto interp = SphericalSplineInterpolator::fit(pos, v, opt);
    const double y_interp = interp.evaluate(q);

    double y_w = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
      y_w += w[i] * v[i];
    }

    assert(approx(y_w, y_interp, 1e-7));
  }

  std::cout << "test_spherical_spline_weights OK\n";
  return 0;
}
