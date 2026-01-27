#include "qeeg/run_meta.hpp"

#include "test_support.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>

using namespace qeeg;

static void write_file(const std::string& path, const std::string& s) {
  // Use std::filesystem::path so UTF-8 paths work on Windows too.
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    throw std::runtime_error("failed to write test file: " + path);
  }
  f << s;
}

int main() {
  try {
    // Exercise the run_meta writer/reader with a UTF-8 (non-ASCII) path.
    // This catches Windows issues where std::ifstream/ofstream opened with a
    // narrow std::string path can fail for non-ASCII filenames.
    {
      const std::string dir = std::string("test_run_meta_") + std::string("\xC2\xB5"); // "µ"
      const std::filesystem::path dir_p = std::filesystem::u8path(dir);
      std::error_code ec;
      std::filesystem::remove_all(dir_p, ec);
      ec.clear();
      std::filesystem::create_directories(dir_p, ec);
      if (ec) throw std::runtime_error("failed to create temp dir");

      const std::filesystem::path meta_p = dir_p / std::filesystem::u8path(std::string("meta_") + std::string("\xC2\xB5") + ".json");
      const std::string meta = meta_p.u8string();

      const std::vector<std::string> outs = {"a.csv", "b.txt"};
      const bool ok = write_run_meta_json(meta, "qeeg_test_tool", dir, "input.csv", outs);
      assert(ok);

      assert(read_run_meta_tool(meta) == "qeeg_test_tool");
      assert(read_run_meta_input_path(meta) == "input.csv");
      const auto got = read_run_meta_outputs(meta);
      assert(got.size() == outs.size());
      assert(got[0] == outs[0]);
      assert(got[1] == outs[1]);

      std::filesystem::remove_all(dir_p, ec);
    }

    const std::string path = "test_run_meta_tmp.json";

    // Also exercise JSON \u escapes (including surrogate pairs).
    //
    // - \u00B5 => U+00B5 MICRO SIGN (UTF-8: C2 B5)
    // - \uD83D\uDE00 => U+1F600 GRINNING FACE (UTF-8: F0 9F 98 80)
    const std::string json = R"JSON({
  "Tool": "qeeg_test_tool",
  "Outputs": [
    "a.csv",
    "b\"c.txt",
    "dir/sub.json",
    "line\nfeed.bin",
    "\u00B5.txt",
    "\uD83D\uDE00.txt"
  ],
  "Other": 123
}
)JSON";

    write_file(path, json);

    const std::string tool = read_run_meta_tool(path);
    assert(tool == "qeeg_test_tool");

    const std::vector<std::string> outs = read_run_meta_outputs(path);
    assert(outs.size() == 6);
    assert(outs[0] == "a.csv");
    assert(outs[1] == "b\"c.txt");
    assert(outs[2] == "dir/sub.json");
    assert(outs[3] == "line\nfeed.bin");
    assert(outs[4] == std::string("\xC2\xB5.txt"));
    assert(outs[5] == std::string("\xF0\x9F\x98\x80.txt"));

    std::cout << "test_run_meta: OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_run_meta failed: " << e.what() << "\n";
    return 1;
  }
}
