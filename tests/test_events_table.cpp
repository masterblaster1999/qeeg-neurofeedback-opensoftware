#include "qeeg/csv_io.hpp"

#include <cassert>
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

  std::cout << "test_events_table: OK\n";
  return 0;
}
