#include "qeeg/ui_dashboard.hpp"

#include "qeeg/utils.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string bin_dir;
  std::string outdir;

  bool embed_help{false};
  bool include_per_tool{false};
  bool tool_shims{true};

  bool open_after{true};
  std::string title{"QEEG Offline Tools"};
};

static void print_help() {
  std::cout
    << "qeeg_bundle_cli\n\n"
    << "Create a self-contained offline app folder for the QEEG tools UI.\n\n"
    << "What it does:\n"
    << "  - Copies executables into <outdir>/bin\n"
    << "  - (Default) Creates per-tool shims in <outdir>/bin so qeeg_*_cli names\n"
    << "    work even when you ship only qeeg_offline_app_cli\n"
    << "  - Generates <outdir>/runs/qeeg_ui.html (static dashboard)\n"
    << "  - Writes start scripts to launch the local UI server\n\n"
    << "Usage:\n"
    << "  qeeg_bundle_cli --bin-dir <build/bin> --outdir <bundle_dir> [options]\n\n"
    << "Options:\n"
    << "  --bin-dir DIR          Directory containing built qeeg_*_cli executables (required).\n"
    << "  --outdir DIR           Output bundle directory to create (required).\n"
    << "  --embed-help           Embed each tool's --help into the HTML (slower; runs tools at bundle-build time).\n"
    << "  --include-per-tool     Copy all qeeg_*_cli executables found in --bin-dir into the bundle.\n"
    << "                        (Default is minimal: qeeg_offline_app_cli + tool shims.)\n"
    << "  --no-tool-shims        Do not create per-tool shims (advanced).\n"
    << "  --no-open              Do not include --open in the generated start scripts.\n"
    << "  --title TEXT           Title for the generated HTML (default: QEEG Offline Tools).\n"
    << "  -h, --help             Show this help.\n\n"
    << "Examples:\n"
    << "  qeeg_bundle_cli --bin-dir ./build --outdir ./qeeg_offline_bundle\n"
    << "  qeeg_bundle_cli --bin-dir ./build --outdir ./bundle --include-per-tool --embed-help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--bin-dir" && i + 1 < argc) {
      a.bin_dir = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--embed-help") {
      a.embed_help = true;
    } else if (arg == "--include-per-tool") {
      a.include_per_tool = true;
    } else if (arg == "--no-tool-shims") {
      a.tool_shims = false;
    } else if (arg == "--no-open") {
      a.open_after = false;
    } else if (arg == "--title" && i + 1 < argc) {
      a.title = argv[++i];
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string exe_name(std::string base) {
#ifdef _WIN32
  if (!qeeg::ends_with(base, ".exe")) base += ".exe";
#endif
  return base;
}

static void copy_file_preserve_perms(const std::filesystem::path& src,
                                    const std::filesystem::path& dst) {
  std::error_code ec;
  std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    throw std::runtime_error("copy failed: " + src.u8string() + " -> " + dst.u8string() + ": " + ec.message());
  }

#ifndef _WIN32
  // Best-effort: preserve execute bits.
  std::error_code ec2;
  const auto perms = std::filesystem::status(src, ec2).permissions();
  if (!ec2) {
    std::filesystem::permissions(dst, perms, ec2);
  }
#endif
}

static bool looks_like_qeeg_tool_exe(const std::filesystem::path& p) {
  const std::string name = p.filename().u8string();
#ifdef _WIN32
  if (!qeeg::ends_with(name, ".exe")) return false;
  std::string base = name.substr(0, name.size() - 4);
#else
  std::string base = name;
#endif
  return qeeg::starts_with(base, "qeeg_") && qeeg::ends_with(base, "_cli");
}

#ifndef _WIN32
static std::string sh_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}
#else
static std::string sh_quote(const std::string& s) {
  // Best-effort Windows cmd.exe quoting.
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out.push_back(c);
  }
  out += "\"";
  return out;
}
#endif

static void write_start_script_sh(const std::filesystem::path& outdir,
                                 bool open_after) {
  const std::filesystem::path script = outdir / "start_qeeg_ui.sh";
  std::ofstream f(script);
  if (!f) throw std::runtime_error("Failed to write: " + script.u8string());

  f << "#!/usr/bin/env bash\n";
  f << "set -e\n";
  f << "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n";
  f << "mkdir -p \"$DIR/runs\"\n";
  f << "\"$DIR/bin/qeeg_offline_app_cli\" qeeg_ui_server_cli ";
  f << "--root \"$DIR/runs\" --bin-dir \"$DIR/bin\" --toolbox qeeg_offline_app_cli ";
  // The bundle already ships runs/qeeg_ui.html; prefer startup without regenerating it.
  f << "--no-generate-ui ";
  if (open_after) {
    f << "--open ";
  }
  f << "\"$@\"\n";

  f.close();

#ifndef _WIN32
  std::error_code ec;
  std::filesystem::permissions(script,
                               std::filesystem::perms::owner_exec |
                                 std::filesystem::perms::owner_read |
                                 std::filesystem::perms::owner_write |
                                 std::filesystem::perms::group_exec |
                                 std::filesystem::perms::group_read |
                                 std::filesystem::perms::others_exec |
                                 std::filesystem::perms::others_read,
                               std::filesystem::perm_options::add,
                               ec);
#endif
}

