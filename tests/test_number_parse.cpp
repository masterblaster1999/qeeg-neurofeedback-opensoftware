#include "qeeg/utils.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

static bool nearly(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using qeeg::to_double;
  using qeeg::to_int;

  // to_int()
  {
    const int v = to_int("42");
    assert(v == 42);
  }

  {
    const int v = to_int("  -10  ");
    assert(v == -10);
  }

  {
    bool threw = false;
    try {
      (void)to_int("12abc");
    } catch (...) {
      threw = true;
    }
    assert(threw);
  }

  // to_double()
  {
    const double v = to_double("1.25");
    assert(nearly(v, 1.25));
  }

  {
    const double v = to_double("  -3.5  ");
    assert(nearly(v, -3.5));
  }

  {
    // Decimal comma convenience: common in some locales.
    const double v = to_double("0,5");
    assert(nearly(v, 0.5));
  }

  {
    bool threw = false;
    try {
      (void)to_double("1.23abc");
    } catch (...) {
      threw = true;
    }
    assert(threw);
  }

  {
    bool threw = false;
    try {
      // Multiple separators should not be accepted.
      (void)to_double("1.2.3");
    } catch (...) {
      threw = true;
    }
    assert(threw);
  }

  std::cout << "ok\n";
  return 0;
}
