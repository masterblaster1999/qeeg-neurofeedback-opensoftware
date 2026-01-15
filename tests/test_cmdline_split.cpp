#include "qeeg/utils.hpp"

#include <cassert>
#include <iostream>

int main() {
  using qeeg::split_commandline_args;
  using qeeg::join_commandline_args_win32;

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
    const auto v = split_commandline_args("--path C:\\temp\\file.txt");
    assert(v.size() == 2);
    assert(v[0] == "--path");
    // Windows-style paths should be preserved without requiring the caller to
    // double-escape every backslash.
    assert(v[1] == "C:\\temp\\file.txt");
  }

  {
    // Backslash escaping of whitespace should still work (useful in the UI server).
    const auto v = split_commandline_args("--input my\\ file.edf --outdir out");
    assert(v.size() == 4);
    assert(v[0] == "--input");
    assert(v[1] == "my file.edf");
    assert(v[2] == "--outdir");
    assert(v[3] == "out");
  }

  {
    // Explicitly empty quoted arguments should be preserved.
    const auto v = split_commandline_args("--flag \"\" --x '' end");
    assert(v.size() == 5);
    assert(v[0] == "--flag");
    assert(v[1].empty());
    assert(v[2] == "--x");
    assert(v[3].empty());
    assert(v[4] == "end");
  }

  {
    const auto v = split_commandline_args("\"\"");
    assert(v.size() == 1);
    assert(v[0].empty());
  }

  // Windows CreateProcess quoting helper (pure string logic, testable on any platform).
  {
    const std::vector<std::string> argv = {
        "C:\\Program Files\\QEEG\\tool.exe",
        "--input",
        "my file.edf",
    };
    const std::string cmd = join_commandline_args_win32(argv);
    assert(cmd == "\"C:\\Program Files\\QEEG\\tool.exe\" --input \"my file.edf\"");
  }

  {
    const std::vector<std::string> argv = {
        "tool.exe",
        "--name",
        "Alpha \"Peak\"",
    };
    const std::string cmd = join_commandline_args_win32(argv);
    assert(cmd == "tool.exe --name \"Alpha \\\"Peak\\\"\"");
  }

  {
    const std::vector<std::string> argv = {
        "tool.exe",
        "--empty",
        "",
    };
    const std::string cmd = join_commandline_args_win32(argv);
    assert(cmd == "tool.exe --empty \"\"");
  }

  {
    // Trailing backslashes need special handling when the arg is quoted.
    const std::vector<std::string> argv = {
        "tool.exe",
        "--dir",
        "C:\\path with space\\",
    };
    const std::string cmd = join_commandline_args_win32(argv);
    assert(cmd == "tool.exe --dir \"C:\\path with space\\\\\"");
  }

  std::cout << "ok\n";
  return 0;
}
