#include "qeeg/cli_input.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// NOTE:
//   We intentionally avoid <cassert>'s assert() for test conditions. Many
//   builds (including CMake Release) compile with -DNDEBUG, which compiles
//   assert() away entirely. That can lead to tests that do nothing in Release
//   *and* to "unused variable" warnings that become hard errors under
//   -Werror.
static void test_check(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::cerr << "Test failure: " << expr << " (" << file << ":" << line << ")\n";
    std::exit(1);
  }
}

#define TEST_CHECK(expr) test_check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

static std::filesystem::path make_temp_dir(const std::string& prefix) {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path();
  const fs::path dir = base / (prefix + qeeg::random_hex_token(8));
  fs::create_directories(dir);
  return dir;
}

static void write_file(const std::filesystem::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary);
  TEST_CHECK(static_cast<bool>(out));
  out << content;
  TEST_CHECK(static_cast<bool>(out));
}

int main() {
  using namespace qeeg;
  namespace fs = std::filesystem;

  // Table resolver selector tests.
  fs::path d1 = make_temp_dir("qeeg_cli_input_table_");
  try {
    const fs::path bandpowers = d1 / "bandpowers.csv";
    const fs::path bandratios = d1 / "bandratios.csv";

    write_file(bandpowers, "channel,alpha\nFz,1.0\n");
    write_file(bandratios, "channel,theta_beta\nFz,2.0\n");

    ResolveInputTableOptions opt;
    opt.preferred_filenames = {"bandpowers.csv"};
    opt.allow_any = true;

    // Even though preferences point at bandpowers, selector should win.
    {
      const auto r = resolve_input_table_path(d1.u8string() + "#bandratios.csv", opt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "bandratios.csv");
    }

    // Glob selector should work.
    {
      const auto r = resolve_input_table_path(d1.u8string() + "#*powers*", opt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "bandpowers.csv");
    }

    // Mismatched selector against a direct file should throw.
    {
      bool threw = false;
      try {
        (void)resolve_input_table_path(bandpowers.u8string() + "#bandratios.csv", opt);
      } catch (const std::exception&) {
        threw = true;
      }
      TEST_CHECK(threw);
    }

  } catch (...) {
    std::error_code ec;
    fs::remove_all(d1, ec);
    throw;
  }
  {
    std::error_code ec;
    fs::remove_all(d1, ec);
  }

  // Generic file resolver selector tests.
  fs::path d2 = make_temp_dir("qeeg_cli_input_file_");
  try {
    const fs::path a = d2 / "a.edf";
    const fs::path b = d2 / "b.bdf";
    write_file(a, "dummy");
    write_file(b, "dummy");

    ResolveInputFileOptions fopt;
    fopt.allowed_extensions = {".edf", ".bdf"};
    fopt.allow_any = true;

    {
      const auto r = resolve_input_file_path(d2.u8string() + "#b.bdf", fopt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "b.bdf");
    }

    // Run-meta selection with selector.
    {
      const fs::path meta = d2 / "demo_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back(a.filename().u8string());
      outs.push_back(b.filename().u8string());
      TEST_CHECK(write_run_meta_json(meta.u8string(), "qeeg_test", d2.u8string(), "", outs));

      const auto r = resolve_input_file_path(meta.u8string() + "#*.edf", fopt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "a.edf");
    }

  } catch (...) {
    std::error_code ec;
    fs::remove_all(d2, ec);
    throw;
  }
  {
    std::error_code ec;
    fs::remove_all(d2, ec);
  }

  std::cout << "All tests passed.\n";
  return 0;
}
