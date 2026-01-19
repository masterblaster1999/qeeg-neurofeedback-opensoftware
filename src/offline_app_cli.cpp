#include "qeeg/utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>


// Declarations of tool entrypoints compiled into this binary.
//
// Each is produced by compiling the corresponding *_cli.cpp translation unit with
// a preprocessor definition that renames `main` -> `<tool>_entry`.
// See CMakeLists.txt for the mapping.
int qeeg_map_cli_entry(int argc, char** argv);
int qeeg_bandpower_cli_entry(int argc, char** argv);
int qeeg_bandratios_cli_entry(int argc, char** argv);
int qeeg_nf_cli_entry(int argc, char** argv);
int qeeg_coherence_cli_entry(int argc, char** argv);
int qeeg_plv_cli_entry(int argc, char** argv);
int qeeg_epoch_cli_entry(int argc, char** argv);
int qeeg_spectrogram_cli_entry(int argc, char** argv);
int qeeg_trace_plot_cli_entry(int argc, char** argv);
int qeeg_spectral_features_cli_entry(int argc, char** argv);
int qeeg_iaf_cli_entry(int argc, char** argv);
int qeeg_microstates_cli_entry(int argc, char** argv);
int qeeg_pac_cli_entry(int argc, char** argv);
int qeeg_artifacts_cli_entry(int argc, char** argv);
int qeeg_reference_cli_entry(int argc, char** argv);
int qeeg_info_cli_entry(int argc, char** argv);
int qeeg_version_cli_entry(int argc, char** argv);
int qeeg_convert_cli_entry(int argc, char** argv);
int qeeg_export_edf_cli_entry(int argc, char** argv);
int qeeg_export_bdf_cli_entry(int argc, char** argv);
int qeeg_export_brainvision_cli_entry(int argc, char** argv);
int qeeg_export_bids_cli_entry(int argc, char** argv);
int qeeg_bids_scan_cli_entry(int argc, char** argv);
int qeeg_export_derivatives_cli_entry(int argc, char** argv);
int qeeg_ui_cli_entry(int argc, char** argv);
int qeeg_ui_server_cli_entry(int argc, char** argv);
int qeeg_clean_cli_entry(int argc, char** argv);
int qeeg_quality_cli_entry(int argc, char** argv);
int qeeg_preprocess_cli_entry(int argc, char** argv);
int qeeg_channel_qc_cli_entry(int argc, char** argv);
int qeeg_bundle_cli_entry(int argc, char** argv);

