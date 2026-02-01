#include "qeeg/run_meta.hpp"

#include "test_support.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

int main() {
  try {
    const std::string path = "test_run_meta_write_tmp.json";
    const std::string tmp_prefix = path + ".tmp.";

    // Cleanup any leftovers from an interrupted run.
    {
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(".", ec)) {
        if (ec) break;
        const std::string name = e.path().filename().u8string();
        if (name == path || name.rfind(tmp_prefix, 0) == 0) {
          std::filesystem::remove(e.path(), ec);
          ec.clear();
        }
      }
    }

    const std::vector<std::string> outputs = {
      "a.csv",
      "b\"c.txt",
      "dir/sub.json",
      "line\nfeed.bin",
      "tab\tchar.dat",
    };

    const bool ok = write_run_meta_json(path,
                                       "qeeg_test_tool",
                                       "outdir",
                                       "input.edf",
                                       outputs);
    assert(ok);

    const std::string tool = read_run_meta_tool(path);
    assert(tool == "qeeg_test_tool");

    const std::string input = read_run_meta_input_path(path);
    assert(input == "input.edf");

    const std::vector<std::string> outs = read_run_meta_outputs(path);
    assert(outs.size() == outputs.size());
    for (size_t i = 0; i < outs.size(); ++i) {
      assert(outs[i] == outputs[i]);
    }

    // Ensure writer emitted provenance fields.
    const std::string ts_local = read_run_meta_timestamp_local(path);
    assert(!ts_local.empty());

    const std::string ts_utc = read_run_meta_timestamp_utc(path);
    assert(!ts_utc.empty());
    assert(ts_utc.back() == 'Z');

    const std::string ver = read_run_meta_version(path);
    assert(!ver.empty());

    const std::string gd = read_run_meta_git_describe(path);
    assert(!gd.empty());

    const std::string bt = read_run_meta_build_type(path);
    assert(!bt.empty());

    const std::string comp = read_run_meta_compiler(path);
    assert(!comp.empty());

    const std::string cs = read_run_meta_cpp_standard(path);
    assert(!cs.empty());

    // Atomic write behavior: ensure no temporary file is left behind.
    {
      size_t tmp_count = 0;
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(".", ec)) {
        if (ec) break;
        const std::string name = e.path().filename().u8string();
        if (name.rfind(tmp_prefix, 0) == 0) {
          ++tmp_count;
        }
      }
      assert(tmp_count == 0);
    }

    // Output normalization: writer should emit safe, normalized relative paths.
    {
      const std::string p4 = "test_run_meta_write_sanitize_tmp.json";
      const std::vector<std::string> outputs2 = {
        "subdir\\file.txt",   // normalize slashes
        "ok/./c.txt",         // collapse dot segments
        "folder/",            // strip trailing slash
        "../evil.txt",        // reject traversal
        "ok/../nope.txt",     // reject traversal (even if lexically normalizable)
        "C:\\secret.txt",     // reject drive prefix
        "/abs.txt",           // leading '/' is stripped to a safe relative path
        "dup.txt",
        "./dup.txt",          // normalizes to dup.txt (dedupe)
      };

      assert(write_run_meta_json(p4, "qeeg_test_tool", "outdir", "input.edf", outputs2));

      std::ifstream f(p4, std::ios::binary);
      assert(f.good());
      std::string s;
      {
        std::ostringstream oss;
        oss << f.rdbuf();
        s = oss.str();
      }

      // Normalized strings should be present.
      assert(s.find("subdir/file.txt") != std::string::npos);
      assert(s.find("ok/c.txt") != std::string::npos);
      assert(s.find("\"abs.txt\"") != std::string::npos);
      assert(s.find("\"folder\"") != std::string::npos);

      // Unsafe / non-normalized strings should not appear.
      assert(s.find("subdir\\\\file.txt") == std::string::npos);
      assert(s.find("ok/./c.txt") == std::string::npos);
      assert(s.find("\"folder/\"") == std::string::npos);
      assert(s.find("../evil.txt") == std::string::npos);
      assert(s.find("ok/../nope.txt") == std::string::npos);
      assert(s.find("C:\\") == std::string::npos);

      // Dedupe: dup.txt should appear only once in Outputs.
      const size_t first = s.find("\"dup.txt\"");
      assert(first != std::string::npos);
      const size_t second = s.find("\"dup.txt\"", first + 1);
      assert(second == std::string::npos);
    }

    // Also validate schema variants we need to support.
    {
      const std::string p2 = "test_run_meta_nested_tmp.json";
      std::ofstream f(p2);
      f << "{\n"
        << "  \"Tool\": \"qeeg_map_cli\",\n"
        << "  \"Input\": { \"Path\": \"nested.edf\" },\n"
        << "  \"Outputs\": []\n"
        << "}\n";
      f.close();
      assert(read_run_meta_input_path(p2) == "nested.edf");
    }

    {
      const std::string p3 = "test_run_meta_legacy_nf_tmp.json";
      std::ofstream f(p3);
      f << "{\n"
        << "  \"Tool\": \"qeeg_nf_cli\",\n"
        << "  \"input_path\": \"nf.edf\",\n"
        << "  \"Outputs\": []\n"
        << "}\n";
      f.close();
      assert(read_run_meta_input_path(p3) == "nf.edf");
    }

    std::cout << "test_run_meta_write: OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_run_meta_write failed: " << e.what() << "\n";
    return 1;
  }
}
