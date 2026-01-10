#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal CSV helpers used by the CLI tools.
//
// Notes:
// - These helpers use comma (",") as the delimiter.
// - They are intentionally dependency-light and only support what we need
//   for simple exports (events CSV + time-series CSV).

// Escape a string for inclusion in a CSV cell.
// - Quotes are doubled.
// - The cell is wrapped in quotes if it contains: comma, quote, or newline.
std::string csv_escape(const std::string& s);

// Write an events CSV with columns: onset_sec,duration_sec,text
void write_events_csv(const std::string& path, const std::vector<AnnotationEvent>& events);

// Convenience overload.
void write_events_csv(const std::string& path, const EEGRecording& rec);

// Write a time-series CSV.
//
// Output format:
//   time,<ch1>,<ch2>,...
//   0.000000, ...
//
// If write_time_column=false, the header omits "time" and no time column is written.
void write_recording_csv(const std::string& path, const EEGRecording& rec, bool write_time_column = true);

} // namespace qeeg
