#include "qeeg/montage.hpp"

#include "test_support.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Built-in montage should support both legacy (T3/T4/T5/T6) and modern
  // (T7/T8/P7/P8) 10-20 labels.
  const Montage builtin = Montage::builtin_standard_1020_19();

  Vec2 p3, p7;
  assert(builtin.has("T3"));
  assert(builtin.has("T7"));
  assert(builtin.get("T3", &p3));
  assert(builtin.get("T7", &p7));
  assert(approx(p3.x, p7.x));
  assert(approx(p3.y, p7.y));

  Vec2 p4, p8;
  assert(builtin.has("T4"));
  assert(builtin.has("T8"));
  assert(builtin.get("T4", &p4));
  assert(builtin.get("T8", &p8));
  assert(approx(p4.x, p8.x));
  assert(approx(p4.y, p8.y));

  Vec2 p5, pp7;
  assert(builtin.has("T5"));
  assert(builtin.has("P7"));
  assert(builtin.get("T5", &p5));
  assert(builtin.get("P7", &pp7));
  assert(approx(p5.x, pp7.x));
  assert(approx(p5.y, pp7.y));

  Vec2 p6, pp8;
  assert(builtin.has("T6"));
  assert(builtin.has("P8"));
  assert(builtin.get("T6", &p6));
  assert(builtin.get("P8", &pp8));
  assert(approx(p6.x, pp8.x));
  assert(approx(p6.y, pp8.y));

  // Common reference suffixes should not prevent montage matching.
  Vec2 pref;
  assert(builtin.has("F3-REF"));
  assert(builtin.get("F3-REF", &pref));

  // Many EDF recordings include a leading modality token like "EEG".
  // That should also match the montage.
  Vec2 pref_eeg;
  assert(builtin.has("EEG F3-REF"));
  assert(builtin.get("EEG F3-REF", &pref_eeg));
  assert(approx(pref_eeg.x, pref.x));
  assert(approx(pref_eeg.y, pref.y));

  // Custom montage files should also be alias-tolerant.
  const std::string path = "tmp_montage_alias.csv";
  {
    std::ofstream out(path);
    out << "name,x,y\n";
    out << "T3,0.1,0.2\n";
    out << "T4,0.3,0.4\n";
  }

  const Montage m = Montage::load_csv(path);
  Vec2 pt7, pt8;
  assert(m.has("T7"));
  assert(m.get("T7", &pt7));
  assert(approx(pt7.x, 0.1));
  assert(approx(pt7.y, 0.2));

  assert(m.has("T8"));
  assert(m.get("T8", &pt8));
  assert(approx(pt8.x, 0.3));
  assert(approx(pt8.y, 0.4));

  std::remove(path.c_str());

  std::cout << "test_montage_aliases OK\n";
  return 0;
}
