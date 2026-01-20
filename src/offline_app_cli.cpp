#include "qeeg/utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <unistd.h>
#else
  #include <unistd.h>
#endif


// Declarations of tool entrypoints compiled into this binary.
//
// Each is produced by compiling the corresponding *_cli.cpp translation unit with
// a preprocessor definition that renames `main` -> `<tool>_entry`.
// See CMakeLists.txt for the mapping.
int qeeg_map_cli_entry(int argc, char** argv);
int qeeg_topomap_cli_entry(int argc, char** argv);
int qeeg_region_summary_cli_entry(int argc, char** argv);
int qeeg_connectivity_map_cli_entry(int argc, char** argv);
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
int qeeg_pipeline_cli_entry(int argc, char** argv);

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
  m["qeeg_topomap_cli"] = &qeeg_topomap_cli_entry;
  m["qeeg_region_summary_cli"] = &qeeg_region_summary_cli_entry;
  m["qeeg_connectivity_map_cli"] = &qeeg_connectivity_map_cli_entry;
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
  m["qeeg_pipeline_cli"] = &qeeg_pipeline_cli_entry;
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
    << "  qeeg_offline_app_cli --list-tools [--json] [--pretty]\n"
    << "  qeeg_offline_app_cli --install-shims [DIR] [--force] [--tool TOOL]... [--dry-run]\n"
    << "  qeeg_offline_app_cli --uninstall-shims [DIR] [--force] [--tool TOOL]... [--dry-run]\n"
    << "  qeeg_offline_app_cli --help\n"
    << "\n"
    << "Notes:\n"
    << "  - Without --force, uninstall only removes shims that appear to point back\n"
    << "    to the currently-running qeeg_offline_app_cli.\n"
    << "  - When dispatching a tool, this sets the environment variable QEEG_TOOLBOX\n"
    << "    (unless already set) to the path of this executable. This lets workflows\n"
    << "    like qeeg_pipeline_cli re-invoke other tools through the same binary.\n"
    << "\n"
    << "Examples:\n"
    << "  qeeg_offline_app_cli qeeg_version_cli\n"
    << "  qeeg_offline_app_cli qeeg_map_cli --help\n"
    << "  qeeg_offline_app_cli --install-shims ./bin\n"
    << "  qeeg_offline_app_cli --install-shims ./bin --tool qeeg_version_cli\n"
    << "  qeeg_offline_app_cli --uninstall-shims ./bin --tool qeeg_version_cli\n"
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

static void set_env_if_unset(const char* key, const std::string& value) {
  if (!key || !*key) return;
  const char* cur = std::getenv(key);
  if (cur && *cur) return;
#if defined(_WIN32)
  (void)_putenv_s(key, value.c_str());
#else
  (void)setenv(key, value.c_str(), 0);
#endif
}


static std::filesystem::path canonicalize_best_effort(const std::filesystem::path& p) {
  if (p.empty()) return {};
  std::error_code ec;
  std::filesystem::path c = std::filesystem::canonical(p, ec);
  if (!ec) return c;
  ec.clear();
  c = std::filesystem::weakly_canonical(p, ec);
  if (!ec) return c;
  ec.clear();
  c = std::filesystem::absolute(p, ec);
  if (!ec) return c;
  return p;
}

static std::filesystem::path resolve_self_path_platform() {
#if defined(_WIN32)
  std::vector<wchar_t> buf(static_cast<size_t>(MAX_PATH));
  for (;;) {
    const DWORD n = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < static_cast<DWORD>(buf.size())) {
      std::filesystem::path p(buf.data());
      return canonicalize_best_effort(p);
    }
    // Buffer too small; grow.
    if (buf.size() > (1u << 20)) return {};
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t sz = 0;
  // First call: get required size.
  (void)_NSGetExecutablePath(nullptr, &sz);
  if (sz == 0) return {};

  std::vector<char> buf(static_cast<size_t>(sz) + 1u);
  if (_NSGetExecutablePath(buf.data(), &sz) != 0) return {};
  buf[static_cast<size_t>(sz)] = '\0';

  std::filesystem::path p = std::filesystem::u8path(buf.data());
  return canonicalize_best_effort(p);
#else
  // Linux and many Unix-like systems expose /proc/self/exe as a symlink.
  std::vector<char> buf(4096);
  for (;;) {
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n < 0) return {};
    if (static_cast<size_t>(n) < buf.size() - 1) {
      buf[static_cast<size_t>(n)] = '\0';
      std::filesystem::path p = std::filesystem::u8path(buf.data());
      return canonicalize_best_effort(p);
    }
    // Buffer too small; grow.
    if (buf.size() > (1u << 20)) return {};
    buf.resize(buf.size() * 2);
  }