namespace {

using EntryFn = int (*)(int, char**);

static std::string strip_exe_suffix(std::string s) {
  if (qeeg::ends_with(s, ".exe")) {
    s.resize(s.size() - 4);
  }
  return s;
}

static std::string base_name(const std::string& path) {
  std::filesystem::path p = std::filesystem::u8path(path);
  const std::string name = p.filename().u8string();
  return name.empty() ? path : name;
}

static std::unordered_map<std::string, EntryFn> make_tools() {
  std::unordered_map<std::string, EntryFn> m;
  m["qeeg_map_cli"] = &qeeg_map_cli_entry;
  m["qeeg_bandpower_cli"] = &qeeg_bandpower_cli_entry;
  m["qeeg_bandratios_cli"] = &qeeg_bandratios_cli_entry;
  m["qeeg_nf_cli"] = &qeeg_nf_cli_entry;
  m["qeeg_coherence_cli"] = &qeeg_coherence_cli_entry;
  m["qeeg_plv_cli"] = &qeeg_plv_cli_entry;
  m["qeeg_epoch_cli"] = &qeeg_epoch_cli_entry;
  m["qeeg_spectrogram_cli"] = &qeeg_spectrogram_cli_entry;
  m["qeeg_trace_plot_cli"] = &qeeg_trace_plot_cli_entry;
  m["qeeg_spectral_features_cli"] = &qeeg_spectral_features_cli_entry;
  m["qeeg_iaf_cli"] = &qeeg_iaf_cli_entry;
  m["qeeg_microstates_cli"] = &qeeg_microstates_cli_entry;
  m["qeeg_pac_cli"] = &qeeg_pac_cli_entry;
  m["qeeg_artifacts_cli"] = &qeeg_artifacts_cli_entry;
  m["qeeg_reference_cli"] = &qeeg_reference_cli_entry;
  m["qeeg_info_cli"] = &qeeg_info_cli_entry;
  m["qeeg_version_cli"] = &qeeg_version_cli_entry;
  m["qeeg_convert_cli"] = &qeeg_convert_cli_entry;
  m["qeeg_export_edf_cli"] = &qeeg_export_edf_cli_entry;
  m["qeeg_export_bdf_cli"] = &qeeg_export_bdf_cli_entry;
  m["qeeg_export_brainvision_cli"] = &qeeg_export_brainvision_cli_entry;
  m["qeeg_export_bids_cli"] = &qeeg_export_bids_cli_entry;
  m["qeeg_bids_scan_cli"] = &qeeg_bids_scan_cli_entry;
  m["qeeg_export_derivatives_cli"] = &qeeg_export_derivatives_cli_entry;
  m["qeeg_ui_cli"] = &qeeg_ui_cli_entry;
  m["qeeg_ui_server_cli"] = &qeeg_ui_server_cli_entry;
  m["qeeg_clean_cli"] = &qeeg_clean_cli_entry;
  m["qeeg_quality_cli"] = &qeeg_quality_cli_entry;
  m["qeeg_preprocess_cli"] = &qeeg_preprocess_cli_entry;
  m["qeeg_channel_qc_cli"] = &qeeg_channel_qc_cli_entry;
  m["qeeg_bundle_cli"] = &qeeg_bundle_cli_entry;
  return m;
}

static void print_help(const std::unordered_map<std::string, EntryFn>& tools) {
  std::cout
    << "qeeg_offline_app_cli\n\n"
    << "Single-binary offline toolbox that dispatches to any built-in qeeg_*_cli tool.\n"
    << "\n"
    << "Why:\n"
    << "  - Bundle the project as a single executable (plus data/output folders).\n"
    << "  - Use with qeeg_ui_server_cli --toolbox to run tools even when individual\n"
    << "    qeeg_*_cli executables are not present in --bin-dir.\n"
    << "  - Optionally create per-tool shims (links) so you can invoke qeeg_*_cli\n"
    << "    directly while still shipping one binary.\n"
    << "\n"
    << "Usage:\n"
    << "  qeeg_offline_app_cli <tool> [args...]\n"
    << "  qeeg_offline_app_cli --list-tools\n"
    << "  qeeg_offline_app_cli --install-shims [DIR] [--force]\n"
    << "  qeeg_offline_app_cli --help\n"
    << "\n"
    << "Examples:\n"
    << "  qeeg_offline_app_cli qeeg_version_cli\n"
    << "  qeeg_offline_app_cli qeeg_map_cli --help\n"
    << "  qeeg_offline_app_cli --install-shims ./bin\n"
    << "  qeeg_ui_server_cli --root . --bin-dir . --toolbox qeeg_offline_app_cli --open\n"
    << "\n"
    << "Tools:\n";

  std::vector<std::string> names;
  names.reserve(tools.size());
  for (const auto& kv : tools) names.push_back(kv.first);
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    std::cout << "  " << n << "\n";
  }
}

static int run_tool(EntryFn fn, const std::string& tool, int argc, char** argv, int start_index) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(std::max(1, argc - start_index + 1)));
  args.push_back(tool);
  for (int i = start_index; i < argc; ++i) {
    args.push_back(argv[i]);
  }

  std::vector<char*> cargs;
  cargs.reserve(args.size() + 1);
  for (auto& s : args) {
    cargs.push_back(const_cast<char*>(s.c_str()));
  }
  cargs.push_back(nullptr);

  return fn(static_cast<int>(args.size()), cargs.data());
}

