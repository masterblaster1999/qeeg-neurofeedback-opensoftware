#pragma once

#include "qeeg/types.hpp"

#include <string>
#include <vector>

namespace qeeg {

enum class BrainVisionBinaryFormat {
  Int16,
  Float32,
};

struct BrainVisionWriterOptions {
  // Binary format written to the .eeg file.
  BrainVisionBinaryFormat binary_format{BrainVisionBinaryFormat::Float32};

  // Character encoding for .vhdr/.vmrk.
  // BrainVision Core File Format 1.0 recommends UTF-8.
  std::string codepage{"UTF-8"};

  // Channel unit written in [Channel Infos].
  // Use an ASCII-safe default to avoid encoding issues ("uV" vs "µV").
  std::string unit{"uV"};

  // --- INT_16 scaling ---

  // For INT_16, physical_value = digital_value * resolution.
  // If this is > 0, a fixed resolution is used for all channels.
  // If 0, per-channel resolution is derived from the channel max.
  double int16_resolution{0.0};

  // If int16_resolution == 0, per-channel resolution is derived as:
  //   max_abs / int16_target_max_digital
  // This leaves some headroom to avoid clipping.
  int int16_target_max_digital{30000};

  // --- marker generation ---

  // Write a default "New Segment" marker at position 1.
  bool write_new_segment_marker{true};

  // If true, write EEGRecording::events into the marker file (as Comment markers).
  bool write_events{true};
};

class BrainVisionWriter {
public:
  // Write a BrainVision set.
  //
  // Provide the output header path (usually ending with .vhdr). The data (.eeg) and
  // marker (.vmrk) files are written next to it with the same basename.
  void write(const EEGRecording& rec, const std::string& vhdr_path,
             const BrainVisionWriterOptions& opts = BrainVisionWriterOptions{});
};

} // namespace qeeg
