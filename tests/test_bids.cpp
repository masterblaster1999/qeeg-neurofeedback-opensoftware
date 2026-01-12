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
    rec.events.push_back({0.5, 0.1, "5"});

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

    // Also test optional extra columns in events.tsv.
    const auto events_extra_tsv = tmp / "sub-01_task-rest_events_extra.tsv";
    const auto events_extra_json = tmp / "sub-01_task-rest_events_extra.json";

    BidsEventsTsvOptions ev_opts;
    ev_opts.include_sample = true;
    ev_opts.sample_index_base = 0;
    ev_opts.include_value = true;
    ev_opts.include_trial_type_levels = true;

    write_bids_events_tsv(events_extra_tsv.u8string(), rec.events, ev_opts, rec.fs_hz);
    write_bids_events_json(events_extra_json.u8string(), ev_opts, rec.events);

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

    const std::string evx = slurp(events_extra_tsv);
    assert(evx.find("onset\tduration\ttrial_type\tsample\tvalue") != std::string::npos);
    assert(evx.find("0.5\t0.1\t5\t125\t5") != std::string::npos);
    assert(evx.find("stim\t250\tn/a") != std::string::npos);

    const std::string evxj = slurp(events_extra_json);
    assert(evxj.find("trial_type") != std::string::npos);
    assert(evxj.find("sample") != std::string::npos);
    assert(evxj.find("value") != std::string::npos);
    assert(evxj.find("\"Levels\"") != std::string::npos);
    assert(evxj.find("\"stim\"") != std::string::npos);
    assert(evxj.find("\"5\"") != std::string::npos);
  }

  // Electrodes + coordsystem helpers.
  {
    const auto tmp = std::filesystem::temp_directory_path() / "qeeg_test_bids_electrodes";
    std::filesystem::create_directories(tmp);

    // Write a small CSV electrodes table and load it.
    const auto in_csv = tmp / "electrodes_in.csv";
    {
      std::ofstream f(in_csv, std::ios::binary);
      assert(static_cast<bool>(f));
      f << "name,x,y,z,type,material,impedance\n";
      f << "Cz,0,0.0714,0.0699,cup,Ag/AgCl,5.5\n";
      f << "REF,n/a,n/a,n/a,,,\n";
    }

    const auto loaded = load_bids_electrodes_table(in_csv.u8string());
    assert(loaded.size() == 2);
    assert(loaded[0].name == "Cz");
    assert(loaded[0].x.has_value());
    assert(loaded[0].y.has_value());
    assert(loaded[0].z.has_value());
    assert(loaded[0].type == "cup");
    assert(loaded[0].material == "Ag/AgCl");
    assert(loaded[0].impedance_kohm.has_value());
    assert(loaded[1].name == "REF");
    assert(!loaded[1].x.has_value());
    assert(!loaded[1].y.has_value());
    assert(!loaded[1].z.has_value());

    // Write electrodes.tsv.
    const auto electrodes_tsv = tmp / "sub-01_task-rest_electrodes.tsv";
    write_bids_electrodes_tsv(electrodes_tsv.u8string(), loaded);

    const std::string el = slurp(electrodes_tsv);
    assert(el.find("name\tx\ty\tz") != std::string::npos);
    assert(el.find("Cz\t0.000000\t0.071400\t0.069900") != std::string::npos);
    assert(el.find("REF\tn/a\tn/a\tn/a") != std::string::npos);

    // Coord system JSON.
    assert(is_valid_bids_coordinate_unit("mm"));
    assert(!is_valid_bids_coordinate_unit("meters"));

    const auto coordsystem_json = tmp / "sub-01_task-rest_coordsystem.json";
    BidsCoordsystemJsonEegMetadata cs;
    cs.eeg_coordinate_system = "CapTrak";
    cs.eeg_coordinate_units = "mm";
    write_bids_coordsystem_json(coordsystem_json.u8string(), cs);

    const std::string csj = slurp(coordsystem_json);
    assert(csj.find("\"EEGCoordinateSystem\": \"CapTrak\"") != std::string::npos);
    assert(csj.find("\"EEGCoordinateUnits\": \"mm\"") != std::string::npos);
  }

  std::cout << "All tests passed.\n";
  return 0;
}
