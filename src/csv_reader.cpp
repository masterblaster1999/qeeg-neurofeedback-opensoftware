#include "qeeg/csv_reader.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace qeeg {

namespace {

char detect_delim(const std::string& header_line) {
  // Heuristic delimiter detection. Comma is the default, but many datasets are
  // exported with ';' or tab delimiters depending on locale/software.
  const size_t n_comma = static_cast<size_t>(std::count(header_line.begin(), header_line.end(), ','));
  const size_t n_semi  = static_cast<size_t>(std::count(header_line.begin(), header_line.end(), ';'));
  const size_t n_tab   = static_cast<size_t>(std::count(header_line.begin(), header_line.end(), '\t'));

  char best = ',';
  size_t best_n = n_comma;
  if (n_semi > best_n) {
    best = ';';
    best_n = n_semi;
  }
  if (n_tab > best_n) {
    best = '\t';
    best_n = n_tab;
  }
  return best;
}

bool is_comment_or_empty(const std::string& t) {
  if (t.empty()) return true;
  if (starts_with(t, "#")) return true;
  if (starts_with(t, "//")) return true;
  return false;
}

double median_inplace(std::vector<double>* v) {
  if (!v || v->empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v->begin(), v->end());
  const size_t n = v->size();
  const size_t mid = n / 2;
  if (n % 2 == 1) return (*v)[mid];
  return 0.5 * ((*v)[mid - 1] + (*v)[mid]);
}

} // namespace

EEGRecording CSVReader::read(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open CSV: " + path);

  // Read header row (skip blank/comment lines).
  std::string header;
  size_t lineno = 0;
  while (std::getline(f, header)) {
    ++lineno;
    std::string t = trim(header);
    if (is_comment_or_empty(t)) continue;
    header = t;
    break;
  }
  if (trim(header).empty()) {
    throw std::runtime_error("CSV: missing header row");
  }

  const char delim = detect_delim(header);

  auto cols = split(trim(header), delim);
  if (cols.size() < 2) throw std::runtime_error("CSV: need at least 2 columns");
  for (auto& c : cols) c = trim(c);

  bool has_time = false;
  bool time_in_ms = false;
  {
    std::string c0 = to_lower(cols[0]);
    // Common conventions for a leading time axis column.
    if (c0 == "t" || c0 == "time" || c0 == "timestamp" || starts_with(c0, "time")) {
      has_time = true;

      // Optional hint for units.
      if (c0.find("ms") != std::string::npos || c0.find("millis") != std::string::npos) {
        time_in_ms = true;
      }
    }
  }

  const size_t first_data_col = has_time ? 1 : 0;
  if (cols.size() - first_data_col < 1) throw std::runtime_error("CSV: no data columns");

  EEGRecording rec;
  rec.channel_names.assign(cols.begin() + static_cast<long>(first_data_col), cols.end());
  rec.data.resize(rec.channel_names.size());

  // If fs is not provided and we have a time column, collect time samples to infer fs.
  std::vector<double> time_values;
  if (has_time && fs_hz_ <= 0.0) time_values.reserve(2048);

  std::string line;
  while (std::getline(f, line)) {
    ++lineno;
    std::string t = trim(line);
    if (is_comment_or_empty(t)) continue;

    auto vals = split(t, delim);
    if (vals.size() != cols.size()) {
      throw std::runtime_error("CSV: column count mismatch at line " + std::to_string(lineno) +
                               " (expected " + std::to_string(cols.size()) + ", got " +
                               std::to_string(vals.size()) + ")");
    }

    if (has_time && fs_hz_ <= 0.0) {
      time_values.push_back(to_double(vals[0]));
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

  // Sampling rate: either provided explicitly or inferred from a time column.
  if (fs_hz_ > 0.0) {
    rec.fs_hz = fs_hz_;
  } else {
    if (!has_time) {
      throw std::runtime_error(
          "CSVReader: fs_hz must be provided (>0) unless the first column is a time axis (e.g., 'time,Ch1,Ch2')");
    }
    if (time_values.size() < 2) {
      throw std::runtime_error("CSV: need at least 2 time samples to infer fs");
    }

    std::vector<double> dts;
    dts.reserve(time_values.size() - 1);
    for (size_t i = 1; i < time_values.size(); ++i) {
      const double dt = time_values[i] - time_values[i - 1];
      if (dt > 0.0 && std::isfinite(dt)) dts.push_back(dt);
    }
    if (dts.empty()) {
      throw std::runtime_error("CSV: time column must be strictly increasing to infer fs");
    }

    double dt_med = median_inplace(&dts);
    if (!(dt_med > 0.0) || !std::isfinite(dt_med)) {
      throw std::runtime_error("CSV: failed to infer dt from time column");
    }

    // Units:
    // - If the header name hints at ms, use that.
    // - Otherwise, use a pragmatic EEG-specific heuristic: if dt is > 1.0, the
    //   file is likely using milliseconds (dt=4 for 250Hz, etc).
    double dt_sec = dt_med;
    if (time_in_ms || dt_med > 1.0) dt_sec = dt_med / 1000.0;

    if (!(dt_sec > 0.0) || !std::isfinite(dt_sec)) {
      throw std::runtime_error("CSV: invalid inferred dt_sec from time column");
    }

    rec.fs_hz = 1.0 / dt_sec;
    if (!(rec.fs_hz > 0.0) || !std::isfinite(rec.fs_hz)) {
      throw std::runtime_error("CSV: failed to infer fs from time column");
    }
  }

  return rec;
}

} // namespace qeeg
