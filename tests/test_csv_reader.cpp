#include "qeeg/csv_reader.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // CSVReader numeric parsing should be locale-independent.
  // Best-effort: switch LC_NUMERIC to a locale that uses a decimal comma if available,
  // then run the full test suite under that locale.
  const char* prev_num_locale_c = std::setlocale(LC_NUMERIC, nullptr);
  const std::string prev_num_locale = prev_num_locale_c ? prev_num_locale_c : "";

  struct LocaleRestore {
    std::string prev;
    ~LocaleRestore() {
      if (!prev.empty()) std::setlocale(LC_NUMERIC, prev.c_str());
    }
  } restore{prev_num_locale};

  const std::vector<const char*> candidates = {
      "de_DE.UTF-8", "de_DE.utf8", "de_DE",
      "fr_FR.UTF-8", "fr_FR.utf8", "fr_FR",
      "es_ES.UTF-8", "es_ES.utf8", "es_ES",
      "it_IT.UTF-8", "it_IT.utf8", "it_IT",
  };

  const char* chosen = nullptr;
  for (const char* loc : candidates) {
    if (std::setlocale(LC_NUMERIC, loc) != nullptr) {
      chosen = loc;
      break;
    }
  }

  if (chosen) {
    std::cout << "test_csv_reader: LC_NUMERIC set to " << chosen << " (was "
              << (prev_num_locale.empty() ? "(null)" : prev_num_locale) << "\n";
  } else {
    std::cout << "test_csv_reader: LC_NUMERIC comma-decimal locale not available; "
              << "continuing with default locale\n";
  }

  // 0) UTF-8 filenames should work (important on Windows).
  {
    const std::filesystem::path dir = std::filesystem::u8path(u8"tmp_\xC2\xB5_csv_reader");
    const std::filesystem::path file = dir / std::filesystem::u8path(u8"time_\xC2\xB5.csv");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    assert(!ec);

    {
      std::ofstream out(file);
      out << "time,C1\n";
      out << "0.000,1\n";
      out << "0.004,2\n";
      out << "0.008,3\n";
    }

    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(file.u8string());
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][2] - 3.0f) < 1e-6f);

    std::filesystem::remove_all(dir, ec);
  }

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

    // Marker code 5 starts at sample index 2.
    assert(rec.events.size() == 1);
    assert(approx(rec.events[0].onset_sec, 2.0 / 250.0));
    assert(approx(rec.events[0].duration_sec, 0.0));
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
    assert(approx(rec.events[0].duration_sec, 0.0));
    assert(rec.events[0].text == "Start");
  }

  std::remove(path9.c_str());



  // 10) Allow missing trailing empty columns (common when the last column is an event/marker stream).
  // Many exporters omit the trailing delimiter when the last cell is empty.
  const std::string path10 = "tmp_marker_trailing_missing.csv";
  {
    std::ofstream out(path10);
    out << "time,C1,C2,event\n";
    out << "0.000,1,2\n";            // missing trailing event cell
    out << "0.004,2,3,Start\n";
    out << "0.008,3,4,Start\n";
    out << "0.012,4,5\n";            // missing trailing event cell
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path10);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 4);
    assert(rec.data[1].size() == 4);

    // "Start" begins at sample 1.
    assert(rec.events.size() == 1);
    assert(approx(rec.events[0].onset_sec, 1.0 / 250.0));
    assert(approx(rec.events[0].duration_sec, 0.0));
    assert(rec.events[0].text == "Start");
  }

  std::remove(path10.c_str());

  // 11) Allow extra trailing delimiters that produce empty columns.
  const std::string path11 = "tmp_extra_trailing_delims.csv";
  {
    std::ofstream out(path11);
    out << "time,C1,event\n";
    out << "0.000,1,Start,\n";  // extra trailing empty field
    out << "0.004,2,,\n";       // event empty + extra trailing empty field
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path11);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 2);

    assert(rec.events.size() == 1);
    assert(approx(rec.events[0].onset_sec, 0.0));
    assert(approx(rec.events[0].duration_sec, 0.0));
    assert(rec.events[0].text == "Start");
  }

  std::remove(path11.c_str());

  // 12) BioTrace+ style hh:mm:ss time axis.
  // BioTrace+ can export time in hh:mm:ss (with optional fractional seconds).
  // We should be able to infer fs from a monotonic hh:mm:ss.xxx column.
  const std::string path12 = "tmp_time_hms.csv";
  {
    std::ofstream out(path12);
    out << "time,C1\n";
    out << "00:00:00.000,1\n";
    out << "00:00:00.004,2\n";
    out << "00:00:00.008,3\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path12);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][2] - 3.0f) < 1e-6f);
  }

  std::remove(path12.c_str());

  // 13) Missing numeric cells: forward-fill by default.
  // This occurs in some BioTrace+ ASCII exports if "repeat slower channels" is disabled.
  const std::string path13 = "tmp_missing_cells_forward_fill.csv";
  {
    std::ofstream out(path13);
    out << "time_ms;EEG;Temp\n";
    out << "0;1;20\n";
    out << "4;2;\n";   // Temp missing
    out << "8;3;21\n";
    out << "12;4;\n";  // Temp missing
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path13);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "EEG");
    assert(rec.channel_names[1] == "Temp");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 4);
    assert(rec.data[1].size() == 4);
    assert(std::fabs(rec.data[1][0] - 20.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][1] - 20.0f) < 1e-6f); // forward-filled
    assert(std::fabs(rec.data[1][2] - 21.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][3] - 21.0f) < 1e-6f); // forward-filled
  }

  std::remove(path13.c_str());

  // 14) NeXus/BioTrace+ style: sample index + time column.
  // Many exports include an explicit sample counter column before the time axis.
  const std::string path14 = "tmp_sample_and_time.csv";
  {
    std::ofstream out(path14);
    out << "BioTrace+ ASCII Export;TEST\n";
    out << "Client;Example\n";
    out << "Sample;Time;C1;C2\n";
    out << "0;00:00:00.000;1;2\n";
    out << "1;00:00:00.004;1.1;2.1\n";
    out << "2;00:00:00.008;1.2;2.2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer from Time
    EEGRecording rec = r.read(path14);
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

  std::remove(path14.c_str());

  // 15) Sample index only: should be ignored as a data channel when fs is provided.
  const std::string path15 = "tmp_sample_index_only.csv";
  {
    std::ofstream out(path15);
    out << "sample,C1,C2\n";
    out << "0,1,2\n";
    out << "1,3,4\n";
  }

  {
    CSVReader r(/*fs_hz=*/250.0); // provided
    EEGRecording rec = r.read(path15);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 2);
    assert(rec.data[1].size() == 2);
    assert(std::fabs(rec.data[0][1] - 3.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][0] - 2.0f) < 1e-6f);
  }

  std::remove(path15.c_str());


  // 16) Unit suffixes in channel headers: strip recognized unit tokens and scale to microvolts.
  // BioTrace+/NeXus ASCII exports often annotate columns with units like "(uV)" or "(mV)".
  const std::string path16 = "tmp_units_in_header.csv";
  {
    std::ofstream out(path16);
    out << "time_ms;EEG1 (mV);Cz [uV];Pz (\xC2\xB5V);EEG2_uV\n";
    out << "0;0.001;10;100;20\n";
    out << "4;0.002;11;101;21\n";
    out << "8;0.003;12;102;22\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path16);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 4);
    assert(rec.channel_names[0] == "EEG1");
    assert(rec.channel_names[1] == "Cz");
    assert(rec.channel_names[2] == "Pz");
    assert(rec.channel_names[3] == "EEG2");
    assert(rec.data.size() == 4);
    assert(rec.data[0].size() == 3);

    // EEG1 is labeled as mV in the header -> scale to microvolts (uV) internally.
    assert(std::fabs(rec.data[0][0] - 1.0f) < 1e-6f); // 0.001 mV -> 1 uV
    assert(std::fabs(rec.data[0][2] - 3.0f) < 1e-6f); // 0.003 mV -> 3 uV

    // Other channels are already uV.
    assert(std::fabs(rec.data[2][1] - 101.0f) < 1e-6f);
    assert(std::fabs(rec.data[3][0] - 20.0f) < 1e-6f);
  }

  std::remove(path16.c_str());

  // 17) BioTrace+ "Include segments" export: segment column should be treated as an event stream.
  // The BioTrace+ user manual describes an option to include segments in ASCII exports.
  // When present, a "Segment" column typically contains labels that are constant over a range.
  const std::string path17 = "tmp_segment_column.csv";
  {
    std::ofstream out(path17);
    out << "time_ms;C1;Segment;C2\n";
    out << "0;1;Baseline;2\n";
    out << "4;1.1;Baseline;2.1\n";
    out << "8;1.2;Train;2.2\n";
    out << "12;1.3;Train;2.3\n";
    out << "16;1.4;;2.4\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer
    EEGRecording rec = r.read(path17);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");

    // Segment labels emitted as events.
    assert(rec.events.size() == 2);
    assert(rec.events[0].text == "Baseline");
    assert(approx(rec.events[0].onset_sec, 0.0));
    assert(approx(rec.events[0].duration_sec, 2.0 / 250.0));

    assert(rec.events[1].text == "Train");
    assert(approx(rec.events[1].onset_sec, 2.0 / 250.0));
    assert(approx(rec.events[1].duration_sec, 2.0 / 250.0));
  }

  std::remove(path17.c_str());


  // 18) Headerless BioTrace+ style: hh:mm:ss axis + footer marker line.
  // Some exports omit the header row entirely, and add a footer like
  // "<end of exported RAW data>".
  const std::string path18 = "tmp_biotrace_headerless_hms.txt";
  {
    std::ofstream out(path18);
    out << "00:00:00\t2.276\n";
    out << "00:00:00\t2.476\n";
    out << "00:00:01\t2.482\n";
    out << "<end of exported RAW data>\n";
  }

  {
    CSVReader r(/*fs_hz=*/32.0); // provided (time column is coarse)
    EEGRecording rec = r.read(path18);
    assert(approx(rec.fs_hz, 32.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Ch1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][0] - 2.276f) < 1e-6f);
    assert(std::fabs(rec.data[0][2] - 2.482f) < 1e-6f);
  }

  std::remove(path18.c_str());

  // 19) Headerless sample index + value, with footer marker.
  // Many BioTrace+ files represent "time" as a sample counter (0,1,2,...).
  const std::string path19 = "tmp_biotrace_headerless_sample.txt";
  {
    std::ofstream out(path19);
    out << "0\t2.276\n";
    out << "1\t2.476\n";
    out << "2\t2.482\n";
    out << "<end of exported RAW data>\n";
  }

  {
    CSVReader r(/*fs_hz=*/32.0); // provided
    EEGRecording rec = r.read(path19);
    assert(approx(rec.fs_hz, 32.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Ch1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][1] - 2.476f) < 1e-6f);
  }

  std::remove(path19.c_str());

  // 20) Headerless segment column: treat repeating labels as a segment stream.
  const std::string path20 = "tmp_biotrace_headerless_segment.txt";
  {
    std::ofstream out(path20);
    out << "00:00:00\t1.0\t\tBaseline\n";
    out << "00:00:00\t2.0\t\tBaseline\n";
    out << "00:00:00\t3.0\t\tTrain\n";
    out << "00:00:00\t4.0\t\tTrain\n";
    out << "00:00:00\t5.0\t\t\n";
    out << "<Unbearbeitete Daten exportiert>\n";
  }

  {
    CSVReader r(/*fs_hz=*/4.0); // provided (time axis is not strictly increasing here)
    EEGRecording rec = r.read(path20);
    assert(approx(rec.fs_hz, 4.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Ch1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 5);

    assert(rec.events.size() == 2);
    assert(rec.events[0].text == "Baseline");
    assert(approx(rec.events[0].onset_sec, 0.0));
    assert(approx(rec.events[0].duration_sec, 2.0 / 4.0));

    assert(rec.events[1].text == "Train");
    assert(approx(rec.events[1].onset_sec, 2.0 / 4.0));
    assert(approx(rec.events[1].duration_sec, 2.0 / 4.0));
  }

  std::remove(path20.c_str());

  // 21) German BioTrace+/NeXus style: sample index + "Zeit" time column.
  // Some installations export headers in German (Beispiele/Zeit) and may use a comma
  // as the decimal separator in the hh:mm:ss,ms time representation.
  const std::string path21 = "tmp_biotrace_de_sample_time.csv";
  {
    std::ofstream out(path21);
    out << "BioTrace+ ASCII Export;TEST\n";
    out << "Beispiele;Zeit;C1;C2\n";
    out << "0;00:00:00,000;1;2\n";
    out << "1;00:00:00,004;1.1;2.1\n";
    out << "2;00:00:00,008;1.2;2.2\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer from Zeit
    EEGRecording rec = r.read(path21);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[0][2] - 1.2f) < 1e-6f);
  }

  std::remove(path21.c_str());

  // 22) Metadata sampling rate line: allow sample-index files to be read without --fs.
  // BioTrace+/NeXus exports sometimes include a "Sample Rate" metadata row.
  const std::string path22 = "tmp_meta_samplerate_sample_axis.csv";
  {
    std::ofstream out(path22);
    out << "Sample Rate;250 Hz\n";
    out << "sample;C1;C2\n";
    out << "0;1;2\n";
    out << "1;3;4\n";
    out << "2;5;6\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0); // infer from metadata
    EEGRecording rec = r.read(path22);
    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(std::fabs(rec.data[1][2] - 6.0f) < 1e-6f);
  }

  std::remove(path22.c_str());



  // 23) UTF-16LE BOM ("Unicode" text) export.
  // Some BioTrace+/NeXus ASCII exports on Windows may be saved as UTF-16.
  const std::string path23 = "tmp_utf16le_bom.tsv";
  {
    std::ofstream out(path23, std::ios::binary);
    assert(out && "failed to open temp file for writing");

    // UTF-16LE BOM
    out.put(static_cast<char>(0xFF));
    out.put(static_cast<char>(0xFE));

    auto write_u16le_ascii = [&](const std::string& s) {
      for (unsigned char c : s) {
        out.put(static_cast<char>(c));
        out.put(static_cast<char>(0x00));
      }
    };

    write_u16le_ascii("time_ms\tC1\tC2\r\n");
    write_u16le_ascii("0\t1\t2\r\n");
    write_u16le_ascii("4\t3\t4\r\n");
    write_u16le_ascii("8\t5\t6\r\n");
  }

  {
    CSVReader r(/*fs_hz=*/0.0);
    EEGRecording rec = r.read(path23);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][2] - 5.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][2] - 6.0f) < 1e-6f);
  }
  std::remove(path23.c_str());



  // 23b) UTF-16LE without BOM.
  // Some Windows tools save UTF-16 text without writing a BOM.
  const std::string path23b = "tmp_utf16le_nobom.tsv";
  {
    std::ofstream out(path23b, std::ios::binary);
    assert(out && "failed to open temp file for writing");

    auto write_u16le_ascii = [&](const std::string& s) {
      for (unsigned char c : s) {
        out.put(static_cast<char>(c));
        out.put(static_cast<char>(0x00));
      }
    };

    write_u16le_ascii("time_ms\tC1\tC2\r\n");
    write_u16le_ascii("0\t1\t2\r\n");
    write_u16le_ascii("4\t3\t4\r\n");
    write_u16le_ascii("8\t5\t6\r\n");
  }

  {
    CSVReader r(/*fs_hz=*/0.0);
    EEGRecording rec = r.read(path23b);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][2] - 5.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][2] - 6.0f) < 1e-6f);
  }
  std::remove(path23b.c_str());



  // 23d) UTF-16LE without BOM, very small file.
  // Ensure the UTF-16 sniffing does not require a large sample.
  const std::string path23d = "tmp_utf16le_nobom_small.tsv";
  {
    std::ofstream out(path23d, std::ios::binary);
    assert(out && "failed to open temp file for writing");

    auto write_u16le_ascii = [&](const std::string& s) {
      for (unsigned char c : s) {
        out.put(static_cast<char>(c));
        out.put(static_cast<char>(0x00));
      }
    };

    write_u16le_ascii("sample\tC1\r\n");
    write_u16le_ascii("0\t1\r\n");
    write_u16le_ascii("1\t2\r\n");
  }

  {
    CSVReader r(/*fs_hz=*/250.0);
    EEGRecording rec = r.read(path23d);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "C1");
    assert(rec.data.size() == 1);
    assert(rec.data[0].size() == 2);
    assert(std::fabs(rec.data[0][0] - 1.0f) < 1e-6f);
    assert(std::fabs(rec.data[0][1] - 2.0f) < 1e-6f);
  }
  std::remove(path23d.c_str());



  // 23c) UTF-16BE without BOM.
  const std::string path23c = "tmp_utf16be_nobom.tsv";
  {
    std::ofstream out(path23c, std::ios::binary);
    assert(out && "failed to open temp file for writing");

    auto write_u16be_ascii = [&](const std::string& s) {
      for (unsigned char c : s) {
        out.put(static_cast<char>(0x00));
        out.put(static_cast<char>(c));
      }
    };

    write_u16be_ascii("time_ms\tC1\tC2\r\n");
    write_u16be_ascii("0\t1\t2\r\n");
    write_u16be_ascii("4\t3\t4\r\n");
    write_u16be_ascii("8\t5\t6\r\n");
  }

  {
    CSVReader r(/*fs_hz=*/0.0);
    EEGRecording rec = r.read(path23c);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "C1");
    assert(rec.channel_names[1] == "C2");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);
    assert(rec.data[1].size() == 3);
    assert(std::fabs(rec.data[0][2] - 5.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][2] - 6.0f) < 1e-6f);
  }
  std::remove(path23c.c_str());


  // 24) Windows-1252 / Latin-1 micro sign (0xB5) in unit suffixes.
  // Some BioTrace+/NeXus ASCII exports may be saved with a legacy single-byte encoding
  // where the micro sign is written as 0xB5 instead of UTF-8.
  // Ensure we still interpret "(µV)" as microvolts, not volts.
  const std::string path24 = "tmp_units_in_header_cp1252.csv";
  {
    std::ofstream out(path24, std::ios::binary);
    out << "time_ms;Pz (\xB5V);EEG1 (mV)\n";
    out << "0;10;0.001\n";
    out << "4;11;0.002\n";
    out << "8;12;0.003\n";
  }

  {
    CSVReader r(/*fs_hz=*/0.0);
    EEGRecording rec = r.read(path24);

    assert(approx(rec.fs_hz, 250.0));
    assert(rec.channel_names.size() == 2);
    assert(rec.channel_names[0] == "Pz");
    assert(rec.channel_names[1] == "EEG1");
    assert(rec.data.size() == 2);
    assert(rec.data[0].size() == 3);

    // Pz is already µV -> values should not be scaled by 1e6.
    assert(std::fabs(rec.data[0][0] - 10.0f) < 1e-6f);
    assert(std::fabs(rec.data[0][2] - 12.0f) < 1e-6f);

    // EEG1 is mV -> scaled to µV.
    assert(std::fabs(rec.data[1][0] - 1.0f) < 1e-6f);
    assert(std::fabs(rec.data[1][2] - 3.0f) < 1e-6f);
  }

  std::remove(path24.c_str());


