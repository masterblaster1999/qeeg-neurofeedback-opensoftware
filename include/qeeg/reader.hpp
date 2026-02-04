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
// - .edf/.EDF/.rec => EDFReader (16-bit EDF/EDF+) by default.
//   If the file header indicates BDF (e.g., BioSemi-style 0xFF "BIOSEMI"),
//   the loader will route it to BDFReader instead (best effort).
// - .bdf/.BDF => BDFReader (24-bit BDF/BDF+)
// - .csv => CSVReader (fs_hz_for_csv can be omitted if the first column is a time axis)
//
// If the input is EDF+/BDF+ and contains an "Annotations" signal, the readers
// will also fill EEGRecording::events with parsed TAL entries.
EEGRecording read_recording_auto(const std::string& path, double fs_hz_for_csv);

} // namespace qeeg
