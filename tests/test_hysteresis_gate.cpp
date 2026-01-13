#include "qeeg/hysteresis_gate.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <limits>

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  using namespace qeeg;

  // Zero hysteresis should match is_reward strict semantics.
  {
    HysteresisGate g(0.0, RewardDirection::Above);
    expect(g.update(2.0, 1.0) == true, "zero hyst above: 2 > 1");
    expect(g.update(1.0, 1.0) == false, "zero hyst above: strict >");
  }
  {
    HysteresisGate g(0.0, RewardDirection::Below);
    expect(g.update(0.5, 1.0) == true, "zero hyst below: 0.5 < 1");
    expect(g.update(1.0, 1.0) == false, "zero hyst below: strict <");
  }

  // Above: ON at thr+h, OFF at thr-h.
  {
    HysteresisGate g(0.5, RewardDirection::Above);
    expect(!g.state(), "initial state false");
    expect(g.update(1.2, 1.0) == false, "above: not yet on (1.2 <= 1.5)");
    expect(g.update(1.6, 1.0) == true, "above: turns on (1.6 > 1.5)");
    expect(g.update(1.4, 1.0) == true, "above: stays on (1.4 >= 0.5)");
    expect(g.update(0.4, 1.0) == false, "above: turns off (0.4 < 0.5)");
  }

  // Below: ON at thr-h, OFF at thr+h.
  {
    HysteresisGate g(0.5, RewardDirection::Below);
    expect(g.update(0.8, 1.0) == false, "below: not yet on (0.8 >= 0.5)");
    expect(g.update(0.4, 1.0) == true, "below: turns on (0.4 < 0.5)");
    expect(g.update(0.6, 1.0) == true, "below: stays on (0.6 <= 1.5)");
    expect(g.update(1.6, 1.0) == false, "below: turns off (1.6 > 1.5)");
  }

  // Non-finite inputs force OFF.
  {
    HysteresisGate g(0.5, RewardDirection::Above);
    expect(g.update(2.0, 1.0) == true, "setup on");
    expect(g.update(std::numeric_limits<double>::quiet_NaN(), 1.0) == false, "NaN value forces off");
  }

  std::cerr << "test_hysteresis_gate OK\n";
  return 0;
}
