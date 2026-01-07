#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal EDF/EDF+ (16-bit) reader.
// - ignores annotation channels
// - reads signals into physical units using per-signal scaling
// - supports "unknown number of records" by inferring from file size (best effort)
class EDFReader {
public:
  EEGRecording read(const std::string& path);
};

} // namespace qeeg
