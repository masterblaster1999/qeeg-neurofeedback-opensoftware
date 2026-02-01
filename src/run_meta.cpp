#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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
  if (!i) return;
  while (*i < s.size() && std::isspace(static_cast<unsigned char>(s[*i])) != 0) {
    ++(*i);
  }
}

static bool parse_hex4(const std::string& s, size_t pos, unsigned* out) {
  if (!out) return false;
  if (pos + 4 > s.size()) return false;
  unsigned v = 0;
  for (size_t k = 0; k < 4; ++k) {
    const char c = s[pos + k];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<unsigned>(10 + (c - 'a'));
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<unsigned>(10 + (c - 'A'));
    } else {
      return false;
    }
  }
  *out = v;
  return true;
}

static void append_utf8(unsigned codepoint, std::string* out) {
  if (!out) return;

  // JSON \u escapes are UTF-16 code units. Surrogate codepoints are not valid
  // scalar values in UTF-8; treat them as replacement characters.
  if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
    codepoint = 0xFFFDu; // U+FFFD replacement
  }

  if (codepoint <= 0x7Fu) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFFu) {
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
          if (cp >= 0xD800u && cp <= 0xDBFFu) {
            // Look for a following low surrogate escape sequence: \uYYYY
            if ((*i + 6) <= s.size() && s[*i] == '\\' && s[*i + 1] == 'u') {
              unsigned low = 0;
              if (parse_hex4(s, *i + 2, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
                // Consume the second escape.
                *i += 6;
                const unsigned hi = cp;
                const unsigned codepoint = 0x10000u + ((hi - 0xD800u) << 10) + (low - 0xDC00u);
                append_utf8(codepoint, &r);
                break;
              }
            }
            // Invalid/missing low surrogate: replacement char.
            append_utf8(0xFFFDu, &r);
            break;
          }

          // Orphan low surrogate?
          if (cp >= 0xDC00u && cp <= 0xDFFFu) {
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

static bool find_value_pos_top_level(const std::string& s,
                                     const std::string& key,
                                     size_t* out_pos) {
  // Find the value position for a top-level JSON object member.
  //
  // This is intentionally lightweight and dependency-free. It is not a general
  // JSON parser. It is designed for small run-meta JSON files emitted by this
  // project.
  //
  // Behavior (best-effort):
  // - Only matches keys in the *top-level* object (depth 1).
  // - Ignores occurrences of keys inside JSON string values.
  int depth = 0;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];

    if (c == '"') {
      const size_t token_start = i;
      std::string tok;
      if (!parse_json_string(s, &i, &tok)) {
        // Malformed string: advance to avoid an infinite loop.
        i = token_start + 1;
        continue;
      }

      if (depth == 1 && tok == key) {
        size_t j = i;
        skip_ws(s, &j);
        if (j < s.size() && s[j] == ':') {
          ++j;
          skip_ws(s, &j);
          if (out_pos) *out_pos = j;
          return true;
        }
      }
      continue;
    }

    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth > 0) --depth;
    }

    ++i;
  }

  return false;
}

static std::string parse_string_value_at(size_t pos, const std::string& s) {
  size_t i = pos;
  skip_ws(s, &i);
  if (i >= s.size() || s[i] != '"') return std::string();
  std::string v;
  if (!parse_json_string(s, &i, &v)) return std::string();
  return v;
}

static std::vector<std::string> parse_string_array_at(size_t pos, const std::string& s) {
  std::vector<std::string> out;
  size_t i = pos;
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
  size_t pos = 0;
  if (!find_value_pos_top_level(s, "Input", &pos)) return std::string();
  size_t i = pos;
  skip_ws(s, &i);
  if (i >= s.size() || s[i] != '{') return std::string();

  const size_t end = find_matching_brace(s, i);
  if (end == std::string::npos || end <= i) return std::string();
  const std::string sub = s.substr(i, end - i);

  size_t p2 = 0;
  if (!find_value_pos_top_level(sub, "Path", &p2)) return std::string();
  return parse_string_value_at(p2, sub);
}

static std::string get_top_level_string(const std::string& s, const std::string& key) {
  size_t pos = 0;
  if (!find_value_pos_top_level(s, key, &pos)) return std::string();
  return parse_string_value_at(pos, s);
}

static std::vector<std::string> get_top_level_string_array(const std::string& s, const std::string& key) {
  size_t pos = 0;
  if (!find_value_pos_top_level(s, key, &pos)) return {};
  return parse_string_array_at(pos, s);
}

static bool normalize_output_rel_path(const std::string& raw, std::string* out_norm) {
  // Thin wrapper around qeeg::normalize_rel_path_safe so run_meta readers and
  // UI code share the exact same relative-path safety rules.
  return qeeg::normalize_rel_path_safe(raw, out_norm);
}

} // namespace

