#include "qeeg/utils.hpp"

#include "test_support.hpp"

#include <iostream>

int main() {
  using qeeg::is_safe_qeeg_cli_tool_name;

  assert(is_safe_qeeg_cli_tool_name("qeeg_map_cli"));
  assert(is_safe_qeeg_cli_tool_name("qeeg_map_cli.exe"));
  assert(is_safe_qeeg_cli_tool_name("qeeg_loreta_metrics_cli"));
  assert(!is_safe_qeeg_cli_tool_name(""));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_test_map_cli"));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli/../evil_cli"));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli\\..\\evil_cli"));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli --help"));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli.."));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli.exe.exe"));
  assert(!is_safe_qeeg_cli_tool_name("qeeg_map_cli\""));

  std::cout << "ok\n";
  return 0;
}
