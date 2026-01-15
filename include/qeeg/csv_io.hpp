#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal CSV helpers used by the CLI tools.
//
// Notes:
// - These helpers use comma (",") as the delimiter.
// - Numeric output uses the classic "C" locale (decimal point '.') to keep comma-delimited CSV parseable regardless of process locale.
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

// Write a BIDS-style events TSV with columns: onset\tduration\ttrial_type
//
// Notes:
// - Uses TAB as the delimiter.
// - Writes 'onset' and 'duration' in seconds, with fixed 6 decimal places.
// - Uses AnnotationEvent.text as the 'trial_type' column.
void write_events_tsv(const std::string& path, const std::vector<AnnotationEvent>& events);

// Convenience overload.
void write_events_tsv(const std::string& path, const EEGRecording& rec);

// Read an events table from a delimited text file (CSV/TSV).
//
// Supported formats:
// - qeeg events CSV written by write_events_csv:
//     onset_sec,duration_sec,text
// - BIDS-style events TSV:
//     onset\tduration\ttrial_type
// - Some locale-specific CSV exports may use ';' (or tab) as the delimiter,
//   often paired with a decimal comma in numeric columns (e.g. "0,5").
//
// The parser is intentionally forgiving:
// - Lines beginning with '#' are ignored.
// - Extra/short rows are padded/trimmed.
// - Column names are matched case-insensitively.
// - A UTF-8 BOM on the header line is tolerated.
//
// Returns a vector of AnnotationEvent (onset_sec, duration_sec, text).
// Invalid rows (missing/invalid onset) are skipped.
std::vector<AnnotationEvent> read_events_table(const std::string& path);

// Write a time-series CSV.
//
// Output format:
//   time,<ch1>,<ch2>,...
//   0.000000, ...
//
// If write_time_column=false, the header omits "time" and no time column is written.
void write_recording_csv(const std::string& path, const EEGRecording& rec, bool write_time_column = true);

} // namespace qeeg
