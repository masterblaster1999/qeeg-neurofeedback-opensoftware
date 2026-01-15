#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace qeeg {

namespace {

static std::string read_all(const std::string& path) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) return std::string();
  std::ostringstream oss;
  oss << f.rdbuf();
  return oss.str();
}

static void skip_ws(const std::string& s, size_t* i) {
  while (*i < s.size() && std::isspace(static_cast<unsigned char>(s[*i])) != 0) {
    ++(*i);
  }
}

static bool parse_hex4(const std::string& s, size_t pos, unsigned* out) {
  if (pos + 4 > s.size()) return false;
  unsigned v = 0;
  for (size_t k = 0; k < 4; ++k) {
    const char c = s[pos + k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(10 + (c - 'a'));
    else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(10 + (c - 'A'));
    else return false;
  }
  *out = v;
  return true;
}

static void append_utf8(unsigned codepoint, std::string* out) {
  if (!out) return;

  // JSON \u escapes are UTF-16 code units. Surrogate codepoints are not valid
  // scalar values in UTF-8; treat them as replacement characters.
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    codepoint = 0xFFFD; // U+FFFD replacement
  }

  if (codepoint <= 0x7F) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

static bool parse_json_string(const std::string& s, size_t* i, std::string* out) {
  if (!i || *i >= s.size() || s[*i] != '"') return false;
  ++(*i);
  std::string r;
  while (*i < s.size()) {
    const char c = s[*i];
    ++(*i);
    if (c == '"') {
      if (out) *out = r;
      return true;
    }
    if (c == '\\') {
      if (*i >= s.size()) return false;
      const char e = s[*i];
      ++(*i);
      switch (e) {
        case '"': r.push_back('"'); break;
        case '\\': r.push_back('\\'); break;
        case '/': r.push_back('/'); break;
        case 'b': r.push_back('\b'); break;
        case 'f': r.push_back('\f'); break;
        case 'n': r.push_back('\n'); break;
        case 'r': r.push_back('\r'); break;
        case 't': r.push_back('\t'); break;
        case 'u': {
          // \uXXXX (UTF-16 code unit). Combine surrogate pairs when present.
          unsigned cp = 0;
          if (!parse_hex4(s, *i, &cp)) return false;
          *i += 4;

          // High surrogate?
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // Look for a following low surrogate escape sequence: \uYYYY
            if ((*i + 6) <= s.size() && s[*i] == '\\' && s[*i + 1] == 'u') {
              unsigned low = 0;
              if (parse_hex4(s, *i + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                // Consume the second escape.
                *i += 6;
                const unsigned hi = cp;
                const unsigned codepoint =
                    0x10000u + ((hi - 0xD800u) << 10) + (low - 0xDC00u);
                append_utf8(codepoint, &r);
                break;
              }
            }
            // Invalid or missing low surrogate: append replacement char.
            append_utf8(0xFFFDu, &r);
            break;
          }

          // Orphan low surrogate?
          if (cp >= 0xDC00 && cp <= 0xDFFF) {
            append_utf8(0xFFFDu, &r);
            break;
          }

          append_utf8(cp, &r);
          break;
        }
        default:
          // Unknown escape: keep the literal char.
          r.push_back(e);
          break;
      }
      continue;
    }
    r.push_back(c);
  }
  return false;
}

static size_t find_key(const std::string& s, const std::string& key) {
  // Find first occurrence of "<key>" (including quotes).
  const std::string needle = "\"" + key + "\"";
  return s.find(needle);
}

static std::vector<std::string> parse_outputs_array_after(size_t pos,
                                                          const std::string& s) {
  std::vector<std::string> out;
  if (pos == std::string::npos) return out;

  size_t i = pos;
  // Seek to ':'
  i = s.find(':', i);
  if (i == std::string::npos) return out;
  ++i;
  skip_ws(s, &i);
  if (i >= s.size() || s[i] != '[') return out;
  ++i;

  while (i < s.size()) {
    skip_ws(s, &i);
    if (i >= s.size()) break;
    if (s[i] == ']') {
      ++i;
      break;
    }
    if (s[i] == ',') {
      ++i;
      continue;
    }
    if (s[i] != '"') {
      // Non-string entry; skip until next comma or closing bracket.
      while (i < s.size() && s[i] != ',' && s[i] != ']') ++i;
      continue;
    }
    std::string val;
    if (!parse_json_string(s, &i, &val)) break;
    out.push_back(val);
  }

  return out;
}

static std::string parse_string_value_after(size_t pos,
                                            const std::string& s) {
  if (pos == std::string::npos) return std::string();
  size_t i = pos;
  i = s.find(':', i);
  if (i == std::string::npos) return std::string();
  ++i;
  skip_ws(s, &i);
  if (i >= s.size() || s[i] != '"') return std::string();
  std::string v;
  if (!parse_json_string(s, &i, &v)) return std::string();
  return v;
}

static size_t find_matching_brace(const std::string& s, size_t open_pos) {
  if (open_pos >= s.size() || s[open_pos] != '{') return std::string::npos;
  int depth = 0;
  bool in_str = false;
  bool esc = false;

  for (size_t i = open_pos; i < s.size(); ++i) {
    const char c = s[i];
    if (in_str) {
      if (esc) {
        esc = false;
        continue;
      }
      if (c == '\\') {
        esc = true;
        continue;
      }
      if (c == '"') {
        in_str = false;
      }
      continue;
    }

    if (c == '"') {
      in_str = true;
      continue;
    }

    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) return i + 1;
    }
  }
  return std::string::npos;
}