static bool has_path_sep(const std::string& s) {
  return (s.find('/') != std::string::npos) || (s.find('\\') != std::string::npos);
}

static std::vector<std::string> split_path_env(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  cur.reserve(128);

#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif

  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

static std::filesystem::path resolve_self_path(const char* argv0) {
  if (!argv0) return {};
  const std::string s = argv0;
  if (s.empty()) return {};

  auto try_abs = [](const std::filesystem::path& p) -> std::filesystem::path {
    std::error_code ec;
    std::filesystem::path a = p;
    if (a.is_relative()) {
      a = std::filesystem::absolute(a, ec);
      if (ec) return {};
    }
    if (std::filesystem::exists(a, ec) && !ec) return a;
    return {};
  };

  // If argv0 already contains a path separator, treat it as a path.
  if (has_path_sep(s)) {
    return try_abs(std::filesystem::u8path(s));
  }

  // Try current directory.
  {
    const auto p = try_abs(std::filesystem::current_path() / std::filesystem::u8path(s));
    if (!p.empty()) return p;
  }

  // Search PATH.
  const char* env = std::getenv("PATH");
  if (env && *env) {
    const std::string pathenv = env;
    for (const auto& d : split_path_env(pathenv)) {
      if (d.empty()) continue;
      const std::filesystem::path base = std::filesystem::u8path(d);
      const auto p1 = try_abs(base / std::filesystem::u8path(s));
      if (!p1.empty()) return p1;
#if defined(_WIN32)
      // If argv0 was extensionless, try adding .exe.
      if (!qeeg::ends_with(s, ".exe")) {
        const auto p2 = try_abs(base / std::filesystem::u8path(s + ".exe"));
        if (!p2.empty()) return p2;
      }
#endif
    }
  }

  return {};
}

static std::string exe_name(std::string base) {
#ifdef _WIN32
  if (!qeeg::ends_with(base, ".exe")) base += ".exe";
#endif
  return base;
}

#ifndef _WIN32
static std::string sh_quote(const std::string& s) {
  // POSIX single-quote escaping:  abc'd  ->  'abc'\''d'
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

static void write_wrapper_script(const std::filesystem::path& dst,
                                 const std::filesystem::path& self_path,
                                 const std::string& tool) {
  std::ofstream f(dst);
  if (!f) {
    throw std::runtime_error("Failed to write shim: " + dst.u8string());
  }

  f << "#!/usr/bin/env bash\n";
  f << "set -e\n";
  f << "exec " << sh_quote(self_path.u8string()) << " " << tool << " \"$@\"\n";

  f.close();

  // Best-effort: mark executable.
  std::error_code ec;
  std::filesystem::permissions(dst,
                               std::filesystem::perms::owner_exec |
                                 std::filesystem::perms::group_exec |
                                 std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add,
                               ec);
}
#endif

static int install_shims(const std::unordered_map<std::string, EntryFn>& tools,
                         const std::filesystem::path& self_path,
                         const std::filesystem::path& dir,
                         bool force) {
  if (self_path.empty() || !std::filesystem::exists(self_path)) {
    std::cerr << "qeeg_offline_app_cli: cannot resolve self executable path.\n";
    std::cerr << "Tip: run from the directory that contains qeeg_offline_app_cli, or provide an explicit path.\n";
    return 2;
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::cerr << "qeeg_offline_app_cli: failed to create directory: " << dir.u8string() << ": " << ec.message() << "\n";
    return 1;
  }

  std::vector<std::string> names;
  names.reserve(tools.size());
  for (const auto& kv : tools) names.push_back(kv.first);
  std::sort(names.begin(), names.end());

  const std::filesystem::path self_filename = self_path.filename();
  size_t created = 0;
  size_t skipped = 0;

  for (const auto& tool : names) {
    const std::filesystem::path dst = dir / std::filesystem::u8path(exe_name(tool));

    std::error_code ec_exists;
    if (std::filesystem::exists(dst, ec_exists) && !ec_exists) {
      if (!force) {
        ++skipped;
        continue;
      }
      std::filesystem::remove(dst, ec_exists);
    }

    std::error_code ec1;
    std::filesystem::create_hard_link(self_path, dst, ec1);
    if (!ec1) {
      ++created;
      continue;
    }

#ifdef _WIN32
    // Windows fallback: copy the binary (bigger, but works everywhere).
    std::error_code ec2;
    std::filesystem::copy_file(self_path, dst, std::filesystem::copy_options::overwrite_existing, ec2);
    if (ec2) {
      std::cerr << "Failed to create shim (hardlink/copy) for " << tool << ": " << ec2.message() << "\n";
      return 1;
    }
    ++created;
#else
    // POSIX fallback: try a symlink relative to the destination directory.
    std::filesystem::path target = self_path;
    std::error_code ec_rel;
    const auto rel = std::filesystem::relative(self_path, dir, ec_rel);
    if (!ec_rel && !rel.empty()) {
      target = rel;
    }

    std::error_code ec2;
    std::filesystem::create_symlink(target, dst, ec2);
    if (!ec2) {
      ++created;
      continue;
    }

    // Final fallback: tiny wrapper script.
    try {
      write_wrapper_script(dst, self_path, tool);
    } catch (const std::exception& e) {
      std::cerr << "Failed to create shim for " << tool << ": " << e.what() << "\n";
      return 1;
    }
    ++created;
#endif
  }

  std::cout << "Installed tool shims into: " << dir.u8string() << "\n";
  std::cout << "  self: " << self_filename.u8string() << "\n";
  std::cout << "  created: " << created << ", skipped: " << skipped << "\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const auto tools = make_tools();

  if (argc <= 0 || !argv || !argv[0]) {
    print_help(tools);
    return 2;
  }

  const std::string invoked = strip_exe_suffix(base_name(argv[0]));

  // If invoked via a copy/symlink named like a specific tool (e.g. qeeg_map_cli),
  // dispatch by argv[0]. This allows "busybox style" multi-call usage.
  auto it0 = tools.find(invoked);
  if (it0 != tools.end()) {
    return run_tool(it0->second, invoked, argc, argv, 1);
  }

  if (argc < 2) {
    print_help(tools);
    return 2;
  }

  const std::string first = argv[1];
  if (first == "-h" || first == "--help") {
    print_help(tools);
    return 0;
  }
  if (first == "--list-tools") {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& kv : tools) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) std::cout << n << "\n";
    return 0;
  }
  if (first == "--install-shims") {
    std::string dir;
    bool force = false;

    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--force") {
        force = true;
      } else if (dir.empty()) {
        dir = a;
      } else {
        std::cerr << "qeeg_offline_app_cli: unknown argument for --install-shims: " << a << "\n";
        return 2;
      }
    }

    const auto self = resolve_self_path(argv[0]);
    if (self.empty()) {
      std::cerr << "qeeg_offline_app_cli: could not resolve self executable path.\n";
      return 2;
    }

    std::filesystem::path out_dir;
    if (dir.empty()) {
      out_dir = self.parent_path();
    } else {
      out_dir = std::filesystem::u8path(dir);
    }

    return install_shims(tools, self, out_dir, force);
  }

  const std::string tool = strip_exe_suffix(first);
  auto it = tools.find(tool);
  if (it == tools.end()) {
    std::cerr << "qeeg_offline_app_cli: unknown tool: " << tool << "\n";
    std::cerr << "Run 'qeeg_offline_app_cli --list-tools' to see available tools.\n";
    return 2;
  }

  return run_tool(it->second, tool, argc, argv, 2);
}
