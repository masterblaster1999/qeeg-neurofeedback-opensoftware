#pragma once

#include <string>
#include <vector>

namespace qeeg {

std::string trim(const std::string& s);

// Remove a UTF-8 BOM (0xEF,0xBB,0xBF) from the beginning of a string if present.
// Many CSV exporters (notably some Windows tools) emit a BOM, which can break
// header parsing if not removed.
std::string strip_utf8_bom(std::string s);

std::vector<std::string> split(const std::string& s, char delim);

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
std::string to_lower(std::string s);

bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

int to_int(const std::string& s);
double to_double(const std::string& s);

bool file_exists(const std::string& path);
void ensure_directory(const std::string& path);

} // namespace qeeg
