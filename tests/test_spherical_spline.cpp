#include "qeeg/spherical_spline.hpp"
#include "qeeg/topomap.hpp"
#include "qeeg/montage.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

static bool approx(double a, double b, double eps) {
  return std::fabs(a - b) <= eps;
}

static qeeg::Vec3 random_unit_vec3(std::mt19937& rng) {
  constexpr double kPi = 3.14159265358979323846;
  std::uniform_real_distribution<double> U01(0.0, 1.0);
  const double u = 2.0 * U01(rng) - 1.0;               // z in [-1,1]
  const double phi = 2.0 * kPi * U01(rng);
  const double t = std::sqrt(std::max(0.0, 1.0 - u*u));
  return qeeg::Vec3{t * std::cos(phi), t * std::sin(phi), u};
}

int main() {
  using namespace qeeg;

  // 1) Constant field should stay ~constant everywhere.
  {
    const double C = 3.14159;

    std::vector<Vec3> pos = {
      normalize_vec3(Vec3{1, 0, 0}),
      normalize_vec3(Vec3{-1, 0, 0}),
      normalize_vec3(Vec3{0, 1, 0}),
      normalize_vec3(Vec3{0, -1, 0}),
      normalize_vec3(Vec3{0, 0, 1}),
      normalize_vec3(Vec3{0, 0, -1}),
      normalize_vec3(Vec3{1, 1, 0.25}),
      normalize_vec3(Vec3{-0.5, 0.8, -0.1}),
    };

    std::vector<double> val(pos.size(), C);

    SphericalSplineOptions opt;
    opt.n_terms = 60;
    opt.m = 4;
    opt.lambda = 1e-8;

    auto interp = SphericalSplineInterpolator::fit(pos, val, opt);

    // Evaluate at fit points
    for (const auto& p : pos) {
      const double y = interp.evaluate(p);
      assert(approx(y, C, 1e-3));
    }

    // Evaluate at random points
    std::mt19937 rng(123);
    for (int i = 0; i < 25; ++i) {
      const Vec3 q = random_unit_vec3(rng);
      const double y = interp.evaluate(q);
      assert(approx(y, C, 1e-3));
    }
  }

  // 2) Topomap generation should produce finite values inside the head mask.
  {
    Montage m = Montage::builtin_standard_1020_19();
    std::vector<std::string> ch = {"Fp1", "Fp2", "F3", "F4", "C3", "C4", "P3", "P4", "O1", "O2"};
    std::vector<double> v;
    v.reserve(ch.size());
    for (size_t i = 0; i < ch.size(); ++i) v.push_back(static_cast<double>(i) - 4.5);

    TopomapOptions topt;
    topt.grid_size = 64;
    topt.method = TopomapInterpolation::SPHERICAL_SPLINE;
    topt.spline.n_terms = 50;
    topt.spline.m = 4;
    topt.spline.lambda = 1e-6;

    Grid2D g = make_topomap(m, ch, v, topt);

    const int N = g.size;
    const int cx = N / 2;
    const int cy = N / 2;
    const float center = g.values[static_cast<size_t>(cy) * static_cast<size_t>(N) + static_cast<size_t>(cx)];
    assert(!std::isnan(center));
  }

  std::cout << "test_spherical_spline OK\n";
  return 0;
}
