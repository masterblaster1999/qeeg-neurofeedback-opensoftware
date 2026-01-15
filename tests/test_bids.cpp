#include "qeeg/bids.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

  // Filename parsing helpers.
  {
    const auto p1 = parse_bids_filename("sub-01_ses-A_task-rest_acq-high_run-01_eeg.edf");
    assert(p1.has_value());
    assert(p1->ent.sub == "01");
    assert(p1->ent.ses == "A");
    assert(p1->ent.task == "rest");
    assert(p1->ent.acq == "high");
    assert(p1->ent.run == "01");
    assert(p1->suffix == "eeg");

    // Works on a bare stem without extension.
    const auto p2 = parse_bids_filename("sub-02_task-nback_events");
    assert(p2.has_value());
    assert(p2->ent.sub == "02");
    assert(p2->ent.task == "nback");
    assert(p2->suffix == "events");

    // Ignores unknown entities (e.g. desc-*) and still extracts the required ones.
    const auto p3 = parse_bids_filename("sub-01_task-rest_desc-qeegmap_bandpowers.csv");
    assert(p3.has_value());
    assert(p3->ent.sub == "01");
    assert(p3->ent.task == "rest");
    assert(p3->suffix == "bandpowers");

    // Double-extension convenience (e.g. compressed TSVs).
    const auto p4 = parse_bids_filename("sub-01_task-rest_events.tsv.gz");
    assert(p4.has_value());
    assert(p4->suffix == "events");

    // Entity chain without suffix.
    const auto p5 = parse_bids_filename("sub-03_task-rest");
    assert(p5.has_value());
    assert(p5->suffix.empty());

    // Missing required entities.
    assert(!parse_bids_filename("task-rest_eeg.edf").has_value());
  }

  // Dataset root finder.
  {
    const auto root = std::filesystem::temp_directory_path() / "qeeg_test_bids_root_finder";
    std::filesystem::create_directories(root / "sub-01" / "eeg");

    // dataset_description.json marks the dataset root.
    {
      std::ofstream f(root / "dataset_description.json", std::ios::binary);
      assert(static_cast<bool>(f));
      f << "{}\n";
    }

    // Create a dummy BIDS file.
    const auto eeg = root / "sub-01" / "eeg" / "sub-01_task-rest_eeg.edf";
    {
      std::ofstream f(eeg, std::ios::binary);
      assert(static_cast<bool>(f));
      f << "dummy";
    }

    const auto found_from_file = find_bids_dataset_root(eeg.u8string());
    assert(found_from_file.has_value());
    assert(*found_from_file == root.u8string());

    const auto found_from_dir = find_bids_dataset_root((root / "sub-01").u8string());
    assert(found_from_dir.has_value());
    assert(*found_from_dir == root.u8string());
  }

  // dataset_description.json helpers.
  {
    const auto tmp = std::filesystem::temp_directory_path() / "qeeg_test_bids_dataset_description";
    std::filesystem::create_directories(tmp);

    // Derivative datasets MUST include GeneratedBy.
    {
      BidsDatasetDescription desc;
      desc.name = "qeeg-derivatives";
      desc.dataset_type = "derivative";

      bool threw = false;
      try {
        write_bids_dataset_description(tmp.u8string(), desc, /*overwrite=*/true);
      } catch (const std::exception&) {
        threw = true;
      }
      assert(threw);
    }

    // With a GeneratedBy entry, the writer should succeed and emit the key.
    {
      BidsDatasetDescription desc;
      desc.name = "qeeg-derivatives";
      desc.dataset_type = "derivative";

      BidsDatasetDescription::GeneratedByEntry g;
      g.name = "qeeg";
      g.version = "0.1.0";
      g.code_url = "https://github.com/masterblaster1999/qeeg-neurofeedback-opensoftware";
      desc.generated_by.push_back(g);

      // Optional provenance.
      BidsDatasetDescription::SourceDatasetEntry s;
      s.url = "file://./";
      desc.source_datasets.push_back(s);

      write_bids_dataset_description(tmp.u8string(), desc, /*overwrite=*/true);
      const std::string dd = slurp(tmp / "dataset_description.json");
      assert(dd.find("\"DatasetType\": \"derivative\"") != std::string::npos);
      assert(dd.find("\"GeneratedBy\"") != std::string::npos);
      assert(dd.find("\"Name\": \"qeeg\"") != std::string::npos);
      assert(dd.find("\"CodeURL\"") != std::string::npos);
      assert(dd.find("\"SourceDatasets\"") != std::string::npos);
    }
  }

  // Round-trip write minimal sidecars.
  {
    EEGRecording rec;
    rec.fs_hz = 250.0;
    rec.channel_names = {"Cz", "VEOG", "TRIG", "REF"};
    rec.data.resize(4);
    rec.data[0] = {0.0f, 1.0f, 2.0f};
    rec.data[1] = {0.0f, 0.0f, 0.0f};
    rec.data[2] = {0.0f, 5.0f, 0.0f};
    rec.data[3] = {0.0f, 0.0f, 0.0f};
    rec.events.push_back({1.0, 0.0, "stim"});
    rec.events.push_back({1e-9, 0.0, "tiny"});
    rec.events.push_back({0.5, 0.1, "5"});
    rec.events.push_back({2.0, 0.5, "NF:Reward"});
    rec.events.push_back({3.0, 0.5, "NF:Artifact"});
    rec.events.push_back({4.0, 0.5, "NF:Baseline"});
    rec.events.push_back({5.0, 0.5, "NF:Train"});
    rec.events.push_back({6.0, 0.5, "NF:Rest"});
    rec.events.push_back({5.0, 0.5, "MS:A"});

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
    assert(ch.find("REF\tREF\tuV") != std::string::npos);

    // Load channels.tsv name list (used by derivatives exporters to preserve ordering).
    {
      const auto names = load_bids_channels_tsv_names(channels_tsv.u8string());
      assert(names.size() == 4);
      assert(names[0] == "Cz");
      assert(names[1] == "VEOG");
      assert(names[2] == "TRIG");
      assert(names[3] == "REF");
    }

    // Also test the overload that writes channels.tsv from a channel name list.
    {
      const auto channels2_tsv = tmp / "sub-01_task-rest_desc-qeegqc_channels.tsv";
      std::vector<std::string> names = {"Cz", "VEOG", "TRIG"};
      std::vector<std::string> status = {"good", "bad", "good"};
      std::vector<std::string> desc = {"", "qeeg_channel_qc:noisy", ""};

      write_bids_channels_tsv(channels2_tsv.u8string(), names, status, desc);
      const std::string ch2 = slurp(channels2_tsv);
      assert(ch2.find("VEOG\tVEOG\tuV\tbad\tqeeg_channel_qc:noisy") != std::string::npos);
      assert(ch2.find("Cz\tEEG\tuV\tgood") != std::string::npos);
      assert(ch2.find("TRIG\tTRIG\tV\tgood") != std::string::npos);
    }

    const std::string ev = slurp(events_tsv);
    assert(ev.find("0.000000001\t0\ttiny") != std::string::npos);
    // Ensure compact decimal formatting (avoid scientific notation in TSV).
    assert(ev.find("e-") == std::string::npos);
    assert(ev.find("E-") == std::string::npos);
    assert(ev.find("e+") == std::string::npos);
    assert(ev.find("E+") == std::string::npos);
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
    assert(evxj.find("\"NF:Reward\"") != std::string::npos);
    assert(evxj.find("\"NF:Artifact\"") != std::string::npos);
    assert(evxj.find("\"NF:Baseline\"") != std::string::npos);
    assert(evxj.find("\"NF:Train\"") != std::string::npos);
    assert(evxj.find("\"NF:Rest\"") != std::string::npos);
    assert(evxj.find("\"MS:A\"") != std::string::npos);
    // NF-specific level descriptions are hard-coded for nicer interoperability.
    assert(evxj.find("Neurofeedback reward active.") != std::string::npos);
    assert(evxj.find("Artifact gate active (data considered contaminated).") != std::string::npos);
    assert(evxj.find("Baseline estimation segment.") != std::string::npos);
    assert(evxj.find("Microstate A segment.") != std::string::npos);
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

    // Also allow simple 2D montage-style input (name,x,y without z). z should be treated as missing.
    const auto in_xy = tmp / "electrodes_in_xy.csv";
    {
      std::ofstream f(in_xy, std::ios::binary);
      assert(static_cast<bool>(f));
      f << "name,x,y\n";
      f << "Fp1,-0.5,0.92\n";
      f << "REF,n/a,n/a\n";
    }

    const auto loaded_xy = load_bids_electrodes_table(in_xy.u8string());
    assert(loaded_xy.size() == 2);
    assert(loaded_xy[0].name == "Fp1");
    assert(loaded_xy[0].x.has_value());
    assert(loaded_xy[0].y.has_value());
    assert(!loaded_xy[0].z.has_value());
    assert(loaded_xy[1].name == "REF");
    assert(!loaded_xy[1].x.has_value());
    assert(!loaded_xy[1].y.has_value());
    assert(!loaded_xy[1].z.has_value());

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
