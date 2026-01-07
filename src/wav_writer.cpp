#include "qeeg/wav_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace qeeg {

static void write_u16_le(std::ofstream& f, uint16_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
}

static void write_u32_le(std::ofstream& f, uint32_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
  f.put(static_cast<char>((v >> 16) & 0xFF));
  f.put(static_cast<char>((v >> 24) & 0xFF));
}

static int16_t float_to_pcm16(float x) {
  if (x >= 1.0f) return 32767;
  if (x <= -1.0f) return static_cast<int16_t>(-32768);
  // Note: we scale by 32767 so that +1 maps to 32767 and -1 is handled by the clamp above.
  const float scaled = x * 32767.0f;
  const long v = std::lround(static_cast<double>(scaled));
  if (v > 32767) return 32767;
  if (v < -32768) return static_cast<int16_t>(-32768);
  return static_cast<int16_t>(v);
}

void write_wav_pcm16(const std::string& path,
                     int sample_rate,
                     const std::vector<std::vector<float>>& channels) {
  if (sample_rate <= 0) throw std::runtime_error("write_wav_pcm16: sample_rate must be > 0");
  if (channels.empty()) throw std::runtime_error("write_wav_pcm16: need at least 1 channel");

  const size_t num_channels = channels.size();
  const size_t n_samples = channels[0].size();
  for (size_t c = 1; c < num_channels; ++c) {
    if (channels[c].size() != n_samples) {
      throw std::runtime_error("write_wav_pcm16: all channels must have same length");
    }
  }

  if (n_samples == 0) throw std::runtime_error("write_wav_pcm16: no samples");
  if (num_channels > 0xFFFFu) throw std::runtime_error("write_wav_pcm16: too many channels");

  const uint16_t bits_per_sample = 16;
  const uint16_t nchan_u16 = static_cast<uint16_t>(num_channels);
  const uint16_t block_align = static_cast<uint16_t>(nchan_u16 * (bits_per_sample / 8));
  const uint32_t byte_rate = static_cast<uint32_t>(sample_rate) * static_cast<uint32_t>(block_align);

  const uint32_t data_bytes = static_cast<uint32_t>(n_samples) * static_cast<uint32_t>(block_align);
  const uint32_t riff_size = 36u + data_bytes;

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to open output WAV: " + path);

  // RIFF header
  f.write("RIFF", 4);
  write_u32_le(f, riff_size);
  f.write("WAVE", 4);

  // fmt chunk
  f.write("fmt ", 4);
  write_u32_le(f, 16u);          // PCM fmt chunk size
  write_u16_le(f, 1u);           // audio format 1 = PCM
  write_u16_le(f, nchan_u16);
  write_u32_le(f, static_cast<uint32_t>(sample_rate));
  write_u32_le(f, byte_rate);
  write_u16_le(f, block_align);
  write_u16_le(f, bits_per_sample);

  // data chunk
  f.write("data", 4);
  write_u32_le(f, data_bytes);

  // Interleaved sample data
  for (size_t i = 0; i < n_samples; ++i) {
    for (size_t c = 0; c < num_channels; ++c) {
      const int16_t s = float_to_pcm16(channels[c][i]);
      write_u16_le(f, static_cast<uint16_t>(s));
    }
  }
}

void write_wav_mono_pcm16(const std::string& path,
                          int sample_rate,
                          const std::vector<float>& mono) {
  write_wav_pcm16(path, sample_rate, std::vector<std::vector<float>>{mono});
}

} // namespace qeeg
