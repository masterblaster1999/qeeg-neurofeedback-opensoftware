#include "qeeg/ui_dashboard.hpp"

#include "qeeg/utils.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

struct Args {
  std::string root;
  std::string output_html;
  std::string bin_dir;
  std::string title{"QEEG Tools UI"};

  bool embed_help{true};
  bool scan_bin_dir{true};
  bool scan_run_meta{true};
  bool open_after{false};
};

static void print_help() {
  std::cout
    << "qeeg_ui_cli\n\n"
    << "Generate a self-contained HTML dashboard that integrates all qeeg_*_cli executables\n"
    << "into one navigable UI (tool list + optional embedded --help + optional run-manifest scan).\n\n"
    << "Usage:\n"
    << "  qeeg_ui_cli --root <dir> [--output qeeg_ui.html] [--bin-dir <build/bin>] [--no-help] [--no-bin-scan]\n\n"
    << "Options:\n"
    << "  --root DIR          Root directory to scan for *_run_meta.json and use as link base (required).\n"
    << "  --output PATH       Output HTML path (default: <root>/qeeg_ui.html).\n"
    << "  --bin-dir DIR       Directory containing executables (used for embedding --help).\n"
    << "  --no-help           Do not embed tool --help outputs (faster / no exe lookup).\n"
    << "  --no-bin-scan       Do not auto-discover tools by scanning --bin-dir for qeeg_*_cli executables.\n"
    << "  --no-scan           Do not scan for *_run_meta.json outputs.\n"
    << "  --title TEXT        Page title (default: QEEG Tools UI).\n"
    << "  --open              Attempt to open the generated HTML in your default browser.\n"
    << "  -h, --help          Show this help.\n";
}

static std::filesystem::path self_dir(char** argv) {
  if (!argv || !argv[0]) return std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::path p = std::filesystem::u8path(argv[0]);
  if (p.empty()) return std::filesystem::current_path();
  if (p.is_relative()) {
    p = std::filesystem::absolute(p, ec);
  }
  if (ec) return std::filesystem::current_path();
  return p.parent_path();
}

static void try_open_browser(const std::filesystem::path& html_path) {
  const std::string p = html_path.u8string();
#if defined(_WIN32)
  // "start" is a shell builtin.
  std::string cmd = "cmd /c start \"\" \"" + p + "\"";
  std::system(cmd.c_str());
#elif defined(__APPLE__)
  std::string cmd = "open \"" + p + "\"";
  std::system(cmd.c_str());
#else
  std::string cmd = "xdg-open \"" + p + "\"";
  std::system(cmd.c_str());
#endif
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--root" && i + 1 < argc) {
      a.root = argv[++i];
    } else if (arg == "--output" && i + 1 < argc) {
      a.output_html = argv[++i];
    } else if (arg == "--bin-dir" && i + 1 < argc) {
      a.bin_dir = argv[++i];
    } else if (arg == "--no-help") {
      a.embed_help = false;
    } else if (arg == "--no-bin-scan") {
      a.scan_bin_dir = false;
    } else if (arg == "--no-scan") {
      a.scan_run_meta = false;
    } else if (arg == "--title" && i + 1 < argc) {
      a.title = argv[++i];
    } else if (arg == "--open") {
      a.open_after = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    if (a.root.empty()) {
      std::cerr << "qeeg_ui_cli: --root is required (see --help)\n";
      return 2;
    }

    // Default output: <root>/qeeg_ui.html
    if (a.output_html.empty()) {
      const std::filesystem::path root = std::filesystem::u8path(a.root);
      a.output_html = (root / "qeeg_ui.html").u8string();
    }

    // Default bin-dir: directory containing this executable.
    if (a.bin_dir.empty() && a.embed_help) {
      a.bin_dir = self_dir(argv).u8string();
    }

    qeeg::UiDashboardArgs u;
    u.root = a.root;
    u.output_html = a.output_html;
    u.bin_dir = a.bin_dir;
    u.embed_help = a.embed_help;
    u.scan_bin_dir = a.scan_bin_dir;
    u.scan_run_meta = a.scan_run_meta;
    u.title = a.title;

    qeeg::write_qeeg_tools_ui_html(u);

    std::cout << "Wrote UI dashboard: " << a.output_html << "\n";
    if (a.open_after) {
      try_open_browser(std::filesystem::u8path(a.output_html));
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
