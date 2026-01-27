#include "qeeg/run_meta.hpp"

#include "test_support.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

int main() {
  try {
    const std::string path = "test_run_meta_write_tmp.json";

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
