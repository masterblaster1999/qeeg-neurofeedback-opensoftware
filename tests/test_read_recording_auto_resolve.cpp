#include "qeeg/edf_writer.hpp"
#include "qeeg/reader.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static qeeg::EEGRecording make_tiny_recording() {
  qeeg::EEGRecording rec;
  rec.fs_hz = 100.0;
  rec.channel_names = {"Cz"};
  rec.data.resize(1);
  rec.data[0].resize(10);
  for (size_t i = 0; i < rec.data[0].size(); ++i) {
    rec.data[0][i] = static_cast<float>(i);
  }
  return rec;
}

static void write_minimal_run_meta(const std::string& path, const std::string& out_name) {
  std::ofstream o(std::filesystem::u8path(path), std::ios::binary);
  assert(o);
  o << "{\n";
  o << "  \"Tool\": \"qeeg_preprocess_cli\",\n";
  o << "  \"Outputs\": [\n";
  o << "    \"" << out_name << "\"\n";
  o << "  ]\n";
  o << "}\n";
}

int main() {
  using namespace qeeg;

  const std::filesystem::path dir = std::filesystem::u8path("test_read_recording_auto_resolve_dir");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);

  // 1) Create a tiny EDF inside a directory and ensure read_recording_auto()
  // can accept the directory path directly.
  const std::string edf_name = "preprocessed.edf";
  const std::filesystem::path edf_path = dir / std::filesystem::u8path(edf_name);
  {
    const EEGRecording rec = make_tiny_recording();
    EDFWriter w;
    EDFWriterOptions opt;
    opt.record_duration_seconds = 1.0;
    w.write(rec, edf_path.u8string(), opt);
  }

  {
    const EEGRecording rec = read_recording_auto(dir.u8string(), /*fs_hz_for_csv=*/0.0);
    assert(rec.fs_hz > 0.0);
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Cz");
    assert(rec.n_samples() >= 10);
  }

  // 2) Create a minimal run meta JSON that references the EDF and ensure
  // read_recording_auto() can accept the *_run_meta.json path.
  const std::filesystem::path meta_path = dir / std::filesystem::u8path("preprocess_run_meta.json");
  write_minimal_run_meta(meta_path.u8string(), edf_name);

  {
    const EEGRecording rec = read_recording_auto(meta_path.u8string(), /*fs_hz_for_csv=*/0.0);
    assert(rec.channel_names.size() == 1);
    assert(rec.channel_names[0] == "Cz");
  }

  std::filesystem::remove_all(dir, ec);

  std::cout << "read_recording_auto resolution test passed.\n";
  return 0;
}
