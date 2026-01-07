#include "qeeg/csv_reader.hpp"

#include "qeeg/utils.hpp"

#include <fstream>
#include <stdexcept>

namespace qeeg {

EEGRecording CSVReader::read(const std::string& path) {
  if (fs_hz_ <= 0.0) throw std::runtime_error("CSVReader: fs_hz must be provided (>0)");

  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open CSV: " + path);

  std::string header;
  if (!std::getline(f, header)) {
    throw std::runtime_error("CSV: missing header row");
  }

  auto cols = split(trim(header), ',');
  if (cols.size() < 2) throw std::runtime_error("CSV: need at least 2 columns");
  for (auto& c : cols) c = trim(c);

  bool has_time = false;
  {
    std::string c0 = to_lower(cols[0]);
    if (c0 == "time" || c0 == "t" || c0 == "timestamp") has_time = true;
  }

  size_t first_data_col = has_time ? 1 : 0;
  if (cols.size() - first_data_col < 1) throw std::runtime_error("CSV: no data columns");

  EEGRecording rec;
  rec.fs_hz = fs_hz_;
  rec.channel_names.assign(cols.begin() + static_cast<long>(first_data_col), cols.end());
  rec.data.resize(rec.channel_names.size());

  std::string line;
  size_t lineno = 1;
  while (std::getline(f, line)) {
    ++lineno;
    std::string t = trim(line);
    if (t.empty()) continue;
    if (starts_with(t, "#")) continue;

    auto vals = split(t, ',');
    if (vals.size() != cols.size()) {
      throw std::runtime_error("CSV: column count mismatch at line " + std::to_string(lineno));
    }

    for (size_t i = first_data_col; i < vals.size(); ++i) {
      double v = to_double(vals[i]);
      rec.data[i - first_data_col].push_back(static_cast<float>(v));
    }
  }

  if (rec.n_samples() == 0) throw std::runtime_error("CSV: no samples found");

  // Ensure equal lengths
  const size_t n = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != n) throw std::runtime_error("CSV: channel length mismatch");
  }

  return rec;
}

} // namespace qeeg
