#include "qeeg/brainvision_writer.hpp"
#include "qeeg/brainvision_reader.hpp"
#include "qeeg/types.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static float read_le_f32(std::istream& is) {
  unsigned char b[4];
  is.read(reinterpret_cast<char*>(b), 4);
  uint32_t u = (uint32_t)b[0]
             | ((uint32_t)b[1] << 8)
             | ((uint32_t)b[2] << 16)
             | ((uint32_t)b[3] << 24);
  float f = 0.0f;
  std::memcpy(&f, &u, 4);
  return f;
}

static int16_t read_le_i16(std::istream& is) {
  unsigned char b[2];
  is.read(reinterpret_cast<char*>(b), 2);
  uint16_t u = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return static_cast<int16_t>(u);
}

static std::string slurp_text(const std::filesystem::path& p) {
  std::ifstream is(p);
  assert(is && "failed to open text file");
  std::string s((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  return s;
}

int main() {
  using namespace qeeg;

  EEGRecording rec;
  rec.channel_names = {"C3", "C4"};
  rec.fs_hz = 100.0;
  const size_t n = 100;
  rec.data.resize(2);
  rec.data[0].resize(n);
  rec.data[1].resize(n);
  for (size_t i = 0; i < n; ++i) {
    rec.data[0][i] = static_cast<float>(i);        // 0,1,2,... uV
    rec.data[1][i] = static_cast<float>(-2.0 * i); // 0,-2,-4,... uV
  }
  rec.events.push_back(AnnotationEvent{0.5, 0.0, "Stim1"});

  const std::filesystem::path outdir = std::filesystem::temp_directory_path() / "qeeg_test_brainvision";
  std::filesystem::create_directories(outdir);

  // --- float32 ---
  {
    const std::filesystem::path vhdr = outdir / "float32.vhdr";
    BrainVisionWriterOptions opts;
    opts.binary_format = BrainVisionBinaryFormat::Float32;
    opts.unit = "uV";

    BrainVisionWriter w;
    w.write(rec, vhdr.string(), opts);

    const std::filesystem::path eeg = outdir / "float32.eeg";
    const std::filesystem::path vmrk = outdir / "float32.vmrk";

    assert(std::filesystem::exists(vhdr));
    assert(std::filesystem::exists(eeg));
    assert(std::filesystem::exists(vmrk));

    assert(std::filesystem::file_size(eeg) == static_cast<uintmax_t>(n * 2 * 4));

    const std::string vhdr_txt = slurp_text(vhdr);
    assert(vhdr_txt.find("NumberOfChannels=2") != std::string::npos);
    assert(vhdr_txt.find("SamplingInterval=10000") != std::string::npos);
    assert(vhdr_txt.find("BinaryFormat=IEEE_FLOAT_32") != std::string::npos);

    const std::string vmrk_txt = slurp_text(vmrk);
    assert(vmrk_txt.find("New Segment") != std::string::npos);
    assert(vmrk_txt.find("Stim1") != std::string::npos);

    std::ifstream is(eeg, std::ios::binary);
    assert(is && "failed to open EEG binary");

    // MULTIPLEXED: (C3[0], C4[0], C3[1], C4[1], ...)
    float c3_0 = read_le_f32(is);
    float c4_0 = read_le_f32(is);
    float c3_1 = read_le_f32(is);
    float c4_1 = read_le_f32(is);

    assert(c3_0 == 0.0f);
    assert(c4_0 == 0.0f);
    assert(c3_1 == 1.0f);
    assert(c4_1 == -2.0f);

    // Round-trip read via BrainVisionReader (.vhdr -> .eeg/.vmrk)
    {
      BrainVisionReader r;
      EEGRecording got = r.read(vhdr.string());
      assert(std::abs(got.fs_hz - rec.fs_hz) < 1e-9);
      assert(got.channel_names == rec.channel_names);
      assert(got.n_samples() == rec.n_samples());
      assert(got.n_channels() == rec.n_channels());
      for (size_t j = 0; j < n; ++j) {
        assert(std::abs(got.data[0][j] - rec.data[0][j]) < 1e-6f);
        assert(std::abs(got.data[1][j] - rec.data[1][j]) < 1e-6f);
      }
      assert(got.events.size() == 1);
      assert(got.events[0].text == "Stim1");
      assert(std::abs(got.events[0].onset_sec - 0.5) < (1.0 / rec.fs_hz));
    }
  }

  // --- int16 (fixed resolution 0.1 uV) ---
  {
    const std::filesystem::path vhdr = outdir / "int16.vhdr";
    BrainVisionWriterOptions opts;
    opts.binary_format = BrainVisionBinaryFormat::Int16;
    opts.unit = "uV";
    opts.int16_resolution = 0.1; // fixed

    BrainVisionWriter w;
    w.write(rec, vhdr.string(), opts);

    const std::filesystem::path eeg = outdir / "int16.eeg";
    const std::filesystem::path vmrk = outdir / "int16.vmrk";

    assert(std::filesystem::exists(vhdr));
    assert(std::filesystem::exists(eeg));
    assert(std::filesystem::exists(vmrk));

    assert(std::filesystem::file_size(eeg) == static_cast<uintmax_t>(n * 2 * 2));

    const std::string vhdr_txt = slurp_text(vhdr);
    assert(vhdr_txt.find("BinaryFormat=INT_16") != std::string::npos);
    // Should contain the fixed resolution for channel 1.
    assert(vhdr_txt.find("Ch1=C3,,0.1,uV") != std::string::npos);

    std::ifstream is(eeg, std::ios::binary);
    assert(is && "failed to open EEG binary");

    // i=0
    int16_t c3_0 = read_le_i16(is);
    int16_t c4_0 = read_le_i16(is);
    // i=1
    int16_t c3_1 = read_le_i16(is);
    int16_t c4_1 = read_le_i16(is);

    // digital = physical/resolution
    assert(c3_0 == 0);
    assert(c4_0 == 0);
    assert(c3_1 == 10);   // 1 / 0.1
    assert(c4_1 == -20);  // -2 / 0.1

    // Round-trip read via BrainVisionReader for INT_16 scaling.
    {
      BrainVisionReader r;
      EEGRecording got = r.read(vhdr.string());
      assert(std::abs(got.fs_hz - rec.fs_hz) < 1e-9);
      assert(got.channel_names == rec.channel_names);
      assert(got.n_samples() == rec.n_samples());
      assert(got.n_channels() == rec.n_channels());
      for (size_t j = 0; j < n; ++j) {
        assert(std::abs(got.data[0][j] - rec.data[0][j]) < 1e-4f);
        assert(std::abs(got.data[1][j] - rec.data[1][j]) < 1e-4f);
      }
      assert(got.events.size() == 1);
      assert(got.events[0].text == "Stim1");
      assert(std::abs(got.events[0].onset_sec - 0.5) < (1.0 / rec.fs_hz));
    }
  }

  // Cleanup (best-effort)
  std::error_code ec;
  std::filesystem::remove_all(outdir, ec);

  std::cout << "OK\n";
  return 0;
}
