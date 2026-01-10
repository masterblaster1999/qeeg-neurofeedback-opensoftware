#include "qeeg/csv_io.hpp"

#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace qeeg {

std::string csv_escape(const std::string& s) {
  bool need = false;
  for (char c : s) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      need = true;
      break;
    }
  }
  if (!need) return s;

  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

void write_events_csv(const std::string& path, const std::vector<AnnotationEvent>& events) {
  std::ofstream o(path);
  if (!o) throw std::runtime_error("Failed to write events CSV: " + path);

  o << "onset_sec,duration_sec,text\n";
  o << std::fixed << std::setprecision(6);
  for (const auto& ev : events) {
    o << ev.onset_sec << "," << ev.duration_sec << "," << csv_escape(ev.text) << "\n";
  }
}

void write_events_csv(const std::string& path, const EEGRecording& rec) {
  write_events_csv(path, rec.events);
}

void write_recording_csv(const std::string& path, const EEGRecording& rec, bool write_time_column) {
  if (rec.fs_hz <= 0.0) {
    throw std::runtime_error("write_recording_csv: rec.fs_hz must be > 0");
  }
  if (rec.n_channels() == 0) {
    throw std::runtime_error("write_recording_csv: recording has no channels");
  }

  const size_t N = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != N) {
      throw std::runtime_error("write_recording_csv: channel length mismatch");
    }
  }

  std::ofstream o(path);
  if (!o) throw std::runtime_error("Failed to write CSV: " + path);

  // Header
  bool first = true;
  if (write_time_column) {
    o << "time";
    first = false;
  }
  for (const auto& name : rec.channel_names) {
    if (!first) o << ",";
    o << csv_escape(name);
    first = false;
  }
  o << "\n";

  o << std::fixed << std::setprecision(6);
  const double inv_fs = 1.0 / rec.fs_hz;

  for (size_t s = 0; s < N; ++s) {
    bool col_first = true;
    if (write_time_column) {
      o << (static_cast<double>(s) * inv_fs);
      col_first = false;
    }
    for (size_t ch = 0; ch < rec.n_channels(); ++ch) {
      if (!col_first) o << ",";
      o << rec.data[ch][s];
      col_first = false;
    }
    o << "\n";
  }
}

} // namespace qeeg
