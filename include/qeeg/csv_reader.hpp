#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

class CSVReader {
public:
  // fs_hz:
  //  - If > 0, it is used as the sampling rate (Hz).
  //  - If <= 0, the reader will attempt to infer the sampling rate from a
  //    leading time column (e.g., header "time,Ch1,Ch2" or "time_ms,...").
  explicit CSVReader(double fs_hz) : fs_hz_(fs_hz) {}

  EEGRecording read(const std::string& path);

private:
  double fs_hz_{0.0};
};

} // namespace qeeg
