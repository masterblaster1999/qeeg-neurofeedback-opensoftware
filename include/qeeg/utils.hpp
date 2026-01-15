#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qeeg {

std::string trim(const std::string& s);

// Remove a UTF-8 BOM (0xEF,0xBB,0xBF) from the beginning of a string if present.
// Many CSV exporters (notably some Windows tools) emit a BOM, which can break
// header parsing if not removed.
std::string strip_utf8_bom(std::string s);

std::vector<std::string> split(const std::string& s, char delim);

// Split a shell-style argument string into tokens.
//
// This is intended for lightweight UI/server integrations where the user
// provides a single "args" string (e.g., "--input a.edf --outdir out").
//
// Supported behaviors (best-effort):
// - Whitespace separates tokens.
// - Double and single quotes may be used to include whitespace.
// - Backslash escaping is supported for convenience when the user needs to
//   include whitespace or quotes in a token (e.g. "my\\ file.edf" or \"quoted\").
//   Backslashes that precede ordinary non-whitespace characters are preserved
//   (important for Windows-style paths like C:\\temp\\file.edf).
//
// This is NOT a full shell parser (no globbing, no env expansion, no nested
// quoting rules). It is intentionally conservative.
std::vector<std::string> split_commandline_args(const std::string& s);

// Join argv into a single Windows command line string suitable for CreateProcess.
//
// On Windows, processes receive a *single* command line string. Most C/C++
// runtimes then split it into argv with rules that treat backslashes specially
// when they precede a double quote. This helper implements a widely-used
// quoting strategy (compatible with the MSVC CRT rules) so that paths with
// spaces, quotes, and trailing backslashes are forwarded correctly.
//
// The returned string is UTF-8 and is intended to be passed to CreateProcessW
// after UTF-8->UTF-16 conversion.
std::string join_commandline_args_win32(const std::vector<std::string>& argv);

// Split a single CSV row into fields.
//
// This is a small, dependency-free parser intended for numeric EEG CSV data.
// It supports the most common RFC-4180 behaviors:
//  - fields may be quoted with double quotes
//  - delimiters inside quoted fields are preserved
//  - escaped quotes inside quoted fields are written as "" and are unescaped
//
// Limitations:
//  - does not support multi-line quoted fields (rows must be single-line)
//
// Notes:
//  - the returned fields are *unquoted* (surrounding quotes removed) and
//    unescaped ("" -> ") so that numeric parsing can operate on values like
//    "1.23".
std::vector<std::string> split_csv_row(const std::string& row, char delim);

// Convert a comma-delimited CSV file to a tab-delimited TSV file.
//
// - Uses split_csv_row() for RFC-4180 style parsing (single-line fields).
// - Output cells are unquoted/unescaped.
// - Any literal tab characters in cells are replaced with a single space.
//
// This is useful when exporting qeeg CSV tables into BIDS-style derivatives,
// which commonly prefer TSV for tabular data.
void convert_csv_file_to_tsv(const std::string& csv_path, const std::string& tsv_path);
std::string to_lower(std::string s);

// Normalize an EEG channel label for robust matching.
//
// Intended use cases:
// - match recording channel names to a montage
// - match CLI-specified channel names to recording channel names
//
// Current normalization steps (best-effort, dependency-free):
// - strip leading/trailing whitespace
// - lowercase
// - strip common reference suffixes like "-REF" / "_ref" / " reference"
// - tolerate common leading modality tokens like "EEG" (e.g., "EEG Fp1-Ref")
// - map a few common 10-20 legacy aliases (e.g. T3->T7, T4->T8, T5->P7, T6->P8)
//
// This is intentionally conservative; it should not attempt aggressive parsing
// that could cause surprising collisions.
std::string normalize_channel_name(std::string s);

bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

// Strict numeric parsing helpers.
//
// These functions trim leading/trailing whitespace and then require that the
// entire remaining string is a valid number (no trailing "abc" fragments).
//
// Notes:
// - to_double() primarily parses numbers using the classic "C" locale so that
//   '.' is treated as the decimal separator regardless of the user's global
//   locale.
// - As a convenience for some locales, to_double() also supports a single
//   decimal comma (e.g., "0,5") when no '.' is present.
int to_int(const std::string& s);
double to_double(const std::string& s);

bool file_exists(const std::string& path);
void ensure_directory(const std::string& path);

// Return a human-readable local timestamp string (best-effort).
//
// Format: ISO-8601 local time with numeric UTC offset, e.g.
//   2026-01-15T13:37:42-05:00
//
// Used for lightweight run metadata JSON and HTML reports.
std::string now_string_local();

// Return a human-readable UTC timestamp string (best-effort).
//
// Format: ISO-8601 UTC time, e.g.
//   2026-01-15T18:37:42Z
std::string now_string_utc();


// Escape a string for safe inclusion in JSON string values.
//
// This is a small helper intended for emitting lightweight JSON sidecars
// (e.g., run metadata) without pulling in a full JSON dependency.
// The returned string does NOT include surrounding quotes.
std::string json_escape(const std::string& s);

// Generate a random hexadecimal token (2*n_bytes characters).
//
// Intended for lightweight security mechanisms in local-only tooling (e.g.
// protecting a localhost API that can launch executables).
//
// Notes:
// - Uses std::random_device for entropy (best-effort).
// - Not intended as a general-purpose crypto library replacement.
std::string random_hex_token(size_t n_bytes = 16);


// HTTP helpers
//
// The built-in UI server supports lightweight HTTP features (e.g., Range) for
// downloading large files (EDF/BDF/zips) without pulling in a full HTTP stack.
//
// parse_http_byte_range() parses the value of a "Range" request header
// (RFC 9110) for a single "bytes" range, clamping it to the given resource
// size.
//
// Examples:
//   bytes=0-99    -> start=0, end=99
//   bytes=500-    -> start=500, end=size-1
//   bytes=-500    -> start=size-500, end=size-1 (suffix range)
//
// Notes:
// - Only a *single* range is supported. Multi-range headers (comma-separated)
//   return HttpRangeResult::kInvalid.
// - Range is only meaningful for size > 0.
//
// Return value semantics:
// - kNone: header missing/empty
// - kInvalid: syntactically invalid/unsupported
// - kUnsatisfiable: syntactically valid but outside the resource size
// - kSatisfiable: range parsed and clamped, with start/end outputs set
enum class HttpRangeResult {
  kNone = 0,
  kInvalid,
  kUnsatisfiable,
  kSatisfiable,
};

HttpRangeResult parse_http_byte_range(const std::string& range_header,
                                      uintmax_t resource_size,
                                      uintmax_t* out_start,
                                      uintmax_t* out_end);

} // namespace qeeg
