#include "qeeg/run_meta.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>

using namespace qeeg;

static void write_file(const std::string& path, const std::string& s) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("failed to write test file: " + path);
  }
  f << s;
}

int main() {
  try {
    const std::string path = "test_run_meta_tmp.json";
    const std::string json =
      "{\n"
      "  \"Tool\": \"qeeg_test_tool\",\n"
      "  \"Outputs\": [\n"
      "    \"a.csv\",\n"
      "    \"b\\\\\\\"c.txt\",\n"
      "    \"dir/sub.json\",\n"
      "    \"line\\nfeed.bin\"\n"
      "  ],\n"
      "  \"Other\": 123\n"
      "}\n";

    write_file(path, json);

    const std::string tool = read_run_meta_tool(path);
    assert(tool == "qeeg_test_tool");

    const std::vector<std::string> outs = read_run_meta_outputs(path);
    assert(outs.size() == 4);
    assert(outs[0] == "a.csv");
    assert(outs[1] == "b\"c.txt");
    assert(outs[2] == "dir/sub.json");
    assert(outs[3] == "line\nfeed.bin");

    std::cout << "test_run_meta: OK\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_run_meta failed: " << e.what() << "\n";
    return 1;
  }
}
