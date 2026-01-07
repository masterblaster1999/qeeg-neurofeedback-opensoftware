#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

class CSVReader {
public:
  explicit CSVReader(double fs_hz) : fs_hz_(fs_hz) {}
  EEGRecording read(const std::string& path);

private:
  double fs_hz_{0.0};
};

} // namespace qeeg
