#include "qeeg/reader.hpp"

#include "qeeg/csv_reader.hpp"
#include "qeeg/bdf_reader.hpp"
#include "qeeg/edf_reader.hpp"
#include "qeeg/utils.hpp"

#include <stdexcept>

namespace qeeg {

EEGRecording read_recording_auto(const std::string& path, double fs_hz_for_csv) {
  std::string low = to_lower(path);

  if (ends_with(low, ".edf") || ends_with(low, ".edf+") || ends_with(low, ".rec")) {
    EDFReader r;
    return r.read(path);
  }
  if (ends_with(low, ".bdf") || ends_with(low, ".bdf+")) {
    BDFReader r;
    return r.read(path);
  }
  if (ends_with(low, ".csv")) {
    CSVReader r(fs_hz_for_csv);
    return r.read(path);
  }

  throw std::runtime_error("Unsupported input file extension (expected .edf/.bdf or .csv): " + path);
}

} // namespace qeeg
