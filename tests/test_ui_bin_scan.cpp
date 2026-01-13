#include "qeeg/ui_dashboard.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static std::string read_all(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

// Smoke test:
// - create a fake "bin dir" containing an extra qeeg_*_cli executable file
// - generate the dashboard with scan_bin_dir enabled
// - verify the extra tool name appears in the HTML

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "qeeg_ui_binscan_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "bin");

  // Create a dummy executable file.
  const std::filesystem::path exe = root / "bin" / "qeeg_extra_cli";
  {
    std::ofstream f(exe, std::ios::binary);
    f << "#!/bin/sh\n";
    f << "echo qeeg_extra_cli help\n";
  }

  // Best-effort: mark as executable on POSIX.
  std::error_code ec;
  std::filesystem::permissions(
      exe,
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add,
      ec);

  qeeg::UiDashboardArgs a;
  a.root = root.u8string();
  a.output_html = (root / "qeeg_ui.html").u8string();
  a.bin_dir = (root / "bin").u8string();
  a.embed_help = false;       // don't execute dummy file during the test
  a.scan_bin_dir = true;
  a.scan_run_meta = false;

  qeeg::write_qeeg_tools_ui_html(a);

  const std::string html = read_all(root / "qeeg_ui.html");
  if (html.find("qeeg_extra_cli") == std::string::npos) {
    std::cerr << "Expected auto-discovered tool name in dashboard HTML\n";
    return 1;
  }

  std::cout << "test_ui_bin_scan: OK\n";
  return 0;
}
