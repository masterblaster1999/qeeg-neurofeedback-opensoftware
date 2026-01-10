#pragma once

#include "qeeg/utils.hpp"

#include <regex>
#include <stdexcept>
#include <string>

namespace qeeg {

// Simple glob-style matching supporting:
//  - '*' : matches any sequence (including empty)
//  - '?' : matches exactly one character
//
// This is intended for lightweight CLI filtering (e.g., event text).
// For full regular expressions, use compile_regex + std::regex_search.
inline bool wildcard_match(const std::string& text_in,
                           const std::string& pattern_in,
                           bool case_sensitive) {
  std::string text = text_in;
  std::string pattern = pattern_in;
  if (!case_sensitive) {
    text = to_lower(std::move(text));
    pattern = to_lower(std::move(pattern));
  }

  size_t t = 0;
  size_t p = 0;
  size_t star = std::string::npos;
  size_t match = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      ++p;
      match = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      ++match;
      t = match;
    } else {
      return false;
    }
  }

  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

// Compile a regular expression using ECMAScript syntax.
// If case_sensitive is false, std::regex_constants::icase is enabled.
//
// Throws std::runtime_error with a user-friendly message on invalid patterns.
inline std::regex compile_regex(const std::string& pattern, bool case_sensitive) {
  try {
    auto flags = std::regex_constants::ECMAScript;
    if (!case_sensitive) flags |= std::regex_constants::icase;
    return std::regex(pattern, flags);
  } catch (const std::regex_error& e) {
    throw std::runtime_error("Invalid regex pattern: '" + pattern + "': " + e.what());
  }
}

inline bool regex_search(const std::string& text, const std::regex& re) {
  return std::regex_search(text, re);
}

} // namespace qeeg
