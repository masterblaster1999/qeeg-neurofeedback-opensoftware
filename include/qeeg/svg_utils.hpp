#pragma once

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>

namespace qeeg {

// Minimal helpers for generating SVG/XML safely (dependency-free).
//
// - svg_escape(): escape text for XML element bodies / attributes.
// - url_escape(): percent-encode a filename/path fragment for use in href/src.
//   (Useful when linking to files that may contain spaces).

inline std::string svg_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

inline std::string url_escape(const std::string& s) {
  std::ostringstream oss;
  oss << std::hex;
  for (unsigned char c : s) {
    // Normalize Windows path separators to URL-style separators.
    //
    // Many qeeg tools generate links for local files (HTML reports, dashboards).
    // On Windows, std::filesystem::path::u8string() can yield native '\\'
    // separators, which can produce broken/odd URLs. Browsers and servers
    // expect '/' separators in URL paths.
    if (c == '\\') c = '/';

    const bool safe = (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~';
    if (safe) {
      oss << static_cast<char>(c);
    } else {
      oss << '%';
      const uint8_t hi = static_cast<uint8_t>((c >> 4) & 0xF);
      const uint8_t lo = static_cast<uint8_t>(c & 0xF);
      oss << static_cast<char>(hi < 10 ? ('0' + hi) : ('A' + (hi - 10)));
      oss << static_cast<char>(lo < 10 ? ('0' + lo) : ('A' + (lo - 10)));
    }
  }
  return oss.str();
}

} // namespace qeeg
