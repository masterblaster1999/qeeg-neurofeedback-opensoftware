#include "qeeg/ui_dashboard.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Very lightweight smoke test:
// - create a fake run_meta file under a temp root
// - generate the dashboard (without embedding help)
// - verify it contains the tool name and a relative link to an output

static std::string read_all(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "qeeg_ui_dash_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "out_map");

  // Create a minimal run manifest for qeeg_map_cli.
  {
    std::ofstream f(root / "out_map" / "map_run_meta.json", std::ios::binary);
    f << "{\n";
    f << "  \"Tool\": \"qeeg_map_cli\",\n";
    f << "  \"Outputs\": [\"report.html\", \"bandpowers.csv\"]\n";
    f << "}\n";
  }

  // Create dummy output files so links are meaningful.
  {
    std::ofstream(root / "out_map" / "report.html") << "<html></html>\n";
    std::ofstream(root / "out_map" / "bandpowers.csv") << "channel,alpha\nCz,1\n";
  }

  qeeg::UiDashboardArgs a;
  a.root = root.u8string();
  a.output_html = (root / "qeeg_ui.html").u8string();
  a.embed_help = false;
  a.scan_run_meta = true;
  a.title = "Test UI";

  qeeg::write_qeeg_tools_ui_html(a);

  const std::string html = read_all(root / "qeeg_ui.html");
  if (html.find("qeeg_map_cli") == std::string::npos) {
    std::cerr << "Expected tool name in dashboard HTML\n";
    return 1;
  }
  if (html.find("out_map/report.html") == std::string::npos) {
    std::cerr << "Expected discovered output link in dashboard HTML\n";
    return 1;
  }

  // The dashboard should embed lightweight previews for CSV/text outputs so you
  // can sanity-check artifacts without opening them in another tool.
  if (html.find("channel,alpha") == std::string::npos || html.find("Cz,1") == std::string::npos) {
    std::cerr << "Expected CSV preview content in dashboard HTML\n";
    return 1;
  }

  std::cout << "test_ui_dashboard: OK\n";
  return 0;
}