#endif
}

static std::filesystem::path resolve_self_path(const char* argv0) {
  // Prefer a platform-specific way to locate the currently-running executable.
  // This makes --install-shims more reliable when argv[0] is ambiguous.
  {
    const std::filesystem::path p = resolve_self_path_platform();
    if (!p.empty()) return p;
  }

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
      out += "'\\\\''";
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

  // NOTE: Keep these comments stable so --uninstall-shims can safely recognize them.
  f << "#!/usr/bin/env bash\n";
  f << "# qeeg_offline_app_cli shim\n";
  f << "# tool: " << tool << "\n";
  f << "# self: " << self_path.u8string() << "\n";
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

struct ShimArgs {
  std::string dir;
  bool force = false;
  bool dry_run = false;
  std::vector<std::string> tools; // If empty: all tools
};

static bool parse_shim_args(int argc, char** argv, int start_index, ShimArgs& out, std::string& err) {
  for (int i = start_index; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--force") {
      out.force = true;
      continue;
    }
    if (a == "--dry-run") {
      out.dry_run = true;
      continue;
    }
    if (a == "--tool") {
      if (i + 1 >= argc) {
        err = "--tool expects a value";
        return false;
      }
      out.tools.push_back(strip_exe_suffix(base_name(argv[++i])));
      continue;
    }
    const std::string prefix = "--tool=";
    if (qeeg::starts_with(a, prefix)) {
      out.tools.push_back(strip_exe_suffix(base_name(a.substr(prefix.size()))));
      continue;
    }

    // Positional: [DIR]
    if (!a.empty() && a[0] == '-') {
      err = "unknown argument: " + a;
      return false;
    }
    if (out.dir.empty()) {
      out.dir = a;
      continue;
    }
    err = "unexpected argument: " + a;
    return false;
  }
  return true;
}

