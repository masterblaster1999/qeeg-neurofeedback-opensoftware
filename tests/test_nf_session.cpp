#include "qeeg/nf_session.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  namespace fs = std::filesystem;

  // Create an isolated temp directory.
  const fs::path tmp_root = fs::temp_directory_path();
  const fs::path dir = tmp_root / "qeeg_test_nf_session";

  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  assert(ec.value() == 0);

  // Create dummy derived events files (both TSV and CSV).
  const fs::path events_csv = dir / "nf_derived_events.csv";
  {
    std::ofstream f(events_csv);
    assert(static_cast<bool>(f));
    f << "onset_sec,duration_sec,text\n";
    f << "0,1.0,NF:Baseline\n";
  }

  const fs::path events_tsv = dir / "nf_derived_events.tsv";
  {
    std::ofstream f(events_tsv);
    assert(static_cast<bool>(f));
    f << "onset\tduration\ttrial_type\n";
    f << "0\t1.0\tNF:Baseline\n";
  }

  // Create a dummy meta file to emulate a user passing a file path to --nf-outdir.
  const fs::path meta = dir / "nf_run_meta.json";
  {
    std::ofstream f(meta);
    assert(static_cast<bool>(f));
    f << "{}\n";
  }

  // 1) Directory path should resolve and prefer TSV.
  {
    const auto p = qeeg::find_nf_derived_events_table(dir.u8string());
    assert(p.has_value());
    assert(fs::u8path(*p) == events_tsv);
  }

  // 2) File path inside the outdir should also resolve to the directory.
  {
    const auto p = qeeg::find_nf_derived_events_table(meta.u8string());
    assert(p.has_value());
    assert(fs::u8path(*p) == events_tsv);
  }

  // 3) If TSV is missing, fall back to CSV.
  {
    fs::remove(events_tsv, ec);
    assert(ec.value() == 0);
    const auto p = qeeg::find_nf_derived_events_table(dir.u8string());
    assert(p.has_value());
    assert(fs::u8path(*p) == events_csv);
  }

  fs::remove_all(dir, ec);

  std::cout << "test_nf_session: OK\n";
  return 0;
}
