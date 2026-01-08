#include "qeeg/montage.hpp"

#include <cassert>
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

  const std::string path = "tmp_montage_semicolon.csv";
  {
    std::ofstream out(path);
    out << "name;x;y\n";
    out << "\"Ch,1\";0.1;0.2\n";
    out << "Fp1;-0.5;0.92\n";
  }

  Montage m = Montage::load_csv(path);

  Vec2 p;
  assert(m.has("Ch,1"));
  assert(m.get("Ch,1", &p));
  assert(approx(p.x, 0.1));
  assert(approx(p.y, 0.2));

  assert(m.has("Fp1"));
  assert(m.get("Fp1", &p));
  assert(approx(p.x, -0.5));
  assert(approx(p.y, 0.92));

  std::remove(path.c_str());

  std::cout << "test_montage_csv OK\n";
  return 0;
}
