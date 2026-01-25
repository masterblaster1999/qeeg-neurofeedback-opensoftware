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

  fs::path root = make_temp_dir("qeeg_cli_input_nested_meta_");
  try {
    const fs::path pre_dir = root / "01_preprocess";
    const fs::path bp_dir = root / "02_bandpower";

    fs::create_directories(pre_dir);
    fs::create_directories(bp_dir);

    // Step outputs.
    const fs::path pre_file = pre_dir / "preprocessed.m2k";
    const fs::path bp_file = bp_dir / "bandpowers.csv";

    write_file(pre_file, "time,Fz\n0.0,1.0\n");
    write_file(bp_file, "channel,alpha\nFz,1.0\n");

    // Child run-meta files.
    const fs::path pre_meta = pre_dir / "preprocess_run_meta.json";
    {
      std::vector<std::string> outs;
      outs.push_back(pre_file.filename().u8string());
      outs.push_back(pre_meta.filename().u8string());
      TEST_CHECK(write_run_meta_json(pre_meta.u8string(), "qeeg_preprocess_cli", pre_dir.u8string(), "", outs));
    }

    const fs::path bp_meta = bp_dir / "bandpower_run_meta.json";
    {
      std::vector<std::string> outs;
      outs.push_back(bp_file.filename().u8string());
      outs.push_back(bp_meta.filename().u8string());
      TEST_CHECK(write_run_meta_json(bp_meta.u8string(), "qeeg_bandpower_cli", bp_dir.u8string(), "", outs));
    }

    // Top-level (pipeline-style) manifest listing nested run-meta files.
    const fs::path pipe_meta = root / "pipeline_run_meta.json";
    {
      std::vector<std::string> outs;
      outs.push_back(pipe_meta.filename().u8string());
      outs.push_back("01_preprocess/preprocess_run_meta.json");
      outs.push_back("02_bandpower/bandpower_run_meta.json");
      TEST_CHECK(write_run_meta_json(pipe_meta.u8string(), "qeeg_pipeline_cli", root.u8string(), "", outs));
    }

    // --- Table resolver: passing the workspace directory should resolve bandpowers.csv.
    ResolveInputTableOptions topt;
    topt.preferred_filenames = {"bandpowers.csv"};
    topt.allow_any = true;

    {
      const auto r = resolve_input_table_path(root.u8string(), topt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "bandpowers.csv");
    }

    // Table resolver: passing pipeline_run_meta.json should also resolve bandpowers.csv.
    {
      const auto r = resolve_input_table_path(pipe_meta.u8string(), topt);
      TEST_CHECK(fs::path(r.path).filename().u8string() == "bandpowers.csv");
    }

    // --- Recording resolver: passing the workspace directory should resolve the *recording* output.
    // In a pipeline workspace, that is typically the preprocess output (preprocessed.*), not a derived table.
    {
      const auto r = resolve_input_recording_path(root.u8string());
      TEST_CHECK(fs::path(r.path).filename().u8string() == "preprocessed.m2k");
    }

    // Recording resolver: passing pipeline_run_meta.json should also resolve preprocessed.m2k.
    {
      const auto r = resolve_input_recording_path(pipe_meta.u8string());
      TEST_CHECK(fs::path(r.path).filename().u8string() == "preprocessed.m2k");
    }

  } catch (...) {
    std::error_code ec;
    fs::remove_all(root, ec);
    throw;
  }

  {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  std::cout << "All tests passed.\n";
  return 0;
}
