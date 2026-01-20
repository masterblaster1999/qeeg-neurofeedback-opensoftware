#include "qeeg/edf_reader.hpp"
#include "qeeg/reader.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::string pad(const std::string& s, size_t n) {
  if (s.size() >= n) return s.substr(0, n);
  return s + std::string(n - s.size(), ' ');
}

static std::string fmt_int(int v, size_t n) {
  return pad(std::to_string(v), n);
}

static std::string fmt_double(double v, size_t n) {
  std::string s = std::to_string(v);
  while (s.size() > 1 && s.find('.') != std::string::npos && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return pad(s, n);
}

static void write_i16_le(std::ofstream& f, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  const unsigned char b0 = static_cast<unsigned char>(u & 0xFFu);
  const unsigned char b1 = static_cast<unsigned char>((u >> 8) & 0xFFu);
  f.write(reinterpret_cast<const char*>(&b0), 1);
  f.write(reinterpret_cast<const char*>(&b1), 1);
}

static std::string make_temp_path() {
  return "test_tmp_edf_reader.edf";
}

int main() {
  using namespace qeeg;

  const std::string path = make_temp_path();
  {
    std::ofstream f(path, std::ios::binary);
    assert(static_cast<bool>(f));

    const int num_signals = 5;
    const int num_records = 1;
    const double record_duration = 1.0;
    // EEG/ExG are high-rate, TRIG/GSR are low-rate, plus an (empty) EDF+ annotations signal.
    const std::vector<int> samples_per_record = {4, 4, 2, 1, 4}; // EEG, ExG, TRIG, GSR(peripheral), Annotations
    const int header_bytes = 256 + 256 * num_signals;

    // Fixed header (256 bytes total)
    f << pad("0", 8);                        // version
    f << pad("TEST", 80);                   // patient
    f << pad("EDFREADER", 80);              // recording
    f << pad("01.01.01", 8);                // start date
    f << pad("01.01.01", 8);                // start time
    f << fmt_int(header_bytes, 8);           // header bytes
    f << pad("", 44);                       // reserved
    f << fmt_int(num_records, 8);            // num records
    f << fmt_double(record_duration, 8);     // record duration
    f << fmt_int(num_signals, 4);            // num signals

    // labels (16)
    f << pad("EEG Fz", 16);
    f << pad("ExG 1", 16);
    f << pad("TRIG", 16);
    f << pad("GSR", 16);
    f << pad("EDF Annotations", 16);

    // transducer (80)
    for (int i = 0; i < num_signals; ++i) f << pad("", 80);

    // phys dim (8)
    f << pad("uV", 8);  // EEG
    f << pad("uV", 8);  // ExG
    f << pad("", 8);    // TRIG (discrete)
    f << pad("uS", 8);  // GSR (peripheral)
    f << pad("", 8);    // annotations

    // phys min/max (8 each) -> keep it identity-ish
    for (int i = 0; i < num_signals; ++i) f << fmt_int(-32768, 8);
    for (int i = 0; i < num_signals; ++i) f << fmt_int(32767, 8);

    // dig min/max (8 each)
    for (int i = 0; i < num_signals; ++i) f << fmt_int(-32768, 8);
    for (int i = 0; i < num_signals; ++i) f << fmt_int(32767, 8);

    // prefilter (80)
    for (int i = 0; i < num_signals; ++i) f << pad("", 80);

    // samples per record (8)
    for (int i = 0; i < num_signals; ++i) f << fmt_int(samples_per_record[i], 8);

    // reserved (32)
    for (int i = 0; i < num_signals; ++i) f << pad("", 32);

    // Sanity check header size
    const auto pos = f.tellp();
    if (pos != std::streampos(header_bytes)) {
      std::cerr << "EDF test fixture: header size mismatch (tellp vs header_bytes)\n";
      return 1;
    }

    // Data record
    const std::vector<int16_t> eeg = {-100, 0, 100, -200};
    const std::vector<int16_t> exg = {1, 2, 3, 4};
    const std::vector<int16_t> trig = {0, 5};

    for (int16_t v : eeg) write_i16_le(f, v);
    for (int16_t v : exg) write_i16_le(f, v);
    for (int16_t v : trig) write_i16_le(f, v);
    write_i16_le(f, 7); // one low-rate peripheral sample

    // Annotation samples: keep empty/zero for this test.
    for (int i = 0; i < samples_per_record[4]; ++i) write_i16_le(f, 0);
  }

  // 1) Direct EDFReader
  {
    EDFReader r;
    EEGRecording rec = r.read(path);

    // The mixed-rate peripheral channel should be kept and resampled to the EEG rate.
    assert(rec.n_channels() == 4);
    assert(rec.channel_names.size() == 4);
    assert(rec.channel_names[0] == "Fz");
    assert(rec.channel_names[1] == "ExG1");
    assert(rec.channel_names[2] == "TRIG");
    assert(rec.channel_names[3] == "GSR");
    assert(rec.fs_hz == 4.0);

    assert(rec.data[0].size() == 4);
    assert(rec.data[1].size() == 4);
    assert(rec.data[2].size() == 4);
    assert(rec.data[3].size() == 4);

    assert(rec.data[0][0] == -100.0f);
    assert(rec.data[0][1] == 0.0f);
    assert(rec.data[0][2] == 100.0f);
    assert(rec.data[0][3] == -200.0f);

    assert(rec.data[1][0] == 1.0f);
    assert(rec.data[1][1] == 2.0f);
    assert(rec.data[1][2] == 3.0f);
    assert(rec.data[1][3] == 4.0f);

    // TRIG is a discrete channel: it should be resampled with hold (no interpolated intermediate codes).
    assert(rec.data[2][0] == 0.0f);
    assert(rec.data[2][1] == 0.0f);
    assert(rec.data[2][2] == 5.0f);
    assert(rec.data[2][3] == 5.0f);

    // The single GSR sample should be stretched to the target length.
    for (float v : rec.data[3]) {
      if (v != 7.0f) {
        std::cerr << "EDFReader: expected peripheral channel to be constant 7.0f\n";
        return 1;
      }
    }
  }

  // 2) read_recording_auto dispatch by extension
  {
    EEGRecording rec = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
    assert(rec.n_channels() == 4);
    assert(rec.channel_names[0] == "Fz");
    assert(rec.channel_names[1] == "ExG1");
    assert(rec.channel_names[2] == "TRIG");
    assert(rec.channel_names[3] == "GSR");
    assert(rec.fs_hz == 4.0);

    // No EDF+ annotations were written; read_recording_auto should recover triggers from TRIG.
    assert(rec.events.size() == 1);
    assert(rec.events[0].text == "5");
    assert(rec.events[0].onset_sec == 0.5);
  }

  std::remove(path.c_str());

  std::cout << "EDFReader test passed.\n";
  return 0;
}
