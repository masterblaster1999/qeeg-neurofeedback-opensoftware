#pragma once

#include <string>

namespace qeeg {

// Options for generating the static QEEG Tools HTML dashboard.
//
// The dashboard is intended to integrate all project executables (CLI tools)
// into one navigable "UI" by:
//   - listing every qeeg_*_cli executable built by the project
//   - optionally embedding each tool's --help output
//   - optionally scanning a root directory for qeeg *_run_meta.json manifests
//     and linking to discovered outputs
struct UiDashboardArgs {
  // Directory to scan for *_run_meta.json files and use as the base for
  // relative links.
  std::string root;

  // Where to write the resulting HTML file.
  std::string output_html;

  // Directory that contains the built executables. Used only when embed_help
  // is true.
  std::string bin_dir;

  // If true, run each executable with --help and embed the output in the UI.
  bool embed_help{true};

  // If true and bin_dir is provided, scan the bin_dir for executables matching
  // the pattern qeeg_*_cli (or qeeg_*_cli.exe on Windows) and include them in
  // the dashboard.
  //
  // This complements the built-in curated tool list so that newly added tools
  // appear automatically without manual updates.
  bool scan_bin_dir{true};

  // If true, scan the root directory for *_run_meta.json files and surface
  // discovered outputs in the UI.
  bool scan_run_meta{true};

  // Page title.
  std::string title{"QEEG Tools UI"};
};

// Generate a single self-contained HTML dashboard.
// Throws std::runtime_error on failure.
void write_qeeg_tools_ui_html(const UiDashboardArgs& args);

} // namespace qeeg
