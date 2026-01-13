#include "qeeg/run_meta.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace qeeg {

namespace {

static std::string read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
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
          unsigned cp = 0;
          if (!parse_hex4(s, *i, &cp)) return false;
          *i += 4;
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

} // namespace qeeg
