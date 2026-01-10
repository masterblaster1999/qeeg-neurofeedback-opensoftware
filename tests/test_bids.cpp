#include "qeeg/bids.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  assert(static_cast<bool>(f));
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

int main() {
  using namespace qeeg;

  // Label validation.
  assert(is_valid_bids_label("01"));
  assert(is_valid_bids_label("sub01"));
  assert(!is_valid_bids_label(""));
  assert(!is_valid_bids_label("sub-01"));
  assert(!is_valid_bids_label("sub_01"));
  assert(!is_valid_bids_label("sub 01"));

  // Entity chain formatting.
  {
    BidsEntities e;
    e.sub = "01";
    e.task = "rest";
    e.ses = "A";
    e.acq = "high";
    e.run = "01";

    const std::string chain = format_bids_entity_chain(e);
    assert(chain == "sub-01_ses-A_task-rest_acq-high_run-01");

    const std::string eeg_stem = format_bids_filename_stem(e, "eeg");
    assert(eeg_stem == "sub-01_ses-A_task-rest_acq-high_run-01_eeg");

    const std::string ch_stem = format_bids_filename_stem(e, "channels");
    assert(ch_stem == "sub-01_ses-A_task-rest_acq-high_run-01_channels");
  }

  // Round-trip write minimal sidecars.
  {
    EEGRecording rec;
    rec.fs_hz = 250.0;
    rec.channel_names = {"Cz", "VEOG", "TRIG"};
    rec.data.resize(3);
    rec.data[0] = {0.0f, 1.0f, 2.0f};
    rec.data[1] = {0.0f, 0.0f, 0.0f};
    rec.data[2] = {0.0f, 5.0f, 0.0f};
    rec.events.push_back({1.0, 0.0, "stim"});

    BidsEegJsonMetadata meta;
    meta.eeg_reference = "Cz";
    meta.task_name = "rest";
    meta.power_line_frequency_hz = 50.0;

    const auto tmp = std::filesystem::temp_directory_path() / "qeeg_test_bids";
    std::filesystem::create_directories(tmp);

    const auto eeg_json = tmp / "sub-01_task-rest_eeg.json";
    const auto channels_tsv = tmp / "sub-01_task-rest_channels.tsv";
    const auto events_tsv = tmp / "sub-01_task-rest_events.tsv";
    const auto events_json = tmp / "sub-01_task-rest_events.json";

    write_bids_eeg_json(eeg_json.u8string(), rec, meta);
    write_bids_channels_tsv(channels_tsv.u8string(), rec);
    write_bids_events_tsv(events_tsv.u8string(), rec.events);
    write_bids_events_json(events_json.u8string());

    const std::string eeg = slurp(eeg_json);
    assert(eeg.find("\"SamplingFrequency\"") != std::string::npos);
    assert(eeg.find("\"EEGReference\": \"Cz\"") != std::string::npos);
    assert(eeg.find("\"PowerLineFrequency\": 50") != std::string::npos);

    const std::string ch = slurp(channels_tsv);
    assert(ch.find("name\ttype\tunits") != std::string::npos);
    assert(ch.find("Cz\tEEG\tuV") != std::string::npos);
    assert(ch.find("VEOG\tVEOG\tuV") != std::string::npos);
    assert(ch.find("TRIG\tTRIG\tV") != std::string::npos);

    const std::string ev = slurp(events_tsv);
    assert(ev.find("onset\tduration\ttrial_type") != std::string::npos);
    assert(ev.find("1") != std::string::npos);
    assert(ev.find("stim") != std::string::npos);

    const std::string evj = slurp(events_json);
    assert(evj.find("trial_type") != std::string::npos);
  }

  std::cout << "All tests passed.\n";
  return 0;
}
