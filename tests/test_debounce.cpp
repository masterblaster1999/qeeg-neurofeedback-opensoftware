#include "qeeg/debounce.hpp"

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

  // Identity behavior when counts are 1.
  {
    BoolDebouncer d(1, 1, false);
    expect(!d.update(false), "initial false stays false");
    expect(d.update(true), "turns on immediately");
    expect(!d.update(false), "turns off immediately");
    expect(d.update(true), "turns on again");
  }

  // Debounced ON/OFF switching.
  {
    BoolDebouncer d(2, 2, false);
    // Need 2 consecutive true to switch on.
    expect(!d.update(true), "1st true should not switch on");
    expect(d.update(true), "2nd true should switch on");
    expect(d.update(true), "stays on");

    // Need 2 consecutive false to switch off.
    expect(d.update(false), "1st false should not switch off");
    expect(!d.update(false), "2nd false should switch off");
    expect(!d.update(false), "stays off");
  }

  // Reset behavior.
  {
    BoolDebouncer d(3, 3, true);
    expect(d.state(), "initial true state");
    d.reset(false);
    expect(!d.state(), "reset to false");
    expect(!d.update(true), "needs 3 consecutive trues after reset");
  }

  std::cerr << "test_debounce OK\n";
  return 0;
}
