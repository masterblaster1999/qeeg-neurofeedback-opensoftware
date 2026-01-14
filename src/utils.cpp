#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace qeeg {

static inline bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

static inline bool is_alnum(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
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

std::vector<std::string> split_commandline_args(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  cur.reserve(s.size());

  bool in_single = false;
  bool in_double = false;

  auto flush = [&]() {
    if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  };

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];

    // Quote handling (best-effort, dependency-free).
    //
    // Single quotes: treat everything literally until the next '.
    // Double quotes: treat everything literally until the next ".
    //
    // Backslash behavior is intentionally conservative:
    // - Outside quotes, backslash is only treated as an escape when it
    //   precedes whitespace or a quote character. Otherwise it is preserved.
    //   This makes Windows-style paths like C:\\temp\\file.edf work without
    //   requiring the user to double-escape every backslash.
    // - Inside double quotes, backslash may escape ", \\, or whitespace.
    // - Inside single quotes, backslash is always literal (common shell rule).
    if (c == '\\' && !in_single) {
      if (i + 1 < s.size()) {
        const char n = s[i + 1];

        const bool n_is_space = is_space(n);
        const bool n_is_quote = (n == '\'' || n == '"');
        const bool n_is_backslash = (n == '\\');

        bool do_escape = false;
        if (in_double) {
          // In double quotes, allow escaping of quotes/backslash/whitespace.
          do_escape = n_is_space || n == '"' || n_is_backslash;
        } else {
          // Outside quotes, only escape whitespace or quotes.
          do_escape = n_is_space || n_is_quote;
        }

        if (do_escape) {
          // Consume the escaped character.
          ++i;
          cur.push_back(n);
          continue;
        }
      }

      // Default: preserve the backslash literally.
      cur.push_back('\\');
      continue;
    }

    if (!in_double && c == '\'') {
      in_single = !in_single;
      continue;
    }
    if (!in_single && c == '"') {
      in_double = !in_double;
      continue;
    }

    if (!in_single && !in_double && is_space(c)) {
      flush();
      continue;
    }

    cur.push_back(c);
  }

  flush();
  return out;
}

std::string random_hex_token(size_t n_bytes) {
  if (n_bytes == 0) n_bytes = 16;
  static const char* kHex = "0123456789abcdef";

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);

  std::string out;
  out.reserve(n_bytes * 2);
  for (size_t i = 0; i < n_bytes; ++i) {
    const int b = dist(gen);
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
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

void convert_csv_file_to_tsv(const std::string& csv_path, const std::string& tsv_path) {
  std::ifstream in(csv_path, std::ios::binary);
  if (!in) throw std::runtime_error("Failed to open CSV: " + csv_path);

  std::ofstream out(tsv_path, std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write TSV: " + tsv_path);

  std::string line;
  bool first_line = true;
  while (std::getline(in, line)) {
    if (first_line) {
      line = strip_utf8_bom(line);
      first_line = false;
    }

    const std::vector<std::string> fields = split_csv_row(line, ',');
    for (size_t i = 0; i < fields.size(); ++i) {
      std::string cell = fields[i];
      // TSV delimiter safety: replace any literal tab characters inside cells.
      for (char& c : cell) {
        if (c == '\t') c = ' ';
      }
      out << cell;
      if (i + 1 < fields.size()) out << '\t';
    }
    out << '\n';
  }
}


std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string normalize_channel_name(std::string s) {
  // NOTE: We keep this intentionally conservative and dependency-free.
  // It is used for lookups and matching, not for displaying labels.

  // Remove BOM defensively in case labels come from CSV headers.
  s = strip_utf8_bom(std::move(s));
  s = trim(s);
  if (s.empty()) return s;

  // Lowercase first so prefix/suffix checks are case-insensitive.
  s = to_lower(std::move(s));

  // Many EEG formats (notably EDF) store labels like "EEG Fp1-REF".
  // For robust matching against montages and CLI user input, we build an
  // alphanumeric-only key and then strip common modality prefixes/suffixes.
  std::string t;
  t.reserve(s.size());
  for (char c : s) {
    if (is_alnum(c)) t.push_back(c);
  }
  if (t.empty()) return t;

  // Strip common leading modality tokens.
  // Order matters: strip longer prefixes first.
  //
  // NOTE: Some devices use labels like "EEG1"/"EEG2" (digits immediately after the prefix).
  // In those cases we *do not* strip the prefix, since it would collapse distinct channels
  // down to bare digits and break matching/uniqueness.
  const std::vector<std::string> prefixes = {"seeg", "ieeg", "eeg"};
  for (const auto& p : prefixes) {
    if (starts_with(t, p) && t.size() > p.size()) {
      const char next = t[p.size()];
      if (std::isalpha(static_cast<unsigned char>(next)) != 0) {
        t = t.substr(p.size());
      }
      break;
    }
  }

  // Strip common reference suffixes.
  const std::vector<std::string> suffixes = {"reference", "ref"};
  for (const auto& suf : suffixes) {
    if (ends_with(t, suf) && t.size() > suf.size()) {
      t = t.substr(0, t.size() - suf.size());
      break;
    }
  }

  if (t.empty()) return t;

  // Common 10-20 legacy aliases.
  // Older: T3 T4 T5 T6; Newer: T7 T8 P7 P8.
  if (t == "t3") return "t7";
  if (t == "t4") return "t8";
  if (t == "t5") return "p7";
  if (t == "t6") return "p8";
  return t;
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

std::string now_string_local() {
  std::time_t t = std::time(nullptr);
  std::string s = std::ctime(&t);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  return s;
}

std::string now_string_utc() {
  std::time_t t = std::time(nullptr);
  std::tm* tm = std::gmtime(&t);
  if (!tm) return std::string();
  std::ostringstream oss;
  oss << std::put_time(tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string json_escape(const std::string& s) {
  // Minimal JSON string escape.
  //
  // Notes:
  // - We assume the input is valid UTF-8 if it contains non-ASCII bytes.
  // - Control characters are escaped; other bytes are preserved.
  std::ostringstream oss;
  oss << std::hex << std::uppercase;
  for (unsigned char uc : s) {
    const char c = static_cast<char>(uc);
    switch (c) {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (uc < 0x20) {
          // JSON requires control chars to be escaped.
          oss << "\\u" << std::setw(4) << std::setfill('0') << static_cast<int>(uc);
          // Reset stream state for subsequent formatting.
          oss << std::setw(0) << std::setfill(' ');
        } else {
          oss << c;
        }
        break;
    }
  }
  return oss.str();
}

} // namespace qeeg
