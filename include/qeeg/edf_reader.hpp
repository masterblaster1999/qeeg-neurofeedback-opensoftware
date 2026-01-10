#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal EDF/EDF+ (16-bit) reader.
// - parses EDF+ annotation channels ("EDF Annotations") into EEGRecording::events
// - reads signals into physical units using per-signal scaling
// - supports "unknown number of records" by inferring from file size (best effort)
// - if the file mixes sampling rates across channels (common in BioTrace+ / NeXus exports with
//   peripherals), keeps all non-annotation channels and resamples them to the highest EEG/ExG-like
//   sampling rate (best effort heuristic)
// - voltage-like channels with physical dimension "mV" or "V" are converted to microvolts
class EDFReader {
public:
  EEGRecording read(const std::string& path);
};

} // namespace qeeg
