#include "qeeg/bdf_reader.hpp"
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
  // EDF/BDF fields are fixed-width ASCII; a simple decimal is fine for tests.
  std::string s = std::to_string(v);
  // Trim trailing zeros to keep it compact.
  while (s.size() > 1 && s.find('.') != std::string::npos && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return pad(s, n);
}

static void write_i24_le(std::ofstream& f, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v) & 0x00FFFFFFu;
  const unsigned char b0 = static_cast<unsigned char>(u & 0xFFu);
  const unsigned char b1 = static_cast<unsigned char>((u >> 8) & 0xFFu);
  const unsigned char b2 = static_cast<unsigned char>((u >> 16) & 0xFFu);
  f.write(reinterpret_cast<const char*>(&b0), 1);
  f.write(reinterpret_cast<const char*>(&b1), 1);
  f.write(reinterpret_cast<const char*>(&b2), 1);
}

static std::string make_temp_path() {
  // Use a simple temp name in the current working directory.
  // (CTest runs in a writable build dir.)
  return "test_tmp_bdf_reader.bdf";
}

int main() {
  using namespace qeeg;

  const std::string path = make_temp_path();
  {
    std::ofstream f(path, std::ios::binary);
    assert(static_cast<bool>(f));

    const int num_signals = 2;
    const int num_records = 1;
    const double record_duration = 1.0;
    const int samples_per_record = 4;
    const int header_bytes = 256 + 256 * num_signals;

    // Fixed header (256 bytes total)
    f << pad("0", 8);                       // version
    f << pad("TEST", 80);                  // patient
    f << pad("BDFREADER", 80);             // recording
    f << pad("01.01.01", 8);               // start date
    f << pad("01.01.01", 8);               // start time
    f << fmt_int(header_bytes, 8);          // header bytes
    f << pad("BIOSEMI", 44);               // reserved
    f << fmt_int(num_records, 8);           // num records
    f << fmt_double(record_duration, 8);    // record duration
    f << fmt_int(num_signals, 4);           // num signals

    // Per-signal header arrays (256 bytes per signal)
    // labels (16)
    f << pad("EEG Fz", 16);
    f << pad("EDF Annotations", 16);

    // transducer (80)
    f << pad("", 80) << pad("", 80);
    // phys dim (8)
    f << pad("uV", 8) << pad("", 8);

    // phys min/max (8 each)
    // Use integer strings (to_double can parse them) so they fit the 8-byte EDF/BDF field.
    f << fmt_int(-8388608, 8) << fmt_int(-8388608, 8);
    f << fmt_int(8388607, 8) << fmt_int(8388607, 8);

    // dig min/max (8 each)
    f << fmt_int(-8388608, 8) << fmt_int(-8388608, 8);
    f << fmt_int(8388607, 8) << fmt_int(8388607, 8);

    // prefilter (80)
    f << pad("", 80) << pad("", 80);

    // samples per record (8)
    f << fmt_int(samples_per_record, 8) << fmt_int(samples_per_record, 8);

    // reserved (32)
    f << pad("", 32) << pad("", 32);

    // Sanity check header size
    const auto pos = f.tellp();
    assert(pos == header_bytes);

    // Data record: signal 0 samples then signal 1 samples
    const std::vector<int32_t> eeg = {-100, 0, 100, -200};
    for (int32_t v : eeg) write_i24_le(f, v);
    for (int i = 0; i < samples_per_record; ++i) write_i24_le(f, 0);
  }

  // 1) Direct BDFReader
  {
    BDFReader r;
    EEGRecording rec = r.read(path);
    assert(rec.n_channels() == 1);
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Fz");
    assert(rec.fs_hz == 4.0);
    assert(rec.data[0].size() == 4);
    assert(rec.data[0][0] == -100.0f);
    assert(rec.data[0][1] == 0.0f);
    assert(rec.data[0][2] == 100.0f);
    assert(rec.data[0][3] == -200.0f);
  }

  // 2) read_recording_auto dispatch by extension
  {
    EEGRecording rec = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
    assert(rec.n_channels() == 1);
    assert(rec.channel_names[0] == "Fz");
    assert(rec.fs_hz == 4.0);
  }

  std::remove(path.c_str());

  std::cout << "BDFReader test passed.\n";
  return 0;
}
