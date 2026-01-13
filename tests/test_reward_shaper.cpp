#include "qeeg/reward_shaper.hpp"

#include <cassert>

using namespace qeeg;

int main() {
  // Dwell: reward should not turn on until dwell time elapses.
  {
    RewardShaper s(/*dwell_seconds=*/0.5, /*refractory_seconds=*/0.0);
    double t = 0.0;
    // Use dt=0.25 (exactly representable) to avoid floating rounding issues.
    t += 0.25;
    assert(s.update(true, 0.25, t, /*freeze=*/false) == false);
    t += 0.25;
    assert(s.update(true, 0.25, t, /*freeze=*/false) == true);
  }

  // Refractory: once reward turns off, it cannot turn on again until refractory passes.
  {
    RewardShaper s(/*dwell_seconds=*/0.0, /*refractory_seconds=*/0.5);
    double t = 0.0;

    // Turn on.
    t += 0.25;
    assert(s.update(true, 0.25, t, false) == true);

    // Keep on.
    t += 0.25;
    assert(s.update(true, 0.25, t, false) == true);

    // Turn off.
    t += 0.25;
    assert(s.update(false, 0.25, t, false) == false);

    // Attempt to re-enable too soon.
    t += 0.25; // since off = 0.25
    assert(s.update(true, 0.25, t, false) == false);

    // After enough time, allow reward again.
    t += 0.25; // since off = 0.5
    assert(s.update(true, 0.25, t, false) == true);
  }

  // Freeze: forces reward off and updates off-time for refractory.
  {
    RewardShaper s(/*dwell_seconds=*/0.0, /*refractory_seconds=*/0.5);
    double t = 0.0;
    t += 0.25;
    assert(s.update(true, 0.25, t, false) == true);
    t += 0.25;
    assert(s.update(true, 0.25, t, true) == false); // freeze
    // Too soon after freeze-off.
    t += 0.25;
    assert(s.update(true, 0.25, t, false) == false);
    // Enough time.
    t += 0.25;
    assert(s.update(true, 0.25, t, false) == true);
  }

  return 0;
}
