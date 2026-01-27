#include "qeeg/csv_io.hpp"

#include "test_support.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using qeeg::AnnotationEvent;
using qeeg::read_events_table;
using qeeg::write_events_csv;

static void expect_near(double a, double b, double eps = 1e-9) {
  const double d = (a > b) ? (a - b) : (b - a);
  assert(d <= eps);
}

int main() {
  namespace fs = std::filesystem;

  const fs::path tmp_dir = fs::temp_directory_path() / "qeeg_test_events_table";
  fs::create_directories(tmp_dir);

  // 1) CSV round-trip (write_events_csv -> read_events_table)
  const fs::path csv_path = tmp_dir / "events.csv";
  const std::vector<AnnotationEvent> events_csv = {
      {1.25, 0.0, "Marker \"A\""},
      {2.0, 0.5, "Segment,with,comma"},
      {3.0, 1.0, "NF:Reward"},
  };
  write_events_csv(csv_path.string(), events_csv);

  const auto loaded_csv = read_events_table(csv_path.string());
  assert(loaded_csv.size() == events_csv.size());
  for (size_t i = 0; i < events_csv.size(); ++i) {
    expect_near(loaded_csv[i].onset_sec, events_csv[i].onset_sec);
    expect_near(loaded_csv[i].duration_sec, events_csv[i].duration_sec);
    assert(loaded_csv[i].text == events_csv[i].text);
  }

  // 2) BIDS-style TSV (onset/duration/trial_type)
  const fs::path tsv_path = tmp_dir / "events.tsv";
  {
    std::ofstream f(tsv_path);
    assert(f.good());
    f << "onset\tduration\ttrial_type\tresponse_time\n";
    f << "0.5\t1.0\tstim\t0.123\n";
    f << "2.0\t0.0\t\"comma,ok\"\t\n";
    f << "3.0\t0.25\tcue\t0.456\n";
  }

  const auto loaded_tsv = read_events_table(tsv_path.string());
  assert(loaded_tsv.size() == 3);
  expect_near(loaded_tsv[0].onset_sec, 0.5);
  expect_near(loaded_tsv[0].duration_sec, 1.0);
  assert(loaded_tsv[0].text == "stim");
  assert(loaded_tsv[1].text == "comma,ok");
  assert(loaded_tsv[2].text == "cue");

  // 3) UTF-8 BOM on header line (common in some Windows CSV exports)
  const fs::path bom_path = tmp_dir / "events_bom.csv";
  {
    std::ofstream f(bom_path, std::ios::binary);
    assert(f.good());
    f << "\xEF\xBB\xBF"; // UTF-8 BOM
    f << "onset_sec,duration_sec,text\n";
    f << "0.0,0.0,Start\n";
    f << "1.0,0.5,Task\n";
  }
  const auto loaded_bom = read_events_table(bom_path.string());
  assert(loaded_bom.size() == 2);
  expect_near(loaded_bom[0].onset_sec, 0.0);
  expect_near(loaded_bom[0].duration_sec, 0.0);
  assert(loaded_bom[0].text == "Start");

  // 4) Semicolon-delimited events table (common in some locales)
  const fs::path semi_path = tmp_dir / "events_semi.csv";
  {
    std::ofstream f(semi_path);
    assert(f.good());
    f << "onset_sec;duration_sec;text\n";
    f << "0.0;0.0;Baseline\n";
    f << "1.0;0.25;\"contains;semicolon\"\n";
  }
  const auto loaded_semi = read_events_table(semi_path.string());
  assert(loaded_semi.size() == 2);
  expect_near(loaded_semi[0].onset_sec, 0.0);
  expect_near(loaded_semi[0].duration_sec, 0.0);
  assert(loaded_semi[0].text == "Baseline");
  assert(loaded_semi[1].text == "contains;semicolon");

  // 5) Semicolon-delimited with decimal comma numbers (common in some locales)
  const fs::path semi_comma_path = tmp_dir / "events_semi_decimal_comma.csv";
  {
    std::ofstream f(semi_comma_path);
    assert(f.good());
    f << "onset_sec;duration_sec;text\n";
    f << "0,5;1,25;DecimalComma\n";
    f << "1.234,5;0;ThousandsDot\n";
  }
  const auto loaded_semi_comma = read_events_table(semi_comma_path.string());
  assert(loaded_semi_comma.size() == 2);
  expect_near(loaded_semi_comma[0].onset_sec, 0.5);
  expect_near(loaded_semi_comma[0].duration_sec, 1.25);
  assert(loaded_semi_comma[0].text == "DecimalComma");
  expect_near(loaded_semi_comma[1].onset_sec, 1234.5);
  expect_near(loaded_semi_comma[1].duration_sec, 0.0);
  assert(loaded_semi_comma[1].text == "ThousandsDot");

  std::cout << "test_events_table: OK\n";
  return 0;
}
