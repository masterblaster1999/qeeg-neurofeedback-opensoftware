#include "qeeg/bandpower.hpp"

#include "test_support.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // 1) Direct numeric IAF value -> generate individualized bands.
  {
    const std::vector<BandDefinition> bands = parse_band_spec("iaf=10");
    assert(bands.size() == 5);

    assert(bands[0].name == "delta");
    assert(approx(bands[0].fmin_hz, 0.5));
    assert(approx(bands[0].fmax_hz, 4.0));

    assert(bands[1].name == "theta");
    assert(approx(bands[1].fmin_hz, 4.0));
    assert(approx(bands[1].fmax_hz, 8.0));

    assert(bands[2].name == "alpha");
    assert(approx(bands[2].fmin_hz, 8.0));
    assert(approx(bands[2].fmax_hz, 12.0));

    assert(bands[3].name == "beta");
    assert(approx(bands[3].fmin_hz, 12.0));
    assert(approx(bands[3].fmax_hz, 30.0));

    assert(bands[4].name == "gamma");
    assert(approx(bands[4].fmin_hz, 30.0));
    assert(approx(bands[4].fmax_hz, 80.0));
  }

  // 2) iaf:DIR prefers iaf_band_spec.txt if present.
  std::filesystem::path dir_spec = "tmp_iaf_dir_spec";
  std::filesystem::create_directories(dir_spec);
  {
    std::ofstream f(dir_spec / "iaf_band_spec.txt");
    assert(!!f);
    f << "delta:0.5-4,theta:4-7,alpha:8-12,beta:13-30,gamma:30-80\n";
  }
  {
    const std::vector<BandDefinition> bands = parse_band_spec(std::string("iaf:") + dir_spec.string());
    assert(bands.size() == 5);

    // Ensure we actually consumed the file (beta starts at 13 here).
    assert(bands[3].name == "beta");
    assert(approx(bands[3].fmin_hz, 13.0));
    assert(approx(bands[3].fmax_hz, 30.0));
  }

  // 3) iaf:DIR falls back to iaf_summary.txt -> generates individualized bands.
  std::filesystem::path dir_summary = "tmp_iaf_dir_summary";
  std::filesystem::create_directories(dir_summary);
  {
    std::ofstream f(dir_summary / "iaf_summary.txt");
    assert(!!f);
    f << "aggregate_iaf_hz=10\n";
  }
  {
    const std::vector<BandDefinition> bands = parse_band_spec(std::string("iaf:") + dir_summary.string());
    assert(bands.size() == 5);

    // Generated bands: beta starts at iaf+2 => 12.
    assert(bands[3].name == "beta");
    assert(approx(bands[3].fmin_hz, 12.0));
    assert(approx(bands[3].fmax_hz, 30.0));
  }

  // 4) iaf:FILE with a single numeric line.
  std::filesystem::path iaf_file = "tmp_iaf_value.txt";
  {
    std::ofstream f(iaf_file);
    assert(!!f);
    f << "10\n";
  }
  {
    const std::vector<BandDefinition> bands = parse_band_spec(std::string("iaf:") + iaf_file.string());
    assert(bands.size() == 5);
    assert(bands[2].name == "alpha");
    assert(approx(bands[2].fmin_hz, 8.0));
    assert(approx(bands[2].fmax_hz, 12.0));
  }

  std::filesystem::remove_all(dir_spec);
  std::filesystem::remove_all(dir_summary);
  std::filesystem::remove(iaf_file);

  std::cout << "test_band_spec_iaf OK\n";
  return 0;
}
