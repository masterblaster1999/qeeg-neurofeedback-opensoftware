#include "qeeg/montage.hpp"
#include "qeeg/topomap.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

int main() {
  using namespace qeeg;

  // Build a small channel set with a known montage.
  Montage montage = Montage::builtin_standard_1020_19();

  std::vector<std::string> ch = {"Fp1", "Fp2", "Cz", "Pz"};
  std::vector<double> v = {1.0,
                           std::numeric_limits<double>::quiet_NaN(),  // masked
                           2.0,
                           3.0};

  TopomapOptions opt;
  opt.grid_size = 32;
  opt.method = TopomapInterpolation::IDW;

  // Should succeed even with one NaN (we still have >= 3 finite channels).
  Grid2D g = make_topomap(montage, ch, v, opt);

  // Verify there is at least one finite value inside the head.
  bool any_finite = false;
  for (float x : g.values) {
    if (std::isfinite(x)) {
      any_finite = true;
      break;
    }
  }
  assert(any_finite);

  // If we mask too many channels (< 3 finite), we should fail.
  std::vector<double> v2 = {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::quiet_NaN(),
                            2.0,
                            std::numeric_limits<double>::quiet_NaN()};
  bool threw = false;
  try {
    (void)make_topomap(montage, ch, v2, opt);
  } catch (const std::exception&) {
    threw = true;
  }
  assert(threw);

  std::cout << "test_topomap_nan OK\n";
  return 0;
}
