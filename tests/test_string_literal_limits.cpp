// Prevent regressions of MSVC error C2026: "string too big, trailing characters truncated"
// by checking that no single raw string literal grows beyond a conservative limit.
//
// This test is intentionally lightweight and only scans the source tree. It does not
// depend on libqeeg.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#ifndef QEEG_SOURCE_DIR
#define QEEG_SOURCE_DIR "."
#endif

static std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::string s;
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n <= 0) return {};
  s.resize(static_cast<size_t>(n));
  in.seekg(0, std::ios::beg);
  in.read(&s[0], n);
  return s;
}

// Scan C++ raw string literals of the form:
//   R"DELIM( ... )DELIM"
// where DELIM may be empty.
static bool check_raw_string_literals(const fs::path& file, size_t limit, size_t* out_max) {
  const std::string text = read_file(file);
  if (text.empty()) {
    std::cerr << "Failed to read source file: " << file.u8string() << "\n";
    return false;
  }

  size_t max_len = 0;
  size_t pos = 0;
  while (true) {
    pos = text.find("R\"", pos);
    if (pos == std::string::npos) break;

    const size_t delim_start = pos + 2;
    const size_t paren = text.find('(', delim_start);
    if (paren == std::string::npos) break;

    const std::string delim = text.substr(delim_start, paren - delim_start);

    // The raw string literal terminator is ")<delim>\"".
    const std::string end = ")" + delim + "\"";
    const size_t end_pos = text.find(end, paren + 1);
    if (end_pos == std::string::npos) {
      // Not a well-formed raw string (or false positive). Move forward.
      pos = paren + 1;
      continue;
    }

    const size_t content_len = end_pos - (paren + 1);
    if (content_len > max_len) max_len = content_len;

    if (content_len > limit) {
      std::cerr << "Raw string literal too long in: " << file.u8string() << "\n";
      std::cerr << "  delimiter: '" << delim << "'\n";
      std::cerr << "  length:    " << content_len << "\n";
      std::cerr << "  limit:     " << limit << "\n";
      return false;
    }

    pos = end_pos + end.size();
  }

  if (out_max) *out_max = max_len;
  return true;
}

static bool is_cpp_text_file(const fs::path& p) {
  const auto ext = p.extension().u8string();
  return (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx");
}

int main() {
  // MSVC's per-literal limit is ~16380 single-byte characters; keep a margin.
  // (We keep this at 16000 to reduce the chance of hitting the limit due to
  // compiler-specific counting or accidental multi-byte expansions.)
  const size_t kLimit = 16000;

  const fs::path root = fs::path(QEEG_SOURCE_DIR);
  const fs::path src_dir = root / "src";
  const fs::path include_dir = root / "include";
  const fs::path tests_dir = root / "tests";

  // If sources aren't present (e.g., running tests from an installed package),
  // report and exit successfully instead of hard failing.
  if (!fs::exists(src_dir)) {
    std::cout << "Skipping raw string literal size check: missing source dir: " << src_dir.u8string() << "\n";
    return 0;
  }

  size_t global_max = 0;
  bool ok = true;

  auto scan_dir = [&](const fs::path& d) {
    if (!fs::exists(d)) return;
    for (const auto& ent : fs::recursive_directory_iterator(d)) {
      if (!ent.is_regular_file()) continue;
      const fs::path p = ent.path();
      if (!is_cpp_text_file(p)) continue;
      size_t max_len = 0;
      if (!check_raw_string_literals(p, kLimit, &max_len)) {
        ok = false;
        return;
      }
      if (max_len > global_max) global_max = max_len;
    }
  };

  scan_dir(src_dir);
  if (ok) scan_dir(include_dir);
  if (ok) scan_dir(tests_dir);

  if (!ok) return 1;

  std::cout << "Max raw string literal length: " << global_max << " (limit " << kLimit << ")\n";
  return 0;
}