// 25) ZIP container sniffing: provide a helpful error for ZIP-like containers.
// Some BioTrace+/NeXus session containers are ZIP-like archives that embed an EDF/BDF/ASCII export.
const std::string path25 = "tmp_zip_container.m2k";
{
  std::ofstream out(path25, std::ios::binary);
  out.write("PK\x03\x04", 4);
  out << "dummy";
}

{
  CSVReader r(/*fs_hz=*/0.0);
  bool threw = false;
  try {
    (void)r.read(path25);
  } catch (const std::exception& e) {
    threw = true;
    const std::string msg = e.what();
    assert(msg.find("ZIP") != std::string::npos || msg.find("zip") != std::string::npos);
    // The error should suggest the extractor script with the correct flag.
    assert(msg.find("biotrace_extract_container.py") != std::string::npos);
    assert(msg.find("--input") != std::string::npos);
  }
  assert(threw);
}

std::remove(path25.c_str());

// 26) Binary blob sniffing: NUL/control bytes should yield a helpful error.
const std::string path26 = "tmp_binary_blob.m2k";
{
  std::ofstream out(path26, std::ios::binary);
  const unsigned char bytes[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0x00, 0x10, 0x11};
  out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

{
  CSVReader r(/*fs_hz=*/0.0);
  bool threw = false;
  try {
    (void)r.read(path26);
  } catch (const std::exception& e) {
    threw = true;
    const std::string msg = e.what();
    assert(msg.find("binary") != std::string::npos || msg.find("Binary") != std::string::npos);
  }
  assert(threw);
}

std::remove(path26.c_str());

  std::cout << "test_csv_reader OK\n";
  return 0;
}
