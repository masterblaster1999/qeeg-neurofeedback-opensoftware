#include "qeeg/ui_dashboard.hpp"

#include "test_support.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Very lightweight smoke test:
// - create fake run_meta files under a temp root
// - generate the dashboard (without embedding help)
// - verify it contains the tool name and a relative link to the *latest* run
//   based on TimestampUTC/TimestampLocal parsing.

static std::string read_all(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

static void write_min_run_meta(const std::filesystem::path& p,
                              const std::string& tool,
                              const std::string& timestamp_local,
                              const std::vector<std::string>& outputs) {
  std::ofstream f(p, std::ios::binary);
  f << "{\n";
  f << "  \"Tool\": \"" << tool << "\",\n";
  if (!timestamp_local.empty()) {
    f << "  \"TimestampLocal\": \"" << timestamp_local << "\",\n";
  }
  f << "  \"Outputs\": [";
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (i) f << ", ";
    f << "\"" << outputs[i] << "\"";
  }
  f << "]\n";
  f << "}\n";
}

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "qeeg_ui_dash_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "out_old");
  std::filesystem::create_directories(root / "out_new");
  std::filesystem::create_directories(root / "ui");

  const std::filesystem::path old_meta = root / "out_old" / "map_run_meta.json";
  const std::filesystem::path new_meta = root / "out_new" / "map_run_meta.json";

  // Create two runs for the same tool. The newer run has a newer TimestampLocal
  // but an *older* file mtime. The dashboard should pick it based on the
  // parsed timestamp rather than mtime.
  write_min_run_meta(old_meta,
                     "qeeg_map_cli",
                     // Older UTC time.
                     "2026-01-02T10:30:00+00:00",
                     std::vector<std::string>{"report.html", "bandpowers.csv"});

  write_min_run_meta(new_meta,
                     "qeeg_map_cli",
                     // Newer UTC time expressed with a numeric offset (tests offset parsing):
                     // 06:00 at UTC-05:00 == 11:00Z
                     "2026-01-02T06:00:00-05:00",
                     std::vector<std::string>{"report.html", "bandpowers.csv", "my file.txt", "missing.csv", "../escape.txt"});

  // Create dummy output files so links are meaningful.
  {
    std::ofstream(root / "out_old" / "report.html") << "<html>OLD</html>\n";
    std::ofstream(root / "out_old" / "bandpowers.csv") << "channel,alpha\nCz,1\n";

    std::ofstream(root / "out_new" / "report.html") << "<html>NEW</html>\n";
    std::ofstream(root / "out_new" / "bandpowers.csv") << "channel,alpha\nCz,2\n";
    std::ofstream(root / "out_new" / "my file.txt") << "hello world\n";
  }

  // Force mtimes so that the old run looks newer by mtime.
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(old_meta, now);
  std::filesystem::last_write_time(new_meta, now - std::chrono::hours(2));

  qeeg::UiDashboardArgs a;
  a.root = root.u8string();
  // Place the HTML under a subdirectory to ensure that dashboard link paths
  // (relative to the HTML) can include "../" segments while injected paths
  // remain workspace-root-relative.
  a.output_html = (root / "ui" / "qeeg_ui.html").u8string();
  a.embed_help = false;
  a.scan_run_meta = true;
  a.title = "Test UI";

  qeeg::write_qeeg_tools_ui_html(a);
// Atomic writer should not leave behind temporary files.
for (const auto& ent : std::filesystem::directory_iterator(root / "ui")) {
  const std::string fn = ent.path().filename().u8string();
  if (fn.find(".tmp.") != std::string::npos) {
    std::cerr << "Did not expect leftover temp file: " << fn << "\n";
    return 1;
  }
}


  const std::string html = read_all(root / "ui" / "qeeg_ui.html");
  if (html.find("qeeg_map_cli") == std::string::npos) {
    std::cerr << "Expected tool name in dashboard HTML\n";
    return 1;
  }

  // Should select out_new based on timestamp.
  if (html.find("../out_new/report.html") == std::string::npos) {
    std::cerr << "Expected discovered output link for latest run (out_new)\n";
    return 1;
  }
  if (html.find("out_old/report.html") != std::string::npos || html.find("../out_old/report.html") != std::string::npos) {
    std::cerr << "Did not expect output link from older run (out_old)\n";
    return 1;
  }

  // Injected paths should be workspace-root-relative (no "../"), even when
  // the dashboard HTML lives in a subdirectory.
  if (html.find("data-path=\"out_new/report.html\"") == std::string::npos) {
    std::cerr << "Expected injected data-path to be workspace-relative (out_new/report.html)\n";
    return 1;
  }
  if (html.find("data-path=\"../out_new/report.html\"") != std::string::npos) {
    std::cerr << "Did not expect injected data-path to contain ../ segments\n";
    return 1;
  }

  // URLs in href/src should be percent-encoded so that spaces and other
  // reserved characters work correctly in browsers.
  if (html.find("../out_new/my%20file.txt") == std::string::npos) {
    std::cerr << "Expected percent-encoded link for out_new/my file.txt\n";
    return 1;
  }

  // The dashboard should embed lightweight previews for CSV/text outputs so you
  // can sanity-check artifacts without opening them in another tool.
  if (html.find("channel,alpha") == std::string::npos || html.find("Cz,2") == std::string::npos) {
    std::cerr << "Expected CSV preview content from latest run (out_new)\n";
    return 1;
  }
  if (html.find("Cz,1") != std::string::npos) {
    std::cerr << "Did not expect CSV preview content from older run (out_old)\n";
    return 1;
  }

  // Unsafe/missing outputs should be surfaced without producing broken links or
  // escaping the dashboard root.
  if (html.find("../out_new/missing.csv") == std::string::npos || html.find("missing file") == std::string::npos) {
    std::cerr << "Expected missing-file marker for out_new/missing.csv\n";
    return 1;
  }
  if (html.find("../escape.txt") == std::string::npos || html.find("unsafe path") == std::string::npos) {
    std::cerr << "Expected unsafe-path marker for ../escape.txt\n";
    return 1;
  }

  std::cout << "test_ui_dashboard: OK\n";
  return 0;
}
