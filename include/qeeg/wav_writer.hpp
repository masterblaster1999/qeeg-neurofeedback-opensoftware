#pragma once

#include <string>
#include <vector>

namespace qeeg {

// Write a basic PCM 16-bit little-endian WAV file.
//
// Notes:
// - channels[ch][i] is sample i of channel ch.
// - All channels must have the same length.
// - Floating-point samples are expected to be roughly in [-1, 1] and are clamped.
// - Output samples are interleaved (ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ...).
void write_wav_pcm16(const std::string& path,
                     int sample_rate,
                     const std::vector<std::vector<float>>& channels);

// Convenience wrapper for mono.
void write_wav_mono_pcm16(const std::string& path,
                          int sample_rate,
                          const std::vector<float>& mono);

} // namespace qeeg
