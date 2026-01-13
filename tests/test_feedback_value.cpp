#include "qeeg/feedback_value.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace qeeg;

static void test_above_basic() {
  // span=2 => metric=thr+1 => 0.5
  const double v = feedback_value(11.0, 10.0, RewardDirection::Above, 2.0);
  assert(std::fabs(v - 0.5) < 1e-12);
}

static void test_above_clamp() {
  // At threshold => 0.
  assert(std::fabs(feedback_value(10.0, 10.0, RewardDirection::Above, 1.0) - 0.0) < 1e-12);
  // Below threshold => 0.
  assert(std::fabs(feedback_value(9.0, 10.0, RewardDirection::Above, 1.0) - 0.0) < 1e-12);
  // Far above threshold => 1.
  assert(std::fabs(feedback_value(100.0, 10.0, RewardDirection::Above, 1.0) - 1.0) < 1e-12);
}

static void test_below_basic() {
  // span=2 => metric=thr-1 => 0.5
  const double v = feedback_value(9.0, 10.0, RewardDirection::Below, 2.0);
  assert(std::fabs(v - 0.5) < 1e-12);
}

static void test_invalid_inputs() {
  // Non-finite -> 0.
  assert(feedback_value(std::numeric_limits<double>::quiet_NaN(), 1.0, RewardDirection::Above, 1.0) == 0.0);
  assert(feedback_value(1.0, std::numeric_limits<double>::infinity(), RewardDirection::Above, 1.0) == 0.0);

  // Invalid span treated as 1.0.
  assert(std::fabs(feedback_value(11.0, 10.0, RewardDirection::Above, 0.0) - 1.0) < 1e-12);
}

int main() {
  test_above_basic();
  test_above_clamp();
  test_below_basic();
  test_invalid_inputs();
  std::cout << "test_feedback_value: ok\n";
  return 0;
}
