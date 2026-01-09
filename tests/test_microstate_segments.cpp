#include "qeeg/microstates.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace qeeg;

static void expect_true(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "TEST FAIL: " << msg << "\n";
    std::exit(1);
  }
}

static bool approx(double a, double b, double tol = 1e-12) {
  return std::fabs(a - b) <= tol;
}

int main() {
  {
    // Simple three-segment label stream.
    const std::vector<int> labels = {0, 0, 0, 1, 1, 1, 0, 0};
    const std::vector<double> corr = {0.5, 0.7, 0.9, 0.2, 0.4, 0.6, 0.1, 0.3};
    const std::vector<double> gfp = {1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 3.0, 3.0};
    const double fs = 10.0;

    auto segs = microstate_segments(labels, corr, gfp, fs, /*include_undefined=*/false);
    expect_true(segs.size() == 3, "expected 3 segments");

    expect_true(segs[0].label == 0, "seg0 label");
    expect_true(segs[0].start_sample == 0 && segs[0].end_sample == 3, "seg0 sample bounds");
    expect_true(approx(segs[0].start_sec, 0.0), "seg0 start_sec");
    expect_true(approx(segs[0].end_sec, 0.3), "seg0 end_sec");
    expect_true(approx(segs[0].duration_sec, 0.3), "seg0 duration");
    expect_true(approx(segs[0].mean_corr, (0.5 + 0.7 + 0.9) / 3.0), "seg0 mean_corr");
    expect_true(approx(segs[0].mean_gfp, 1.0), "seg0 mean_gfp");

    expect_true(segs[1].label == 1, "seg1 label");
    expect_true(segs[1].start_sample == 3 && segs[1].end_sample == 6, "seg1 sample bounds");
    expect_true(approx(segs[1].duration_sec, 0.3), "seg1 duration");
    expect_true(approx(segs[1].mean_gfp, 2.0), "seg1 mean_gfp");

    expect_true(segs[2].label == 0, "seg2 label");
    expect_true(segs[2].start_sample == 6 && segs[2].end_sample == 8, "seg2 sample bounds");
    expect_true(approx(segs[2].duration_sec, 0.2), "seg2 duration");
    expect_true(approx(segs[2].mean_gfp, 3.0), "seg2 mean_gfp");
  }

  {
    // Verify skipping undefined segments by default.
    const std::vector<int> labels = {0, 0, -1, -1, 1};
    const std::vector<double> corr = {1.0, 1.0, 0.0, 0.0, 0.5};
    const std::vector<double> gfp = {1.0, 1.0, 2.0, 2.0, 3.0};
    const double fs = 1.0;

    auto segs = microstate_segments(labels, corr, gfp, fs, /*include_undefined=*/false);
    expect_true(segs.size() == 2, "expected 2 segments when skipping undefined");
    expect_true(segs[0].label == 0 && segs[0].start_sample == 0 && segs[0].end_sample == 2, "seg0 bounds");
    expect_true(segs[1].label == 1 && segs[1].start_sample == 4 && segs[1].end_sample == 5, "seg1 bounds");

    auto segs2 = microstate_segments(labels, corr, gfp, fs, /*include_undefined=*/true);
    expect_true(segs2.size() == 3, "expected 3 segments when including undefined");
    expect_true(segs2[1].label == -1 && segs2[1].start_sample == 2 && segs2[1].end_sample == 4,
                "undefined segment bounds");
  }

  {
    // Error paths: length mismatch and invalid fs.
    bool threw = false;
    try {
      microstate_segments(std::vector<int>{0}, std::vector<double>{0.1, 0.2}, std::vector<double>{1.0}, 10.0, false);
    } catch (...) {
      threw = true;
    }
    expect_true(threw, "mismatched lengths should throw");

    threw = false;
    try {
      microstate_segments(std::vector<int>{0}, std::vector<double>{0.1}, std::vector<double>{1.0}, 0.0, false);
    } catch (...) {
      threw = true;
    }
    expect_true(threw, "fs_hz <= 0 should throw");
  }

  std::cout << "OK\n";
  return 0;
}
