#include "qeeg/utils.hpp"

#include <cassert>
#include <iostream>

int main() {
  using qeeg::split_commandline_args;

  {
    const auto v = split_commandline_args("--input a.edf --outdir out");
    assert(v.size() == 4);
    assert(v[0] == "--input");
    assert(v[1] == "a.edf");
    assert(v[2] == "--outdir");
    assert(v[3] == "out");
  }

  {
    const auto v = split_commandline_args("--name \"Alpha Peak\" --x '1 2 3'");
    assert(v.size() == 4);
    assert(v[0] == "--name");
    assert(v[1] == "Alpha Peak");
    assert(v[2] == "--x");
    assert(v[3] == "1 2 3");
  }

  {
    const auto v = split_commandline_args("--path C:\\\\temp\\\\file.txt");
    assert(v.size() == 2);
    assert(v[0] == "--path");
    assert(v[1].find("temp") != std::string::npos);
  }

  std::cout << "ok\n";
  return 0;
}
