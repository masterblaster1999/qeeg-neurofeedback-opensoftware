#include "qeeg/montage.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  const Montage m = Montage::builtin_standard_1010_61();

  // Should include a reasonably sized 10-10 set.
  const std::vector<std::string> names = m.channel_names();
  assert(names.size() == 61);

  // Spot-check a few midline points (coordinates are approximate but deterministic).
  Vec2 p;

  assert(m.has("Fpz"));
  assert(m.get("Fpz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, 0.98));

  assert(m.has("Fz"));
  assert(m.get("Fz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, 0.62));

  assert(m.has("FCz"));
  assert(m.get("FCz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, 0.34));

  assert(m.has("Cz"));
  assert(m.get("Cz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, 0.0));

  assert(m.has("CPz"));
  assert(m.get("CPz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, -0.34));

  assert(m.has("Pz"));
  assert(m.get("Pz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, -0.62));

  assert(m.has("POz"));
  assert(m.get("POz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, -0.84));

  assert(m.has("Oz"));
  assert(m.get("Oz", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, -0.98));

  // Legacy 10-20 aliases should still match via normalize_channel_name.
  // (T3/T4/T5/T6 -> T7/T8/P7/P8)
  assert(m.has("T3"));
  assert(m.has("T4"));
  assert(m.has("T5"));
  assert(m.has("T6"));

  // EDF-style labels with prefixes/suffixes should match.
  assert(m.has("EEG Fpz-REF"));
  assert(m.get("EEG Fpz-REF", &p));
  assert(approx(p.x, 0.0));
  assert(approx(p.y, 0.98));

  std::cout << "test_montage_builtin_1010 OK\n";
  return 0;
}
