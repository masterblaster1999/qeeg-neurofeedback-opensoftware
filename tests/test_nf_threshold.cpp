#include "qeeg/nf_threshold.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  using namespace qeeg;

  // Parsing (case-insensitive + a few aliases)
  expect(parse_reward_direction("above") == RewardDirection::Above, "parse above");
  expect(parse_reward_direction("Below") == RewardDirection::Below, "parse below");

  bool threw = false;
  try {
    (void)parse_reward_direction("sideways");
  } catch (...) {
    threw = true;
  }
  expect(threw, "invalid reward direction should throw");

  // Reward predicate
  expect(is_reward(2.0, 1.0, RewardDirection::Above), "above reward");
  expect(!is_reward(1.0, 1.0, RewardDirection::Above), "above strict >");
  expect(is_reward(0.5, 1.0, RewardDirection::Below), "below reward");
  expect(!is_reward(1.0, 1.0, RewardDirection::Below), "below strict <");

  // Adaptation sign: when rr > target, Above should increase threshold, Below should decrease.
  {
    const double thr = 10.0;
    const double rr = 0.8;
    const double target = 0.6;
    const double eta = 0.1;
    const double thr_above = adapt_threshold(thr, rr, target, eta, RewardDirection::Above);
    const double thr_below = adapt_threshold(thr, rr, target, eta, RewardDirection::Below);
    expect(thr_above > thr, "above: rr>target should increase threshold");
    expect(thr_below < thr, "below: rr>target should decrease threshold");
  }

  // When rr < target, Above should decrease threshold, Below should increase.
  {
    const double thr = 10.0;
    const double rr = 0.4;
    const double target = 0.6;
    const double eta = 0.1;
    const double thr_above = adapt_threshold(thr, rr, target, eta, RewardDirection::Above);
    const double thr_below = adapt_threshold(thr, rr, target, eta, RewardDirection::Below);
    expect(thr_above < thr, "above: rr<target should decrease threshold");
    expect(thr_below > thr, "below: rr<target should increase threshold");
  }

  // Avoid getting stuck at zero
  {
    const double thr0 = 0.0;
    const double out = adapt_threshold(thr0, 0.8, 0.6, 0.1, RewardDirection::Above);
    expect(std::isfinite(out) && out != 0.0, "threshold=0 should be nudged away from 0");
  }

  std::cerr << "test_nf_threshold OK\n";
  return 0;
}
