#include "qeeg/robust_stats.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Even count => average the middle two.
  {
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    const double med = median_inplace(&v);
    assert(approx(med, 2.5));
  }

  // Convenience overload: median_inplace(vector<>&)
  {
    std::vector<double> v = {1.0, 2.0, 3.0};
    const double med = median_inplace(v);
    assert(approx(med, 2.0));
  }


  // Outlier should not move the median much; MAD scale should be small.
  {
    const std::vector<double> v = {1.0, 2.0, 3.0, 4.0, 100.0};
    std::vector<double> tmp = v;
    const double med = median_inplace(&tmp);
    assert(approx(med, 3.0));
    const double scale = robust_scale(v, med);
    // abs deviations: {2,1,0,1,97} => MAD=1 => scale ~ 1.4826
    assert(approx(scale, 1.4826, 1e-4));
  }

  // Quantiles (linearly interpolated empirical quantile with q*(n-1))
  {
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    assert(approx(quantile_inplace(&v, 0.0), 1.0));
  }
  {
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    // Reference overload
    assert(approx(quantile_inplace(v, 0.5), 2.5));
  }
  {
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    assert(approx(quantile_inplace(&v, 1.0), 4.0));
  }
  {
    // Median for even n is the average of the two middle values.
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    assert(approx(quantile_inplace(&v, 0.5), 2.5));
  }
  {
    // q=0.25 => idx=0.75 => 1 + 0.75*(2-1)=1.75
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    assert(approx(quantile_inplace(&v, 0.25), 1.75));
  }
  {
    // q is clamped to [0,1]
    std::vector<double> v = {4.0, 1.0, 3.0, 2.0};
    assert(approx(quantile_inplace(&v, -1.0), 1.0));
  }
  {
    std::vector<double> v = {4.0, 1.0, 3.0, 2.0};
    assert(approx(quantile_inplace(&v, 2.0), 4.0));
  }

  // Constant data => MAD == 0, fallback should yield a sane non-zero scale.
  {
    const std::vector<double> v = {1.0, 1.0, 1.0};
    std::vector<double> tmp = v;
    const double med = median_inplace(&tmp);
    assert(approx(med, 1.0));
    const double scale = robust_scale(v, med);
    assert(approx(scale, 1.0));
  }

  std::cout << "test_robust_stats OK\n";
  return 0;
}
