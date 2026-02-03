#pragma once

#include <cctype>
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

// Validate a CLI tool name coming from an untrusted source (e.g., UI server API).
//
// Accepts:
//   qeeg_*_cli         (POSIX-style)
//   qeeg_*_cli.exe     (Windows)
// Rejects:
//   any path separators, whitespace, dots, quotes, or other punctuation
//   qeeg_test_* tools
//
// This prevents path traversal like "qeeg_map_cli/../evil_cli" from escaping
// the configured --bin-dir when resolving the executable.
inline bool is_safe_qeeg_cli_tool_name(const std::string& tool) {
  std::string base = tool;
  if (ends_with(base, ".exe")) {
    base = base.substr(0, base.size() - 4);
  }
  if (base.empty()) return false;
  for (char c : base) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) continue;
    if (c == '_') continue;
    return false;
  }
  if (!starts_with(base, "qeeg_")) return false;
  if (ends_with(base, "_cli") == false) return false;
  if (starts_with(base, "qeeg_test_")) return false;
  return true;
}

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

// Write a text file to disk (UTF-8 bytes, best-effort).
//
// Returns true on success, false on failure.
//
// Notes:
// - Parent directories are created (best-effort).
// - The file is written in binary mode to avoid newline translation.
bool write_text_file(const std::string& path, const std::string& content);

// Atomically write a text file by writing to a temporary file in the same
// directory and renaming it into place (best-effort).
//
// This pattern reduces the chance that readers observe a partially-written
// JSON/HTML file (e.g., if the process crashes mid-write).
//
// Notes:
// - The temporary file is created in the destination directory so that the
//   rename is most likely to remain on the same filesystem.
// - On POSIX filesystems, rename within the same filesystem is typically
//   atomic; on Windows, std::filesystem::rename semantics vary, so this
//   function falls back to removing an existing destination and renaming
//   again (not perfectly atomic, but best-effort).
// - Any temporary file is removed on failure (best-effort).
bool write_text_file_atomic(const std::string& path, const std::string& content);

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

// Parse an ISO-8601 / RFC3339-style timestamp and convert it to UTC milliseconds
// since the Unix epoch (1970-01-01T00:00:00Z).
//
// Supported forms (best-effort):
//   - YYYY-MM-DDTHH:MM:SSZ
//   - YYYY-MM-DDTHH:MM:SS.sssZ
//   - YYYY-MM-DDTHH:MM:SS±HH:MM
//   - YYYY-MM-DDTHH:MM:SS.sss±HH:MM
//
// Notes:
// - Fractional seconds are truncated to milliseconds.
// - This is intentionally lightweight (no locale dependence, no DST rules).
//
// Returns true on success and writes the UTC timestamp to out_utc_ms.
bool parse_iso8601_to_utc_millis(const std::string& ts, int64_t* out_utc_ms);


// Escape a string for safe inclusion in JSON string values.
//
// This is a small helper intended for emitting lightweight JSON sidecars
// (e.g., run metadata) without pulling in a full JSON dependency.
// The returned string does NOT include surrounding quotes.
std::string json_escape(const std::string& s);

// Tiny JSON extractors for simple {"key": value}-style objects.
//
// These helpers are intentionally small and dependency-free. They are NOT a
// general JSON parser; they are intended for reading small JSON objects
// produced by this project (e.g., UI server request bodies).
//
// Behavior (best-effort):
// - Searches only the top-level object (depth 1).
// - Ignores occurrences of keys inside JSON string values.
// - For string values, supports standard JSON escapes including \uXXXX
//   sequences and UTF-16 surrogate pairs.
//
// Value semantics:
// - json_find_string_value(): returns empty string if missing or not a string
// - json_find_bool_value(): returns default_value if missing/unparseable;
//   accepts true/false, 1/0, and quoted variants like "yes"/"no"
// - json_find_int_value(): returns default_value if missing/unparseable;
//   accepts numbers or quoted numbers
std::string json_find_string_value(const std::string& s, const std::string& key);
bool json_find_bool_value(const std::string& s, const std::string& key, bool default_value);
int json_find_int_value(const std::string& s, const std::string& key, int default_value);

// Parse a top-level JSON array of strings.
//
// Example:
//   ["qeeg_map_cli","qeeg_topomap_cli"]
//
// Supports standard JSON string escapes including \uXXXX and UTF-16 surrogate
// pairs. Whitespace is permitted between tokens.
//
// Returns true on success and fills `out`. On failure, returns false and writes
// a best-effort error message into `err` (if provided).
bool json_parse_string_array(const std::string& s, std::vector<std::string>* out, std::string* err = nullptr);

// Percent-encode a URL path for safe use in HTML href/src attributes.
//
// This treats the input as UTF-8 bytes and encodes any byte that is not an
// RFC 3986 "unreserved" character or a forward slash '/'.
//
// Notes:
// - Existing '%' characters are encoded as "%25" to avoid accidental decoding.
// - This is intended for the URL *path* portion (not application/x-www-form-urlencoded).
// - Windows path separators ('\\') are normalized to '/'.
std::string url_encode_path(const std::string& path);

// Normalize and validate a relative path string for safe joining.
//
// Intended for run meta Outputs[] entries and UI-discovered artifacts.
// This helper is intentionally conservative: it rejects ".." traversal
// segments and Windows drive prefixes ("C:").
//
// Normalizations (best-effort):
// - trims leading/trailing whitespace
// - converts '\\' to '/' (to tolerate Windows-style paths)
// - strips leading '/' so "/abs" cannot be treated as an absolute path when joined
// - strips trailing '/' so directory paths like "outdir/" are accepted
// - lexically normalizes '.' segments (no filesystem access)
//
// On success, writes a normalized POSIX-style relative path (with '/' separators)
// to out_norm and returns true.
bool normalize_rel_path_safe(const std::string& raw, std::string* out_norm);


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
