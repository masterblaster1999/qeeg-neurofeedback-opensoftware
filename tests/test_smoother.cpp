#include "qeeg/smoother.hpp"

#include "test_support.hpp"
#include <cmath>
#include <limits>

using namespace qeeg;

static bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

int main() {
  // Disabled mode behaves as pass-through.
  {
    ExponentialSmoother s;
    s.set_time_constant(0.0);
    const double y1 = s.update(1.0, 0.1);
    const double y2 = s.update(2.0, 0.1);
    assert(y1 == 1.0);
    assert(y2 == 2.0);
  }

  // Enabled mode: step response roughly matches 1 - exp(-dt/tau).
  {
    ExponentialSmoother s(1.0);
    const double y0 = s.update(0.0, 0.1);
    assert(y0 == 0.0);

    const double y1 = s.update(1.0, 1.0);
    const double expected = 1.0 - std::exp(-1.0);
    assert(near(y1, expected, 1e-6));
  }

  // Non-finite inputs do not update.
  {
    ExponentialSmoother s(0.5);
    (void)s.update(0.25, 0.1);
    const double y_prev = s.value();
    const double y_nan = s.update(std::numeric_limits<double>::quiet_NaN(), 0.1);
    assert(std::isfinite(y_prev));
    assert(y_nan == y_prev);
  }

  return 0;
}