static std::string parse_input_path_from_input_object(const std::string& s) {
  const size_t pos = find_key(s, "Input");
  if (pos == std::string::npos) return std::string();

  size_t i = s.find(':', pos);
  if (i == std::string::npos) return std::string();
  ++i;
  skip_ws(s, &i);
  if (i >= s.size() || s[i] != '{') return std::string();

  const size_t end = find_matching_brace(s, i);
  if (end == std::string::npos || end <= i) return std::string();
  const std::string sub = s.substr(i, end - i);

  const size_t p2 = find_key(sub, "Path");
  if (p2 == std::string::npos) return std::string();
  return parse_string_value_after(p2, sub);
}

} // namespace

std::vector<std::string> read_run_meta_outputs(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return {};
  const size_t pos = find_key(s, "Outputs");
  return parse_outputs_array_after(pos, s);
}

std::string read_run_meta_tool(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  const size_t pos = find_key(s, "Tool");
  return parse_string_value_after(pos, s);
}

std::string read_run_meta_input_path(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();

  // Primary: run_meta.cpp writer schema.
  {
    const size_t pos = find_key(s, "InputPath");
    const std::string v = parse_string_value_after(pos, s);
    if (!v.empty()) return v;
  }

  // Legacy: nf_run_meta.json uses "input_path".
  {
    const size_t pos = find_key(s, "input_path");
    const std::string v = parse_string_value_after(pos, s);
    if (!v.empty()) return v;
  }

  // Common: Input: { Path: ... }.
  return parse_input_path_from_input_object(s);
}



bool write_run_meta_json(const std::string& json_path,
                         const std::string& tool,
                         const std::string& outdir,
                         const std::string& input_path,
                         const std::vector<std::string>& outputs) {
  std::ofstream out(std::filesystem::u8path(json_path), std::ios::binary);
  if (!out) return false;

  auto write_string_or_null = [&](const std::string& s) {
    if (s.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(s) << "\"";
    }
  };

  out << "{\n";
  out << "  \"Tool\": \"" << json_escape(tool) << "\",\n";
  out << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";
  out << "  \"OutputDir\": \"" << json_escape(outdir) << "\",\n";
  out << "  \"InputPath\": ";
  write_string_or_null(input_path);
  out << ",\n";

  out << "  \"Outputs\": [\n";
  for (size_t i = 0; i < outputs.size(); ++i) {
    out << "    \"" << json_escape(outputs[i]) << "\"";
    if (i + 1 < outputs.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return true;
}

} // namespace qeeg
