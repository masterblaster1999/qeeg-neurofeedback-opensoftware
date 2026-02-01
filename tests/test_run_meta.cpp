#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

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

    
    // Regression: ensure key lookup doesn't match "Outputs" text inside JSON string values.
    //
    // A naive substring search for "\"Outputs\"" can match the escaped quotes
    // inside "Note", then incorrectly parse the fake array in the string.
    {
      const std::string path2 = "test_run_meta_key_match_tmp.json";
      const std::string json2 = R"JSON({
  "Tool": "qeeg_test_tool",
  "Note": "This string mentions \"Outputs\": [\"fake.csv\"] and should not affect parsing.",
  "Outputs": [
    "real.csv"
  ]
}
)JSON";
      write_file(path2, json2);

      const std::vector<std::string> outs2 = read_run_meta_outputs(path2);
      assert(outs2.size() == 1);
      assert(outs2[0] == "real.csv");
    }







    // Regression: ensure key lookup is restricted to the top-level object.
    //
    // A run meta file can contain nested objects that may also include keys like
    // "Tool" or "Outputs". Readers should prefer the top-level values.
    {
      const std::string path3 = "test_run_meta_nested_key_tmp.json";
      const std::string json3 = R"JSON({
  "Nested": {"Outputs": ["fake.csv"], "Tool": "fake_tool"},
  "Tool": "qeeg_test_tool",
  "Outputs": [
    "real.csv"
  ]
}
)JSON";
      write_file(path3, json3);

      assert(read_run_meta_tool(path3) == "qeeg_test_tool");
      const std::vector<std::string> outs3 = read_run_meta_outputs(path3);
      assert(outs3.size() == 1);
      assert(outs3[0] == "real.csv");
    }

    // Output path safety: read_run_meta_outputs() should ignore traversal and
    // absolute/drive-prefixed entries.
    {
      const std::string path4 = "test_run_meta_outputs_sanitize_tmp.json";
      const std::string json4 = R"JSON({
  "Tool": "qeeg_test_tool",
  "Outputs": [
    "ok.csv",
    "ok_dir/",
    "../escape.csv",
    "dir/../escape2.csv",
    "/leading/slash.csv",
    "dir\\file name.txt",
    "C:\\temp\\evil.csv",
    "D:evil.csv",
    "\u0000bad.csv"
  ]
}
)JSON";
      write_file(path4, json4);

      const std::vector<std::string> outs4 = read_run_meta_outputs(path4);
      assert(outs4.size() == 4);
      assert(outs4[0] == "ok.csv");
      assert(outs4[1] == "ok_dir");
      assert(outs4[2] == "leading/slash.csv");
      assert(outs4[3] == "dir/file name.txt");
    }


    // Shared relative-path normalizer (used for Outputs[] safety + UI links).
    {
      std::string norm;
      assert(normalize_rel_path_safe("./a/b", &norm) && norm == "a/b");
      assert(normalize_rel_path_safe("dir\\file name.txt", &norm) && norm == "dir/file name.txt");
      assert(normalize_rel_path_safe("/leading/slash.csv", &norm) && norm == "leading/slash.csv");
      assert(normalize_rel_path_safe("a/b/", &norm) && norm == "a/b");
      assert(!normalize_rel_path_safe("../escape.csv", &norm));
      assert(!normalize_rel_path_safe("dir/../escape2.csv", &norm));
      assert(!normalize_rel_path_safe("C:evil.csv", &norm));
      assert(!normalize_rel_path_safe(".", &norm));
    }

    // Tiny JSON extractor helpers (used by qeeg_ui_server_cli request parsing).
    {
      const std::string s = R"JSON({
  "dir": "top",
  "show_hidden": true,
  "desc": "no",
  "max_results": "123",
  "nested": {"dir": "nested"},
  "note": "this string mentions \"dir\": \"fake\" and should not affect parsing",
  "emoji": "\uD83D\uDE00"
})JSON";

      // Top-level keys only (nested.dir should not shadow dir).
      assert(json_find_string_value(s, "dir") == "top");
      assert(json_find_bool_value(s, "show_hidden", false) == true);
      assert(json_find_bool_value(s, "desc", true) == false); // "no" -> false
      assert(json_find_int_value(s, "max_results", 0) == 123);

      // Missing/invalid => defaults.
      assert(json_find_string_value(s, "missing") == "");
      assert(json_find_bool_value(s, "missing", true) == true);
      assert(json_find_int_value(s, "missing", 7) == 7);

      // Surrogate pair decode.
      assert(json_find_string_value(s, "emoji") == std::string("\xF0\x9F\x98\x80"));
    }

    // URL path encoding helper.
    {
      assert(url_encode_path("a/b c.txt") == "a/b%20c.txt");
      // Normalize Windows separators.
      assert(url_encode_path("dir\\file name.txt") == "dir/file%20name.txt");
      // '%' must be encoded so browsers don't misinterpret it as an escape prefix.
      assert(url_encode_path("100%") == "100%25");
    }

std::cout << "test_run_meta: OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_run_meta failed: " << e.what() << "\n";
    return 1;
  }
}