static bool select_tools(const std::unordered_map<std::string, EntryFn>& tools,
                         const std::vector<std::string>& requested,
                         std::vector<std::string>& out,
                         std::string& err) {
  out.clear();
  if (requested.empty()) {
    out.reserve(tools.size());
    for (const auto& kv : tools) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return true;
  }

  out.reserve(requested.size());
  for (const auto& t0 : requested) {
    const std::string t = strip_exe_suffix(t0);
    if (tools.find(t) == tools.end()) {
      err = "unknown tool: " + t;
      return false;
    }
    out.push_back(t);
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

static int install_shims(const std::filesystem::path& self_path,
                         const std::filesystem::path& dir,
                         const std::vector<std::string>& tool_names,
                         bool force,
                         bool dry_run) {
  if (self_path.empty() || !std::filesystem::exists(self_path)) {
    std::cerr << "qeeg_offline_app_cli: cannot resolve self executable path.\n";
    std::cerr << "Tip: run from the directory that contains qeeg_offline_app_cli, or provide an explicit path.\n";
    return 2;
  }

  bool would_create_dir = false;

  std::error_code ec;
  const bool dir_exists = std::filesystem::exists(dir, ec);
  if (ec) {
    std::cerr << "qeeg_offline_app_cli: failed to stat directory: " << dir.u8string() << ": " << ec.message() << "\n";
    return 1;
  }

  if (!dry_run) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::cerr << "qeeg_offline_app_cli: failed to create directory: " << dir.u8string() << ": " << ec.message() << "\n";
      return 1;
    }
    ec.clear();
    if (!std::filesystem::is_directory(dir, ec) || ec) {
      std::cerr << "qeeg_offline_app_cli: shim path is not a directory: " << dir.u8string() << "\n";
      return 1;
    }
  } else {
    // Dry-run MUST NOT modify the filesystem.
    // We still validate obvious path issues when the directory already exists.
    if (dir_exists) {
      ec.clear();
      if (!std::filesystem::is_directory(dir, ec) || ec) {
        std::cerr << "qeeg_offline_app_cli: shim path is not a directory: " << dir.u8string() << "\n";
        return 1;
      }
    } else {
      would_create_dir = true;
    }
  }

  if (dry_run && would_create_dir) {
    std::cout << "[dry-run] would create directory: " << dir.u8string() << "\n";
  }

  const std::filesystem::path self_filename = self_path.filename();
  size_t created = 0;
  size_t skipped = 0;

  for (const auto& tool : tool_names) {
    const std::filesystem::path dst = dir / std::filesystem::u8path(exe_name(tool));

    std::error_code ec_exists;
    if (std::filesystem::exists(dst, ec_exists) && !ec_exists) {
      if (!force) {
        ++skipped;
        continue;
      }
      if (!dry_run) {
        std::filesystem::remove(dst, ec_exists);
      }
    }

    if (dry_run) {
      std::cout << "[dry-run] would create shim: " << dst.u8string() << " -> " << self_filename.u8string() << "\n";
      ++created;
      continue;
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

  std::cout << (dry_run ? "Dry-run: would install tool shims into: " : "Installed tool shims into: ")
            << dir.u8string() << "\n";
  std::cout << "  self: " << self_filename.u8string() << "\n";
  std::cout << "  created: " << created << ", skipped: " << skipped << "\n";
  if (dry_run) {
    std::cout << "  note: dry-run (no changes were made)\n";
  }
  return 0;
}

static bool fnv1a64_file(const std::filesystem::path& p, std::uint64_t& out, std::string& err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    err = "cannot open: " + p.u8string();
    return false;
  }

  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;

  std::uint64_t h = kOffset;
  std::vector<char> buf(64 * 1024);
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
      h ^= static_cast<unsigned char>(buf[static_cast<size_t>(i)]);
      h *= kPrime;
    }
  }

  if (!f.eof()) {
    err = "read failure: " + p.u8string();
    return false;
  }

  out = h;
  return true;
}

#ifndef _WIN32
static bool is_wrapper_script_for_self(const std::filesystem::path& dst,
                                       const std::filesystem::path& self_canon,
                                       const std::string& tool) {
  std::ifstream f(dst);
  if (!f) return false;

  std::string text;
  text.reserve(4096);

  std::string line;
  for (int i = 0; i < 12 && std::getline(f, line); ++i) {
    text += line;
    text.push_back('\n');
    if (text.size() > 16 * 1024) break;
  }

  if (text.rfind("#!/", 0) != 0) return false;

  // Marker-based detection (preferred).
  const std::string marker = "# qeeg_offline_app_cli shim";
  if (text.find(marker) != std::string::npos) {
    const std::string tool_prefix = "# tool: ";
    const std::string self_prefix = "# self: ";

    const auto tool_pos = text.find(tool_prefix);
    const auto self_pos = text.find(self_prefix);
    if (tool_pos != std::string::npos && self_pos != std::string::npos) {
      const auto tool_end = text.find('\n', tool_pos);
      const auto self_end = text.find('\n', self_pos);

      const std::string tool_line = text.substr(tool_pos + tool_prefix.size(),
                                                tool_end == std::string::npos ? std::string::npos
                                                                              : tool_end - (tool_pos + tool_prefix.size()));
      const std::string self_line = text.substr(self_pos + self_prefix.size(),
                                                self_end == std::string::npos ? std::string::npos
                                                                              : self_end - (self_pos + self_prefix.size()));

      if (tool_line == tool && !self_line.empty()) {
        const std::filesystem::path p = canonicalize_best_effort(std::filesystem::u8path(self_line));
        if (!p.empty() && !self_canon.empty() && p == self_canon) {
          return true;
        }
      }
    }
    // Marker present but couldn't validate; fall through to heuristic.
  }

  // Heuristic for old wrapper scripts: look for an exec line referencing this tool and the
  // current executable filename.
  if (text.find("exec ") == std::string::npos) return false;
  if (text.find(tool) == std::string::npos) return false;
  const std::string self_file = self_canon.filename().u8string();
  if (!self_file.empty() && text.find(self_file) != std::string::npos) return true;
  const std::string self_full = self_canon.u8string();
  if (!self_full.empty() && text.find(self_full) != std::string::npos) return true;
  return false;
}
#endif

