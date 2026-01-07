#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

class IRecordingReader {
public:
  virtual ~IRecordingReader() = default;
  virtual EEGRecording read(const std::string& path) = 0;
};

// Read a file based on extension:
// - .edf/.EDF => EDFReader (16-bit EDF/EDF+)
// - .csv => CSVReader (requires fs_hz_for_csv > 0)
EEGRecording read_recording_auto(const std::string& path, double fs_hz_for_csv);

} // namespace qeeg
