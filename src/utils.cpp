#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace qeeg {

static inline bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && is_space(s[b])) ++b;
  size_t e = s.size();
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string strip_utf8_bom(std::string s) {
  // UTF-8 BOM bytes: EF BB BF
  if (s.size() >= 3) {
    const unsigned char b0 = static_cast<unsigned char>(s[0]);
    const unsigned char b1 = static_cast<unsigned char>(s[1]);
    const unsigned char b2 = static_cast<unsigned char>(s[2]);
    if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
      return s.substr(3);
    }
  }
  return s;
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  // Handle trailing empty field
  if (!s.empty() && s.back() == delim) out.emplace_back("");
  return out;
}

std::vector<std::string> split_csv_row(const std::string& row, char delim) {
  // Minimal CSV parser for one physical line.
  //
  // Supports quoted fields with "" escaping.
  // This is intentionally permissive about whitespace after closing quotes.

  std::vector<std::string> out;
  std::string field;
  field.reserve(row.size());

  bool in_quotes = false;
  bool after_closing_quote = false;

  for (size_t i = 0; i < row.size(); ++i) {
    char c = row[i];

    // Common Windows line endings: getline() strips '\n' but not '\r'.
    if (!in_quotes && c == '\r') {
      // Ignore stray CR outside quotes.
      continue;
    }

    if (in_quotes) {
      if (c == '"') {
        // Escaped quote: ""
        if ((i + 1) < row.size() && row[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
          after_closing_quote = true;
        }
      } else {
        field.push_back(c);
      }
      continue;
    }

    // Not in quotes.
    if (after_closing_quote) {
      // RFC4180 requires next char to be delimiter or end-of-line; be tolerant
      // and allow whitespace before the delimiter.
      if (c == delim) {
        out.push_back(field);
        field.clear();
        after_closing_quote = false;
        continue;
      }
      if (is_space(c)) {
        continue;
      }
      // Unexpected char after closing quote: treat it as literal.
      after_closing_quote = false;
      field.push_back(c);
      continue;
    }

    if (c == delim) {
      out.push_back(field);
      field.clear();
      continue;
    }

    if (c == '"') {
      // Start quote if the field is empty or only whitespace.
      bool only_ws = field.empty();
      if (!only_ws) {
        only_ws = true;
        for (char fc : field) {
          if (!is_space(fc)) {
            only_ws = false;
            break;
          }
        }
      }
      if (only_ws) {
        field.clear();
        in_quotes = true;
        continue;
      }
      // Otherwise: quote inside unquoted field.
      field.push_back(c);
      continue;
    }

    field.push_back(c);
  }

  if (in_quotes) {
    throw std::runtime_error("split_csv_row: unterminated quoted field");
  }

  // Flush last field.
  out.push_back(field);
  return out;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

static std::string strip_common_ref_suffix(std::string s) {
  // Only remove "ref/reference" when it is clearly a suffix separated by a
  // delimiter, to avoid clobbering legitimate channel names.
  //
  // Examples: "F3-REF", "F3_ref", "F3 reference".
  const std::string t = trim(s);

  // Try longest suffixes first.
  const std::vector<std::string> suffixes = {
      "-reference",
      "_reference",
      " reference",
      "-ref",
      "_ref",
      " ref",
  };
  for (const auto& suf : suffixes) {
    if (ends_with(t, suf) && t.size() > suf.size()) {
      const std::string head = trim(t.substr(0, t.size() - suf.size()));
      if (!head.empty()) return head;
    }
  }
  return t;
}

std::string normalize_channel_name(std::string s) {
  // NOTE: We keep this intentionally conservative and dependency-free.
  // It is used for lookups and matching, not for displaying labels.

  // Remove BOM defensively in case labels come from CSV headers.
  s = strip_utf8_bom(std::move(s));
  s = trim(s);
  if (s.empty()) return s;

  // Lowercase first so suffix checks are case-insensitive.
  s = to_lower(std::move(s));
  s = strip_common_ref_suffix(std::move(s));

  // Common 10-20 legacy aliases.
  // Older: T3 T4 T5 T6; Newer: T7 T8 P7 P8.
  if (s == "t3") return "t7";
  if (s == "t4") return "t8";
  if (s == "t5") return "p7";
  if (s == "t6") return "p8";
  return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int to_int(const std::string& s) {
  try {
    size_t idx = 0;
    int v = std::stoi(trim(s), &idx, 10);
    if (idx == 0) throw std::invalid_argument("no digits");
    return v;
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse int from '" + s + "': " + e.what());
  }
}

double to_double(const std::string& s) {
  try {
    size_t idx = 0;
    double v = std::stod(trim(s), &idx);
    if (idx == 0) throw std::invalid_argument("no digits");
    return v;
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse double from '" + s + "': " + e.what());
  }
}

bool file_exists(const std::string& path) {
  return std::filesystem::exists(std::filesystem::u8path(path));
}

void ensure_directory(const std::string& path) {
  std::filesystem::create_directories(std::filesystem::u8path(path));
}

} // namespace qeeg
