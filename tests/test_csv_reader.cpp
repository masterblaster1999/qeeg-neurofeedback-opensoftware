#include "qeeg/csv_reader.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // 1) Infer fs from a seconds-based time column.
  const std::string path1 = "tmp_time_seconds.csv";
  {
    std::ofstream out(path1);
    out << "time,C1,C2\n";
    out << "0.000,1.0,2.0\n";
    out << "0.004,1.1,2.1\n";
    out << "0.008,1.2,2.2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path1);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][1] - 1.1f) < 1e-6f);
    assert(std::fabs(rec.data[1][2] - 2.2f) < 1e-6f);
  }

  std::remove(path1.c_str());

  // 2) Infer fs from an ms-based time column + semicolon delimiter, skipping a comment line.
  const std::string path2 = "tmp_time_ms_semicolon.csv";
  {
    std::ofstream out(path2);
    out << "# comment line\n";
    out << "time_ms;C1;C2\n";
    out << "0;1;2\n";
    out << "4;1.1;2.1\n";
    out << "8;1.2;2.2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path2);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][0] - 1.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][1] - 2.1f) < 1e-6f);
  }

  std::remove(path2.c_str());

  // 3) Quoted fields + delimiter detection that ignores commas inside quotes.
  // This is common when channel labels are exported as quoted strings that may
  // include commas.
  const std::string path3 = "tmp_quoted_semicolon.csv";
  {
    std::ofstream out(path3);
    out << "time_ms;\"Ch,1,2\";\"Ch,3,4\"\n";
    out << "0;\"1\";\"2\"\n";
    out << "4;\"1.1\";\"2.1\"\n";
    out << "8;\"1.2\";\"2.2\"\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path3);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "Ch,1,2");
    assert(rec.channel_names[1] == "Ch,3,4");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][2] - 1.2f) < 1e-6f);
    assert(std::fabs(rec.data[1][0] - 2.0f) < 1e-6f);
  }

  std::remove(path3.c_str());

  // 4) UTF-8 BOM at file start (common in some Windows CSV exporters).
  // Ensure BOM does not break "time" column detection.
  const std::string path4 = "tmp_bom_time.csv";
  {
    std::ofstream out(path4, std::ios::binary);
    out << "\xEF\xBB\xBFtime,C1\n";
    out << "0.000,1\n";
    out << "0.004,2\n";
    out << "0.008,3\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path4);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][2] - 3.0f) < 1e-6f);
  }

  std::remove(path4.c_str());

  std::cout << "test_csv_reader OK\n";
  return 0;
}
