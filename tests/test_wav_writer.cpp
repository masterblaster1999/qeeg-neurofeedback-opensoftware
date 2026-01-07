#include "qeeg/wav_writer.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint16_t read_u16_le(const std::vector<uint8_t>& b, size_t off) {
  assert(off + 2 <= b.size());
  return static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8);
}

static uint32_t read_u32_le(const std::vector<uint8_t>& b, size_t off) {
  assert(off + 4 <= b.size());
  return static_cast<uint32_t>(b[off]) |
         (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) |
         (static_cast<uint32_t>(b[off + 3]) << 24);
}

static bool bytes_eq(const std::vector<uint8_t>& b, size_t off, const char* s4) {
  return off + 4 <= b.size() &&
         b[off + 0] == static_cast<uint8_t>(s4[0]) &&
         b[off + 1] == static_cast<uint8_t>(s4[1]) &&
         b[off + 2] == static_cast<uint8_t>(s4[2]) &&
         b[off + 3] == static_cast<uint8_t>(s4[3]);
}

int main() {
  using namespace qeeg;

  const int sr = 8000;
  const size_t n = static_cast<size_t>(sr / 10); // 0.1s
  const double two_pi = 2.0 * std::acos(-1.0);

  std::vector<float> mono(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    mono[i] = static_cast<float>(0.5 * std::sin(two_pi * 440.0 * t));
  }

  const std::string path = "qeeg_test_wav_writer.wav";
  write_wav_mono_pcm16(path, sr, mono);

  std::ifstream f(path, std::ios::binary);
  assert(f && "failed to open written wav file");
  f.seekg(0, std::ios::end);
  const std::streamoff sz = f.tellg();
  f.seekg(0, std::ios::beg);

  std::vector<uint8_t> bytes(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(bytes.data()), sz);
  assert(f.good());

  // Basic RIFF/WAVE header checks (PCM16 with 16-byte fmt chunk => 44-byte header)
  assert(bytes.size() == 44 + n * 2);
  assert(bytes_eq(bytes, 0, "RIFF"));
  assert(bytes_eq(bytes, 8, "WAVE"));
  assert(bytes_eq(bytes, 12, "fmt "));

  const uint32_t riff_size = read_u32_le(bytes, 4);
  assert(riff_size == 36 + n * 2);

  const uint32_t fmt_size = read_u32_le(bytes, 16);
  assert(fmt_size == 16);

  const uint16_t audio_format = read_u16_le(bytes, 20);
  const uint16_t n_channels = read_u16_le(bytes, 22);
  const uint32_t sample_rate = read_u32_le(bytes, 24);
  const uint16_t bits_per_sample = read_u16_le(bytes, 34);

  assert(audio_format == 1);
  assert(n_channels == 1);
  assert(sample_rate == static_cast<uint32_t>(sr));
  assert(bits_per_sample == 16);

  assert(bytes_eq(bytes, 36, "data"));
  const uint32_t data_bytes = read_u32_le(bytes, 40);
  assert(data_bytes == n * 2);

  // Spot-check a couple of samples to ensure data isn't all zero.
  const int16_t s0 = static_cast<int16_t>(read_u16_le(bytes, 44));
  const int16_t s1 = static_cast<int16_t>(read_u16_le(bytes, 46));
  assert(s0 == 0);
  assert(s1 != 0);

  std::remove(path.c_str());
  std::cout << "All tests passed.\n";
  return 0;
}
