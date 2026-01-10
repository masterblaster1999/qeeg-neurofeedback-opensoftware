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

  // 5) Semicolon-delimited + European decimal comma.
  // This is a very common export format in locales where ',' is the decimal
  // separator and ';' is used as the delimiter.
  const std::string path5 = "tmp_decimal_comma.csv";
  {
    std::ofstream out(path5);
    out << "time;C1;C2\n";
    out << "0,000;1,0;2,0\n";
    out << "0,004;1,1;2,1\n";
    out << "0,008;1,2;2,2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path5);
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

  std::remove(path5.c_str());

  // 6) German-style thousands dot + decimal comma.
  // Example: "1.234,5" should parse as 1234.5
  const std::string path6 = "tmp_thousands_dot_decimal_comma.csv";
  {
    std::ofstream out(path6);
    out << "time;C1\n";
    out << "0,000;1.234,5\n";
    out << "0,004;1.234,6\n";
    out << "0,008;1.234,7\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path6);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][0] - 1234.5f) < 1e-6f);
    assert(std::fabs(rec.data[0][2] - 1234.7f) < 1e-6f);
  }

  std::remove(path6.c_str());

  // 7) BioTrace+ style metadata lines before the actual header.
  // Some ASCII exporters prepend one or more free-form text lines.
  const std::string path7 = "tmp_biotrace_metadata_lines.txt";
  {
    std::ofstream out(path7);
    out << "BioTrace+ ASCII Export\n";
    out << "Client: TEST\n";
    out << "time_ms;C1;C2\n";
    out << "0;1;2\n";
    out << "4;1.1;2.1\n";
    out << "8;1.2;2.2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path7);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][2] - 1.2f) < 1e-6f);
    assert(std::fabs(rec.data[1][0] - 2.0f) < 1e-6f);
  }

  std::remove(path7.c_str());

  // 8) Marker/event columns: treat common marker column names as an event stream.
  // This is useful for ASCII exports where event markers are stored as a
  // dedicated column (e.g., "Marker" with integer codes).
  const std::string path8 = "tmp_marker_column.csv";
  {
    std::ofstream out(path8);
    out << "time_ms;C1;Marker;C2\n";
    out << "0;1;0;2\n";
    out << "4;1.1;0;2.1\n";
    out << "8;1.2;5;2.2\n";
    out << "12;1.3;5;2.3\n";
    out << "16;1.4;0;2.4\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path8);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 5);
    assert(rec.data[1].size() == 5);
    assert(std::fabs(rec.data[0][3] - 1.3f) < 1e-6f);
    assert(std::fabs(rec.data[1][4] - 2.4f) < 1e-6f);

    // Marker code 5 active for 2 samples starting at sample index 2.
    assert(rec.events.size() == 1);
    assert(approx(rec.events[0].onset_sec, 2.0 / 250.0));
    assert(approx(rec.events[0].duration_sec, 2.0 / 250.0));
    assert(rec.events[0].text == "5");
  }

  std::remove(path8.c_str());

  // 9) Marker column with string labels.
  const std::string path9 = "tmp_marker_string.csv";
  {
    std::ofstream out(path9);
    out << "time,C1,event\n";
    out << "0.000,1,\n";
    out << "0.004,2,Start\n";
    out << "0.008,3,Start\n";
    out << "0.012,4,\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path9);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 4);

    assert(rec.events.size() == 1);
    assert(approx(rec.events[0].onset_sec, 1.0 / 250.0));
    assert(approx(rec.events[0].duration_sec, 2.0 / 250.0));
    assert(rec.events[0].text == "Start");
  }

  std::remove(path9.c_str());

  std::cout << "test_csv_reader OK\n";
  return 0;
}
