#include "qeeg/running_stats.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <limits>

static bool approx(double a, double b, double eps = 1e-12) {
  return (a > b ? a - b : b - a) <= eps;
}

int main() {
  using namespace qeeg;

  {
    RunningStats rs;
    rs.add(1.0);
    rs.add(2.0);
    rs.add(3.0);
    rs.add(4.0);

    assert(rs.n() == 4);
    assert(approx(rs.mean(), 2.5, 1e-12));

    // Population variance of [1,2,3,4] is 1.25, sample variance is 1.666666...
    assert(approx(rs.variance_population(), 1.25, 1e-12));
    assert(approx(rs.variance_sample(), 1.6666666666666667, 1e-12));

    assert(approx(rs.stddev_population(), std::sqrt(1.25), 1e-12));
    assert(approx(rs.stddev_sample(), std::sqrt(1.6666666666666667), 1e-12));
  }

  {
    RunningStats rs;
    rs.add(std::numeric_limits<double>::quiet_NaN());
    rs.add(std::numeric_limits<double>::infinity());
    rs.add(-std::numeric_limits<double>::infinity());
    rs.add(10.0);
    assert(rs.n() == 1);
    assert(approx(rs.mean(), 10.0, 1e-12));
    assert(!std::isfinite(rs.variance_sample()));
  }

  std::cout << "All tests passed.\n";
  return 0;
}
