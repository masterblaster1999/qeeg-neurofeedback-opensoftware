#include "qeeg/bandpower.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  const std::string path = "tmp_band_spec.txt";
  {
    std::ofstream f(path);
    assert(!!f);
    f << "# Example band spec (one per line)\n";
    f << "alpha:8-12\n";
    f << "beta:13-30\n";
  }

  const std::vector<BandDefinition> bands = parse_band_spec("@" + path);
  assert(bands.size() == 2);

  assert(bands[0].name == "alpha");
  assert(approx(bands[0].fmin_hz, 8.0));
  assert(approx(bands[0].fmax_hz, 12.0));

  assert(bands[1].name == "beta");
  assert(approx(bands[1].fmin_hz, 13.0));
  assert(approx(bands[1].fmax_hz, 30.0));

  std::remove(path.c_str());

  std::cout << "test_band_spec_file OK\n";
  return 0;
}
