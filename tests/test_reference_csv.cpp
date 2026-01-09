#include "qeeg/bandpower.hpp"

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

  // 1) Semicolon-delimited + quoted channel names containing commas.
  const std::string path1 = "tmp_reference_semicolon.csv";
  {
    std::ofstream out(path1);
    out << "channel;band;mean;std\n";
    out << "\"Ch,1\";alpha;3.5;0.5\n";
    out << "Pz;beta;2.0;1.0\n";
  }

  {
    ReferenceStats ref = load_reference_csv(path1);
    assert(ref.mean.size() == 2);
    assert(ref.stdev.size() == 2);

    // Keys are band|channel, both lowercased.
    assert(ref.mean.count("alpha|ch,1") == 1);
    assert(ref.stdev.count("alpha|ch,1") == 1);
    assert(approx(ref.mean.at("alpha|ch,1"), 3.5));
    assert(approx(ref.stdev.at("alpha|ch,1"), 0.5));

    assert(ref.mean.count("beta|pz") == 1);
    assert(ref.stdev.count("beta|pz") == 1);
    assert(approx(ref.mean.at("beta|pz"), 2.0));
    assert(approx(ref.stdev.at("beta|pz"), 1.0));
  }

  std::remove(path1.c_str());

  // 2) Comma-delimited with comments + extra columns (ignored).
  const std::string path2 = "tmp_reference_extra_cols.csv";
  {
    std::ofstream out(path2);
    out << "# comment\n";
    out << "channel,band,mean,std,n\n";
    out << "Fz,alpha,1.25,0.25,10\n";
  }

  {
    ReferenceStats ref = load_reference_csv(path2);
    assert(ref.mean.size() == 1);
    assert(ref.stdev.size() == 1);
    assert(ref.mean.count("alpha|fz") == 1);
    assert(approx(ref.mean.at("alpha|fz"), 1.25));
    assert(approx(ref.stdev.at("alpha|fz"), 0.25));
  }

  std::remove(path2.c_str());

  // 3) Comment metadata parsing (written by qeeg_reference_cli).
  const std::string path3 = "tmp_reference_meta.csv";
  {
    std::ofstream out(path3);
    out << "# qeeg_reference_cli\n";
    out << "# n_files=3\n";
    out << "# log10_power=1\n";
    out << "# relative_power=1\n";
    out << "# relative_fmin_hz=1\n";
    out << "# relative_fmax_hz=45\n";
    out << "# robust=0\n";
    out << "channel,band,mean,std\n";
    out << "Cz,alpha,1.0,0.1\n";
  }

  {
    ReferenceStats ref = load_reference_csv(path3);
    assert(ref.mean.size() == 1);
    assert(ref.stdev.size() == 1);

    assert(ref.meta_n_files_present);
    assert(ref.meta_n_files == 3);

    assert(ref.meta_log10_power_present);
    assert(ref.meta_log10_power == true);

    assert(ref.meta_relative_power_present);
    assert(ref.meta_relative_power == true);
    assert(ref.meta_relative_fmin_hz_present);
    assert(approx(ref.meta_relative_fmin_hz, 1.0));
    assert(ref.meta_relative_fmax_hz_present);
    assert(approx(ref.meta_relative_fmax_hz, 45.0));

    assert(ref.meta_robust_present);
    assert(ref.meta_robust == false);
  }

  std::remove(path3.c_str());

  std::cout << "test_reference_csv OK\n";
  return 0;
}
