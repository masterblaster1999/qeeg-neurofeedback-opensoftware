#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal BrainVision Core Data Format reader.
//
// Supports the common 3-file set:
// - Header:  .vhdr (INI-like text)
// - Markers: .vmrk (INI-like text)
// - Binary:  .eeg  (INT_16 or IEEE_FLOAT_32, little-endian)
//
// The reader converts the data to EEGRecording::data in microvolts when possible,
// using the per-channel resolution and unit fields in [Channel Infos].
class BrainVisionReader {
public:
  EEGRecording read(const std::string& vhdr_path);
};

} // namespace qeeg