static bool is_shim_to_self(const std::filesystem::path& dst,
                            const std::filesystem::path& self_canon,
                            const std::string& tool,
                            const std::uintmax_t self_size,
                            std::optional<std::uint64_t>& self_hash) {
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(dst, ec);
  if (ec) return false;

  if (std::filesystem::is_symlink(st)) {
    std::filesystem::path target = std::filesystem::read_symlink(dst, ec);
    if (ec) return false;
    if (target.is_relative()) {
      target = dst.parent_path() / target;
    }
    const auto target_canon = canonicalize_best_effort(target);
    return !target_canon.empty() && !self_canon.empty() && target_canon == self_canon;
  }

  // Hardlink (or same file): equivalent() should return true.
  ec.clear();
  if (std::filesystem::equivalent(dst, self_canon, ec) && !ec) {
    return true;
  }

#ifndef _WIN32
  // Wrapper scripts are used as a last-resort fallback on POSIX.
  if (std::filesystem::is_regular_file(st)) {
    if (is_wrapper_script_for_self(dst, self_canon, tool)) return true;
  }
#endif

  // Windows fallback can be a full copy of the binary. Detect by comparing a lightweight hash.
  if (std::filesystem::is_regular_file(st)) {
    ec.clear();
    const std::uintmax_t dst_size = std::filesystem::file_size(dst, ec);
    if (ec) return false;
    if (dst_size != self_size) return false;

    std::uint64_t dst_hash = 0;
    std::string err;
    if (!fnv1a64_file(dst, dst_hash, err)) return false;

    if (!self_hash.has_value()) {
      std::uint64_t h = 0;
      if (!fnv1a64_file(self_canon, h, err)) return false;
      self_hash = h;
    }

    return dst_hash == *self_hash;
  }

  return false;
}

