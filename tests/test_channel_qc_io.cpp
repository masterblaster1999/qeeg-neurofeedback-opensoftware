#include "qeeg/channel_qc_io.hpp"

#include "qeeg/utils.hpp"

#include "test_support.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
  using namespace qeeg;

  // 1) Parse channel_qc.csv (format produced by qeeg_channel_qc_cli).
  const std::string csv_path = "tmp_channel_qc.csv";
  {
    std::ofstream out(csv_path);
    out << "channel,min,max,ptp,mean,stddev,robust_scale,artifact_bad_window_fraction,abs_corr_with_mean,flatline,noisy,artifact_often_bad,corr_low,bad,reasons\n";
    out << "EEG Fp1-REF,0,1,1,0,0.1,0.1,0,0,1,0,0,0,1,flatline\n";
    out << "Cz,0,2,2,0,0.2,0.2,0,0,0,0,0,0,0,\n";
  }

  {
    ChannelQcMap m = load_channel_qc_csv(csv_path);
    assert(m.size() == 2);

    const std::string k_fp1 = normalize_channel_name("Fp1");
    const std::string k_cz = normalize_channel_name("Cz");
    assert(m.count(k_fp1) == 1);
    assert(m.count(k_cz) == 1);

    assert(m.at(k_fp1).bad == true);
    assert(m.at(k_fp1).reasons == "flatline");
    assert(m.at(k_fp1).name == "EEG Fp1-REF");
    assert(m.at(k_cz).bad == false);
    assert(m.at(k_cz).name == "Cz");
  }

  // Also preserve file order when requested.
  {
    const auto names = load_channel_qc_csv_channel_names(csv_path);
    assert(names.size() == 2);
    assert(names[0] == "EEG Fp1-REF");
    assert(names[1] == "Cz");
  }

  std::remove(csv_path.c_str());

  // 1b) Parse a semicolon-delimited QC table (common spreadsheet export in some locales).
  // Also include a decimal-comma numeric cell to ensure we don't accidentally split on ','.
  const std::string sc_path = "tmp_channel_qc_semicolon.csv";
  {
    std::ofstream out(sc_path);
    out << "channel;ptp;bad;reasons\n";
    out << "EEG Fp1-REF;0,1;1;flatline\n";
    out << "Cz;0,2;0;\n";
  }
  {
    ChannelQcMap m = load_channel_qc_csv(sc_path);
    assert(m.size() == 2);
    assert(m.at(normalize_channel_name("Fp1")).bad == true);
    assert(m.at(normalize_channel_name("Fp1")).reasons == "flatline");
    assert(m.at(normalize_channel_name("Fp1")).name == "EEG Fp1-REF");
    assert(m.at(normalize_channel_name("Cz")).bad == false);
    assert(m.at(normalize_channel_name("Cz")).name == "Cz");
  }
  {
    const auto names = load_channel_qc_csv_channel_names(sc_path);
    assert(names.size() == 2);
    assert(names[0] == "EEG Fp1-REF");
    assert(names[1] == "Cz");
  }
  std::remove(sc_path.c_str());

  // 1c) Parse a tab-delimited QC table.
  const std::string tsv_path = "tmp_channel_qc.tsv";
  {
    std::ofstream out(tsv_path);
    out << "channel\tbad\treasons\n";
    out << "Fz\t1\tnoisy\n";
    out << "Pz\t0\t\n";
  }
  {
    ChannelQcMap m = load_channel_qc_csv(tsv_path);
    assert(m.size() == 2);
    assert(m.at(normalize_channel_name("Fz")).bad == true);
    assert(m.at(normalize_channel_name("Fz")).reasons == "noisy");
    assert(m.at(normalize_channel_name("Fz")).name == "Fz");
    assert(m.at(normalize_channel_name("Pz")).bad == false);
    assert(m.at(normalize_channel_name("Pz")).name == "Pz");
  }
  {
    const auto names = load_channel_qc_csv_channel_names(tsv_path);
    assert(names.size() == 2);
    assert(names[0] == "Fz");
    assert(names[1] == "Pz");
  }
  std::remove(tsv_path.c_str());

  // 2) Parse bad_channels.txt (one channel per line).
  const std::string bad_path = "tmp_bad_channels.txt";
  {
    std::ofstream out(bad_path);
    out << "# comment\n";
    out << "T3\n";       // legacy alias; normalize_channel_name maps T3->T7
    out << "  Pz  \n";
  }

  {
    ChannelQcMap m = load_bad_channels_list(bad_path);
    assert(m.size() == 2);
    assert(m.at(normalize_channel_name("T7")).bad == true);
    assert(m.at(normalize_channel_name("T7")).name == "T3");
    assert(m.at(normalize_channel_name("Pz")).bad == true);
    assert(m.at(normalize_channel_name("Pz")).name == "Pz");
  }

  std::remove(bad_path.c_str());

  // 3) load_channel_qc_any on a directory (prefer channel_qc.csv over bad_channels.txt).
  const std::string dir = "tmp_qc_dir";
  std::filesystem::create_directories(dir);
  const std::string csv_in_dir = (std::filesystem::path(dir) / "channel_qc.csv").u8string();
  const std::string txt_in_dir = (std::filesystem::path(dir) / "bad_channels.txt").u8string();
  {
    std::ofstream out(csv_in_dir);
    out << "channel,bad,reasons\n";
    out << "Fz,1,noisy\n";
  }
  {
    std::ofstream out(txt_in_dir);
    out << "Cz\n";
  }

  {
    std::string resolved;
    ChannelQcMap m = load_channel_qc_any(dir, &resolved);
    assert(resolved.find("channel_qc.csv") != std::string::npos);
    assert(m.size() == 1);
    assert(m.at(normalize_channel_name("Fz")).bad == true);
    assert(m.at(normalize_channel_name("Fz")).reasons == "noisy");
    assert(m.at(normalize_channel_name("Fz")).name == "Fz");
  }

  std::filesystem::remove_all(dir);

  std::cout << "test_channel_qc_io OK\n";
  return 0;
}