static void write_start_script_bat(const std::filesystem::path& outdir,
                                  bool open_after) {
  const std::filesystem::path script = outdir / "start_qeeg_ui.bat";
  std::ofstream f(script);
  if (!f) throw std::runtime_error("Failed to write: " + script.u8string());

  f << "@echo off\r\n";
  f << "setlocal\r\n";
  f << "set DIR=%~dp0\r\n";
  f << "if not exist \"%DIR%runs\" mkdir \"%DIR%runs\"\r\n";
  f << "\"%DIR%bin\\qeeg_offline_app_cli.exe\" qeeg_ui_server_cli ";
  f << "--root \"%DIR%runs\" --bin-dir \"%DIR%bin\" --toolbox qeeg_offline_app_cli ";
  f << "--no-generate-ui ";
  if (open_after) {
    f << "--open ";
  }
  f << "%*\r\n";
  f << "endlocal\r\n";
}

static void install_tool_shims(const std::filesystem::path& out_bin,
                              const std::filesystem::path& offline_app_dst) {
  // Use the toolbox itself to create shims. This keeps the tool list in one
  // place and matches the busybox-style argv[0] dispatch used by qeeg_offline_app_cli.
  const std::string cmd = sh_quote(offline_app_dst.u8string()) + " --install-shims " + sh_quote(out_bin.u8string());
  const int rc = std::system(cmd.c_str());
  if (rc != 0) {
    throw std::runtime_error("Failed to install tool shims (exit code " + std::to_string(rc) + ")");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    if (a.bin_dir.empty() || a.outdir.empty()) {
      std::cerr << "qeeg_bundle_cli: --bin-dir and --outdir are required (see --help)\n";
      return 2;
    }

    const std::filesystem::path bin_dir = std::filesystem::u8path(a.bin_dir);
    const std::filesystem::path outdir = std::filesystem::u8path(a.outdir);
    const std::filesystem::path out_bin = outdir / "bin";
    const std::filesystem::path out_runs = outdir / "runs";

    qeeg::ensure_directory(outdir.u8string());
    qeeg::ensure_directory(out_bin.u8string());
    qeeg::ensure_directory(out_runs.u8string());

    const std::filesystem::path offline_app_src = bin_dir / std::filesystem::u8path(exe_name("qeeg_offline_app_cli"));
    if (!std::filesystem::exists(offline_app_src)) {
      throw std::runtime_error("Required executable not found in --bin-dir: " + offline_app_src.u8string());
    }

    const std::filesystem::path offline_app_dst = out_bin / offline_app_src.filename();
    copy_file_preserve_perms(offline_app_src, offline_app_dst);

    if (a.include_per_tool) {
      for (const auto& ent : std::filesystem::directory_iterator(bin_dir)) {
        if (!ent.is_regular_file()) continue;
        const std::filesystem::path p = ent.path();
        if (!looks_like_qeeg_tool_exe(p)) continue;

        // Skip the toolbox itself (already copied).
        if (p.filename() == offline_app_src.filename()) continue;

        copy_file_preserve_perms(p, out_bin / p.filename());
      }
    }

    if (a.tool_shims) {
      // Create per-tool shims in bin/ so qeeg_*_cli commands work even in
      // minimal mode. On Unix this will typically produce hardlinks/symlinks.
      // On Windows this typically produces .exe hardlinks (or copies as a fallback).
      install_tool_shims(out_bin, offline_app_dst);
    }

    // Generate the static UI under runs/ so the server can serve it without also
    // exposing the bin/ folder under the same root.
    qeeg::UiDashboardArgs u;
    u.root = out_runs.u8string();
    u.output_html = (out_runs / "qeeg_ui.html").u8string();

    // If embed_help=true, the generator runs tools from u.bin_dir.
    // Using the bundle's bin/ directory keeps the final folder self-contained.
    u.bin_dir = out_bin.u8string();
    // Also point the UI generator at the multicall runner so it can embed
    // help output even when tool shims are disabled.
    u.toolbox = offline_app_dst.u8string();
    u.embed_help = a.embed_help;
    u.scan_bin_dir = true;
    u.scan_run_meta = true;
    u.title = a.title;

    qeeg::write_qeeg_tools_ui_html(u);

    // Start scripts for convenience.
    write_start_script_sh(outdir, a.open_after);
    write_start_script_bat(outdir, a.open_after);

    std::cout << "Wrote offline bundle: " << outdir.u8string() << "\n";
    std::cout << "  - bin/: executables (qeeg_offline_app_cli + tool shims)\n";
    std::cout << "  - runs/qeeg_ui.html: dashboard\n";
    std::cout << "  - start_qeeg_ui.sh / start_qeeg_ui.bat: launchers\n";

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