static int uninstall_shims(const std::filesystem::path& self_path,
                           const std::filesystem::path& dir,
                           const std::vector<std::string>& tool_names,
                           bool force,
                           bool dry_run) {
  if (self_path.empty() || !std::filesystem::exists(self_path)) {
    std::cerr << "qeeg_offline_app_cli: cannot resolve self executable path.\n";
    return 2;
  }

  const std::filesystem::path self_canon = canonicalize_best_effort(self_path);
  std::error_code ec;
  const std::uintmax_t self_size = std::filesystem::file_size(self_canon, ec);
  if (ec) {
    std::cerr << "qeeg_offline_app_cli: cannot stat self executable: " << self_canon.u8string() << ": " << ec.message() << "\n";
    return 1;
  }

  size_t removed = 0;
  size_t skipped = 0;
  size_t missing = 0;
  std::optional<std::uint64_t> self_hash;

  for (const auto& tool : tool_names) {
    const std::filesystem::path dst = dir / std::filesystem::u8path(exe_name(tool));

    ec.clear();
    if (!std::filesystem::exists(dst, ec) || ec) {
      ++missing;
      continue;
    }

    if (!force) {
      if (!is_shim_to_self(dst, self_canon, tool, self_size, self_hash)) {
        ++skipped;
        continue;
      }
    }

    if (dry_run) {
      std::cout << "[dry-run] would remove shim: " << dst.u8string() << "\n";
      ++removed;
      continue;
    }

    ec.clear();
    std::filesystem::remove(dst, ec);
    if (ec) {
      std::cerr << "qeeg_offline_app_cli: failed to remove: " << dst.u8string() << ": " << ec.message() << "\n";
      return 1;
    }
    ++removed;
  }

  std::cout << "Uninstalled tool shims from: " << dir.u8string() << "\n";
  std::cout << "  removed: " << removed << ", skipped: " << skipped << ", missing: " << missing << "\n";
  if (dry_run) {
    std::cout << "  note: dry-run (no changes were made)\n";
  }

  if (!force && skipped > 0) {
    std::cout << "Tip: re-run with --force to remove shims even if they do not appear to match the current toolbox.\n";
  }

  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const auto tools = make_tools();

  if (argc <= 0 || !argv || !argv[0]) {
    print_help(tools);
    return 2;
  }

  // Make CLI cross-integration smoother: when dispatching tools from this single-binary
  // toolbox, expose the toolbox path to child workflows (e.g. qeeg_pipeline_cli) via
  // QEEG_TOOLBOX, unless the user already set it explicitly.
  const std::filesystem::path self_path = resolve_self_path(argv[0]);
  if (!self_path.empty()) {
    set_env_if_unset("QEEG_TOOLBOX", self_path.u8string());
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
    bool json = false;
    bool pretty = false;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--json") {
        json = true;
      } else if (a == "--pretty") {
        pretty = true;
      } else {
        std::cerr << "qeeg_offline_app_cli: unknown argument for --list-tools: " << a << "\n";
        return 2;
      }
    }

    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& kv : tools) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    if (!json) {
      for (const auto& n : names) std::cout << n << "\n";
      return 0;
    }

    // JSON array of tool names (for machine-readable discovery).
    // This is intentionally dependency-free and uses qeeg::json_escape.
    const char* nl = pretty ? "\n" : "";
    const char* ind = pretty ? "  " : "";
    std::cout << "[" << nl;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) std::cout << "," << (pretty ? "\n" : "");
      std::cout << ind << "\"" << qeeg::json_escape(names[i]) << "\"";
    }
    std::cout << nl;
    std::cout << "]\n";
    return 0;
  }
  if (first == "--install-shims") {
    ShimArgs args;
    std::string err;
    if (!parse_shim_args(argc, argv, 2, args, err)) {
      std::cerr << "qeeg_offline_app_cli: " << err << "\n";
      return 2;
    }

    std::vector<std::string> tool_names;
    if (!select_tools(tools, args.tools, tool_names, err)) {
      std::cerr << "qeeg_offline_app_cli: " << err << "\n";
      return 2;
    }

    const auto self = resolve_self_path(argv[0]);
    if (self.empty()) {
      std::cerr << "qeeg_offline_app_cli: could not resolve self executable path.\n";
      return 2;
    }

    std::filesystem::path out_dir;
    if (args.dir.empty()) {
      out_dir = self.parent_path();
    } else {
      out_dir = std::filesystem::u8path(args.dir);
    }

    return install_shims(self, out_dir, tool_names, args.force, args.dry_run);
  }
  if (first == "--uninstall-shims") {
    ShimArgs args;
    std::string err;
    if (!parse_shim_args(argc, argv, 2, args, err)) {
      std::cerr << "qeeg_offline_app_cli: " << err << "\n";
      return 2;
    }

    std::vector<std::string> tool_names;
    if (!select_tools(tools, args.tools, tool_names, err)) {
      std::cerr << "qeeg_offline_app_cli: " << err << "\n";
      return 2;
    }

    const auto self = resolve_self_path(argv[0]);
    if (self.empty()) {
      std::cerr << "qeeg_offline_app_cli: could not resolve self executable path.\n";
      return 2;
    }

    std::filesystem::path out_dir;
    if (args.dir.empty()) {
      out_dir = self.parent_path();
    } else {
      out_dir = std::filesystem::u8path(args.dir);
    }

    return uninstall_shims(self, out_dir, tool_names, args.force, args.dry_run);
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