RunMetaSummary read_run_meta_summary(const std::string& json_path) {
  RunMetaSummary r;

  const std::string s = read_all(json_path);
  if (s.empty()) return r;

  r.tool = get_top_level_string(s, "Tool");
  r.timestamp_local = get_top_level_string(s, "TimestampLocal");
  r.timestamp_utc = get_top_level_string(s, "TimestampUTC");

  // Version: prefer QeegVersion, fallback to Version (nf_run_meta.json).
  r.version = get_top_level_string(s, "QeegVersion");
  if (r.version.empty()) {
    r.version = get_top_level_string(s, "Version");
  }

  r.git_describe = get_top_level_string(s, "GitDescribe");
  r.build_type = get_top_level_string(s, "BuildType");
  r.compiler = get_top_level_string(s, "Compiler");
  r.cpp_standard = get_top_level_string(s, "CppStandard");

  r.outputs = get_top_level_string_array(s, "Outputs");

  // Input path: supports multiple schemas.
  r.input_path = get_top_level_string(s, "InputPath");
  if (r.input_path.empty()) {
    r.input_path = get_top_level_string(s, "input_path");
  }
  if (r.input_path.empty()) {
    r.input_path = parse_input_path_from_input_object(s);
  }

  return r;
}

std::vector<std::string> read_run_meta_outputs(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return {};

  const std::vector<std::string> raw = get_top_level_string_array(s, "Outputs");
  if (raw.empty()) return {};

  std::vector<std::string> out;
  out.reserve(raw.size());
  for (const auto& r : raw) {
    std::string norm;
    if (normalize_output_rel_path(r, &norm)) {
      out.push_back(norm);
    }
  }
  return out;
}

std::string read_run_meta_tool(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "Tool");
}

std::string read_run_meta_timestamp_local(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "TimestampLocal");
}

std::string read_run_meta_timestamp_utc(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "TimestampUTC");
}

std::string read_run_meta_version(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();

  // Preferred: standard run_meta.cpp writer.
  const std::string v = get_top_level_string(s, "QeegVersion");
  if (!v.empty()) return v;

  // Fallback: nf_run_meta.json uses "Version".
  return get_top_level_string(s, "Version");
}

std::string read_run_meta_git_describe(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "GitDescribe");
}

std::string read_run_meta_build_type(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "BuildType");
}

std::string read_run_meta_compiler(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "Compiler");
}

std::string read_run_meta_cpp_standard(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();
  return get_top_level_string(s, "CppStandard");
}

std::string read_run_meta_input_path(const std::string& json_path) {
  const std::string s = read_all(json_path);
  if (s.empty()) return std::string();

  // Primary: run_meta.cpp writer schema.
  {
    const std::string v = get_top_level_string(s, "InputPath");
    if (!v.empty()) return v;
  }

  // Legacy: nf_run_meta.json uses "input_path".
  {
    const std::string v = get_top_level_string(s, "input_path");
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
  std::ostringstream out;

  auto write_string_or_null = [&](const std::string& s) {
    if (s.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(s) << "\"";
    }
  };

  out << "{\n";
  out << "  \"Tool\": \"" << json_escape(tool) << "\",\n";
  out << "  \"QeegVersion\": \"" << json_escape(qeeg::version_string()) << "\",\n";
  out << "  \"GitDescribe\": \"" << json_escape(qeeg::git_describe_string()) << "\",\n";
  out << "  \"BuildType\": \"" << json_escape(qeeg::build_type_string()) << "\",\n";
  out << "  \"Compiler\": \"" << json_escape(qeeg::compiler_string()) << "\",\n";
  out << "  \"CppStandard\": \"" << json_escape(qeeg::cpp_standard_string()) << "\",\n";
  out << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";
  out << "  \"TimestampUTC\": \"" << json_escape(now_string_utc()) << "\",\n";
  out << "  \"OutputDir\": \"" << json_escape(outdir) << "\",\n";
  out << "  \"InputPath\": ";
  write_string_or_null(input_path);
  out << ",\n";

  // Normalize and de-duplicate outputs defensively.
  //
  // Run meta "Outputs" entries are intended to be relative paths under OutputDir.
  // We normalize them to POSIX-style slashes and reject path traversal ("..") so
  // downstream tools and the UI can safely join OutputDir + output.
  std::vector<std::string> safe_outputs;
  safe_outputs.reserve(outputs.size());
  std::unordered_set<std::string> seen;
  seen.reserve(outputs.size());
  for (const auto& o : outputs) {
    std::string norm;
    if (!normalize_rel_path_safe(o, &norm)) {
      continue;
    }
    if (seen.insert(norm).second) {
      safe_outputs.push_back(norm);
    }
  }

  out << "  \"Outputs\": [\n";
  for (size_t i = 0; i < safe_outputs.size(); ++i) {
    out << "    \"" << json_escape(safe_outputs[i]) << "\"";
    if (i + 1 < safe_outputs.size()) out << ",";
    out << "\n";
  }

  out << "  ]\n";
  out << "}\n";
  return write_text_file_atomic(json_path, out.str());
}

} // namespace qeeg
