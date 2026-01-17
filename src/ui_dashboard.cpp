#include "qeeg/ui_dashboard.hpp"

#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace qeeg {

namespace {

struct ToolSpec {
  std::string name;
  std::string group;
  std::string description;
  std::string example;
  // UI helper: suggested default flags for injecting the Workspace-selected
  // file/folder into an argument string. The UI also tries to infer additional
  // path-taking flags from embedded --help output and offers them in a dropdown.
  std::string inject_flag_file{"--input"};
  std::string inject_flag_dir{""};
};

static bool looks_like_qeeg_cli_exe_name(const std::string& filename, std::string* out_base) {
  // Accept:
  //   qeeg_*_cli           (POSIX)
  //   qeeg_*_cli.exe       (Windows)
  // Reject:
  //   qeeg_test_*          (tests)
  //   anything without the qeeg_ prefix
  //   anything without _cli suffix
  std::string base = filename;

#if defined(_WIN32)
  // On Windows we primarily expect .exe binaries.
  if (ends_with(base, ".exe")) {
    base = base.substr(0, base.size() - 4);
  } else {
    return false;
  }
#else
  // On POSIX we accept extensionless executables.
  // (We still handle .exe defensively for cross-built artifacts.)
  if (ends_with(base, ".exe")) {
    base = base.substr(0, base.size() - 4);
  }
#endif

  if (!starts_with(base, "qeeg_")) return false;
  if (!ends_with(base, "_cli")) return false;
  if (starts_with(base, "qeeg_test_")) return false;

  if (out_base) *out_base = base;
  return true;
}

static std::vector<std::string> discover_cli_tools_in_bin_dir(const std::filesystem::path& bin_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (bin_dir.empty() || !std::filesystem::exists(bin_dir, ec)) return out;

  for (auto it = std::filesystem::directory_iterator(bin_dir, ec);
       it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    std::string base;
    if (!looks_like_qeeg_cli_exe_name(it->path().filename().u8string(), &base)) continue;
    out.push_back(base);
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::string infer_group_from_name(const std::string& tool) {
  // Heuristic grouping for tools that are auto-discovered but not part of the
  // curated metadata list.
  if (tool.find("_ui_") != std::string::npos) return "UI";
  if (tool.find("_nf_") != std::string::npos) return "Neurofeedback";
  if (tool.find("microstates") != std::string::npos) return "Microstates";
  if (tool.find("plv") != std::string::npos || tool.find("coherence") != std::string::npos || tool.find("pac") != std::string::npos) {
    return "Connectivity";
  }
  if (tool.find("map") != std::string::npos || tool.find("spectrogram") != std::string::npos || tool.find("spectral_features") != std::string::npos || tool.find("epoch") != std::string::npos || tool.find("iaf") != std::string::npos || tool.find("reference") != std::string::npos || tool.find("bandpower") != std::string::npos || tool.find("bandratios") != std::string::npos) {
    return "Spectral & Maps";
  }
  if (tool.find("channel_qc") != std::string::npos || tool.find("preprocess") != std::string::npos || tool.find("clean") != std::string::npos || tool.find("artifact") != std::string::npos) {
    return "Preprocess & Clean";
  }
  if (tool.find("export") != std::string::npos || tool.find("convert") != std::string::npos || tool.find("info") != std::string::npos || tool.find("quality") != std::string::npos || tool.find("trace_plot") != std::string::npos) {
    return "Inspect & Convert";
  }
  return "Other";
}

static std::vector<ToolSpec> default_tools() {
  // Curated metadata for the most commonly used tools.
  //
  // When UiDashboardArgs::scan_bin_dir is enabled, any additional qeeg_*_cli
  // binaries found in --bin-dir will be auto-added to the UI.
  return {
    {"qeeg_info_cli", "Inspect & Convert",
     "Quick summary of a recording: sampling rate, channel list, events.",
     "qeeg_info_cli --input session.edf --channels --events"},

    {"qeeg_quality_cli", "Inspect & Convert",
     "Line-noise scan (50/60 Hz) and basic quality heuristics.",
     "qeeg_quality_cli --input session.edf"},

    {"qeeg_trace_plot_cli", "Inspect & Convert",
     "Render a quick stacked trace plot to SVG (shareable raw view).",
     "qeeg_trace_plot_cli --input session.edf --outdir out_traces"},

    {"qeeg_convert_cli", "Inspect & Convert",
     "Convert/normalize inputs to CSV and optionally remap/rename channels.",
     "qeeg_convert_cli --input session.edf --output session.csv --events-out events.csv"},

    {"qeeg_export_edf_cli", "Inspect & Convert",
     "Export to EDF/EDF+ (optionally embed events) for round-trip friendliness.",
     "qeeg_export_edf_cli --input session.csv --output session.edf --fs 256"},

    {"qeeg_export_bdf_cli", "Inspect & Convert",
     "Export to BDF/BDF+ (24-bit; optional embedded annotations).",
     "qeeg_export_bdf_cli --input session.edf --output session.bdf"},

    {"qeeg_export_brainvision_cli", "Inspect & Convert",
     "Export to BrainVision (vhdr/vmrk/eeg) format.",
     "qeeg_export_brainvision_cli --input session.edf --output session.vhdr"},

    {"qeeg_export_bids_cli", "Inspect & Convert",
     "Export a raw recording to a BIDS-like EEG layout (channels/events sidecars).",
     "qeeg_export_bids_cli --input session.edf --out-dir bids --sub 01 --task rest",
     "--input", "--out-dir"},

    {"qeeg_export_derivatives_cli", "Inspect & Convert",
     "Package qeeg outputs (map/spec/qc/iaf/nf/microstates) into a BIDS derivatives layout.",
     "qeeg_export_derivatives_cli --bids-root bids --pipeline qeeg --sub 01 --task rest --map-outdir out_map",
     "--bids-file", "--bids-root"},

    {"qeeg_bids_scan_cli", "Inspect & Convert",
     "Scan a BIDS-EEG dataset for recordings + sidecars (lightweight sanity checks).",
     "qeeg_bids_scan_cli --dataset /path/to/bids --outdir out_bids_scan",
     "--dataset", "--dataset"},

    {"qeeg_preprocess_cli", "Preprocess & Clean",
     "Apply simple offline preprocessing (CAR/notch/bandpass) and export cleaned signals.",
     "qeeg_preprocess_cli --input session.edf --output cleaned.csv --notch 50 --bandpass 1 40"},

    {"qeeg_clean_cli", "Preprocess & Clean",
     "Extract clean (good) segments using artifact detection; optional per-segment export.",
     "qeeg_clean_cli --input session.edf --outdir out_clean --pad 0.25 --min-good 2 --export-csv"},

    {"qeeg_channel_qc_cli", "Preprocess & Clean",
     "Channel quality control (flag noisy/flat channels; emits QC manifests).",
     "qeeg_channel_qc_cli --input session.edf --outdir out_qc"},

    {"qeeg_artifacts_cli", "Preprocess & Clean",
     "Offline artifact scanning/gating helpers.",
     "qeeg_artifacts_cli --input session.edf --outdir out_artifacts"},

    {"qeeg_map_cli", "Spectral & Maps",
     "Compute per-channel bandpowers and render topographic scalp maps.",
     "qeeg_map_cli --input session.edf --outdir out_map --html-report"},

    {"qeeg_bandpower_cli", "Spectral & Maps",
     "Compute per-channel bandpower features (CSV + JSON sidecar).",
     "qeeg_bandpower_cli --input session.edf --outdir out_bp"},

    {"qeeg_bandratios_cli", "Spectral & Maps",
     "Derive common neurofeedback band ratios from a bandpowers.csv table.",
     "qeeg_bandratios_cli --bandpowers out_bp/bandpowers.csv --outdir out_ratios --ratio theta/beta",
     "--bandpowers"},

    {"qeeg_spectral_features_cli", "Spectral & Maps",
     "Spectral summary table per channel (entropy, SEF95, peak frequency, ...).",
     "qeeg_spectral_features_cli --input session.edf --outdir out_spec"},

    {"qeeg_reference_cli", "Spectral & Maps",
     "Build a reference CSV (mean/std) for z-scoring bandpowers/topomaps.",
     "qeeg_reference_cli --input subj01.edf --input subj02.edf --outdir out_ref"},

    {"qeeg_spectrogram_cli", "Spectral & Maps",
     "Compute spectrograms (time-frequency view) for channels.",
     "qeeg_spectrogram_cli --input session.edf --outdir out_spec"},

    {"qeeg_epoch_cli", "Spectral & Maps",
     "Epoch/segment extraction + feature exports (bandpower, etc.).",
     "qeeg_epoch_cli --input session.edf --outdir out_epochs"},

    {"qeeg_iaf_cli", "Spectral & Maps",
     "Estimate individual alpha frequency (IAF) and optionally emit IAF-derived bands.",
     "qeeg_iaf_cli --input session.edf --outdir out_iaf"},

    {"qeeg_coherence_cli", "Connectivity",
     "Magnitude-squared coherence (matrix + edge list exports).",
     "qeeg_coherence_cli --input session.edf --outdir out_coh"},

    {"qeeg_plv_cli", "Connectivity",
     "Phase locking value (PLV) connectivity.",
     "qeeg_plv_cli --input session.edf --outdir out_plv"},

    {"qeeg_pac_cli", "Connectivity",
     "Phase-amplitude coupling (PAC) metrics.",
     "qeeg_pac_cli --input session.edf --outdir out_pac"},

    {"qeeg_microstates_cli", "Microstates",
     "First-pass microstate analysis (GFP peaks + clustering; templates + events).",
     "qeeg_microstates_cli --input session.edf --outdir out_ms --html-report"},

    {"qeeg_nf_cli", "Neurofeedback",
     "Real-time/offline neurofeedback engine (optionally emits a BioTrace-style HTML UI).",
     "qeeg_nf_cli --input session.edf --outdir out_nf --biotrace-ui"},

    {"qeeg_ui_cli", "UI",
     "Generate a static dashboard that links all qeeg tools and outputs.",
     "qeeg_ui_cli --root . --output qeeg_ui.html",
     "--output", "--root"},

    {"qeeg_ui_server_cli", "UI",
     "Serve the dashboard locally and enable one-click runs via a small local-only HTTP API.",
     "qeeg_ui_server_cli --root . --bin-dir ./build --port 8765 --open",
     "", "--root"},
  };
}

static std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

static std::string safe_id(const std::string& s) {
  // Convert to a safe HTML id.
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) out.push_back(c);
    else out.push_back('_');
  }
  if (out.empty()) out = "tool";
  return out;
}

static std::string infer_args_from_example(const std::string& tool_name,
                                          const std::string& example) {
  // Best-effort: if the example starts with the tool name, strip it.
  // Otherwise return the whole string so users can copy/paste as-is.
  std::string ex = trim(example);
  if (starts_with(ex, tool_name)) {
    ex = trim(ex.substr(tool_name.size()));
  }
  return ex;
}

static bool looks_absoluteish_path(const std::string& p) {
  if (p.empty()) return false;
  // Unix absolute.
  if (p[0] == '/' || p[0] == '\\') return true;
  // Windows drive letter, e.g. C:\ or C:/
  if (p.size() >= 2) {
    const unsigned char c0 = static_cast<unsigned char>(p[0]);
    if (std::isalpha(c0) != 0 && p[1] == ':') return true;
  }
  return false;
}

static std::string join_cmd_tokens(const std::vector<std::string>& toks) {
  std::string out;
  for (size_t i = 0; i < toks.size(); ++i) {
    if (i) out.push_back(' ');
    out += toks[i];
  }
  return out;
}

static std::string infer_ui_run_args_from_example(const std::string& tool_name,
                                                 const std::string& example) {
  // For the "Run" panel, we default output paths into the per-job run directory
  // created by qeeg_ui_server_cli (via {{RUN_DIR}}).
  //
  // This keeps UI-launched runs self-contained even when users keep the default
  // args and only swap out --input.
  std::string args = infer_args_from_example(tool_name, example);

  std::vector<std::string> toks = split_commandline_args(args);

  auto prefix_run_dir_for_dir = [](std::string* v) {
    if (!v || v->empty()) return;
    if (starts_with(*v, "{{RUN_DIR")) return;
    if (looks_absoluteish_path(*v)) return;
    // Strip a leading "./" for nicer output.
    if (starts_with(*v, "./")) *v = v->substr(2);
    *v = std::string("{{RUN_DIR}}/") + *v;
  };

  auto prefix_run_dir_for_file = [](std::string* v) {
    if (!v || v->empty()) return;
    if (starts_with(*v, "{{RUN_DIR")) return;
    if (looks_absoluteish_path(*v)) return;
    const std::filesystem::path p = std::filesystem::u8path(*v);
    const std::string base = p.filename().u8string();
    if (base.empty()) return;
    *v = std::string("{{RUN_DIR}}/") + base;
  };

  auto rewrite_flag_value = [&](const std::string& flag, bool is_dir) {
    const std::string eq = flag + "=";
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i] == flag) {
        if (i + 1 >= toks.size()) continue;
        if (is_dir) prefix_run_dir_for_dir(&toks[i + 1]);
        else prefix_run_dir_for_file(&toks[i + 1]);
        ++i;
        continue;
      }
      if (starts_with(toks[i], eq)) {
        std::string v = toks[i].substr(eq.size());
        if (is_dir) prefix_run_dir_for_dir(&v);
        else prefix_run_dir_for_file(&v);
        toks[i] = eq + v;
      }
    }
  };

  // Common output options used across tools.
  rewrite_flag_value("--outdir", true);
  rewrite_flag_value("--out-dir", true);
  rewrite_flag_value("--out", false);
  rewrite_flag_value("--output", false);
  rewrite_flag_value("--events-out", false);
  rewrite_flag_value("--events-out-tsv", false);
  rewrite_flag_value("--channel-map-template", false);

  return join_cmd_tokens(toks);
}



static std::string to_upper_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

static std::string strip_token_edges(std::string s) {
  s = trim(s);
  if (s.empty()) return s;

  auto is_edge = [](char c) {
    return c == '<' || c == '>' || c == '[' || c == ']' || c == '(' || c == ')' ||
           c == '{' || c == '}' || c == ',' || c == ':' || c == ';';
  };

  while (!s.empty() && is_edge(s.front())) s.erase(s.begin());
  while (!s.empty() && is_edge(s.back())) s.pop_back();
  return s;
}

static bool token_is_pathish_placeholder(const std::string& tok_raw) {
  // Most qeeg CLI tools document filesystem values using placeholders like:
  //   PATH / FILE / DIR / <dir> / <path>
  // We use this to infer which flags can accept the "selected path" from the
  // workspace browser.
  std::string tok = strip_token_edges(tok_raw);
  if (tok.empty()) return false;
  tok = to_upper_ascii(tok);

  if (tok.find("PATH") != std::string::npos) return true;
  if (tok.find("FILE") != std::string::npos) return true;
  if (tok.find("DIR") != std::string::npos) return true;
  if (tok.find("FOLDER") != std::string::npos) return true;
  if (tok.find("ROOT") != std::string::npos) return true;
  if (tok.find("DATASET") != std::string::npos) return true;

  return false;
}

static std::vector<std::string> extract_path_flags_from_help(const std::string& help) {
  // Best-effort parsing of --help output.
  //
  // We specifically want flags that take a filesystem path (input/output dirs,
  // BIDS roots, etc.) so the UI can offer a dropdown of "Inject selected path
  // into <flag>" options.
  std::vector<std::string> out;
  if (help.empty()) return out;

  auto add = [&](const std::string& f) {
    if (f.empty()) return;
    if (std::find(out.begin(), out.end(), f) != out.end()) return;
    out.push_back(f);
  };

  std::istringstream iss(help);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim(line);
    if (t.empty()) continue;

    // Option lines typically begin with a flag.
    if (starts_with(t, "--")) {
      std::istringstream ls(t);
      std::string flag;
      std::string placeholder;
      ls >> flag;
      ls >> placeholder;

      // Strip trailing punctuation for patterns like "--input,".
      if (!flag.empty() && (flag.back() == ',' || flag.back() == ';')) flag.pop_back();

      if (!flag.empty() && token_is_pathish_placeholder(placeholder)) {
        add(flag);
      }

      // Also capture alias flags, e.g. "Alias: --input".
      const size_t alias_pos = t.find("Alias:");
      if (alias_pos != std::string::npos) {
        const std::string after = t.substr(alias_pos + 6);
        std::istringstream as(after);
        std::string tok;
        while (as >> tok) {
          if (!tok.empty() && (tok.back() == ',' || tok.back() == ';')) tok.pop_back();
          if (starts_with(tok, "--")) add(tok);
        }
      }
    }
  }

  return out;
}

static std::string capture_stdout(const std::string& cmd, size_t max_bytes) {
  if (max_bytes == 0) max_bytes = 1024 * 1024;

#if defined(_WIN32)
  FILE* pipe = _popen(cmd.c_str(), "r");
#else
  FILE* pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) return std::string();

  std::string out;
  out.reserve(4096);
  char buf[4096];
  while (std::fgets(buf, static_cast<int>(sizeof(buf)), pipe) != nullptr) {
    out.append(buf);
    if (out.size() >= max_bytes) break;
  }

#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return out;
}

static std::string read_text_file_head(const std::filesystem::path& path,
                                      size_t max_bytes,
                                      size_t max_lines,
                                      bool* out_truncated) {
  if (out_truncated) *out_truncated = false;
  if (max_bytes == 0) max_bytes = 16 * 1024;
  if (max_lines == 0) max_lines = 80;

  std::ifstream f(path, std::ios::binary);
  if (!f) return std::string();

  std::string out;
  out.reserve(std::min<size_t>(max_bytes, 4096));

  std::string line;
  size_t n_lines = 0;
  bool truncated = false;

  while (n_lines < max_lines && std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // Ensure we always leave room for a newline.
    if (out.size() + line.size() + 1 > max_bytes) {
      const size_t remain = (max_bytes > out.size()) ? (max_bytes - out.size()) : 0;
      if (remain > 0) {
        const size_t take = std::min(remain, line.size());
        out.append(line.data(), take);
        if (out.size() < max_bytes) out.push_back('\n');
      }
      truncated = true;
      break;
    }
    out += line;
    out.push_back('\n');
    ++n_lines;
    if (out.size() >= max_bytes) {
      truncated = true;
      break;
    }
  }

  // If we hit the line cap and there's more content, mark as truncated.
  if (!truncated && n_lines >= max_lines && !f.eof()) {
    truncated = true;
  }

  if (out_truncated) *out_truncated = truncated;
  return out;
}

static std::filesystem::path resolve_exe_path(const std::filesystem::path& bin_dir,
                                              const std::string& exe_name) {
  std::filesystem::path p = bin_dir / exe_name;
  if (std::filesystem::exists(p)) return p;
  // Try with .exe (useful on Windows and also on POSIX when cross-built artifacts
  // are copied around with extensions intact).
  std::filesystem::path pe = p;
  pe += ".exe";
  if (std::filesystem::exists(pe)) return pe;
  return std::filesystem::path();
}

struct RunInfo {
  std::filesystem::path meta_path;   // absolute
  std::filesystem::path meta_dir;    // absolute
  std::string input_path;            // as stored in JSON (best-effort)
  std::vector<std::string> outputs;  // as stored in JSON
  std::filesystem::file_time_type mtime{};
};

static std::unordered_map<std::string, RunInfo> scan_latest_runs_by_tool(
    const std::filesystem::path& root) {
  std::unordered_map<std::string, RunInfo> best;
  std::error_code ec;

  if (!std::filesystem::exists(root, ec)) return best;

  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const auto p = it->path();
    if (!ends_with(p.filename().u8string(), "_run_meta.json")) continue;

    const std::string tool = read_run_meta_tool(p.u8string());
    if (tool.empty()) continue;

    RunInfo info;
    info.meta_path = p;
    info.meta_dir = p.parent_path();
    info.input_path = read_run_meta_input_path(p.u8string());
    info.outputs = read_run_meta_outputs(p.u8string());
    info.mtime = std::filesystem::last_write_time(p, ec);
    if (ec) info.mtime = std::filesystem::file_time_type{};

    auto jt = best.find(tool);
    if (jt == best.end()) {
      best[tool] = info;
      continue;
    }
    // Keep the newest by last_write_time when available.
    if (info.mtime > jt->second.mtime) {
      best[tool] = info;
    }
  }

  return best;
}

static std::string rel_link(const std::filesystem::path& from_dir,
                            const std::filesystem::path& target) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(target, from_dir, ec);
  if (ec || rel.empty()) {
    // Fall back to a generic path string (may still work as file://)
    return target.generic_u8string();
  }
  return rel.generic_u8string();
}

static void write_html(std::ostream& o, const UiDashboardArgs& args) {
  std::vector<ToolSpec> tools = default_tools();

  // Auto-discover tools present in the bin directory and add any missing ones
  // to the dashboard.
  if (args.scan_bin_dir && !args.bin_dir.empty()) {
    const std::filesystem::path bin_dir = std::filesystem::u8path(args.bin_dir);
    const std::vector<std::string> discovered = discover_cli_tools_in_bin_dir(bin_dir);

    std::unordered_map<std::string, size_t> idx;
    idx.reserve(tools.size());
    for (size_t i = 0; i < tools.size(); ++i) idx[tools[i].name] = i;

    for (const auto& name : discovered) {
      if (idx.find(name) != idx.end()) continue;
      ToolSpec t;
      t.name = name;
      t.group = infer_group_from_name(name);
      t.description = "Auto-discovered executable in --bin-dir.";
      t.example = name + " --help";
      tools.push_back(std::move(t));
    }
  }

  const std::filesystem::path root = std::filesystem::u8path(args.root);
  const std::filesystem::path out_html = std::filesystem::u8path(args.output_html);
  const std::filesystem::path out_dir = out_html.parent_path();

  // Optionally scan for run manifests.
  std::unordered_map<std::string, RunInfo> runs;
  if (args.scan_run_meta) {
    runs = scan_latest_runs_by_tool(root);
  }

  // Pre-capture help output so we can build a left nav with status.
  std::unordered_map<std::string, std::string> help_by_tool;
  std::unordered_map<std::string, bool> exe_found;

  const std::filesystem::path bin_dir = std::filesystem::u8path(args.bin_dir);
  if (args.embed_help && !args.bin_dir.empty()) {
    for (const auto& t : tools) {
      const std::filesystem::path exe = resolve_exe_path(bin_dir, t.name);
      exe_found[t.name] = !exe.empty();
      if (exe.empty()) {
        help_by_tool[t.name] = std::string();
        continue;
      }
      // Capture both stdout and stderr for tools that emit help to stderr.
      const std::string cmd = "\"" + exe.u8string() + "\" --help 2>&1";
      help_by_tool[t.name] = capture_stdout(cmd, 2 * 1024 * 1024);
    }
  }

  // Group tools.
  std::map<std::string, std::vector<ToolSpec>> by_group;
  for (const auto& t : tools) {
    by_group[t.group].push_back(t);
  }

  size_t n_with_runs = 0;
  for (const auto& t : tools) {
    if (runs.find(t.name) != runs.end()) ++n_with_runs;
  }

  o << "<!doctype html>\n"
    << "<html lang=\"en\">\n"
    << "<head>\n"
    << "  <meta charset=\"utf-8\">\n"
    << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    << "  <title>" << html_escape(args.title) << "</title>\n"
    << "  <style>\n"
    << "    :root{--bg:#0b1020;--panel:#111a33;--panel2:#0f1730;--text:#e7ecff;--muted:#a9b3d6;--accent:#6aa6ff;--good:#39d98a;--bad:#ff5c7a;--code:#0b0f1c;--border:rgba(255,255,255,0.10);}\n"
    << "    html,body{height:100%;margin:0;background:var(--bg);color:var(--text);font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial;}\n"
    << "    a{color:var(--accent);text-decoration:none} a:hover{text-decoration:underline}\n"
    << "    .layout{display:grid;grid-template-columns:320px 1fr;min-height:100vh}\n"
    << "    .sidebar{position:sticky;top:0;align-self:start;height:100vh;overflow:auto;background:var(--panel);border-right:1px solid var(--border)}\n"
    << "    .sidebar-top{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:16px 16px 8px}\n"
    << "    .sidebar-top h1{font-size:16px;margin:0}\n"
    << "    .sidebar .meta{color:var(--muted);margin:0 16px 12px;font-size:12px;line-height:1.4}\n"
    << "    .sidebar-close{display:none}\n"
    << "    #navToggleBtn{display:none}\n"
    << "    .sidebar-backdrop{position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:9000}\n"
    << "    .search{padding:0 16px 12px} .search input{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--border);background:var(--panel2);color:var(--text)}\n"
    << "    .group{margin:10px 0 16px} .group-title{padding:8px 16px;color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}\n"
    << "    .nav a{display:flex;gap:8px;align-items:center;padding:8px 16px;border-left:3px solid transparent;font-size:13px}\n"
    << "    .nav a:hover{background:rgba(255,255,255,0.04)}\n"
    << "    .nav-label{flex:1 1 auto;min-width:0}\n"
    << "    .nav-badge{margin-left:auto;flex:0 0 auto;font-size:11px;color:var(--text);border:1px solid var(--border);border-radius:999px;padding:1px 7px;background:rgba(106,166,255,0.12)}\n"
    << "    .nav-badge.running{color:var(--good);border-color:rgba(57,217,138,0.30);background:rgba(57,217,138,0.14)}\n"
    << "    .nav-badge.queued{color:var(--muted);background:rgba(255,255,255,0.04)}\n"
    << "    .nav-badge.stopping{color:var(--accent);background:rgba(106,166,255,0.12)}\n"
    << "    .dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex:0 0 auto}\n"
    << "    .dot.run{background:var(--good)}\n"
    << "    .dot.missing{background:var(--bad)}\n"
    << "    .content{padding:20px 24px 60px}\n"
    << "    .hero{background:linear-gradient(180deg, rgba(106,166,255,0.12), rgba(17,26,51,0));border:1px solid var(--border);border-radius:16px;padding:18px 18px 14px;margin-bottom:18px}\n"
    << "    .hero h2{margin:0 0 6px;font-size:18px}\n"
    << "    .hero p{margin:0;color:var(--muted);font-size:13px;line-height:1.5}\n"
    << "    .tool{border:1px solid var(--border);border-radius:16px;background:var(--panel2);padding:16px 16px 14px;margin:14px 0}\n"
    << "    .tool h3{margin:0 0 6px;font-size:16px} .tool .desc{margin:0 0 10px;color:var(--muted);font-size:13px;line-height:1.45}\n"
    << "    .row{display:flex;gap:12px;flex-wrap:wrap;margin-top:10px}\n"
    << "    .card{flex:1 1 380px;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:12px;padding:12px}\n"
    << "    .card h4{margin:0 0 8px;font-size:13px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
    << "    pre{background:var(--code);border:1px solid var(--border);border-radius:12px;padding:10px;overflow:auto;margin:0}\n"
    << "    code{font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, \"Liberation Mono\", \"Courier New\", monospace;font-size:12px;line-height:1.4}\n"
    << "    .btn{display:inline-flex;align-items:center;gap:8px;border:1px solid var(--border);background:rgba(255,255,255,0.04);color:var(--text);padding:8px 10px;border-radius:10px;cursor:pointer;font-size:12px}\n"
    << "    .btn:hover{background:rgba(255,255,255,0.08)}\n"
    << "    .btn.active{background:rgba(106,166,255,0.14);border-color:rgba(106,166,255,0.35)}\n"
    << "    .btn[disabled]{opacity:.55;cursor:not-allowed}\n"
    << "    .input{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--border);background:var(--panel);color:var(--text);font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, \"Liberation Mono\", \"Courier New\", monospace;font-size:12px;box-sizing:border-box}\n"
    << "    .statusline{margin-top:10px;color:var(--muted);font-size:12px;line-height:1.35;word-break:break-word}\n"
    << "    .statusline b{color:var(--text)}\n"
    << "    .outputs a{display:block;padding:4px 0;font-size:13px;word-break:break-all}\n"
    << "    .outrow{display:flex;gap:10px;align-items:center;flex-wrap:wrap;padding:4px 0}\n"
    << "    .outputs .outrow a{display:inline-block;padding:0;font-size:13px;word-break:break-all}\n"
    << "    details{margin-top:10px} summary{cursor:pointer;color:var(--accent);font-size:13px}\n"
    << "    .hidden{display:none !important}\n"
    << "    .pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);color:var(--muted);font-size:12px;margin-left:8px}\n"
    << "    .pill.run{color:var(--good)}\n"
    << "    .pill.live{color:var(--accent)}\n"
    << "    .pill.live.running{color:var(--good)}\n"
    << "    .pill.live.queued{color:var(--muted)}\n"
    << "    .pill.live.stopping{color:var(--accent)}\n"
    << "    table{width:100%;border-collapse:collapse}\n"
    << "    th,td{padding:6px 8px;border-bottom:1px solid var(--border);font-size:12px;vertical-align:top}\n"
    << "    th{color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}\n"
    << "    .small{font-size:12px;color:var(--muted)}\n"
    << "    #fsPanel.drop-active{outline:2px dashed rgba(106,166,255,0.55);outline-offset:4px;border-radius:12px}\n"
    << "    .fs-row[draggable=\"true\"]{cursor:grab}\n"
    << "    .fs-row.fs-reveal{background:rgba(106,166,255,0.14)}\n"
    << "    .fs-row.fs-reveal td{border-bottom-color:rgba(106,166,255,0.35)}\n"
    << "    input.drop-args{outline:2px dashed rgba(106,166,255,0.55);outline-offset:2px}\n"
    << "    .toast-wrap{position:fixed;right:16px;bottom:16px;z-index:10000;display:flex;flex-direction:column;gap:10px;max-width:min(360px, calc(100vw - 32px))}\n"
    << "    .toast{background:var(--panel2);border:1px solid var(--border);border-radius:12px;padding:10px 12px;box-shadow:0 10px 40px rgba(0,0,0,0.55);font-size:12px;color:var(--text);opacity:1;transform:translateY(0);transition:opacity .18s ease, transform .18s ease}\n"
    << "    .toast.fade{opacity:0;transform:translateY(6px)}\n"
    << "    @media (prefers-reduced-motion: reduce){.toast{transition:none}}\n"
    << "    .logtail{max-height:220px}\n"
    << "    .modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,0.55);display:flex;align-items:center;justify-content:center;padding:20px;z-index:9999}\n"
    << "    .modal{background:var(--panel2);border:1px solid var(--border);border-radius:16px;max-width:980px;width:100%;max-height:90vh;overflow:auto;box-shadow:0 10px 50px rgba(0,0,0,0.55)}\n"
    << "    .modal-header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 14px;border-bottom:1px solid var(--border)}\n"
    << "    .modal-title{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}\n"
    << "    .modal-body{padding:12px 14px}\n"
    << "    .file-actions{display:flex;gap:10px;flex-wrap:wrap}\n"
    << "    .note-preview{border:1px solid var(--border);border-radius:12px;padding:10px;background:var(--code);max-height:64vh;overflow:auto;margin-top:10px}\n"
    << "    .note-preview h1,.note-preview h2,.note-preview h3,.note-preview h4{margin:10px 0 6px;font-size:13px;color:var(--text)}\n"
    << "    .note-preview p{margin:6px 0;color:var(--text);font-size:13px;line-height:1.45}\n"
    << "    .note-preview ul{margin:6px 0 6px 18px}\n"
    << "    .note-preview li{margin:3px 0}\n"
    << "    .csv-controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px}\n"
    << "    .csv-viewwrap{border:1px solid var(--border);border-radius:12px;overflow:auto;max-height:64vh}\n"
    << "    .csv-table{min-width:640px}\n"
    << "    .csv-table thead th{position:sticky;top:0;background:var(--panel2);z-index:1}\n"
    << "    .csv-table td.num{text-align:right;color:var(--text)}\n"
    << "    .csv-heat{border:1px solid var(--border);border-radius:12px;background:#000;display:block;max-width:100%}\n"
    << "    .csv-heat-meta{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:8px}\n"
    << "    .qeeg-controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px}\n"
    << "    .qeeg-grid{display:grid;grid-template-columns:1fr;gap:10px}\n    .qeeg-grid.two{grid-template-columns:1fr}\n    @media (min-width: 980px){.qeeg-grid.two{grid-template-columns:1fr 420px}}\n    .qeeg-topo{width:100%;height:auto;max-width:420px}\n    .qeeg-topo-meta{display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;margin-top:8px}\n"
    << "    .qeeg-chart{border:1px solid var(--border);border-radius:12px;background:rgba(0,0,0,0.18);display:block;max-width:100%}\n"
    << "    .qeeg-kv{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:8px}\n"
    << "    .batch-viewwrap{border:1px solid var(--border);border-radius:12px;overflow:auto;max-height:46vh}\n"
    << "    .batch-table{min-width:640px}\n"
    << "    .batch-table thead th{position:sticky;top:0;background:var(--panel2);z-index:1}\n"
    << "    .cmd-list{border:1px solid var(--border);border-radius:12px;overflow:auto;max-height:62vh;background:rgba(255,255,255,0.02)}\n"
    << "    .cmd-item{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;padding:8px 10px;border-bottom:1px solid var(--border);cursor:pointer}\n"
    << "    .cmd-item:last-child{border-bottom:none}\n"
    << "    .cmd-item:hover{background:rgba(255,255,255,0.05)}\n"
    << "    .cmd-item.active{background:rgba(106,166,255,0.14)}\n"
    << "    .cmd-left{display:flex;flex-direction:column;gap:2px}\n"
    << "    .cmd-title{font-size:13px;color:var(--text)}\n"
    << "    .cmd-sub{font-size:11px;color:var(--muted)}\n"
    << "    .kbd{font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, \"Liberation Mono\", \"Courier New\", monospace;font-size:11px;color:var(--muted);border:1px solid var(--border);border-radius:8px;padding:2px 8px;white-space:nowrap;align-self:center}\n"
    << "    #bgFxCanvas{position:fixed;inset:0;width:100vw;height:100vh;z-index:0;pointer-events:none;opacity:0.55}\n"
    << "    .layout{position:relative;z-index:1}\n"
    << "    .fx-card canvas{width:100%;height:140px;display:block;border-radius:12px;border:1px solid var(--border);background:rgba(0,0,0,0.18)}\n"
    << "    .fx-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:10px}\n"
    << "    .selbar{margin-top:10px}\n"
    << "    .selrow{display:flex;gap:10px;flex-wrap:wrap;align-items:center}\n"
    << "    .selrow code{max-width:100%;overflow:auto}\n"
    << "    .sel-suggest{margin-top:8px}\n"
    << "    .sel-suggest b{color:var(--muted)}\n"
    << "    .btn.chip{padding:6px 10px;border-radius:999px}\n"
    << "    .fx-tabs{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}\n"
    << "    .fx-tab.active{background:rgba(106,166,255,0.14);border-color:rgba(106,166,255,0.35)}\n"
    << "    .runs-tabs{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:8px}\n"
    << "    .runs-tab.active{background:rgba(106,166,255,0.14);border-color:rgba(106,166,255,0.35)}\n"
    << "    .fx-stage{margin-top:8px}\n"
    << "    .fx-tools{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:6px}\n"
    << "    .fx-note{font-size:11px;color:var(--muted);margin-top:6px}\n"
    << "    @media (max-width: 980px){\n      .layout{grid-template-columns:1fr}\n      .sidebar{position:fixed;left:0;top:0;bottom:0;width:min(92vw,340px);max-width:340px;transform:translateX(-110%);transition:transform .18s ease;z-index:9001;box-shadow:0 10px 50px rgba(0,0,0,0.55)}\n      .sidebar.open{transform:translateX(0)}\n      .sidebar-close{display:inline-flex}\n      #navToggleBtn{display:inline-flex}\n      body.sidebar-open{overflow:hidden}\n      .content{padding:16px 14px 70px}\n    }\n    @media (prefers-reduced-motion: reduce){#bgFxCanvas{display:none}.sidebar{transition:none}}\n"
    << "  </style>\n"
    << "</head>\n";

  o << "<body>\n";
  o << "<canvas id=\"bgFxCanvas\" aria-hidden=\"true\"></canvas>\n";
  o << "<div class=\"layout\">\n";
  o << "  <div id=\"sidebarBackdrop\" class=\"sidebar-backdrop hidden\" aria-hidden=\"true\"></div>\n";

  // Sidebar.
  o << "  <aside id=\"sidebar\" class=\"sidebar\" aria-label=\"Tool navigation\">\n";
  o << "    <div class=\"sidebar-top\">\n";
  o << "      <h1>" << html_escape(args.title) << "</h1>\n";
  o << "      <button class=\"btn sidebar-close\" id=\"sidebarCloseBtn\" type=\"button\" aria-label=\"Close navigation\">Close</button>\n";
  o << "    </div>\n";
  o << "    <div class=\"meta\">\n";
  o << "      Root: <code>" << html_escape(args.root) << "</code><br>\n";
  o << "      Tools with discovered runs: <b>" << n_with_runs << "</b> / " << tools.size() << "\n";
  if (args.embed_help) {
    o << "<br>Help embedding: <b>on</b>";
    if (args.bin_dir.empty()) o << " (no --bin-dir; help sections may be empty)";
  } else {
    o << "<br>Help embedding: <b>off</b>";
  }
  o << "    </div>\n";
  o << "    <div class=\"search\"><input id=\"search\" placeholder=\"Filter tools…\" autocomplete=\"off\"></div>\n";

  o << "    <nav class=\"nav\">\n";
  for (const auto& kv : by_group) {
    o << "      <div class=\"group\">\n";
    o << "        <div class=\"group-title\">" << html_escape(kv.first) << "</div>\n";
    for (const auto& t : kv.second) {
      const std::string id = safe_id(t.name);
      const std::string nav_badge_id = std::string("nav_") + id + "_live";
      const bool has_run = runs.find(t.name) != runs.end();
      // If embed_help enabled, we can also mark missing executables.
      bool missing = false;
      if (args.embed_help && !args.bin_dir.empty()) {
        auto itf = exe_found.find(t.name);
        if (itf != exe_found.end() && !itf->second) missing = true;
      }
      o << "        <a href=\"#" << id << "\" data-tool=\"" << html_escape(t.name)
        << "\" data-group=\"" << html_escape(t.group) << "\">";
      o << "<span class=\"dot";
      if (has_run) o << " run";
      if (missing) o << " missing";
      o << "\"></span>";
      o << "<span class=\"nav-label\">" << html_escape(t.name) << "</span>";
      o << "<span class=\"nav-badge hidden\" id=\"" << html_escape(nav_badge_id) << "\"></span>";
      o << "</a>\n";
    }
    o << "      </div>\n";
  }
  o << "    </nav>\n";
  o << "  </aside>\n";

  // Content.
  o << "  <main class=\"content\">\n";
  o << "    <div class=\"hero\">\n";
  o << "      <h2>All qeeg executables, one place</h2>\n";
  o << "      <p>This is a self-contained dashboard that lists every <code>qeeg_*_cli</code> executable, embeds its <code>--help</code> output (optional), and links any discovered run outputs via <code>*_run_meta.json</code> manifests.</p>\n";
  o << "      <p style=\"margin-top:10px\"><b>Optional:</b> serve this dashboard using <code>qeeg_ui_server_cli</code> to enable one-click runs from the browser (local-only).</p>\n";
  o << "      <div id=\"apiStatus\" style=\"margin-top:10px;color:var(--muted);font-size:12px\">Run API: <b>checking…</b></div>\n";
  o << "      <div id=\"presetsStatus\" style=\"margin-top:6px;color:var(--muted);font-size:12px\">Presets: <b>local</b></div>\n";
  o << "      <div class=\"selbar\">\n";
  o << "        <div class=\"selrow\">\n";
  o << "          <span class=\"small\">Selected path:</span>\n";
  o << "          <code id=\"selectedInput\">(none)</code><span id=\"selectedInputType\" class=\"pill\" style=\"display:none\">file</span>\n";
  o << "          <button class=\"btn\" id=\"selBrowseBtn\" type=\"button\" disabled title=\"Open selection in Workspace browser\">Browse</button>\n";
  o << "          <button class=\"btn\" id=\"selPreviewBtn\" type=\"button\" disabled title=\"Preview selected file\">Preview</button>\n";
  o << "          <button class=\"btn\" id=\"selCopyBtn\" type=\"button\" disabled title=\"Copy selected path\">Copy</button>\n";
  o << "          <button class=\"btn\" id=\"selClearBtn\" type=\"button\" disabled title=\"Clear selection\">Clear</button>\n";
  o << "        </div>\n";
  o << "        <div id=\"selSuggest\" class=\"small sel-suggest\"></div>\n";
  o << "      </div>\n";
  o << "      <div class=\"fx-row\">\n";
  o << "        <button class=\"btn\" id=\"navToggleBtn\" type=\"button\" aria-label=\"Open navigation menu\" aria-controls=\"sidebar\" aria-expanded=\"false\">Menu</button>\n";
  o << "        <button class=\"btn\" id=\"fxToggleBtn\" aria-label=\"Toggle procedural animations\">Animations: <span id=\"fxToggleState\">on</span></button>\n";
  o << "        <button class=\"btn\" id=\"cmdOpenBtn\" type=\"button\" aria-label=\"Open command palette (Ctrl+K)\">Palette (Ctrl+K)</button>\n";
  o << "        <span id=\"fxHint\" class=\"small\"></span>\n";
  o << "      </div>\n";
  o << "      <div class=\"row\" style=\"margin-top:12px\">\n";
  o << "        <div class=\"card\" style=\"flex:1 1 420px\">\n";
  o << "          <h4>Recent UI runs</h4>\n";
  o << "          <div id=\"runsPanel\" class=\"small\">(available when served via <code>qeeg_ui_server_cli</code>)</div>\n";
  o << "        </div>\n";
  o << "        <div class=\"card\" style=\"flex:1 1 420px\">\n";
  o << "          <h4>Workspace browser</h4>\n";
  o << "          <div id=\"fsPanel\" class=\"small\">(available when served via <code>qeeg_ui_server_cli</code>)</div>\n";
  o << "        </div>\n";
  o << "        <div class=\"card fx-card\" style=\"flex:1 1 420px\">\n";
  o << "          <h4>Procedural visuals</h4>\n";
  o << "          <div id=\"fxActivityLabel\" class=\"small\">Decorative previews (no data). Activity: <b>idle</b>.</div>\n";
  o << "          <div class=\"fx-tabs\" role=\"tablist\" aria-label=\"Procedural visualization mode\">\n";
  o << "            <button class=\"btn fx-tab\" id=\"fxTabEeg\" data-mode=\"eeg\" type=\"button\">EEG</button>\n";
  o << "            <button class=\"btn fx-tab\" id=\"fxTabTopo\" data-mode=\"topo\" type=\"button\">Topomap</button>\n";
  o << "            <button class=\"btn fx-tab\" id=\"fxTabSpec\" data-mode=\"spec\" type=\"button\">Spectrogram</button>\n";
  o << "            <button class=\"btn fx-tab\" id=\"fxTabFlow\" data-mode=\"flow\" type=\"button\">Flow field</button>\n";
  o << "          </div>\n";
  o << "          <div id=\"fxModeHint\" class=\"fx-note\">Mode: <b>EEG</b> (synthetic)</div>\n";
  o << "          <div class=\"fx-tools\">\n";
  o << "            <button class=\"btn\" id=\"fxSnapBtn\" type=\"button\" title=\"Download a PNG snapshot of the current synthetic panel\">Snapshot PNG</button>\n";
  o << "            <button class=\"btn\" id=\"fxRecBtn\" type=\"button\" title=\"Record a short WebM clip of the current synthetic panel\">Record 5s</button>\n";
  o << "            <span id=\"fxRecHint\" class=\"small\"></span>\n";
  o << "          </div>\n";
  o << "          <div class=\"fx-stage\">\n";
  o << "            <canvas id=\"fxEegCanvas\" width=\"800\" height=\"220\"></canvas>\n";
  o << "            <canvas id=\"fxTopoCanvas\" width=\"800\" height=\"220\" class=\"hidden\"></canvas>\n";
  o << "            <canvas id=\"fxSpecCanvas\" width=\"800\" height=\"220\" class=\"hidden\"></canvas>\n";
  o << "            <canvas id=\"fxFlowCanvas\" width=\"800\" height=\"220\" class=\"hidden\"></canvas>\n";
  o << "          </div>\n";
  o << "        </div>\n";
  o << "      </div>\n";
  o << "    </div>\n";

  for (const auto& kv : by_group) {
    o << "    <h2 style=\"margin:22px 0 8px;font-size:14px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em\">"
      << html_escape(kv.first) << "</h2>\n";
    for (const auto& t : kv.second) {
      const std::string id = safe_id(t.name);
      const bool has_run = runs.find(t.name) != runs.end();
      o << "    <section class=\"tool\" id=\"" << id
        << "\" data-tool=\"" << html_escape(t.name)
        << "\" data-group=\"" << html_escape(t.group) << "\">\n";
      o << "      <h3>" << html_escape(t.name);
      if (has_run) o << " <span class=\"pill run\">run found</span>";
      // Live status badge (running/queued/stopping) is populated by the server-run API.
      o << " <span class=\"pill live hidden\" id=\"" << id << "_live\"></span>";
      o << "</h3>\n";
      o << "      <p class=\"desc\">" << html_escape(t.description) << "</p>\n";

      // If we know where binaries live, link the executable itself (handy for
      // quickly verifying what is installed/built when sharing the HTML).
      if (!args.bin_dir.empty()) {
        const std::filesystem::path exe = resolve_exe_path(bin_dir, t.name);
        if (!exe.empty()) {
          const std::string href = rel_link(out_dir, exe);
          o << "      <div style=\"color:var(--muted);font-size:12px;margin:0 0 10px\">"
            << "Binary: <a href=\"" << html_escape(href) << "\"><code>" << html_escape(href) << "</code></a>";
          o << "</div>\n";
        } else {
          o << "      <div style=\"color:var(--muted);font-size:12px;margin:0 0 10px\">"
            << "Binary: <span style=\"color:var(--bad)\">not found in bin-dir</span>";
          o << "</div>\n";
        }
      }

      o << "      <div class=\"row\">\n";

      // Example command.
      o << "        <div class=\"card\">\n";
      o << "          <h4>Example</h4>\n";
      const std::string ex_id = id + "_ex";
      o << "          <div style=\"display:flex;justify-content:space-between;gap:10px;align-items:flex-start\">\n";
      o << "            <pre style=\"flex:1\"><code id=\"" << ex_id << "\">"
        << html_escape(t.example) << "</code></pre>\n";
      o << "            <button class=\"btn\" onclick=\"copyText('" << ex_id << "')\">Copy</button>\n";
      o << "          </div>\n";
      o << "        </div>\n";

      // Optional Run panel (enabled when served by qeeg_ui_server_cli).
      {
        const std::string args_id = id + "_args";
        const std::string status_id = id + "_status";
        const std::string runbtn_id = id + "_runbtn";
        const std::string stopbtn_id = id + "_stopbtn";
        const std::string logwrap_id = id + "_logwrap";
        const std::string log_id = id + "_log";
        const std::string outwrap_id = id + "_outwrap";
        const std::string out_id = id + "_out";
        const std::string default_args = infer_ui_run_args_from_example(t.name, t.example);
        const std::string helpcode_id = id + "_help";

        // Flags that can accept a filesystem path. Used by the "Inject path" UI helper
        // so users can route the Workspace selection into any relevant flag
        // (e.g., --input, --outdir, --bids-root, --map-outdir, ...).
        std::vector<std::string> inject_flags;
        inject_flags.reserve(16);
        auto add_inject_flag = [&](const std::string& f) {
          if (f.empty()) return;
          if (std::find(inject_flags.begin(), inject_flags.end(), f) != inject_flags.end()) return;
          inject_flags.push_back(f);
        };
        add_inject_flag(t.inject_flag_file);
        add_inject_flag(t.inject_flag_dir);

        // If help was embedded, infer additional path flags from --help output.
        auto ith = help_by_tool.find(t.name);
        if (ith != help_by_tool.end()) {
          const std::vector<std::string> inferred = extract_path_flags_from_help(ith->second);
          for (const auto& f : inferred) add_inject_flag(f);
        }

        const std::string injectsel_id = id + "_inject";

        o << "        <div class=\"card\">\n";
        o << "          <h4>Run (optional)</h4>\n";
        o << "          <div style=\"color:var(--muted);font-size:12px;margin-bottom:8px\">";
        o << "Requires <code>qeeg_ui_server_cli</code> (local).\n";
        o << "          </div>\n";
        o << "          <input class=\"input\" id=\"" << args_id << "\" data-default=\"" << html_escape(default_args) << "\" placeholder=\"Args (e.g. --input file.edf --outdir out)\" value=\"" << html_escape(default_args) << "\">\n";
        o << "          <div style=\"display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap\">\n";
        o << "            <button class=\"btn run-btn\" id=\"" << runbtn_id << "\" data-tool=\"" << html_escape(t.name) << "\" data-args-id=\"" << args_id << "\" data-status-id=\"" << status_id << "\" data-stop-id=\"" << stopbtn_id << "\" data-logwrap-id=\"" << logwrap_id << "\" data-log-id=\"" << log_id << "\" disabled onclick=\"runTool(this)\">Run</button>\n";
        o << "            <button class=\"btn run-btn batch-btn\" data-tool=\"" << html_escape(t.name) << "\" data-args-id=\"" << args_id << "\" data-inject-sel-id=\"" << injectsel_id << "\" disabled onclick=\"openBatchRunner(this)\">Batch</button>\n";
        // Copy full command (includes tool name).
        const std::string full_id = id + "_full";
        const std::string preset_id = id + "_preset";
        o << "            <button class=\"btn\" onclick=\"copyFullCmd('" << full_id << "','" << args_id << "','" << html_escape(t.name) << "')\">Copy full command</button>\n";
        if (args.embed_help) {
          o << "            <button class=\"btn\" data-tool=\"" << html_escape(t.name)
            << "\" data-args-id=\"" << args_id
            << "\" data-help-id=\"" << helpcode_id
            << "\" data-inject-sel-id=\"" << injectsel_id
            << "\" onclick=\"openFlagHelper(this)\">Flags</button>\n";
        }
        o << "            <button class=\"btn\" id=\"" << stopbtn_id << "\" data-status-id=\"" << status_id << "\" disabled onclick=\"stopJob(this)\">Stop</button>\n";
        o << "            <button class=\"btn\" data-status-id=\"" << status_id << "\" data-logwrap-id=\"" << logwrap_id << "\" data-log-id=\"" << log_id << "\" onclick=\"toggleLog(this)\">Tail log</button>\n";
        o << "            <button class=\"btn\" data-status-id=\"" << status_id << "\" data-outwrap-id=\"" << outwrap_id << "\" data-out-id=\"" << out_id << "\" onclick=\"toggleOutputs(this)\">Outputs</button>\\n";

        // Path injection dropdown (populated from embedded --help output when available).
        o << "            <select class=\"input inject-select\" id=\"" << injectsel_id
          << "\" data-default-file=\"" << html_escape(t.inject_flag_file)
          << "\" data-default-dir=\"" << html_escape(t.inject_flag_dir)
          << "\" style=\"max-width:220px;width:auto\" title=\"Inject flag\" aria-label=\"Inject flag\">\n";
        o << "              <option value=\"\">Auto (recommended)</option>\n";
        for (const auto& f : inject_flags) {
          o << "              <option value=\"" << html_escape(f) << "\">" << html_escape(f) << "</option>\n";
        }
        o << "            </select>\n";

        o << "            <button class=\"btn use-input-btn\" data-args-id=\"" << args_id
          << "\" data-inject-sel-id=\"" << injectsel_id
          << "\" disabled onclick=\"useSelectedInput(this)\">Inject path</button>\n";
        o << "            <span id=\"" << full_id << "\" class=\"hidden\"></span>\n";
        o << "          </div>\n";
        o << "          <div style=\"display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap\">\n";
        o << "            <select class=\"input preset-select\" id=\"" << preset_id << "\" data-tool=\"" << html_escape(t.name) << "\" data-args-id=\"" << args_id << "\" style=\"max-width:240px\"></select>\n";
        o << "            <button class=\"btn\" onclick=\"savePreset(\'" << html_escape(t.name) << "\',\'" << args_id << "\',\'" << preset_id << "\')\">Save preset</button>\n";
        o << "            <button class=\"btn\" onclick=\"deletePreset(\'" << html_escape(t.name) << "\',\'" << preset_id << "\')\">Delete</button>\n";
        o << "            <button class=\"btn\" onclick=\"exportPresets(\'" << html_escape(t.name) << "\')\">Export</button>\n";
        o << "            <button class=\"btn\" onclick=\"importPresets(\'" << html_escape(t.name) << "\',\'" << preset_id << "\')\">Import</button>\n";
        o << "            <button class=\"btn\" onclick=\"resetArgs(\'" << args_id << "\')\">Reset</button>\n";
        o << "          </div>\n";
        o << "          <div class=\"statusline\" id=\"" << status_id << "\" data-outwrap-id=\"" << outwrap_id << "\" data-out-id=\"" << out_id << "\">Server not detected. Start: <code>qeeg_ui_server_cli --root . --bin-dir ./build</code></div>\\n";
        o << "          <div id=\"" << logwrap_id << "\" class=\"hidden\" style=\"margin-top:10px\">\n";
        o << "            <div style=\"display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap\">\n";
        o << "              <div class=\"small\">Log tail (live while open; latest ~64KB)</div>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" data-log-id=\"" << log_id << "\" onclick=\"refreshLog(this)\">Refresh</button>\n";
        o << "            </div>\n";
        o << "            <pre class=\"logtail\"><code id=\"" << log_id << "\"></code></pre>\n";
        o << "          </div>\n";
        o << "          <div id=\"" << outwrap_id << "\" class=\"hidden\" style=\"margin-top:10px\">\\n";
        o << "            <div style=\"display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap\">\\n";
        o << "              <div class=\"small\">Run outputs (from <code>ui_server_run_meta.json</code>)</div>\\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" onclick=\"browseRunDirFromStatus(this)\" title=\"Open this run folder in Workspace browser\">Browse</button>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" onclick=\"downloadZip(this)\" title=\"Download this run as a .zip\">Download zip</button>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" onclick=\"deleteRun(this)\" title=\"Delete this run folder under ui_runs (cleanup disk)\">Delete run</button>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" onclick=\"openNotesFromStatus(this)\" title=\"Edit a markdown note saved alongside this run\">Notes</button>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" data-out-id=\"" << out_id << "\" onclick=\"refreshOutputs(this)\">Refresh</button>\\n";
        o << "            </div>\\n";
        o << "            <div id=\"" << out_id << "\" class=\"small\" style=\"margin-top:8px\">No job yet.</div>\\n";
        o << "          </div>\\n";
        o << "        </div>\n";
      }

      // Outputs discovered.
      o << "        <div class=\"card outputs\">\n";
      o << "          <h4>Discovered outputs</h4>\n";
      auto itrun = runs.find(t.name);
      if (itrun == runs.end()) {
        o << "          <div style=\"color:var(--muted);font-size:13px\">No <code>*_run_meta.json</code> found under root for this tool.</div>\n";
      } else {
        const RunInfo& ri = itrun->second;
        // Link to the run meta file.
        o << "          <div style=\"color:var(--muted);font-size:12px;margin-bottom:6px\">"
          << "Run meta: <a href=\"" << html_escape(rel_link(out_dir, ri.meta_path)) << "\">"
          << html_escape(rel_link(out_dir, ri.meta_path)) << "</a></div>\n";

        if (!ri.input_path.empty()) {
          std::string base = ri.input_path;
          try {
            const std::filesystem::path ip = std::filesystem::u8path(ri.input_path);
            if (!ip.filename().empty()) base = ip.filename().u8string();
          } catch (...) {
            // Keep the raw string.
          }
          o << "          <div style=\"color:var(--muted);font-size:12px;margin-bottom:6px\">"
            << "Input: <code title=\"" << html_escape(ri.input_path) << "\">"
            << html_escape(base) << "</code></div>\n";
        }

        // Always list the meta itself and outputs.
        if (ri.outputs.empty()) {
          o << "          <div style=\"color:var(--muted);font-size:13px\">No Outputs[] listed in the run meta.</div>\n";
        } else {
          // Separate images for lightweight previews.
          std::vector<std::filesystem::path> image_paths;
          std::vector<std::filesystem::path> text_paths;
          for (const auto& rel : ri.outputs) {
            const std::filesystem::path p = ri.meta_dir / std::filesystem::u8path(rel);
            const std::string href = rel_link(out_dir, p);
            o << "          <div class=\"outrow\">\n";
            o << "            <button class=\"btn\" data-path=\"" << html_escape(href)
              << "\" onclick=\"selectPathAuto(this.dataset.path)\">Use as input</button>\n";
            o << "            <a href=\"" << html_escape(href)
              << "\" target=\"_blank\" rel=\"noopener\">" << html_escape(href) << "</a>\n";
            o << "          </div>\n";
            const std::string ext = to_lower(p.extension().u8string());
            if (ext == ".bmp" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp") {
              image_paths.push_back(p);
            }
            if (ext == ".csv" || ext == ".tsv" || ext == ".json" || ext == ".txt" || ext == ".log" || ext == ".md") {
              text_paths.push_back(p);
            }
          }
          // Preview a small subset of images (avoid bloating the page).
          const size_t max_previews = 6;
          if (!image_paths.empty()) {
            o << "          <div style=\"display:flex;flex-wrap:wrap;gap:8px;margin-top:10px\">\n";
            for (size_t k = 0; k < image_paths.size() && k < max_previews; ++k) {
              const std::string src = rel_link(out_dir, image_paths[k]);
              o << "            <a href=\"" << html_escape(src) << "\">"
                << "<img src=\"" << html_escape(src) << "\" style=\"width:96px;height:96px;object-fit:contain;border-radius:10px;border:1px solid var(--border);background:#000\">"
                << "</a>\n";
            }
            o << "          </div>\n";
          }

          // Lightweight text previews for small artifacts (CSV/TSV/JSON/TXT).
          const size_t max_text_previews = 4;
          if (!text_paths.empty()) {
            for (size_t k = 0; k < text_paths.size() && k < max_text_previews; ++k) {
              std::error_code tec;
              if (!std::filesystem::exists(text_paths[k], tec)) continue;
              bool truncated = false;
              const std::string preview = read_text_file_head(text_paths[k], 24 * 1024, 80, &truncated);
              if (preview.empty()) continue;

              const std::string label = text_paths[k].filename().u8string();
              o << "          <details>\n";
              o << "            <summary>Preview " << html_escape(label);
              if (truncated) o << " (truncated)";
              o << "</summary>\n";
              o << "            <pre><code>" << html_escape(preview) << "</code></pre>\n";
              o << "          </details>\n";
            }
          }
        }
      }
      o << "        </div>\n";

      o << "      </div>\n"; // row

      // Help (embedded)
      if (args.embed_help) {
        o << "      <details>\n";
        o << "        <summary>Show --help output</summary>\n";

        std::string help;
        auto ith = help_by_tool.find(t.name);
        if (ith != help_by_tool.end()) help = ith->second;

        if (help.empty()) {
          // If embed_help enabled but missing exe/help.
          o << "        <div style=\"color:var(--muted);font-size:13px;margin:10px 0\">";
          if (args.bin_dir.empty()) {
            o << "No <code>--bin-dir</code> provided, so help was not captured.";
          } else {
            o << "Help output not captured (executable not found in <code>" << html_escape(args.bin_dir) << "</code> or the tool wrote no help to stdout).";
          }
          o << "</div>\n";
        } else {
          o << "        <pre><code id=\"" << id << "_help\">" << html_escape(help) << "</code></pre>\n";
        }
        o << "      </details>\n";
      }

      o << "    </section>\n";
    }
  }

  o << "  </main>\n";
  o << "</div>\n";

  // JS: search + copy + optional run API.
  
  // Preview modal (used by the dynamic Outputs viewer).
  o << R"HTML(
<div id="previewBackdrop" class="modal-backdrop hidden" onclick="if(event.target===this) closePreview();">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="previewTitle">
    <div class="modal-header">
      <div class="modal-title" id="previewTitle">Preview</div>
      <div class="file-actions">
        <a class="btn" id="previewOpenLink" href="#" target="_blank" rel="noopener">Open</a>
        <button class="btn" onclick="closePreview()">Close</button>
      </div>
    </div>
    <div class="modal-body" id="previewBody"></div>
  </div>
</div>
)HTML";

  // Flag helper modal (parses embedded --help output into a clickable option list).
  o << R"HTML(
<div id="flagsBackdrop" class="modal-backdrop hidden" onclick="if(event.target===this) closeFlagHelper();">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="flagsTitle">
    <div class="modal-header">
      <div class="modal-title" id="flagsTitle">Flags</div>
      <div class="file-actions">
        <button class="btn" onclick="closeFlagHelper()">Close</button>
      </div>
    </div>
    <div class="modal-body">
      <div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px">
        <input class="input" id="flagsSearch" placeholder="filter flags (e.g., --input)" style="max-width:360px">
        <span class="small" id="flagsMeta"></span>
      </div>
      <div id="flagsList" class="small"></div>
    </div>
  </div>
</div>
)HTML";


  // Run notes modal (saved under ui_runs/<run>/note.md via /api/note).
  o << R"HTML(
<div id="noteBackdrop" class="modal-backdrop hidden" onclick="if(event.target===this) closeNotes();">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="noteTitle">
    <div class="modal-header">
      <div class="modal-title" id="noteTitle">Run notes</div>
      <div class="file-actions">
        <a class="btn" id="noteOpenLink" href="#" target="_blank" rel="noopener">Open file</a>
        <button class="btn" id="noteSaveBtn" onclick="saveNotes()">Save</button>
        <button class="btn" onclick="closeNotes()">Close</button>
      </div>
    </div>
    <div class="modal-body">
      <div class="small" style="margin-bottom:10px">Run dir: <code id="noteRunDir"></code></div>
      <div class="runs-tabs" style="margin-bottom:8px">
        <button class="btn runs-tab" id="noteTabEdit" type="button" onclick="setNotesView('edit')">Edit</button>
        <button class="btn runs-tab" id="noteTabPreview" type="button" onclick="setNotesView('preview')">Preview</button>
        <span id="noteStatus" class="small"></span>
      </div>
      <textarea class="input" id="noteText" style="min-height:280px;white-space:pre" placeholder="# Notes&#10;&#10;- ..."></textarea>
      <div id="notePreview" class="note-preview hidden"></div>
    </div>
  </div>
</div>
)HTML";
  // Batch runner modal (multi-input queue helper).
  o << R"HTML(
<div id="batchBackdrop" class="modal-backdrop hidden" onclick="if(event.target===this) closeBatch();">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="batchTitle">
    <div class="modal-header">
      <div class="modal-title" id="batchTitle">Batch run</div>
      <div class="file-actions">
        <button class="btn" id="batchRunBtn" onclick="runBatch()">Run batch</button>
        <button class="btn" onclick="closeBatch()">Close</button>
      </div>
    </div>
    <div class="modal-body">
      <div class="small" style="margin-bottom:10px">Tool: <code id="batchTool"></code></div>
      <div class="small" style="margin-bottom:10px">Directory: <code id="batchDir"></code></div>

      <div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px">
        <input class="input" id="batchFilter" placeholder="filter files (substring or *.ext)" style="max-width:340px">
        <select class="input" id="batchExt" style="max-width:170px;width:auto" aria-label="Extension filter">
          <option value="">All</option>
          <option value=".edf">.edf</option>
          <option value=".bdf">.bdf</option>
          <option value=".csv">.csv</option>
          <option value=".tsv">.tsv</option>
          <option value=".set">.set</option>
          <option value=".vhdr">.vhdr</option>
        </select>
        <button class="btn" onclick="batchSelectAll()">Select all</button>
        <button class="btn" onclick="batchClearSelection()">Clear</button>
        <button class="btn" onclick="batchRefresh()">Refresh</button>
        <span class="small" id="batchCount"></span>
      </div>

      <div class="batch-viewwrap">
        <table class="batch-table" id="batchTable"></table>
      </div>

      <div style="margin-top:14px">
        <div class="small" style="margin-bottom:6px">Args template</div>
        <input class="input" id="batchArgs" placeholder="e.g. --input {input} --outdir {{RUN_DIR}}/out_map">
        <div class="small" style="margin-top:8px">
          Placeholders: <code>{input}</code>, <code>{name}</code>, <code>{stem}</code>, <code>{index}</code>.<br>
          Server placeholders still work: <code>{{RUN_DIR}}</code>, <code>{{RUN_DIR_ABS}}</code>.
        </div>
      </div>

      <div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:10px">
        <select class="input" id="batchInjectFlag" style="max-width:220px;width:auto" aria-label="Inject flag"></select>
        <button class="btn" onclick="batchInsertInput()">Insert {input}</button>
        <button class="btn" onclick="batchUseInjectFlag()" title="Append/replace inject flag with {input}">Use inject flag</button>
        <span class="small" id="batchStatus"></span>
      </div>
      <div id="batchProgress" class="small" style="margin-top:10px"></div>
    </div>
  </div>
</div>
)HTML";


  // Command palette modal (Ctrl+K quick actions).
  o << R"HTML(
<div id="cmdBackdrop" class="modal-backdrop hidden" onclick="if(event.target===this) closeCmdPalette();">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="cmdTitle">
    <div class="modal-header">
      <div class="modal-title" id="cmdTitle">Command palette</div>
      <div class="file-actions">
        <span class="kbd" title="Keyboard shortcut">Ctrl+K</span>
        <button class="btn" onclick="closeCmdPalette()">Close</button>
      </div>
    </div>
    <div class="modal-body">
      <input class="input" id="cmdInput" placeholder="Type a command… (e.g., map, workspace, history)" autocomplete="off">
      <div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-top:10px">
        <span class="small" id="cmdMeta"></span>
        <span class="small">↑/↓ navigate • Enter select • Esc close</span>
      </div>
      <div id="cmdList" class="cmd-list" role="listbox" aria-label="Commands" style="margin-top:10px"></div>
    </div>
  </div>
</div>
)HTML";

o << "  <div id=\"toastWrap\" class=\"toast-wrap\" aria-live=\"polite\" aria-atomic=\"false\"></div>\n";

o << R"JS(<script>
function copyText(id){const el=document.getElementById(id); if(!el) return; const t=el.innerText||el.textContent||''; navigator.clipboard.writeText(t).then(()=>{showToast('Copied');},()=>{});}
function copyFullCmd(_ignored, argsId, tool){const el=document.getElementById(argsId); const args=(el&&el.value)?el.value:''; const cmd=(tool+' '+args).trim(); navigator.clipboard.writeText(cmd).then(()=>{showToast('Copied full command');},()=>{});}
const search=document.getElementById('search');
const sidebarEl=document.getElementById('sidebar');
const sidebarBackdrop=document.getElementById('sidebarBackdrop');
const navToggleBtn=document.getElementById('navToggleBtn');
const sidebarCloseBtn=document.getElementById('sidebarCloseBtn');
let sidebarLastFocus=null;
function norm(s){return (s||'').toLowerCase();}
function applyFilter(){const q=norm(search.value);
  document.querySelectorAll('[data-tool]').forEach(el=>{
    const name=norm(el.getAttribute('data-tool'));
    const ok = q==='' || name.includes(q);
    if(el.tagName==='A') el.classList.toggle('hidden', !ok);
    else el.classList.toggle('hidden', !ok);
  });
  // Hide empty group titles in sidebar
  document.querySelectorAll('.group').forEach(g=>{
    const links=[...g.querySelectorAll('a[data-tool]')];
    const any=links.some(a=>!a.classList.contains('hidden'));
    g.classList.toggle('hidden', !any);
  });
}
search.addEventListener('input', applyFilter);

function isSmallScreen(){try{return window.matchMedia && window.matchMedia('(max-width: 980px)').matches;}catch(e){return false;}}
function setSidebarOpen(open, opts){
  opts = opts || {};
  const restoreFocus = (opts.restoreFocus !== false);
  if(!sidebarEl || !sidebarBackdrop || !navToggleBtn) return;
  const isOpen = sidebarEl.classList.contains('open');
  if(open===isOpen) return;
  if(open){
    // Avoid stacking overlays (palette vs. drawer) on small screens.
    try{ closeCmdPalette(); }catch(_e){}
    sidebarLastFocus = document.activeElement;
    sidebarEl.classList.add('open');
    sidebarBackdrop.classList.remove('hidden');
    document.body.classList.add('sidebar-open');
    navToggleBtn.setAttribute('aria-expanded','true');
    setTimeout(()=>{try{if(search) search.focus();}catch(_e){}},0);
  }else{
    sidebarEl.classList.remove('open');
    sidebarBackdrop.classList.add('hidden');
    document.body.classList.remove('sidebar-open');
    navToggleBtn.setAttribute('aria-expanded','false');
    const lf = sidebarLastFocus;
    sidebarLastFocus = null;
    if(restoreFocus && lf && typeof lf.focus==='function'){try{lf.focus();}catch(_e){}}
  }
}
function toggleSidebar(){ if(!sidebarEl) return; setSidebarOpen(!sidebarEl.classList.contains('open')); }
function closeSidebar(){ setSidebarOpen(false); }
function initSidebar(){
  if(navToggleBtn) navToggleBtn.addEventListener('click', ()=>{ toggleSidebar(); });
  if(sidebarBackdrop) sidebarBackdrop.addEventListener('click', ()=>{ closeSidebar(); });
  if(sidebarCloseBtn) sidebarCloseBtn.addEventListener('click', ()=>{ closeSidebar(); });

  document.querySelectorAll('.sidebar a[href^="#"]').forEach(a=>{
    a.addEventListener('click', ()=>{
      if(isSmallScreen()) setSidebarOpen(false, {restoreFocus:false});
    });
  });

  // If we resize out of the small-screen breakpoint, ensure we clear overlay state.
  window.addEventListener('resize', ()=>{
    if(!isSmallScreen()){
      if(sidebarBackdrop) sidebarBackdrop.classList.add('hidden');
      document.body.classList.remove('sidebar-open');
      if(navToggleBtn) navToggleBtn.setAttribute('aria-expanded','false');
      if(sidebarEl) sidebarEl.classList.remove('open');
    }
  });
}

let qeegApiOk=false;
let qeegApiToken='';
let runsTimer=null;
let runsViewMode='session';
let runsHistory=[];
let runsHistoryFetchedAt=0;
let runsHistoryTimer=null;
let selectedInputPath='';
let selectedInputType='file'; // 'file' | 'dir'
let fsCurrentDir='';
let fsEntries=[];
let fsFindResults=[];
let fsFindMeta=null;
let fsFindRunning=false;
let fsShowHidden=false;
let fsSortMode='name'; // 'name' | 'mtime' | 'size'
let fsSortDesc=false;
let fsUploadInput=null;
let fsUploading=false;
let fsDragDepth=0;
let fsNavHistory=[];
let fsNavPos=-1;
let fsPendingRevealPath='';
let fsUploadStatusTimer=null;
let fxEnabled=true;
let fxMode='eeg'; // 'eeg' | 'topo' | 'spec'
let fxEngine=null;
let fxRunningCount=0;
let fxReduceMotion=false;
let fxHasUserPref=false;
let notesRunDir='';
let notesDirty=false;
let notesView='edit';
let notesLastFocus=null;
let presetsStore=null;
let presetsSource='local'; // 'local' | 'server'
let presetsServerEnabled=false;
let presetsSyncing=false;
let presetsSaveTimer=null;
let presetsSavePending=false;
let presetsImportInput=null;
let presetsImportTool='';
let presetsImportSelId='';

let cmdLastFocus=null;
let cmdItems=[];
let cmdFiltered=[];
let cmdSel=0;

// Live job counts per tool (from /api/runs). Used to decorate the nav + palette
// so busy tools are easier to spot.
let toolLive={}; // toolName -> {running, queued, stopping}





function esc(s){
  return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}


function showToast(msg, ms){
  msg = String(msg||'');
  ms = Number(ms||0);
  if(!isFinite(ms) || ms<=0) ms = 2400;
  const wrap = document.getElementById('toastWrap');
  if(!wrap) return;
  const div = document.createElement('div');
  div.className = 'toast';
  div.textContent = msg;
  wrap.appendChild(div);
  const fadeAt = Math.max(0, ms - 180);
  setTimeout(()=>{ try{ div.classList.add('fade'); }catch(e){} }, fadeAt);
  setTimeout(()=>{ try{ div.remove(); }catch(e){ try{ if(div.parentNode) div.parentNode.removeChild(div); }catch(e2){} } }, ms);
}

function uiScrollBehavior(){
  return (fxReduceMotion ? 'auto' : 'smooth');
}

function isModalOpen(id){
  const el=document.getElementById(id);
  return !!(el && !el.classList.contains('hidden'));
}

function closeCmdPalette(){
  const back=document.getElementById('cmdBackdrop');
  if(!back || back.classList.contains('hidden')) return;
  back.classList.add('hidden');
  const lf = cmdLastFocus;
  cmdLastFocus=null;
  if(lf && lf.focus) try{ lf.focus(); }catch(e){}
}

function focusToolSection(sec){
  if(!sec) return;
  try{ sec.scrollIntoView({behavior:uiScrollBehavior(), block:'start'}); }catch(e){}
  try{ location.hash = '#'+sec.id; }catch(e){}
  const argsEl = document.getElementById((sec.id||'') + '_args');
  if(argsEl){
    try{ argsEl.focus(); argsEl.select(); }catch(e){}
  }
}

function buildCmdItems(){
  const items=[];

  // Selection actions (if a Workspace selection exists).
  if(selectedInputPath){
    const base = selectionBaseName(selectedInputPath);
    items.push({title:'Selection: Copy path', subtitle:String(selectedInputPath||''), action:()=>{ try{ selectionCopy(); }catch(e){} }});
    if(selectedInputType!=='dir'){
      items.push({title:'Selection: Preview', subtitle: base, action:()=>{ try{ selectionPreview(); }catch(e){} }});
    }
    items.push({title:'Selection: Browse in Workspace', subtitle:'Reveal selection in browser', action:()=>{ try{ selectionBrowse(); }catch(e){} }});
    items.push({title:'Selection: Clear', subtitle:'Clear selected path', action:()=>{ try{ selectionClear(); }catch(e){} }});
  }

  // Quick actions
  items.push({title:'Workspace: Root', subtitle:'Browse --root', action:()=>{ try{ fsRoot(); }catch(e){} }});
  if(fsNavPos>0){ items.push({title:'Workspace: Back', subtitle:'Go to previous folder', action:()=>{ try{ initFsBrowser(); fsBack(); }catch(e){} }}); }
  if(fsNavPos>=0 && fsNavPos < (fsNavHistory.length-1)){ items.push({title:'Workspace: Forward', subtitle:'Go to next folder', action:()=>{ try{ initFsBrowser(); fsForward(); }catch(e){} }}); }
  items.push({title:'Workspace: ui_runs', subtitle:'Browse UI runs', action:()=>{ try{ fsRuns(); }catch(e){} }});
  items.push({title:'Workspace: Trash', subtitle:'Browse .qeeg_trash', action:()=>{ try{ fsTrashDir(); }catch(e){} }});
  items.push({title:'Workspace: Upload…', subtitle:'Upload files into current folder', action:()=>{ try{ initFsBrowser(); fsOpenUpload(); }catch(e){} }});
  items.push({title:'Workspace: New folder…', subtitle:'Create folder in current directory', action:()=>{ try{ initFsBrowser(); fsNewFolder(); }catch(e){} }});
  items.push({title:'Workspace: Refresh', subtitle:'Refresh Workspace listing', action:()=>{ try{ initFsBrowser(); refreshFs(); }catch(e){} }});
  items.push({title:'Workspace: Find…', subtitle:'Focus recursive search in Workspace browser', action:()=>{ try{ initFsBrowser(); const q=document.getElementById('fsFindQ'); if(q){ q.focus(); q.select(); } }catch(e){} }});
  items.push({title:'Runs: Session view', subtitle:'Jobs since server start', action:()=>{ try{ setRunsView('session', false); }catch(e){} }});
  items.push({title:'Runs: History view', subtitle:'Scan ui_runs', action:()=>{ try{ setRunsView('history', false); refreshHistory(true); }catch(e){} }});
  items.push({title:(fxEnabled?'Animations: Disable':'Animations: Enable'), subtitle:'Procedural canvas effects', action:()=>{ try{ fxEnabled = !fxEnabled; fxSavePref(); fxApply(); }catch(e){} }});
  items.push({title:(fsShowHidden?'Workspace: Hide dotfiles':'Workspace: Show dotfiles'), subtitle:'Toggle hidden files in browser', action:()=>{ try{ fsSetShowHidden(!fsShowHidden); }catch(e){} }});

  // Tools
  const secs=[...document.querySelectorAll('section.tool[data-tool]')];
  secs.sort((a,b)=> norm(a.getAttribute('data-tool')).localeCompare(norm(b.getAttribute('data-tool'))));
  for(const sec of secs){
    const name = sec.getAttribute('data-tool')||'';
    const group = sec.getAttribute('data-group')||'';
    if(!name) continue;
    const live = toolLive[name];
    const hint = liveToHint(live);
    items.push({
      title:'Tool: '+name,
      subtitle: group || 'Tool',
      hint: hint,
      action:()=>{ focusToolSection(sec); }
    });
  }
  return items;
}

function renderCmdList(){
  const list=document.getElementById('cmdList');
  const meta=document.getElementById('cmdMeta');
  if(!list) return;

  if(cmdFiltered.length===0){
    list.innerHTML = '<div class="small" style="color:var(--muted);padding:10px">No matches.</div>';
    if(meta) meta.textContent = '0 items';
    return;
  }

  let html='';
  for(let i=0;i<cmdFiltered.length;i++){
    const it=cmdFiltered[i];
    const active = (i===cmdSel);
    html += '<div class="cmd-item'+(active?' active':'')+'" role="option" aria-selected="'+(active?'true':'false')+'" onclick="cmdActivate('+i+')">';
    html += '<div class="cmd-left">';
    html += '<div class="cmd-title">'+esc(it.title||'')+'</div>';
    if(it.subtitle) html += '<div class="cmd-sub">'+esc(it.subtitle)+'</div>';
    html += '</div>';
    if(it.hint) html += '<div class="kbd">'+esc(it.hint)+'</div>';
    html += '</div>';
  }
  list.innerHTML = html;
  if(meta) meta.textContent = cmdFiltered.length + ' item(s)';

  const activeEl = list.querySelector('.cmd-item.active');
  if(activeEl) activeEl.scrollIntoView({block:'nearest'});
}

function cmdFilterItems(){
  const input=document.getElementById('cmdInput');
  const q = norm(input ? input.value : '');
  cmdFiltered = cmdItems.filter(it=>{
    const hay = norm((it.title||'')+' '+(it.subtitle||''));
    return q==='' || hay.includes(q);
  });
  cmdSel = 0;
  renderCmdList();
}

function cmdActivate(i){
  cmdSel = i;
  const it = cmdFiltered[i];
  if(!it || !it.action) return;
  closeCmdPalette();
  try{ it.action(); }catch(e){}
}

function openCmdPalette(){
  const back=document.getElementById('cmdBackdrop');
  const input=document.getElementById('cmdInput');
  if(!back || !input) return;

  // Don't open if another modal is open (except this one).
  if(isModalOpen('previewBackdrop') || isModalOpen('flagsBackdrop') || isModalOpen('noteBackdrop') || isModalOpen('batchBackdrop')) return;

  // If the navigation drawer is open (mobile), close it so we don't stack overlays.
  // Preserve the *pre-drawer* focus (sidebarLastFocus) so Esc returns to something visible.
  let lf = document.activeElement;
  if(sidebarEl && sidebarEl.classList.contains('open')){
    lf = sidebarLastFocus || navToggleBtn || lf;
    try{ setSidebarOpen(false, {restoreFocus:false}); }catch(e){}
  }
  cmdLastFocus = lf;
  back.classList.remove('hidden');
  cmdItems = buildCmdItems();
  cmdSel = 0;
  input.value = '';
  cmdFiltered = cmdItems.slice();
  renderCmdList();
  setTimeout(()=>{ try{ input.focus(); }catch(e){} }, 0);
}

function cmdOnKey(e){
  if(e.key==='ArrowDown'){
    e.preventDefault();
    cmdSel = Math.min(cmdSel+1, cmdFiltered.length-1);
    renderCmdList();
  }else if(e.key==='ArrowUp'){
    e.preventDefault();
    cmdSel = Math.max(cmdSel-1, 0);
    renderCmdList();
  }else if(e.key==='Enter'){
    e.preventDefault();
    cmdActivate(cmdSel);
  }
}

function initCmdPalette(){
  const btn = document.getElementById('cmdOpenBtn');
  if(btn) btn.addEventListener('click', (e)=>{ e.preventDefault(); openCmdPalette(); });
  const input = document.getElementById('cmdInput');
  if(input){
    input.addEventListener('input', cmdFilterItems);
    input.addEventListener('keydown', cmdOnKey);
  }
}

function apiHeaders(extra){
  const h = Object.assign({}, extra||{});
  if(qeegApiToken) h['X-QEEG-Token']=qeegApiToken;
  return h;
}

async function apiFetch(url, opts){
  opts = opts||{};
  opts.headers = apiHeaders(opts.headers||{});
  opts.cache = opts.cache||'no-store';
  return fetch(url, opts);
}


// ----- Procedural animations (optional) -----
// Decorative only: a canvas background + a selectable synthetic visualization.
// Modes:
//   - EEG: multi-band synthetic waves
//   - Topomap: synthetic scalp field (interpolated from moving "electrode" sources)
//   - Spectrogram: synthetic time-frequency waterfall
//
// Nothing here reads any recording data.

function clamp01(x){
  x = Number(x);
  if(!isFinite(x)) return 0;
  if(x < 0) return 0;
  if(x > 1) return 1;
  return x;
}

function lerp(a,b,t){
  t = clamp01(t);
  return a + (b-a)*t;
}

function getCssVar(name, fallback){
  try{
    const v = getComputedStyle(document.documentElement).getPropertyValue(name);
    return (v && v.trim()) ? v.trim() : (fallback||'');
  }catch(e){
    return fallback||'';
  }
}

function parseHexColor(hex){
  hex = String(hex||'').trim();
  if(!hex) hex = '#6aa6ff';
  if(hex[0] === '#') hex = hex.slice(1);
  if(hex.length === 3){
    hex = hex[0]+hex[0]+hex[1]+hex[1]+hex[2]+hex[2];
  }
  if(hex.length !== 6){
    return {r:106,g:166,b:255};
  }
  const n = parseInt(hex, 16);
  if(!isFinite(n)) return {r:106,g:166,b:255};
  return {r:(n>>16)&255, g:(n>>8)&255, b:n&255};
}

function mixRgb(a,b,t){
  return {
    r: Math.round(lerp(a.r,b.r,t)),
    g: Math.round(lerp(a.g,b.g,t)),
    b: Math.round(lerp(a.b,b.b,t))
  };
}

function rgba(rgb, a){
  a = clamp01(a);
  return 'rgba('+rgb.r+','+rgb.g+','+rgb.b+','+a.toFixed(3)+')';
}

function hashSeed(str){
  str = String(str||'');
  // FNV-1a (32-bit)
  let h = 2166136261>>>0;
  for(let i=0;i<str.length;i++){
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h>>>0;
}

function mulberry32(seed){
  // Small, fast, seeded PRNG (32-bit state).
  let a = (seed>>>0);
  return function(){
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t>>>15), t | 1);
    t ^= t + Math.imul(t ^ (t>>>7), t | 61);
    t ^= (t>>>14);
    return (t>>>0) / 4294967296;
  };
}

function fxActivity(){
  // Map running jobs to [0,1]. When the local server is not connected,
  // this will remain at the idle baseline.
  return clamp01(0.15 + 0.18 * (fxRunningCount||0));
}

function fxModeLabel(m){
  if(m==='topo') return 'Topomap';
  if(m==='spec') return 'Spectrogram';
  if(m==='flow') return 'Flow field';
  return 'EEG';
}

function fxShowCanvas(mode){
  const eeg = document.getElementById('fxEegCanvas');
  const topo = document.getElementById('fxTopoCanvas');
  const spec = document.getElementById('fxSpecCanvas');
  const flow = document.getElementById('fxFlowCanvas');
  if(eeg)  eeg.classList.toggle('hidden', mode!=='eeg');
  if(topo) topo.classList.toggle('hidden', mode!=='topo');
  if(spec) spec.classList.toggle('hidden', mode!=='spec');
  if(flow) flow.classList.toggle('hidden', mode!=='flow');
}

class QeegFxEngine{
  constructor(bgCanvas, eegCanvas, topoCanvas, specCanvas, flowCanvas){
    this.bg = bgCanvas;
    this.eeg = eegCanvas;
    this.topo = topoCanvas;
    this.spec = specCanvas;
    this.flow = flowCanvas;

    this.ctxBg = (this.bg && this.bg.getContext) ? this.bg.getContext('2d', {alpha:true}) : null;
    this.ctxEeg = (this.eeg && this.eeg.getContext) ? this.eeg.getContext('2d', {alpha:true}) : null;
    this.ctxTopo = (this.topo && this.topo.getContext) ? this.topo.getContext('2d', {alpha:true}) : null;
    this.ctxSpec = (this.spec && this.spec.getContext) ? this.spec.getContext('2d', {alpha:true}) : null;
    this.ctxFlow = (this.flow && this.flow.getContext) ? this.flow.getContext('2d', {alpha:true}) : null;

    this.dpr = 1;
    this.seed = hashSeed(document.title + '|' + location.pathname);
    this.rng = mulberry32(this.seed);

    this.particles = [];
    this.t = 0;
    this.running = false;
    this.lastTs = 0;
    this.lastDrawTs = 0;

    this.mode = 'eeg';

    this.accent = parseHexColor(getCssVar('--accent', '#6aa6ff'));
    this.muted  = parseHexColor(getCssVar('--muted',  '#a9b3d6'));
    this.good   = parseHexColor(getCssVar('--good',   '#39d98a'));
    this.bad    = parseHexColor(getCssVar('--bad',    '#ff5c7a'));

    // Topomap offscreen buffer (lower-res for speed).
    this.topoOff = null;
    this.topoOffCtx = null;
    this.topoImg = null;
    this.topoElectrodes = [];
    this.lastTopoTs = 0;

    // Spectrogram state.
    this.specInit = false;

    // Flow field state.
    this.flowInit = false;
    this.flowParticles = [];

    this.resize();
    this.initParticles();
    this.initTopo();
    this.initSpectrogram(true);
    this.initFlow(true);
    this.renderStatic();
  }

  setMode(mode){
    mode = (mode==='topo' || mode==='spec' || mode==='flow' || mode==='eeg') ? mode : 'eeg';
    if(this.mode === mode) return;
    this.mode = mode;
    if(mode==='spec'){
      this.initSpectrogram(true);
    }
    if(mode==='flow'){
      this.initFlow(true);
    }
    this.renderStatic();
  }

  resize(){
    this.dpr = Math.max(1, Math.min(2, window.devicePixelRatio||1));

    const fit = (c)=>{
      if(!c) return;
      const rect = c.getBoundingClientRect();
      const w = Math.max(1, Math.floor(rect.width  * this.dpr));
      const h = Math.max(1, Math.floor(rect.height * this.dpr));
      if(c.width  !== w) c.width  = w;
      if(c.height !== h) c.height = h;
    };

    if(this.bg){
      this.bg.width  = Math.max(1, Math.floor(window.innerWidth  * this.dpr));
      this.bg.height = Math.max(1, Math.floor(window.innerHeight * this.dpr));
    }
    fit(this.eeg);
    fit(this.topo);
    fit(this.spec);
    fit(this.flow);

    // Re-init buffers sized to DPR / canvas dimensions.
    this.initTopo();
    this.initSpectrogram(true);
    this.initFlow(true);
  }

  initParticles(){
    if(!this.bg) return;
    const w = this.bg.width;
    const h = this.bg.height;

    // Scale particle count with viewport size (clamped).
    const density = (w*h)/(1200*1200);
    const n = Math.max(40, Math.min(120, Math.floor(70 * density + 40)));

    this.particles = [];
    for(let i=0;i<n;i++){
      const speed = 0.25 + this.rng()*0.75;
      const ang = this.rng() * Math.PI*2;
      this.particles.push({
        x: this.rng()*w,
        y: this.rng()*h,
        vx: Math.cos(ang)*speed,
        vy: Math.sin(ang)*speed,
        r: 1 + this.rng()*1.8,
        phase: this.rng()*Math.PI*2
      });
    }
  }

  initTopo(){
    // Create a small offscreen canvas for fast scalar-field rendering.
    if(!this.topoOff){
      this.topoOff = document.createElement('canvas');
      this.topoOffCtx = this.topoOff.getContext('2d', {alpha:true});
    }
    const base = 160;
    const res = Math.max(96, Math.min(220, Math.floor(base * this.dpr)));
    if(this.topoOff.width !== res) this.topoOff.width = res;
    if(this.topoOff.height !== res) this.topoOff.height = res;
    if(this.topoOffCtx){
      this.topoImg = this.topoOffCtx.createImageData(res, res);
    }else{
      this.topoImg = null;
    }

    // Synthetic electrode locations in a rough 10-20-ish ring + center points.
    const pts = [
      [-0.65,  0.55], [0.0,  0.72], [0.65, 0.55],
      [-0.82,  0.10], [0.0,  0.22], [0.82, 0.10],
      [-0.72, -0.28], [0.0, -0.12], [0.72,-0.28],
      [-0.45, -0.68], [0.0, -0.78], [0.45,-0.68]
    ];
    this.topoElectrodes = pts.map((p,i)=>({
      x: p[0],
      y: p[1],
      phase: this.rng()*Math.PI*2 + i*0.4,
      k: 0.6 + this.rng()*0.9
    }));
    this.lastTopoTs = 0;
  }

  initSpectrogram(clear){
    if(!this.ctxSpec || !this.spec) return;
    if(clear || !this.specInit){
      const w = this.spec.width;
      const h = this.spec.height;
      this.ctxSpec.clearRect(0,0,w,h);
      this.ctxSpec.fillStyle = 'rgba(0,0,0,0.18)';
      this.ctxSpec.fillRect(0,0,w,h);
      this.specInit = true;
    }
  }

  initFlow(force){
    const ctx = this.ctxFlow;
    if(!ctx || !this.flow) return;

    const w = this.flow.width;
    const h = this.flow.height;

    // Keep particle count roughly tied to CSS pixel area (not DPR-scaled pixel area)
    // so higher-DPR screens don't explode CPU usage.
    const cssArea = (w/this.dpr) * (h/this.dpr);
    const n = Math.max(220, Math.min(1400, Math.floor(cssArea / 350)));

    if(force || !this.flowInit || this.flowParticles.length !== n){
      this.flowParticles = [];
      for(let i=0;i<n;i++){
        this.flowParticles.push({
          x: this.rng()*w,
          y: this.rng()*h,
          px: 0,
          py: 0,
          age: this.rng()*2.5,
          life: 2.0 + this.rng()*3.0
        });
      }
      this.flowInit = true;

      // Prime the canvas with a dark base.
      ctx.clearRect(0,0,w,h);
      ctx.fillStyle = 'rgba(0,0,0,0.18)';
      ctx.fillRect(0,0,w,h);
    }
  }


  start(){
    if(this.running) return;
    this.running = true;
    this.lastTs = 0;

    const loop = (ts)=>{
      if(!this.running) return;
      this.frame(ts);
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
  }

  stop(){
    this.running = false;
  }

  frame(ts){
    if(!this.lastTs) this.lastTs = ts;
    const dt = Math.min(0.05, Math.max(0.0, (ts - this.lastTs)/1000));
    this.lastTs = ts;

    // Throttle to ~30fps to reduce CPU/battery usage.
    if(this.lastDrawTs && (ts - this.lastDrawTs) < 1000/30) return;
    this.lastDrawTs = ts;

    this.t += dt;
    this.draw(dt);
  }

  draw(dt){
    this.drawBackground(dt);

    if(this.mode === 'topo'){
      this.drawTopo(dt);
    }else if(this.mode === 'spec'){
      this.drawSpec(dt);
    }else if(this.mode === 'flow'){
      this.drawFlow(dt);
    }else{
      this.drawEeg(dt);
    }
  }

  renderStatic(){
    // Render one frame (used when FX is disabled / reduced-motion).
    this.draw(0.016);
  }

  drawBackground(dt){
    const ctx = this.ctxBg;
    if(!ctx || !this.bg) return;

    const w = this.bg.width;
    const h = this.bg.height;
    ctx.clearRect(0,0,w,h);

    const act = fxActivity();
    const speed = (0.35 + act*1.1) * this.dpr;
    const linkDist = (120 + act*60) * this.dpr;
    const linkDist2 = linkDist*linkDist;

    // Update particle positions.
    for(const p of this.particles){
      // Gentle procedural drift (deterministic per-particle).
      const drift = 0.25 * Math.sin(this.t*0.7 + p.phase);

      p.x += (p.vx + drift)*speed;
      p.y += (p.vy + drift*0.6)*speed;

      // Wrap.
      if(p.x < -10) p.x = w+10;
      if(p.x > w+10) p.x = -10;
      if(p.y < -10) p.y = h+10;
      if(p.y > h+10) p.y = -10;

      // Very small velocity wobble to avoid perfect loops.
      p.phase += dt * (0.35 + act*0.8);
      p.vx += 0.002 * Math.cos(p.phase);
      p.vy += 0.002 * Math.sin(p.phase*1.3);
    }

    // Connection lines.
    ctx.lineWidth = 1 * this.dpr;
    for(let i=0;i<this.particles.length;i++){
      const a = this.particles[i];
      for(let j=i+1;j<this.particles.length;j++){
        const b = this.particles[j];
        const dx = a.x - b.x;
        const dy = a.y - b.y;
        const d2 = dx*dx + dy*dy;
        if(d2 > linkDist2) continue;
        const d = Math.sqrt(d2);
        const alpha = (1 - d/linkDist) * (0.12 + 0.22*act);
        ctx.strokeStyle = rgba(this.accent, alpha);
        ctx.beginPath();
        ctx.moveTo(a.x, a.y);
        ctx.lineTo(b.x, b.y);
        ctx.stroke();
      }
    }

    // Nodes.
    for(const p of this.particles){
      const alpha = 0.22 + 0.25*act;
      ctx.fillStyle = rgba(this.accent, alpha);
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.r*this.dpr, 0, Math.PI*2);
      ctx.fill();
    }
  }

  drawEeg(dt){
    const ctx = this.ctxEeg;
    if(!ctx || !this.eeg) return;

    const w = this.eeg.width;
    const h = this.eeg.height;

    ctx.clearRect(0,0,w,h);
    ctx.fillStyle = 'rgba(0,0,0,0.12)';
    ctx.fillRect(0,0,w,h);

    const act = fxActivity();

    const bands = [
      {name:'delta', hz:2.0,  amp:1.00},
      {name:'theta', hz:6.0,  amp:0.85},
      {name:'alpha', hz:10.0, amp:0.70},
      {name:'beta',  hz:18.0, amp:0.55},
      {name:'gamma', hz:35.0, amp:0.42}
    ];

    const rowH = h / bands.length;

    ctx.lineWidth = 1.5 * this.dpr;
    ctx.font = (11*this.dpr)+'px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace';
    ctx.textBaseline = 'top';

    for(let i=0;i<bands.length;i++){
      const b = bands[i];
      const y0 = (i + 0.5) * rowH;
      const A  = (rowH*0.28) * b.amp * (0.55 + 0.55*act);

      // Baseline.
      ctx.strokeStyle = rgba(this.muted, 0.12);
      ctx.beginPath();
      ctx.moveTo(0, y0);
      ctx.lineTo(w, y0);
      ctx.stroke();

      // Label.
      ctx.fillStyle = rgba(this.muted, 0.55);
      ctx.fillText(b.name, 8*this.dpr, (i*rowH + 6*this.dpr));

      // Wave (synthetic / procedural).
      const basePhase = this.t * (0.9 + 0.15*act) * b.hz;
      ctx.strokeStyle = rgba(this.accent, 0.30 + 0.09*i);
      ctx.beginPath();

      const step = Math.max(2, Math.floor(4*this.dpr));
      for(let x=0;x<=w;x+=step){
        const xn = x / w;

        // Sum-of-sines + slow modulation. The constants are tuned purely for visuals.
        const mod = 0.65 + 0.35*Math.sin(this.t*0.6 + i*1.7);
        const v =
          Math.sin(2*Math.PI*(xn*(b.hz*0.35) + basePhase*0.07)) +
          0.45*Math.sin(2*Math.PI*(xn*(b.hz*0.18) + basePhase*0.04 + 0.3*i)) +
          0.18*Math.sin(2*Math.PI*(xn*(b.hz*0.62) + basePhase*0.11 + 0.8));

        const y = y0 + A*mod*v;
        if(x===0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
  }


  drawFlow(dt){
    const ctx = this.ctxFlow;
    if(!ctx || !this.flow) return;

    const w = this.flow.width;
    const h = this.flow.height;
    const act = fxActivity();

    if(!this.flowInit) this.initFlow(true);

    // Trail fade.
    ctx.fillStyle = 'rgba(0,0,0,'+(0.10 + 0.04*(1-act)).toFixed(3)+')';
    ctx.fillRect(0,0,w,h);

    // Stroke settings.
    ctx.lineWidth = 1.0 * this.dpr;
    ctx.strokeStyle = rgba(this.accent, 0.16 + 0.18*act);

    const t = this.t;
    const stepBase = (26 + 22*act) * this.dpr;
    const speed = 0.85 + 1.35*act;

    ctx.beginPath();

    for(const p of this.flowParticles){
      p.px = p.x;
      p.py = p.y;

      const nx = (p.x / w) * 2 - 1;
      const ny = (p.y / h) * 2 - 1;

      // Potential field (analytic curl) -> swirling vector field.
      // p1 = sin(nx*f1 + t*s1) * cos(ny*f2 - t*s2)
      const f1 = 3.0, f2 = 3.6;
      const s1 = 0.7, s2 = 0.9;
      const sNx = Math.sin(nx*f1 + t*s1);
      const cNy = Math.cos(ny*f2 - t*s2);

      const dp1_dnx = Math.cos(nx*f1 + t*s1) * f1 * cNy;
      const dp1_dny = sNx * (-Math.sin(ny*f2 - t*s2) * f2);

      // p2 = 0.5 * sin((nx+ny)*a1 + t*s3) * cos((nx-ny)*b1 - t*s4)
      const a1 = 2.2, b1 = 1.8;
      const s3 = 0.4, s4 = 0.3;
      const A = (nx+ny)*a1 + t*s3;
      const B = (nx-ny)*b1 - t*s4;
      const sinA = Math.sin(A), cosA = Math.cos(A);
      const sinB = Math.sin(B), cosB = Math.cos(B);

      const dp2_dnx = 0.5*(cosA*a1*cosB + sinA*(-sinB)*b1);
      const dp2_dny = 0.5*(cosA*a1*cosB + sinA*(-sinB)*(-b1));

      const dnx = dp1_dnx + dp2_dnx;
      const dny = dp1_dny + dp2_dny;

      // Curl: v = (∂p/∂y, -∂p/∂x)
      let vx = dny;
      let vy = -dnx;

      // Normalize for stable speed.
      const mag = Math.sqrt(vx*vx + vy*vy) + 1e-6;
      vx /= mag;
      vy /= mag;

      const step = stepBase * dt * speed;
      p.x += vx * step;
      p.y += vy * step;

      p.age += dt;

      // Respawn if out-of-bounds or too old.
      if(p.x < -10 || p.x > w+10 || p.y < -10 || p.y > h+10 || p.age > p.life){
        p.x = this.rng()*w;
        p.y = this.rng()*h;
        p.px = p.x;
        p.py = p.y;
        p.age = 0;
        p.life = 2.0 + this.rng()*3.0;
        continue;
      }

      ctx.moveTo(p.px, p.py);
      ctx.lineTo(p.x, p.y);
    }

    ctx.stroke();

    // Label.
    ctx.font = (11*this.dpr)+'px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace';
    ctx.fillStyle = rgba(this.muted, 0.55);
    ctx.fillText('synthetic flow field', 8*this.dpr, 8*this.dpr);
  }

  drawTopo(dt){
    const ctx = this.ctxTopo;
    if(!ctx || !this.topo || !this.topoOff || !this.topoOffCtx || !this.topoImg) return;

    const now = performance && performance.now ? performance.now() : (this.t*1000);

    // Topomap is heavier: limit updates to ~15fps.
    if(this.lastTopoTs && (now - this.lastTopoTs) < (1000/15)) return;
    this.lastTopoTs = now;

    const act = fxActivity();

    const W = this.topoOff.width;
    const H = this.topoOff.height;
    const img = this.topoImg;
    const data = img.data;

    // Head circle in offscreen coords.
    const cx = W*0.5;
    const cy = H*0.5;
    const R  = Math.min(W,H)*0.44;

    // Precompute dynamic electrode values.
    const elec = this.topoElectrodes.map((e, i)=>{
      const drift = 0.06 * Math.sin(this.t*0.55 + e.phase);
      const drift2 = 0.05 * Math.cos(this.t*0.42 + e.phase*1.3);
      return {
        x: e.x + drift,
        y: e.y + drift2,
        v: Math.sin(this.t*(0.8 + 0.35*act)*e.k + e.phase) * (0.45 + 0.55*act)
      };
    });

    // Weight sigma in normalized head coords.
    const sigma = 0.45;

    let k = 0;
    for(let y=0;y<H;y++){
      const yn = (y - cy)/R; // normalized [-1,1]
      for(let x=0;x<W;x++){
        const xn = (x - cx)/R;
        const rr = xn*xn + yn*yn;
        if(rr > 1.0){
          // Transparent outside head.
          data[k++] = 0;
          data[k++] = 0;
          data[k++] = 0;
          data[k++] = 0;
          continue;
        }

        let sum = 0;
        let wsum = 0;
        for(const e of elec){
          const dx = xn - e.x;
          const dy = yn - e.y;
          const d2 = dx*dx + dy*dy;
          const w = Math.exp(-d2/(2*sigma*sigma));
          sum += w * e.v;
          wsum += w;
        }
        let v = (wsum > 1e-9) ? (sum/wsum) : 0;
        // v in [-1,1] approximately.
        v = Math.max(-1, Math.min(1, v));

        // Map negative to "bad" (reddish), positive to accent (blue), with muted center.
        let rgb;
        if(v >= 0){
          rgb = mixRgb(this.muted, this.accent, v);
        }else{
          rgb = mixRgb(this.muted, this.bad, -v);
        }

        // Vignette.
        const edge = Math.sqrt(rr);
        const a = 0.92 - 0.22*edge;
        data[k++] = rgb.r;
        data[k++] = rgb.g;
        data[k++] = rgb.b;
        data[k++] = Math.round(255*clamp01(a));
      }
    }

    this.topoOffCtx.putImageData(img, 0, 0);

    // Composite to visible canvas.
    const w = this.topo.width;
    const h = this.topo.height;

    ctx.clearRect(0,0,w,h);
    ctx.fillStyle = 'rgba(0,0,0,0.12)';
    ctx.fillRect(0,0,w,h);

    // Draw scaled topomap.
    ctx.save();
    ctx.imageSmoothingEnabled = true;
    const size = Math.min(w, h) * 0.92;
    const dx = (w - size)/2;
    const dy = (h - size)/2;
    ctx.globalAlpha = 0.95;
    ctx.drawImage(this.topoOff, dx, dy, size, size);
    ctx.restore();

    // Draw head outline + nose.
    const cx2 = w*0.5;
    const cy2 = h*0.5;
    const R2  = size*0.5;

    ctx.lineWidth = 2 * this.dpr;
    ctx.strokeStyle = rgba(this.muted, 0.38);
    ctx.beginPath();
    ctx.arc(cx2, cy2, R2, 0, Math.PI*2);
    ctx.stroke();

    // Simple nose triangle.
    ctx.strokeStyle = rgba(this.muted, 0.28);
    ctx.beginPath();
    ctx.moveTo(cx2, cy2 - R2);
    ctx.lineTo(cx2 - R2*0.08, cy2 - R2*0.88);
    ctx.lineTo(cx2 + R2*0.08, cy2 - R2*0.88);
    ctx.closePath();
    ctx.stroke();

    // Electrodes.
    ctx.fillStyle = rgba(this.good, 0.55);
    for(const e of elec){
      const ex = cx2 + e.x*R2;
      const ey = cy2 + e.y*R2;
      ctx.beginPath();
      ctx.arc(ex, ey, 2.2*this.dpr, 0, Math.PI*2);
      ctx.fill();
    }

    // Label.
    ctx.font = (11*this.dpr)+'px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace';
    ctx.fillStyle = rgba(this.muted, 0.55);
    ctx.fillText('synthetic topomap', 8*this.dpr, 8*this.dpr);
  }

  drawSpec(dt){
    const ctx = this.ctxSpec;
    if(!ctx || !this.spec) return;

    const w = this.spec.width;
    const h = this.spec.height;
    const act = fxActivity();

    // Shift left to create a waterfall effect.
    const shift = Math.max(1, Math.floor(2 * this.dpr));
    ctx.save();
    ctx.globalCompositeOperation = 'source-over';
    ctx.drawImage(this.spec, -shift, 0);

    // Slight fade on the new column region.
    ctx.fillStyle = 'rgba(0,0,0,0.12)';
    ctx.fillRect(w-shift, 0, shift, h);

    // Generate a synthetic spectrum column.
    // y=0 top is "high freq"; y=h bottom is "low freq".
    const maxHz = 45;
    const stepY = Math.max(1, Math.floor(2 * this.dpr));

    const peak = (hz, mu, sig)=> Math.exp(-((hz-mu)*(hz-mu))/(2*sig*sig));

    for(let y=0;y<h;y+=stepY){
      const hz = (1 - (y / h)) * maxHz;

      // Time-varying amplitudes for each band.
      const aDelta = 0.50 + 0.35*Math.sin(this.t*0.45 + 0.1);
      const aTheta = 0.55 + 0.35*Math.sin(this.t*0.55 + 1.1);
      const aAlpha = 0.65 + 0.35*Math.sin(this.t*0.60 + 2.2);
      const aBeta  = 0.45 + 0.35*Math.sin(this.t*0.80 + 3.0);
      const aGamma = 0.35 + 0.30*Math.sin(this.t*0.95 + 4.2);

      let p =
        aDelta*peak(hz, 2.0,  1.5) +
        aTheta*peak(hz, 6.0,  2.0) +
        aAlpha*peak(hz, 10.0, 2.0) +
        aBeta *peak(hz, 18.0, 3.0) +
        aGamma*peak(hz, 35.0, 5.0);

      // Activity increases contrast a bit.
      p = clamp01(p * (0.55 + 0.65*act));

      const rgb = mixRgb(this.muted, this.accent, p);
      ctx.fillStyle = rgba(rgb, 0.95);
      ctx.fillRect(w-shift, y, shift, stepY);
    }

    // Grid lines + labels (light).
    ctx.strokeStyle = rgba(this.muted, 0.12);
    ctx.lineWidth = 1*this.dpr;
    for(const frac of [0.25, 0.5, 0.75]){
      const yy = Math.floor(frac*h);
      ctx.beginPath();
      ctx.moveTo(0, yy);
      ctx.lineTo(w, yy);
      ctx.stroke();
    }
    ctx.font = (11*this.dpr)+'px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace';
    ctx.fillStyle = rgba(this.muted, 0.55);
    ctx.fillText('synthetic spectrogram', 8*this.dpr, 8*this.dpr);

    ctx.restore();
  }
}

function fxUpdateUi(){
  const st = document.getElementById('fxToggleState');
  const hint = document.getElementById('fxHint');
  if(st) st.textContent = fxEnabled ? 'on' : 'off';

  if(hint){
    if(fxReduceMotion && !fxHasUserPref){
      hint.textContent = 'Disabled by system reduced-motion preference.';
    }else{
      hint.textContent = fxEnabled ? 'Procedural canvas effects are enabled (local-only).' : 'Animations disabled.';
    }
  }

  const lab = document.getElementById('fxActivityLabel');
  if(lab){
    if(fxRunningCount > 0){
      lab.innerHTML = 'Decorative previews (no data). Activity: <b>'+fxRunningCount+'</b> running job(s).';
    }else{
      lab.innerHTML = 'Decorative previews (no data). Activity: <b>idle</b>.';
    }
  }

  const modeHint = document.getElementById('fxModeHint');
  if(modeHint){
    modeHint.innerHTML = 'Mode: <b>'+fxModeLabel(fxMode)+'</b> (synthetic)';
  }

  // Mark active tab button.
  const tabIds = {'eeg':'fxTabEeg','topo':'fxTabTopo','spec':'fxTabSpec','flow':'fxTabFlow'};
  for(const k in tabIds){
    const el = document.getElementById(tabIds[k]);
    if(el) el.classList.toggle('active', k===fxMode);
  }
}

function fxSavePref(){
  try{
    localStorage.setItem('qeeg_fx_enabled', fxEnabled ? '1' : '0');
    fxHasUserPref = true;
  }catch(e){}
}

function fxSaveMode(){
  try{
    localStorage.setItem('qeeg_fx_mode', fxMode);
  }catch(e){}
}

function fxApply(){
  if(!fxEngine){
    fxUpdateUi();
    return;
  }
  fxShowCanvas(fxMode);
  fxEngine.setMode(fxMode);

  if(fxEnabled){
    fxEngine.start();
  }else{
    fxEngine.stop();
    fxEngine.renderStatic();
  }
  fxUpdateUi();
}

function fxSetMode(mode){
  mode = (mode==='topo' || mode==='spec' || mode==='flow' || mode==='eeg') ? mode : 'eeg';
  fxMode = mode;
  fxSaveMode();
  fxApply();
}

function fxSetRunningCount(n){
  fxRunningCount = Math.max(0, parseInt(n||0,10) || 0);
  fxUpdateUi();
}

function initFx(){
  const bg   = document.getElementById('bgFxCanvas');
  const eeg  = document.getElementById('fxEegCanvas');
  const topo = document.getElementById('fxTopoCanvas');
  const spec = document.getElementById('fxSpecCanvas');
  const flow = document.getElementById('fxFlowCanvas');

  fxReduceMotion = !!(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);

  // URL override: ?fx=0 or ?fx=1
  // Mode override: ?fxmode=eeg|topo|spec|flow
  let urlFx = '';
  let urlMode = '';
  try{
    const sp = new URLSearchParams(location.search);
    urlFx = sp.get('fx') || '';
    urlMode = sp.get('fxmode') || '';
  }catch(e){}

  if(urlFx==='0' || urlFx==='1'){
    fxEnabled = (urlFx==='1');
    fxHasUserPref = true;
  }else if(!fxHasUserPref){
    fxEnabled = !fxReduceMotion;
  }

  if(urlMode==='eeg' || urlMode==='topo' || urlMode==='spec' || urlMode==='flow'){
    fxMode = urlMode;
  }

  fxShowCanvas(fxMode);

  if(bg || eeg || topo || spec || flow){
    fxEngine = new QeegFxEngine(bg, eeg, topo, spec, flow);
  }

  // Toggle button.
  const btn = document.getElementById('fxToggleBtn');
  if(btn){
    btn.onclick = ()=>{
      fxEnabled = !fxEnabled;
      fxSavePref();
      fxApply();
    };
  }

  // Mode buttons.
  document.querySelectorAll('.fx-tab').forEach(b=>{
    b.addEventListener('click', ()=>{
      const m = b.getAttribute('data-mode') || 'eeg';
      fxSetMode(m);
    });
  });

  // Snapshot / Record helpers (local-only).
  const recHint = document.getElementById('fxRecHint');
  const setRecHint = (t)=>{ if(recHint) recHint.textContent = t || ''; };

  const activeCanvas = ()=>{
    if(fxMode==='topo') return topo;
    if(fxMode==='spec') return spec;
    if(fxMode==='flow') return flow;
    return eeg;
  };

  const fileStamp = ()=>{
    try{
      return new Date().toISOString().replace(/[:.]/g,'-');
    }catch(e){
      return ''+Date.now();
    }
  };

  const snapBtn = document.getElementById('fxSnapBtn');
  if(snapBtn){
    snapBtn.addEventListener('click', ()=>{
      const c = activeCanvas();
      if(!c) return;
      try{
        const a = document.createElement('a');
        a.href = c.toDataURL('image/png');
        a.download = 'qeeg_fx_'+fxMode+'_'+fileStamp()+'.png';
        document.body.appendChild(a);
        a.click();
        a.remove();
        setRecHint('Saved snapshot PNG.');
      }catch(e){
        setRecHint('Snapshot failed: '+(e && e.message ? e.message : e));
      }
    });
  }

  let rec = null;
  let recStream = null;
  let recChunks = [];
  let recTimer = null;

  const recBtn = document.getElementById('fxRecBtn');
  if(recBtn){
    recBtn.addEventListener('click', ()=>{
      if(rec) return;

      const c = activeCanvas();
      if(!c || !c.captureStream || (typeof MediaRecorder === 'undefined')){
        setRecHint('Recording not supported in this browser.');
        return;
      }

      const seconds = 5;
      const fname = 'qeeg_fx_'+fxMode+'_'+fileStamp()+'.webm';

      recChunks = [];
      try{
        recStream = c.captureStream(30);
      }catch(e){
        setRecHint('Could not start capture: '+(e && e.message ? e.message : e));
        recStream = null;
        return;
      }

      let options = {};
      try{
        const mimes = ['video/webm;codecs=vp9','video/webm;codecs=vp8','video/webm'];
        let chosen = '';
        for(const m of mimes){
          if(MediaRecorder.isTypeSupported && MediaRecorder.isTypeSupported(m)){
            chosen = m; break;
          }
        }
        if(chosen) options = {mimeType: chosen};
      }catch(e){}

      try{
        rec = new MediaRecorder(recStream, options);
      }catch(e){
        try{
          rec = new MediaRecorder(recStream);
        }catch(e2){
          setRecHint('MediaRecorder failed to start.');
          rec = null;
          try{ recStream.getTracks().forEach(t=>t.stop()); }catch(ex){}
          recStream = null;
          return;
        }
      }

      rec.ondataavailable = (ev)=>{
        if(ev && ev.data && ev.data.size>0) recChunks.push(ev.data);
      };

      rec.onerror = ()=>{
        try{
          if(rec && rec.state !== 'inactive') rec.stop();
        }catch(e){}
      };

      rec.onstop = ()=>{
        let blob = null;
        try{
          blob = new Blob(recChunks, {type: (rec && rec.mimeType) ? rec.mimeType : 'video/webm'});
        }catch(e){
          blob = new Blob(recChunks);
        }

        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = fname;
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(()=>{ try{ URL.revokeObjectURL(url); }catch(e){} }, 4000);

        // Cleanup.
        try{
          if(recStream) recStream.getTracks().forEach(t=>t.stop());
        }catch(e){}
        recStream = null;
        rec = null;
        recChunks = [];

        if(recTimer) clearTimeout(recTimer);
        recTimer = null;

        recBtn.disabled = false;
        recBtn.textContent = 'Record 5s';
        setRecHint('Saved '+fname);
      };

      try{
        rec.start();
      }catch(e){
        setRecHint('Recording failed to start.');
        try{ recStream.getTracks().forEach(t=>t.stop()); }catch(ex){}
        recStream = null;
        rec = null;
        return;
      }

      recBtn.disabled = true;
      recBtn.textContent = 'Recording…';
      setRecHint('Recording '+seconds+'s…');

      recTimer = setTimeout(()=>{
        try{
          if(rec && rec.state !== 'inactive') rec.stop();
        }catch(e){}
      }, seconds*1000);
    });
  }

  // Resize handler.
  let resizeTimer = null;
  window.addEventListener('resize', ()=>{
    if(!fxEngine) return;
    if(resizeTimer) clearTimeout(resizeTimer);
    resizeTimer = setTimeout(()=>{
      fxEngine.resize();
      fxEngine.initParticles();
      fxEngine.renderStatic();
    }, 120);
  });

  // Pause when tab is hidden to reduce CPU.
  document.addEventListener('visibilitychange', ()=>{
    if(!fxEngine) return;
    if(document.hidden){
      fxEngine.stop();
    }else{
      fxApply();
    }
  });

  // Live update if user toggles OS reduced-motion while the page is open.
  if(window.matchMedia){
    try{
      const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
      mq.addEventListener('change', (ev)=>{
        fxReduceMotion = !!(ev && ev.matches);
        if(fxReduceMotion && !fxHasUserPref){
          fxEnabled = false;
        }
        fxApply();
      });
    }catch(e){}
  }

  fxApply();
}
// ----- end Procedural animations -----

function loadUiState(){
  fxReduceMotion = !!(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  try{
    selectedInputPath = localStorage.getItem('qeeg_selected_input') || '';
    selectedInputType = localStorage.getItem('qeeg_selected_input_type') || 'file';
    fsCurrentDir = localStorage.getItem('qeeg_fs_dir') || '';
    fsShowHidden = (localStorage.getItem('qeeg_fs_hidden') || '0') === '1';
    const sm = localStorage.getItem('qeeg_fs_sort') || '';
    if(sm==='name' || sm==='mtime' || sm==='size') fsSortMode = sm;
    fsSortDesc = (localStorage.getItem('qeeg_fs_desc') || '0') === '1';
    const fm = localStorage.getItem('qeeg_fx_mode');
    if(fm==='eeg' || fm==='topo' || fm==='spec'){
      fxMode = fm;
    }
    const fx = localStorage.getItem('qeeg_fx_enabled');
    if(fx==='0' || fx==='1'){
      fxEnabled = (fx==='1');
      fxHasUserPref = true;
    }
  }catch(e){}
  if(!fxHasUserPref){
    fxEnabled = !fxReduceMotion;
  }
}

function getInjectFlag(btn){
  if(!btn || !btn.dataset) return '';

  // New-style per-tool injection dropdown (preferred).
  const selId = btn.dataset.injectSelId || '';
  if(selId){
    const sel = document.getElementById(selId);
    if(sel){
      let flag = sel.value || '';
      if(!flag){
        flag = (selectedInputType==='dir') ? (sel.dataset.defaultDir||'') : (sel.dataset.defaultFile||'');
        if(!flag && selectedInputType!=='dir') flag='--input';
      }
      return flag;
    }
  }

  // Backward compat with older dashboards (data-flag-file/dir on the button).
  const isDir = (selectedInputType==='dir');
  let flag = isDir ? (btn.dataset.flagDir||'') : (btn.dataset.flagFile||'');
  if(!flag && !isDir) flag='--input';
  return flag;
}

function initInjectSelects(){
  document.querySelectorAll('.inject-select').forEach(sel=>{
    sel.addEventListener('change', ()=>{ updateSelectedInputUi(); });
  });
}

function updateSelectedInputUi(){
  const el=document.getElementById('selectedInput');
  if(el){
    el.textContent = selectedInputPath ? selectedInputPath : '(none)';
  }
  const ty=document.getElementById('selectedInputType');
  if(ty){
    if(!selectedInputPath){
      ty.style.display='none';
    }else{
      ty.style.display='inline-block';
      ty.textContent = (selectedInputType==='dir') ? 'dir' : 'file';
    }
  }

  // Enable/disable "Inject path" buttons based on selection + chosen inject flag.
  document.querySelectorAll('.use-input-btn').forEach(b=>{
    if(!selectedInputPath){ b.disabled=true; return; }
    const flag = getInjectFlag(b);
    b.disabled = !flag;
  });

  // Selection action buttons in the hero card.
  const bBrowse=document.getElementById('selBrowseBtn');
  if(bBrowse) bBrowse.disabled = !(selectedInputPath && qeegApiOk && qeegApiToken);
  const bPrev=document.getElementById('selPreviewBtn');
  if(bPrev) bPrev.disabled = !(selectedInputPath && selectedInputType!=='dir');
  const bCopy=document.getElementById('selCopyBtn');
  if(bCopy) bCopy.disabled = !selectedInputPath;
  const bClr=document.getElementById('selClearBtn');
  if(bClr) bClr.disabled = !selectedInputPath;

  try{ renderSelectionSuggestions(); }catch(e){}
}

function setSelectedInput(p, type){
  selectedInputPath = p || '';
  selectedInputType = (type==='dir') ? 'dir' : 'file';
  try{
    localStorage.setItem('qeeg_selected_input', selectedInputPath);
    localStorage.setItem('qeeg_selected_input_type', selectedInputType);
  }catch(e){}
  updateSelectedInputUi();
}

function initSelectionBar(){
  const bBrowse=document.getElementById('selBrowseBtn');
  if(bBrowse) bBrowse.onclick=()=>{ try{ selectionBrowse(); }catch(e){} };
  const bPrev=document.getElementById('selPreviewBtn');
  if(bPrev) bPrev.onclick=()=>{ try{ selectionPreview(); }catch(e){} };
  const bCopy=document.getElementById('selCopyBtn');
  if(bCopy) bCopy.onclick=()=>{ try{ selectionCopy(); }catch(e){} };
  const bClr=document.getElementById('selClearBtn');
  if(bClr) bClr.onclick=()=>{ try{ selectionClear(); }catch(e){} };
}

function selectionBaseName(p){
  p = String(p||'').replace(/\\/g,'/').replace(/\/+$/,'');
  const parts = p.split('/').filter(x=>x!=='');
  return parts.length ? parts[parts.length-1] : p;
}

function selectionDirForBrowse(){
  const p = String(selectedInputPath||'').replace(/\\/g,'/').replace(/\/+$/,'');
  if(!p) return '';
  if(selectedInputType==='dir') return p;
  const i = p.lastIndexOf('/');
  if(i<0) return '';
  return p.slice(0,i);
}

function selectionBrowse(){
  if(!selectedInputPath) return;
  if(!(qeegApiOk && qeegApiToken)){
    alert('Workspace browsing is available only when served via qeeg_ui_server_cli.');
    return;
  }
  const d = selectionDirForBrowse();
  if(selectedInputType==='file') browseInWorkspace(d, selectedInputPath);
  else browseInWorkspace(d);
}

function selectionPreview(){
  if(!selectedInputPath) return;
  if(selectedInputType==='dir'){
    alert('Preview is available for files (select a file in Workspace).');
    return;
  }
  const label = selectionBaseName(selectedInputPath);
  openPreview(encodePath(selectedInputPath), label);
}

function selectionCopy(){
  if(!selectedInputPath) return;
  copyPath(selectedInputPath);
}

function selectionClear(){
  setSelectedInput('', 'file');
}

function getInjectFlagForTool(tool){
  tool = String(tool||'');
  const sec = findToolSectionByName(tool);
  if(!sec) return '';
  const sel = document.getElementById((sec.id||'') + '_inject');
  if(sel){
    let flag = sel.value || '';
    if(!flag){
      flag = (selectedInputType==='dir') ? (sel.dataset.defaultDir||'') : (sel.dataset.defaultFile||'');
      if(!flag && selectedInputType!=='dir') flag='--input';
    }
    return flag;
  }
  if(selectedInputType!=='dir') return '--input';
  return '';
}

function selectionSuggestTool(tool, doRun){
  tool = String(tool||'');
  const sec = findToolSectionByName(tool);
  if(!sec){
    alert('Tool not found: '+tool);
    return;
  }
  const argsEl = document.getElementById((sec.id||'') + '_args');
  if(argsEl && selectedInputPath){
    const flag = getInjectFlagForTool(tool);
    if(flag){
      argsEl.value = setFlagValue(argsEl.value, flag, selectedInputPath);
    }
  }
  focusToolSection(sec);

  if(doRun && qeegApiOk && qeegApiToken){
    const runBtn = document.getElementById((sec.id||'') + '_runbtn');
    if(runBtn && !runBtn.disabled){
      runTool(runBtn);
    }else{
      alert('Run API not available (or tool is disabled).');
    }
  }
}

function getSelectionSuggestions(){
  const out=[];
  if(!selectedInputPath) return out;
  const p = String(selectedInputPath||'').replace(/\\/g,'/');
  const base = selectionBaseName(p).toLowerCase();
  const ext = pathExt(base);

  if(selectedInputType==='dir'){
    // Directory selections are often datasets (e.g., BIDS roots) or output folders.
    out.push({label:'BIDS scan', tool:'qeeg_bids_scan_cli'});
    out.push({label:'Export derivatives', tool:'qeeg_export_derivatives_cli'});
    out.push({label:'Generate UI', tool:'qeeg_ui_cli'});
  }else if(ext==='edf' || ext==='bdf' || ext==='set' || ext==='vhdr'){
    out.push({label:'Info', tool:'qeeg_info_cli'});
    out.push({label:'Quality', tool:'qeeg_quality_cli'});
    out.push({label:'Preprocess', tool:'qeeg_preprocess_cli'});
    out.push({label:'Bandpower', tool:'qeeg_bandpower_cli'});
    out.push({label:'Topomap', tool:'qeeg_map_cli'});
    out.push({label:'Spectrogram', tool:'qeeg_spectrogram_cli'});
    out.push({label:'Microstates', tool:'qeeg_microstates_cli'});
  }else if(ext==='csv' || ext==='tsv'){
    if(base.includes('bandpowers') || base.includes('bandpower')){
      out.push({label:'Bandratios', tool:'qeeg_bandratios_cli'});
    }
  }

  // Only show tools that exist in this dashboard build.
  return out.filter(s=> !!findToolSectionByName(s.tool));
}

function renderSelectionSuggestions(){
  const box=document.getElementById('selSuggest');
  if(!box) return;
  if(!selectedInputPath){
    box.innerHTML = '';
    return;
  }
  const sugg = getSelectionSuggestions();
  if(!sugg || sugg.length===0){
    box.innerHTML = '<span class="small">Tip: pick an <b>Inject flag</b> on a tool card, then click <b>Inject path</b>.</span>';
    return;
  }

  let html = '<div><b>Suggested:</b> ';
  for(const s of sugg){
    html += '<button class="btn chip" type="button" onclick="selectionSuggestTool(\''+escJs(s.tool)+'\', false)">'+esc(s.label)+'</button> ';
  }
  html += '</div>';

  if(qeegApiOk && qeegApiToken){
    html += '<div style="margin-top:6px"><b>Quick run:</b> ';
    for(const s of sugg){
      html += '<button class="btn chip" type="button" onclick="selectionSuggestTool(\''+escJs(s.tool)+'\', true)">'+esc('Run '+s.label)+'</button> ';
    }
    html += '</div>';
  }else{
    html += '<div style="margin-top:6px"><span class="small">Serve via <code>qeeg_ui_server_cli</code> to enable quick-run.</span></div>';
  }

  box.innerHTML = html;
}


function quoteArgIfNeeded(s){
  s = String(s||'');
  if(s==='') return '""';
  if(/[ \t\n\r"]/g.test(s) || s.includes("'")){
    return '"'+s.replace(/\\/g,'\\\\').replace(/"/g,'\\"')+'"';
  }
  return s;
}

function escapeRegExp(s){return String(s||'').replace(/[.*+?^${}()|[\]\\]/g,'\\$&');}
function escJs(s){return String(s||'').replace(/\\/g,'\\\\').replace(/'/g,"\\'");}

function setFlagValue(args, flag, value){
  args = String(args||'');
  const v = quoteArgIfNeeded(value);
  const f = escapeRegExp(flag);

  // --flag=value
  const reEq = new RegExp(f+'=("[^"]*"|\\\'[^\\\']*\\\'|[^\\s]+)');
  if(reEq.test(args)){
    return args.replace(reEq, flag+'='+v);
  }

  // --flag value
  const re = new RegExp(f+'\\s+("[^"]*"|\\\'[^\\\']*\\\'|[^\\s]+)');
  if(re.test(args)){
    return args.replace(re, flag+' '+v);
  }

  return (args.trim()+' '+flag+' '+v).trim();
}

function useSelectedInput(btn){
  const argsId = (btn && btn.dataset) ? btn.dataset.argsId : '';
  const el = document.getElementById(argsId);
  if(!el) return;
  if(!selectedInputPath){
    alert('Select a file or directory first (Workspace browser).');
    return;
  }

  const flag = getInjectFlag(btn);
  if(!flag){
    const isDir = (selectedInputType==='dir');
    alert(isDir
      ? 'Selected path is a directory. Choose a directory-related flag from the dropdown (e.g., --outdir/--dataset/--bids-root) or select a file instead.'
      : 'No inject flag detected for this tool. Choose a flag from the dropdown or edit args manually.');
    return;
  }

  el.value = setFlagValue(el.value, flag, selectedInputPath);
}

function resetArgs(argsId){
  const el=document.getElementById(argsId);
  if(!el) return;
  const d = el.getAttribute('data-default') || '';
  el.value = d;
}


function presetsLegacyKey(tool){ return 'qeeg_presets_'+tool; }
function presetsStoreKey(){ return 'qeeg_presets_store_v1'; }

function emptyPresetsStore(){
  return {version:1, tools:{}};
}

function sanitizePresetMap(m){
  const out = {};
  if(!m || typeof m !== 'object') return out;
  for(const k in m){
    if(!Object.prototype.hasOwnProperty.call(m,k)) continue;
    const v = m[k];
    if(typeof v === 'string') out[String(k)] = v;
  }
  return out;
}

function normalizePresetsStore(s){
  // Accept either:
  //   {version:1, tools:{ tool:{name:args,...}, ...}}
  // or legacy-ish:
  //   {tool:{name:args,...}, ...}
  if(!s || typeof s !== 'object') return emptyPresetsStore();
  let tools = {};
  if(s.tools && typeof s.tools === 'object'){
    tools = s.tools;
  } else {
    tools = s;
  }
  const out = emptyPresetsStore();
  out.tools = {};
  for(const t in tools){
    if(!Object.prototype.hasOwnProperty.call(tools,t)) continue;
    if(t==='version' || t==='updated' || t==='exported') continue;
    const map = tools[t];
    if(!map || typeof map !== 'object') continue;
    const sm = sanitizePresetMap(map);
    if(Object.keys(sm).length) out.tools[String(t)] = sm;
  }
  return out;
}

function storeHasPresets(store){
  if(!store || typeof store !== 'object') return false;
  const tools = (store.tools && typeof store.tools === 'object') ? store.tools : store;
  for(const t in tools){
    if(!Object.prototype.hasOwnProperty.call(tools,t)) continue;
    if(t==='version' || t==='updated' || t==='exported') continue;
    const m = tools[t];
    if(m && typeof m === 'object' && Object.keys(m).length) return true;
  }
  return false;
}

function ensurePresetsStore(){
  if(presetsStore) return;
  let store = null;
  try{
    const s = localStorage.getItem(presetsStoreKey());
    if(s) store = normalizePresetsStore(JSON.parse(s));
  }catch(e){ store = null; }
  if(!store) store = emptyPresetsStore();

  // Migration: import per-tool legacy keys if the v1 store is empty.
  if(!storeHasPresets(store)){
    const tools = [];
    document.querySelectorAll('.preset-select').forEach(sel=>{
      if(sel && sel.dataset && sel.dataset.tool) tools.push(sel.dataset.tool);
    });
    const uniq = Array.from(new Set(tools));
    for(const tool of uniq){
      try{
        const s = localStorage.getItem(presetsLegacyKey(tool));
        if(!s) continue;
        const m = sanitizePresetMap(JSON.parse(s));
        if(Object.keys(m).length){
          store.tools[tool] = m;
        }
      }catch(e){}
    }
    try{ localStorage.setItem(presetsStoreKey(), JSON.stringify(store)); }catch(e){}
  }

  presetsStore = store;
  updatePresetsStatus();
}

function saveLocalPresetsStore(){
  ensurePresetsStore();
  try{ localStorage.setItem(presetsStoreKey(), JSON.stringify(presetsStore||emptyPresetsStore())); }catch(e){}
}

function schedulePresetsPersist(){
  saveLocalPresetsStore();
  updatePresetsStatus();

  if(!(qeegApiOk && qeegApiToken) || !presetsServerEnabled) return;

  presetsSavePending = true;
  if(presetsSaveTimer) return;
  presetsSaveTimer = setTimeout(async ()=>{
    presetsSaveTimer = null;
    if(!presetsSavePending) return;
    presetsSavePending = false;
    try{
      await apiFetch('/api/presets', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(presetsStore||emptyPresetsStore())});
      presetsSource = 'server';
      updatePresetsStatus();
    }catch(e){
      // keep local fallback
    }
  }, 600);
}

function loadPresets(tool){
  ensurePresetsStore();
  tool = String(tool||'');
  if(!tool) return {};
  const tools = (presetsStore && presetsStore.tools && typeof presetsStore.tools === 'object') ? presetsStore.tools : {};
  const m = (tools && tools[tool] && typeof tools[tool] === 'object') ? tools[tool] : {};
  return Object.assign({}, m);
}

function savePresets(tool, obj){
  ensurePresetsStore();
  tool = String(tool||'');
  if(!tool) return;
  if(!presetsStore.tools || typeof presetsStore.tools !== 'object') presetsStore.tools = {};
  presetsStore.tools[tool] = sanitizePresetMap(obj||{});
  schedulePresetsPersist();
}

function refreshAllPresetSelects(){
  document.querySelectorAll('.preset-select').forEach(sel=>{ populatePresetSelect(sel); });
}

async function maybeInitServerPresets(){
  if(!(qeegApiOk && qeegApiToken)) return;
  if(presetsSyncing) return;
  if(presetsServerEnabled) return; // already synced/enabled for this page
  presetsSyncing = true;
  ensurePresetsStore();
  try{
    const r = await apiFetch('/api/presets');
    const j = await r.json();
    if(!r.ok) throw new Error(j && j.error ? j.error : 'presets failed');

    const serverStore = normalizePresetsStore((j && j.presets) ? j.presets : {});
    const serverHas = storeHasPresets(serverStore);
    const localHas = storeHasPresets(presetsStore);

    presetsServerEnabled = true;
    presetsSource = 'server';

    if(serverHas){
      presetsStore = serverStore;
      saveLocalPresetsStore();
      refreshAllPresetSelects();
    } else if(localHas){
      // Bootstrap server with local presets so they persist under --root.
      try{
        await apiFetch('/api/presets', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(presetsStore)});
      }catch(e){}
    }
  }catch(e){
    // Server doesn't support presets endpoint or is unavailable; keep local-only.
    presetsServerEnabled = false;
    presetsSource = 'local';
  }finally{
    presetsSyncing = false;
    updatePresetsStatus();
  }
}

function updatePresetsStatus(){
  const el = document.getElementById('presetsStatus');
  if(!el) return;
  const src = (presetsSource==='server') ? 'server' : 'local';
  let extra = '';
  if(src==='server') extra = ' (<code>qeeg_ui_presets.json</code>)';
  el.innerHTML = 'Presets: <b>'+src+'</b>' + extra;
}

function exportPresets(tool){
  ensurePresetsStore();
  tool = String(tool||'');
  if(!tool){ alert('Missing tool name.'); return; }
  const presets = loadPresets(tool);
  const payload = {version:1, tool:tool, exported:new Date().toISOString(), presets:presets};
  const data = JSON.stringify(payload, null, 2);
  try{
    const blob = new Blob([data], {type:'application/json'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'qeeg_presets_' + tool + '.json';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(()=>{ try{ URL.revokeObjectURL(url);}catch(e){} }, 1500);
  }catch(e){
    alert('Export failed: ' + (e && e.message ? e.message : String(e)));
  }
}

function ensurePresetsImportInput(){
  if(presetsImportInput) return presetsImportInput;
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = 'application/json,.json';
  inp.style.display = 'none';
  inp.addEventListener('change', ()=>{
    const file = inp.files && inp.files[0] ? inp.files[0] : null;
    // Reset value so selecting the same file again still triggers change.
    inp.value = '';
    if(!file) return;

    const reader = new FileReader();
    reader.onload = ()=>{
      try{
        const text = String(reader.result||'');
        const obj = JSON.parse(text);
        let tool = presetsImportTool || '';
        if(obj && typeof obj.tool === 'string' && !tool) tool = obj.tool;
        if(!tool){ alert('Import missing tool name.'); return; }

        let incoming = null;
        if(obj && typeof obj === 'object' && obj.presets && typeof obj.presets === 'object'){
          incoming = obj.presets;
        } else if(obj && typeof obj === 'object'){
          incoming = obj;
        }

        const m = sanitizePresetMap(incoming);
        if(Object.keys(m).length===0){
          alert('No presets found in file.');
          return;
        }

        const existing = loadPresets(tool);
        let collisions = 0;
        for(const k in m){
          if(Object.prototype.hasOwnProperty.call(existing,k)) collisions++;
        }
        if(collisions>0){
          if(!confirm('Import will overwrite '+collisions+' preset(s) for '+tool+'. Continue?')) return;
        }

        const merged = Object.assign({}, existing, m);
        savePresets(tool, merged);
        refreshAllPresetSelects();
        if(presetsImportSelId){
          const sel=document.getElementById(presetsImportSelId);
          if(sel) populatePresetSelect(sel);
        }
        alert('Imported '+Object.keys(m).length+' preset(s) for '+tool+'.');
      }catch(e){
        alert('Import failed: ' + (e && e.message ? e.message : String(e)));
      } finally {
        presetsImportTool='';
        presetsImportSelId='';
      }
    };
    reader.onerror = ()=>{
      alert('Import failed: cannot read file.');
      presetsImportTool='';
      presetsImportSelId='';
    };
    reader.readAsText(file);
  });
  document.body.appendChild(inp);
  presetsImportInput = inp;
  return inp;
}

function importPresets(tool, selId){
  tool = String(tool||'');
  if(!tool){ alert('Missing tool name.'); return; }
  presetsImportTool = tool;
  presetsImportSelId = selId || '';
  const inp = ensurePresetsImportInput();
  inp.click();
}

function populatePresetSelect(sel){
  if(!sel) return;
  const tool = sel.dataset ? sel.dataset.tool : '';
  const presets = loadPresets(tool);
  const names = Object.keys(presets).sort();
  let html = '<option value="">Presets…</option>';
  for(const n of names){
    html += '<option value="'+esc(n)+'">'+esc(n)+'</option>';
  }
  sel.innerHTML = html;
}

function initPresets(){
  ensurePresetsStore();
  document.querySelectorAll('.preset-select').forEach(sel=>{
    populatePresetSelect(sel);
    sel.addEventListener('change', ()=>{
      const tool = sel.dataset ? sel.dataset.tool : '';
      const argsId = sel.dataset ? sel.dataset.argsId : '';
      const name = sel.value || '';
      if(!name) return;
      const presets = loadPresets(tool);
      const val = presets[name];
      const el = document.getElementById(argsId);
      if(el && typeof val === 'string') el.value = val;
    });
  });
  updatePresetsStatus();
}

function savePreset(tool, argsId, selId){
  const el = document.getElementById(argsId);
  if(!el) return;
  const name = prompt('Preset name for '+tool+':');
  if(!name) return;
  const presets = loadPresets(tool);
  presets[name] = el.value || '';
  savePresets(tool, presets);
  const sel = document.getElementById(selId);
  if(sel){
    populatePresetSelect(sel);
    sel.value = name;
  }
}

function deletePreset(tool, selId){
  const sel = document.getElementById(selId);
  if(!sel) return;
  const name = sel.value || '';
  if(!name){ alert('Choose a preset to delete.'); return; }
  if(!confirm('Delete preset "'+name+'" for '+tool+'?')) return;
  const presets = loadPresets(tool);
  delete presets[name];
  savePresets(tool, presets);
  populatePresetSelect(sel);
}

function humanSize(n){
  n = Number(n||0);
  if(!isFinite(n) || n<=0) return '0 B';
  const units=['B','KB','MB','GB','TB'];
  let u=0;
  while(n>=1024 && u<units.length-1){ n/=1024; ++u; }
  const digits = (u===0) ? 0 : (n>=10 ? 1 : 2);
  return n.toFixed(digits)+' '+units[u];
}

function fmtLocalTimeSec(ts){
  const t = Number(ts||0);
  if(!isFinite(t) || t<=0) return '';
  try{
    const d = new Date(t*1000);
    // Use the browser's locale for readability.
    return d.toLocaleString();
  }catch(e){
    return '';
  }
}

function fsSetShowHidden(v){
  fsShowHidden = !!v;
  try{ localStorage.setItem('qeeg_fs_hidden', fsShowHidden ? '1' : '0'); }catch(e){}
}

function fsSetSortMode(m){
  m = String(m||'').toLowerCase();
  if(m!=='mtime' && m!=='size' && m!=='name') m='name';
  fsSortMode = m;
  try{ localStorage.setItem('qeeg_fs_sort', fsSortMode); }catch(e){}
}

function fsSetSortDesc(v){
  fsSortDesc = !!v;
  try{ localStorage.setItem('qeeg_fs_desc', fsSortDesc ? '1' : '0'); }catch(e){}
}

function fsRoot(){ fsEnter(''); }
function fsRuns(){ fsEnter('ui_runs'); }
function fsTrashDir(){ fsEnter('.qeeg_trash'); }

async function fsNewFolder(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const base = fsCurrentDir ? fsCurrentDir : '.';
  let name = prompt('New folder name (inside '+base+'):', 'new_folder');
  if(name===null) return;
  name = String(name||'').trim();
  if(!name) return;
  try{
    const r=await apiFetch('/api/fs_mkdir',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dir:fsCurrentDir, name:name})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'mkdir failed');
    refreshFs();
  }catch(e){
    alert('Create folder failed: '+(e&&e.message?e.message:String(e)));
  }
}

async function fsRenamePath(path, type){
  if(!(qeegApiOk && qeegApiToken)) return;
  path = String(path||'');
  if(!path) return;
  const parts = path.split('/').filter(p=>p);
  const base = parts.length ? parts[parts.length-1] : path;
  let newName = prompt('Rename "'+base+'" to:', base);
  if(newName===null) return;
  newName = String(newName||'').trim();
  if(!newName || newName===base) return;
  try{
    const r=await apiFetch('/api/fs_rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path, new_name:newName})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'rename failed');
    const newPath = (j && j.path) ? String(j.path) : '';
    if(newPath && selectedInputPath===path){
      setSelectedInput(newPath, (type==='dir')?'dir':'file');
    }
    refreshFs();
  }catch(e){
    alert('Rename failed: '+(e&&e.message?e.message:String(e)));
  }
}

async function fsTrashItem(path, type){
  if(!(qeegApiOk && qeegApiToken)) return;
  path = String(path||'');
  if(!path) return;
  const parts = path.split('/').filter(p=>p);
  const base = parts.length ? parts[parts.length-1] : path;
  if(!confirm('Move to .qeeg_trash?\n\n'+base)) return;
  try{
    const r=await apiFetch('/api/fs_trash',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'trash failed');
    const newPath = (j && j.path) ? String(j.path) : '';
    if(newPath && selectedInputPath===path){
      setSelectedInput(newPath, (type==='dir')?'dir':'file');
    }
    refreshFs();
  }catch(e){
    alert('Trash failed: '+(e&&e.message?e.message:String(e)));
  }
}

function fsSetUploadStatus(msg){
  const el=document.getElementById('fsUploadStatus');
  if(!el) return;
  el.textContent = msg || '';
  if(fsUploadStatusTimer){ clearTimeout(fsUploadStatusTimer); fsUploadStatusTimer=null; }
  if(msg){
    fsUploadStatusTimer = setTimeout(()=>{
      try{ const e2=document.getElementById('fsUploadStatus'); if(e2) e2.textContent=''; }catch(e){}
      fsUploadStatusTimer=null;
    }, 4500);
  }
}

function ensureFsUploadInput(){
  if(fsUploadInput) return fsUploadInput;
  const inp=document.createElement('input');
  inp.type='file';
  inp.multiple=true;
  inp.className='hidden';
  inp.addEventListener('change', ()=>{
    const files = inp.files;
    // Clear selection early so selecting the same file twice triggers change.
    inp.value='';
    if(files && files.length) fsUploadFiles(files);
  });
  document.body.appendChild(inp);
  fsUploadInput = inp;
  return inp;
}

function fsOpenUpload(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const base = fsCurrentDir ? fsCurrentDir : '.';
  fsSetUploadStatus('Select files to upload into '+base+'…');
  const inp = ensureFsUploadInput();
  inp.click();
}

async function fsUploadFiles(files){
  if(!(qeegApiOk && qeegApiToken)) return;
  if(!files || !files.length) return;
  if(fsUploading){
    alert('An upload is already running. Please wait for it to finish.');
    return;
  }
  fsUploading = true;
  try{
    const dir = fsCurrentDir || '';
    for(let i=0; i<files.length; ++i){
      const file = files[i];
      if(!file) continue;
      let overwrite = false;
      while(true){
        fsSetUploadStatus('Uploading '+(i+1)+'/'+files.length+': '+(file.name||'file')+'…');
        try{
          const url = '/api/fs_upload?dir='+encodeURIComponent(dir)+'&name='+encodeURIComponent(file.name||'upload.bin')+'&overwrite='+(overwrite?1:0);
          const r = await apiFetch(url, {
            method:'POST',
            headers:{'Content-Type': (file.type||'application/octet-stream')},
            body:file
          });
          let j=null;
          try{ j = await r.json(); }catch(e){ j=null; }
          if(r.status===409){
            const msg = (j && j.error) ? String(j.error) : 'destination exists';
            if(!overwrite && confirm('Upload conflict for '+file.name+': '+msg+'\n\nOverwrite existing file?')){
              overwrite = true;
              continue;
            }
            break; // skip
          }
          if(!r.ok){
            throw new Error((j && j.error) ? String(j.error) : ('upload failed (HTTP '+r.status+')'));
          }
          break;
        }catch(e){
          alert('Upload failed for '+(file.name||'file')+': '+(e&&e.message?e.message:String(e)));
          break;
        }
      }
    }
    fsSetUploadStatus('Upload complete.');
    refreshFs();
  } finally {
    fsUploading = false;
  }
}

function fsSetFindStatus(msg){
  const el=document.getElementById('fsFindStatus');
  if(!el) return;
  el.textContent = msg || '';
}

function fsShowFindWrap(show){
  const w=document.getElementById('fsFindWrap');
  if(w) w.classList.toggle('hidden', !show);
}

function fsClearFind(silent){
  fsFindResults = [];
  fsFindMeta = null;
  fsSetFindStatus('');
  const out=document.getElementById('fsFindResults');
  if(out) out.innerHTML = '';
  const sum=document.getElementById('fsFindSummary');
  if(sum) sum.textContent = '';
  fsShowFindWrap(false);
  if(!silent){
    const qEl=document.getElementById('fsFindQ');
    if(qEl) try{ qEl.focus(); }catch(e){}
  }
}

function fsParentDir(p){
  p = String(p||'').replace(/\\/g,'/');
  const parts = p.split('/').filter(x=>x);
  if(parts.length<=1) return '';
  parts.pop();
  return parts.join('/');
}

function renderFsFind(){
  const out=document.getElementById('fsFindResults');
  const sum=document.getElementById('fsFindSummary');
  if(!out || !sum) return;

  if(!fsFindMeta){
    fsShowFindWrap(false);
    out.innerHTML = '';
    sum.textContent = '';
    return;
  }

  fsShowFindWrap(true);
  const m = fsFindMeta;
  let summary = '— "'+String(m.q||'')+'"';
  summary += ' (scanned '+String(m.scanned||0)+' entries';
  if(m.elapsed_ms){ summary += ', '+String(m.elapsed_ms)+' ms'; }
  summary += ')';
  if(m.truncated){ summary += ' • truncated'; }
  sum.textContent = summary;

  const res = fsFindResults || [];
  if(!res.length){
    out.innerHTML = '<span class="small">No matches.</span>';
    return;
  }

  let html = '<table><thead><tr><th>Path</th><th>Type</th><th>Size</th><th>Modified</th><th>Actions</th></tr></thead><tbody>';
  for(const e of res){
    const path = (e && e.path) ? String(e.path) : '';
    const name = (e && e.name) ? String(e.name) : '';
    const type = (e && e.type) ? String(e.type) : '';
    const size = (e && e.size) ? Number(e.size) : 0;
    const mtime = (e && e.mtime) ? Number(e.mtime) : 0;
    const pjs = escJs(path);
    const njs = escJs(name);
    const parent = fsParentDir(path);
    const parentJs = escJs(parent);

    let actions = '';
    if(type==='dir'){
      actions =
        '<button class="btn" onclick="fsEnter(\\''+pjs+'\\')">Open</button> '+
        '<button class="btn" onclick="selectFromFs(\\''+pjs+'\\',\\'dir\\')">Select</button> '+
        '<button class="btn" onclick="copyPath(\\''+pjs+'\\')">Copy</button>';
    }else{
      actions =
        '<button class="btn" onclick="selectFromFs(\\''+pjs+'\\',\\'file\\')">Select</button> '+
        '<button class="btn" onclick="copyPath(\\''+pjs+'\\')">Copy</button> '+
        '<button class="btn" onclick="openPreview(encodePath(\\''+pjs+'\\'),\\''+njs+'\\')">Preview</button> '+
        '<a href="'+encodePath(path)+'" target="_blank">Open</a> '+
        '<button class="btn" onclick="browseInWorkspace(\\''+parentJs+'\\',\\''+pjs+'\\')">Reveal</button>';
    }

    const pathCell = (type==='dir')
      ? '<a href="#" onclick="fsEnter(\\''+pjs+'\\');return false;">📁 '+esc(path)+'</a>'
      : '<code>'+esc(path)+'</code>';

    html += '<tr class="fs-row" data-path="'+esc(path)+'" data-type="'+esc(type)+'" draggable="true"><td>'+pathCell+'</td><td>'+esc(type)+'</td><td>'+(type==='file'?esc(humanSize(size)):'')+'</td><td>'+esc(fmtLocalTimeSec(mtime))+'</td><td>'+actions+'</td></tr>';
  }
  html += '</tbody></table>';
  out.innerHTML = html;
}

async function fsFindRun(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const qEl=document.getElementById('fsFindQ');
  const q = String(qEl ? qEl.value : '').trim();
  if(!q){
    alert('Enter a find query (substring or glob like *.edf).');
    if(qEl) try{ qEl.focus(); }catch(e){}
    return;
  }
  if(fsFindRunning){
    alert('A find request is already running.');
    return;
  }

  fsFindRunning = true;
  fsSetFindStatus('Searching…');
  try{
    const typeEl=document.getElementById('fsFindType');
    const depthEl=document.getElementById('fsFindDepth');
    const type = typeEl ? String(typeEl.value||'any') : 'any';
    const depth = depthEl ? Number(depthEl.value||8) : 8;
    const r=await apiFetch('/api/find',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dir:fsCurrentDir, q:q, type:type, max_depth:depth, max_results:200, show_hidden:!!fsShowHidden})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'find failed');

    fsFindResults = (j && j.results) ? j.results : [];
    fsFindMeta = {
      dir: (j && j.dir) ? String(j.dir) : fsCurrentDir,
      q: q,
      scanned: (j && j.scanned) ? Number(j.scanned) : 0,
      elapsed_ms: (j && j.elapsed_ms) ? Number(j.elapsed_ms) : 0,
      truncated: !!(j && j.truncated)
    };

    fsSetFindStatus('Done.');
    renderFsFind();
  }catch(e){
    fsSetFindStatus('');
    alert('Find failed: '+(e&&e.message?e.message:String(e)));
  }finally{
    fsFindRunning = false;
    setTimeout(()=>{ try{ if(!fsFindRunning) fsSetFindStatus(''); }catch(e){} }, 2000);
  }
}


function fsIsFileDrag(ev){
  try{
    const dt = ev && ev.dataTransfer ? ev.dataTransfer : null;
    if(!dt) return false;
    // Prefer the types list when available.
    if(dt.types && dt.types.length){
      for(let i=0; i<dt.types.length; ++i){
        if(String(dt.types[i]).toLowerCase() === 'files') return true;
      }
    }
    return !!(dt.files && dt.files.length);
  }catch(e){
    return false;
  }
}

function fsSetDropHint(show){
  const fp=document.getElementById('fsPanel');
  if(fp) fp.classList.toggle('drop-active', !!show);
  const h=document.getElementById('fsDropHint');
  if(h) h.classList.toggle('hidden', !show);
}

function initFsDrop(){
  const fp=document.getElementById('fsPanel');
  if(!fp) return;
  if(fp.dataset && fp.dataset.dropInited==='1') return;
  if(fp.dataset) fp.dataset.dropInited='1';

  fp.addEventListener('dragenter', (e)=>{
    if(!fsIsFileDrag(e)) return;
    e.preventDefault();
    fsDragDepth++;
    fsSetDropHint(true);
  });
  fp.addEventListener('dragover', (e)=>{
    if(!fsIsFileDrag(e)) return;
    e.preventDefault();
    fsSetDropHint(true);
  });
  fp.addEventListener('dragleave', (e)=>{
    if(!fsIsFileDrag(e)) return;
    fsDragDepth = Math.max(0, fsDragDepth-1);
    if(fsDragDepth===0) fsSetDropHint(false);
  });
  fp.addEventListener('drop', (e)=>{
    if(!fsIsFileDrag(e)) return;
    e.preventDefault();
    fsDragDepth = 0;
    fsSetDropHint(false);
    const dt = e.dataTransfer;
    if(dt && dt.files && dt.files.length){
      fsUploadFiles(dt.files);
    }
  });
}


function fsOnDragStart(e){
  try{
    const row = e.target && e.target.closest ? e.target.closest('tr[data-path]') : null;
    if(!row) return;
    const p = row.getAttribute('data-path') || '';
    if(!p) return;
    const t = row.getAttribute('data-type') || '';
    if(e.dataTransfer){
      try{ e.dataTransfer.setData('application/x-qeeg-path', p); }catch(e2){}
      try{ e.dataTransfer.setData('application/x-qeeg-type', t); }catch(e2){}
      try{ e.dataTransfer.setData('text/plain', p); }catch(e2){}
      try{ e.dataTransfer.effectAllowed='copy'; }catch(e2){}
    }
  }catch(e2){}
}


function renderFsCrumbs(){
  const c = document.getElementById('fsCrumbs');
  if(!c) return;
  const parts = (fsCurrentDir||'').split('/').filter(p=>p);
  let html = '<a href="#" onclick="fsEnter(\'\');return false;">root</a>';
  let acc = '';
  for(const p of parts){
    acc = acc ? (acc + '/' + p) : p;
    html += ' / <a href="#" onclick="fsEnter(\''+escJs(acc)+'\');return false;">'+esc(p)+'</a>';
  }
  c.innerHTML = html;
}

function fsSetDir(d){
  fsCurrentDir = d || '';
  try{ localStorage.setItem('qeeg_fs_dir', fsCurrentDir); }catch(e){}
}

function fsHistoryInit(){
  if(fsNavPos>=0 && Array.isArray(fsNavHistory) && fsNavHistory.length) return;
  fsNavHistory = [fsCurrentDir || ''];
  fsNavPos = 0;
}

function fsUpdateNavButtons(){
  const bBack=document.getElementById('fsBackBtn');
  if(bBack) bBack.disabled = !(fsNavPos > 0);
  const bFwd=document.getElementById('fsFwdBtn');
  if(bFwd) bFwd.disabled = !(fsNavPos >= 0 && fsNavPos < (fsNavHistory.length-1));
}

function fsHistoryPush(d){
  fsHistoryInit();
  d = d || '';
  if(fsNavHistory[fsNavPos] === d) return;
  // Truncate forward history if we navigated after going back.
  fsNavHistory = fsNavHistory.slice(0, fsNavPos+1);
  fsNavHistory.push(d);
  fsNavPos = fsNavHistory.length-1;
  fsUpdateNavButtons();
}

function fsBack(){
  fsHistoryInit();
  if(fsNavPos <= 0) return;
  fsNavPos--;
  const d = fsNavHistory[fsNavPos] || '';
  fsEnter(d, {noHistory:true});
  fsUpdateNavButtons();
}

function fsForward(){
  fsHistoryInit();
  if(fsNavPos < 0 || fsNavPos >= fsNavHistory.length-1) return;
  fsNavPos++;
  const d = fsNavHistory[fsNavPos] || '';
  fsEnter(d, {noHistory:true});
  fsUpdateNavButtons();
}

function fsEnter(d, opts){
  opts = opts || {};
  d = String(d||'').replace(/\\/g,'/').replace(/\/+$/,'');
  if(!d) d = '';

  fsHistoryInit();
  if(!opts.noHistory){
    fsHistoryPush(d);
  }else{
    fsUpdateNavButtons();
  }

  // If we're revealing a file in this directory, clear the filter so the row is visible.
  if(opts.reveal){
    fsPendingRevealPath = String(opts.reveal||'').replace(/\\/g,'/').replace(/\/+$/,'');
    const flt = document.getElementById('fsFilter');
    if(flt) flt.value = '';
  }

  fsSetDir(d);
  fsClearFind(true);
  refreshFs();
}

// Integrate runs/outputs with the Workspace browser.
// Opens the given directory in the Workspace panel and scrolls the panel into view.
function browseInWorkspace(dir, revealPath){
  if(!(qeegApiOk && qeegApiToken)){
    alert('Workspace browsing is available only when served via qeeg_ui_server_cli.');
    return;
  }
  dir = String(dir||'').replace(/\\/g,'/').replace(/\/+$/,'');
  if(!dir) dir = '';
  const opts = {};
  if(revealPath){
    opts.reveal = String(revealPath||'');
  }
  fsEnter(dir, opts);
  const card = document.getElementById('fsPanel');
  if(card){
    try{ card.scrollIntoView({behavior:uiScrollBehavior(), block:'start'}); }catch(e){}
  }
}

function fsApplyPendingReveal(){
  const target = fsPendingRevealPath;
  if(!target) return;
  const list=document.getElementById('fsList');
  if(!list){ fsPendingRevealPath=''; return; }
  let row=null;
  const rows=list.querySelectorAll('tr[data-path]');
  for(const r of rows){
    if((r.getAttribute('data-path')||'')===target){ row=r; break; }
  }
  // Clear either way so we don't keep trying forever.
  fsPendingRevealPath='';
  if(!row) return;
  try{ row.classList.add('fs-reveal'); }catch(e){}
  try{ row.scrollIntoView({behavior:uiScrollBehavior(), block:'center'}); }catch(e){}
  setTimeout(()=>{ try{ row.classList.remove('fs-reveal'); }catch(e){} }, 1200);
}

function fsUp(){
  if(!fsCurrentDir) return;
  const parts = fsCurrentDir.split('/').filter(p=>p);
  if(parts.length===0) return;
  parts.pop();
  fsEnter(parts.join('/'));
}

function copyPath(p){
  navigator.clipboard.writeText(String(p||'')).then(()=>{showToast('Copied path');},()=>{});
}

function selectFromFs(p, type){
  setSelectedInput(String(p||''), type||'file');
}

async function selectPathAuto(p){
  p = String(p||'');
  if(!p){
    setSelectedInput('', 'file');
    return;
  }
  // Best-effort: detect directories via /api/list (available only when served by qeeg_ui_server_cli).
  if(!(qeegApiOk && qeegApiToken)){
    setSelectedInput(p, 'file');
    return;
  }
  try{
    const r=await apiFetch('/api/list',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dir:p})});
    if(r.ok){
      const j=await r.json();
      if(j && j.ok){
        setSelectedInput(p, 'dir');
        return;
      }
    }
  }catch(e){}
  setSelectedInput(p, 'file');
}

function getDraggedPath(ev){
  try{
    const dt = ev.dataTransfer;
    if(!dt) return '';
    let p = '';
    try{ p = dt.getData('application/x-qeeg-path') || ''; }catch(e){}
    if(!p){
      try{ p = dt.getData('text/plain') || ''; }catch(e){}
    }
    p = String(p||'').trim();
    if(p.includes('\n')) p = p.split('\n')[0].trim();
    return p;
  }catch(e){
    return '';
  }
}

function initArgsDrop(){
  // Allow dropping Workspace paths onto any *_args input to auto-select and inject.
  const argsInputs = document.querySelectorAll('input[id$="_args"]');
  argsInputs.forEach((el)=>{
    if(el.dataset && el.dataset.dropInited==='1') return;
    if(el.dataset) el.dataset.dropInited='1';

    el.addEventListener('dragover', (e)=>{
      const dt = e.dataTransfer;
      // Ignore OS file drags.
      if(dt && dt.files && dt.files.length) return;
      const p = getDraggedPath(e);
      if(!p) return;
      e.preventDefault();
      el.classList.add('drop-args');
    });
    el.addEventListener('dragleave', ()=>{ el.classList.remove('drop-args'); });
    el.addEventListener('drop', async (e)=>{
      const dt = e.dataTransfer;
      if(dt && dt.files && dt.files.length) return;
      const p = getDraggedPath(e);
      if(!p) return;
      e.preventDefault();
      el.classList.remove('drop-args');

      try{ await selectPathAuto(p); }catch(e2){ setSelectedInput(p, 'file'); }
      const sec = el.closest ? el.closest('section.tool') : null;
      const tool = sec ? (sec.getAttribute('data-tool')||'') : '';
      if(tool){
        const flag = getInjectFlagForTool(tool);
        if(flag){
          el.value = setFlagValue(el.value, flag, selectedInputPath);
          showToast('Injected into '+tool);
          return;
        }
      }
      // Fallback: append quoted path.
      el.value = (String(el.value||'').trim() + ' ' + quoteArgIfNeeded(selectedInputPath || p)).trim();
      showToast('Inserted path');
    });
  });
}

function initFsBrowser(){
  const fp = document.getElementById('fsPanel');
  if(!fp) return;
  if(!(qeegApiOk && qeegApiToken)){
    fp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
    return;
  }
  if(fp.dataset && fp.dataset.inited==='1') return;
  if(fp.dataset) fp.dataset.inited='1';

  fp.innerHTML =
    '<div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:8px">'+
      '<button class="btn" id="fsUpBtn">Up</button>'+
      '<button class="btn" id="fsBackBtn" title="Back (history)">←</button>'+
      '<button class="btn" id="fsFwdBtn" title="Forward (history)">→</button>'+
      '<button class="btn" id="fsRootBtn" title="Go to root">Root</button>'+
      '<button class="btn" id="fsRunsBtn" title="Go to ui_runs">ui_runs</button>'+
      '<button class="btn" id="fsTrashBtn" title="Go to .qeeg_trash (moved items)">Trash</button>'+
      '<button class="btn" id="fsMkdirBtn" title="Create a folder in the current directory">New folder</button>'+
      '<button class="btn" id="fsUploadBtn" title="Upload files into the current directory (or drop files onto this panel)">Upload</button>'+
      '<button class="btn" id="fsRefreshBtn">Refresh</button>'+
      '<span class="small">Dir: <code id="fsDirLabel"></code></span>'+
      '<select class="input" id="fsSort" style="max-width:160px;width:auto" title="Sort">'+
        '<option value="name">Sort: name</option>'+
        '<option value="mtime">Sort: modified</option>'+
        '<option value="size">Sort: size</option>'+
      '</select>'+
      '<label class="small" style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="fsDesc">desc</label>'+
      '<label class="small" style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="fsHidden">hidden</label>'+
      '<input class="input" id="fsFilter" style="max-width:220px" placeholder="filter (e.g., .edf)">'+
      '<span id="fsUploadStatus" class="small"></span>'+
    '</div>'+
    '<div id="fsDropHint" class="small hidden" style="margin-bottom:8px;padding:8px;border:1px dashed var(--border);border-radius:12px;background:rgba(255,255,255,0.03)">Drop files to upload into this directory.</div>'+
    '<div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:8px">'+
      '<input class="input" id="fsFindQ" style="max-width:240px" placeholder="find recursively (e.g., *.edf)">'+
      '<select class="input" id="fsFindType" style="max-width:160px;width:auto" title="Find type">'+
        '<option value="any">type: any</option>'+
        '<option value="file">type: files</option>'+
        '<option value="dir">type: dirs</option>'+
      '</select>'+
      '<select class="input" id="fsFindDepth" style="max-width:160px;width:auto" title="Max depth">'+
        '<option value="2">depth: 2</option>'+
        '<option value="4">depth: 4</option>'+
        '<option value="6">depth: 6</option>'+
        '<option value="8">depth: 8</option>'+
        '<option value="12">depth: 12</option>'+
      '</select>'+
      '<button class="btn" id="fsFindBtn" title="Search recursively under the current directory">Find</button>'+
      '<button class="btn" id="fsFindClearBtn" title="Clear find results">Clear</button>'+
      '<span id="fsFindStatus" class="small"></span>'+
    '</div>'+
    '<div id="fsFindWrap" class="hidden" style="margin-bottom:10px;padding:10px;border:1px solid var(--border);border-radius:12px;background:rgba(255,255,255,0.03)">'+
      '<div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap">'+
        '<div class="small"><b>Find</b> <span id="fsFindSummary"></span></div>'+
        '<button class="btn" id="fsFindCloseBtn">Close</button>'+
      '</div>'+
      '<div id="fsFindResults" class="small" style="margin-top:8px"></div>'+
    '</div>'+
    '<div id="fsCrumbs" class="small" style="margin-bottom:8px"></div>'+
    '<div id="fsList" class="small"></div>';

  const up=document.getElementById('fsUpBtn'); if(up) up.onclick=fsUp;
  const bk=document.getElementById('fsBackBtn'); if(bk) bk.onclick=fsBack;
  const fw=document.getElementById('fsFwdBtn'); if(fw) fw.onclick=fsForward;
  const rt=document.getElementById('fsRootBtn'); if(rt) rt.onclick=fsRoot;
  const rn=document.getElementById('fsRunsBtn'); if(rn) rn.onclick=fsRuns;
  const tr=document.getElementById('fsTrashBtn'); if(tr) tr.onclick=fsTrashDir;
  const mk=document.getElementById('fsMkdirBtn'); if(mk) mk.onclick=fsNewFolder;
  const upl=document.getElementById('fsUploadBtn'); if(upl) upl.onclick=fsOpenUpload;
  const rf=document.getElementById('fsRefreshBtn'); if(rf) rf.onclick=refreshFs;
  const flt=document.getElementById('fsFilter'); if(flt) flt.addEventListener('input', renderFs);

  const fq=document.getElementById('fsFindQ');
  if(fq){
    fq.addEventListener('keydown', (e)=>{
      if(e.key==='Enter'){ e.preventDefault(); fsFindRun(); }
    });
  }
  const fbtn=document.getElementById('fsFindBtn'); if(fbtn) fbtn.onclick=fsFindRun;
  const fclr=document.getElementById('fsFindClearBtn'); if(fclr) fclr.onclick=()=>fsClearFind(false);
  const fcls=document.getElementById('fsFindCloseBtn'); if(fcls) fcls.onclick=()=>fsClearFind(true);

  const ftype=document.getElementById('fsFindType');
  if(ftype){
    try{
      const v=localStorage.getItem('qeeg_fs_find_type');
      if(v==='any' || v==='file' || v==='dir') ftype.value = v;
    }catch(e){}
    ftype.onchange = ()=>{ try{ localStorage.setItem('qeeg_fs_find_type', ftype.value); }catch(e){} };
  }
  const fdepth=document.getElementById('fsFindDepth');
  if(fdepth){
    try{
      const v=localStorage.getItem('qeeg_fs_find_depth');
      if(v) fdepth.value = v;
      else fdepth.value = '6';
    }catch(e){ fdepth.value='6'; }
    fdepth.onchange = ()=>{ try{ localStorage.setItem('qeeg_fs_find_depth', fdepth.value); }catch(e){} };
  }

  initFsDrop();

  // Enable drag-and-drop of Workspace paths into tool args.
  const lst=document.getElementById('fsList');
  if(lst && lst.dataset && lst.dataset.dragInited!=='1'){
    lst.dataset.dragInited='1';
    lst.addEventListener('dragstart', fsOnDragStart);
  }
  const fr=document.getElementById('fsFindResults');
  if(fr && fr.dataset && fr.dataset.dragInited!=='1'){
    fr.dataset.dragInited='1';
    fr.addEventListener('dragstart', fsOnDragStart);
  }

  fsHistoryInit();
  fsUpdateNavButtons();

  const sort=document.getElementById('fsSort');
  if(sort){
    sort.value = fsSortMode || 'name';
    sort.onchange = ()=>{ fsSetSortMode(sort.value); refreshFs(); };
  }
  const desc=document.getElementById('fsDesc');
  if(desc){
    desc.checked = !!fsSortDesc;
    desc.onchange = ()=>{ fsSetSortDesc(desc.checked); refreshFs(); };
  }
  const hid=document.getElementById('fsHidden');
  if(hid){
    hid.checked = !!fsShowHidden;
    hid.onchange = ()=>{ fsSetShowHidden(hid.checked); refreshFs(); };
  }

  refreshFs();
}

async function refreshFs(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const lbl=document.getElementById('fsDirLabel');
  if(lbl) lbl.textContent = fsCurrentDir ? fsCurrentDir : '.';
  renderFsCrumbs();
  const list=document.getElementById('fsList');
  if(list) list.textContent = 'Loading…';
  try{
    const r=await apiFetch('/api/list',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dir:fsCurrentDir, show_hidden:!!fsShowHidden, sort:fsSortMode||'name', desc:!!fsSortDesc})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'list failed');
    fsEntries = (j && j.entries) ? j.entries : [];
    renderFs();
  }catch(e){
    if(list) list.innerHTML = '<span class="small">Error: '+esc(e&&e.message?e.message:String(e))+'</span>';
  }
}

function renderFs(){
  const list=document.getElementById('fsList');
  if(!list) return;
  const fltEl=document.getElementById('fsFilter');
  const q = norm(fltEl ? fltEl.value : '');
  let entries = fsEntries || [];
  if(q){
    entries = entries.filter(e=>{
      const n = norm(e && e.name ? e.name : '');
      const p = norm(e && e.path ? e.path : '');
      return n.includes(q) || p.includes(q);
    });
  }
  if(!entries || entries.length===0){
    list.innerHTML = '<span class="small">No entries.</span>';
    return;
  }

  let html = '<table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Modified</th><th>Actions</th></tr></thead><tbody>';
  for(const e of entries){
    const name = (e && e.name) ? String(e.name) : '';
    const path = (e && e.path) ? String(e.path) : name;
    const type = (e && e.type) ? String(e.type) : '';
    const size = (e && e.size) ? Number(e.size) : 0;
    const mtime = (e && e.mtime) ? Number(e.mtime) : 0;
    const pjs = escJs(path);
    const njs = escJs(name);
    const tjs = escJs(type);
    const inTrash = (path === '.qeeg_trash' || path.startsWith('.qeeg_trash/'));
    let actions = '';
    if(type==='dir'){
      actions =
        '<button class="btn" onclick="fsEnter(\\''+pjs+'\\')">Open</button> '+
        '<button class="btn" onclick="selectFromFs(\\''+pjs+'\\',\\'dir\\')">Select</button> '+
        '<button class="btn" onclick="copyPath(\\''+pjs+'\\')">Copy</button> '+
        '<button class="btn" onclick="fsRenamePath(\\''+pjs+'\\',\\''+tjs+'\\')">Rename</button> '+
        (inTrash ? '' : '<button class="btn" onclick="fsTrashItem(\\''+pjs+'\\',\\''+tjs+'\\')">Trash</button>');
    }else{
      actions =
        '<button class="btn" onclick="selectFromFs(\\''+pjs+'\\',\\'file\\')">Select</button> '+
        '<button class="btn" onclick="copyPath(\\''+pjs+'\\')">Copy</button> '+
        '<button class="btn" onclick="openPreview(encodePath(\\''+pjs+'\\'),\\''+njs+'\\')">Preview</button> '+
        '<a href="'+encodePath(path)+'" target="_blank">Open</a> '+
        '<button class="btn" onclick="fsRenamePath(\\''+pjs+'\\',\\''+tjs+'\\')">Rename</button> '+
        (inTrash ? '' : '<button class="btn" onclick="fsTrashItem(\\''+pjs+'\\',\\''+tjs+'\\')">Trash</button>');
    }
    const nameCell = (type==='dir')
      ? '<a href="#" onclick="fsEnter(\\''+pjs+'\\');return false;">📁 '+esc(name)+'</a>'
      : esc(name);
    html += '<tr class="fs-row" data-path="'+esc(path)+'" data-type="'+esc(type)+'" draggable="true"><td>'+nameCell+'</td><td>'+esc(type)+'</td><td>'+(type==='file'?esc(humanSize(size)):'')+'</td><td>'+esc(fmtLocalTimeSec(mtime))+'</td><td>'+actions+'</td></tr>';
  }
  html += '</tbody></table>';
  list.innerHTML = html;
  fsApplyPendingReveal();
}


function setApiUi(){
  const s=document.getElementById('apiStatus');
  if(s){
    const label = (qeegApiOk && qeegApiToken) ? 'connected' : (qeegApiOk ? 'token-missing' : 'offline');
    s.innerHTML = 'Run API: <b>'+label+'</b>';
  }
  document.querySelectorAll('.run-btn').forEach(b=>{b.disabled = !(qeegApiOk && qeegApiToken);});
  document.querySelectorAll('.statusline').forEach(el=>{
    if(!(qeegApiOk && qeegApiToken)) return;
    if(el.dataset && el.dataset.jobId) return;
    const t = (el.textContent||'');
    if(t.includes('Server not detected')){
      el.innerHTML='Ready (local server connected).';
    }
  });
  const rp = document.getElementById('runsPanel');
  if(rp){
    if(!(qeegApiOk && qeegApiToken)){
      rp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
      if(rp.dataset) rp.dataset.inited='';
      stopHistoryPoll();
    } else {
      initRunsPanel();
    }
  }
  const fp = document.getElementById('fsPanel');
  if(fp && !(qeegApiOk && qeegApiToken)){
    fp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
    if(fp.dataset){ fp.dataset.inited=''; fp.dataset.dropInited=''; }
  }
  updateSelectedInputUi();
}

function stopHistoryPoll(){
  if(runsHistoryTimer){ clearInterval(runsHistoryTimer); runsHistoryTimer=null; }
}

function startHistoryPoll(){
  if(runsHistoryTimer) return;
  refreshHistory(true);
  runsHistoryTimer = setInterval(()=>{
    if(runsViewMode==='history') refreshHistory(false);
  }, 12000);
}

function setRunsView(mode, silent){
  mode = (mode==='history') ? 'history' : 'session';
  runsViewMode = mode;
  try{ localStorage.setItem('qeeg_runs_view', runsViewMode); }catch(e){}

  const b1=document.getElementById('runsTabSession');
  const b2=document.getElementById('runsTabHistory');
  if(b1) b1.classList.toggle('active', runsViewMode==='session');
  if(b2) b2.classList.toggle('active', runsViewMode==='history');

  const sess=document.getElementById('runsSessionView');
  const hist=document.getElementById('runsHistoryView');
  if(sess) sess.classList.toggle('hidden', runsViewMode!=='session');
  if(hist) hist.classList.toggle('hidden', runsViewMode!=='history');

  const rf=document.getElementById('runsHistoryRefresh');
  if(rf) rf.disabled = (runsViewMode!=='history');

  if(runsViewMode==='history'){
    startHistoryPoll();
    if(!silent) refreshHistory(true);
  } else {
    stopHistoryPoll();
  }
}

function initRunsPanel(){
  const rp=document.getElementById('runsPanel');
  if(!rp) return;
  if(!(qeegApiOk && qeegApiToken)){
    rp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
    if(rp.dataset) rp.dataset.inited='';
    stopHistoryPoll();
    return;
  }
  if(rp.dataset && rp.dataset.inited==='1') return;
  if(rp.dataset) rp.dataset.inited='1';

  try{ runsViewMode = localStorage.getItem('qeeg_runs_view') || 'session'; }catch(e){ runsViewMode='session'; }

  rp.innerHTML =
    '<div class="runs-tabs">'+
      '<button class="btn runs-tab" id="runsTabSession" type="button">Session</button>'+
      '<button class="btn runs-tab" id="runsTabHistory" type="button">History</button>'+
      '<button class="btn" id="runsHistoryRefresh" type="button" title="Scan ui_runs for past jobs">Refresh history</button>'+
      '<span id="runsHistoryStamp" class="small"></span>'+
    '</div>'+
    '<div id="runsSessionView" class="small"></div>'+
    '<div id="runsHistoryView" class="small hidden"></div>';

  const b1=document.getElementById('runsTabSession');
  const b2=document.getElementById('runsTabHistory');
  const rf=document.getElementById('runsHistoryRefresh');
  if(b1) b1.onclick=()=>setRunsView('session');
  if(b2) b2.onclick=()=>setRunsView('history');
  if(rf) rf.onclick=()=>refreshHistory(true);

  setRunsView(runsViewMode, true);
}

function renderHistory(){
  const hv=document.getElementById('runsHistoryView');
  if(!hv) return;

  const stamp=document.getElementById('runsHistoryStamp');
  if(stamp){
    if(runsHistoryFetchedAt>0){
      stamp.textContent = 'Updated ' + new Date(runsHistoryFetchedAt).toLocaleTimeString();
    } else {
      stamp.textContent = '';
    }
  }

  const rows = Array.isArray(runsHistory) ? runsHistory : [];
  if(rows.length===0){
    hv.innerHTML = '<span class="small">No history yet (no <code>ui_runs/</code> folders found).</span>';
    return;
  }
  let html = '<table><thead><tr><th>Tool</th><th>Status</th><th>Started</th><th>Input</th><th>Actions</th></tr></thead><tbody>';
  for(let i=0; i<rows.length; ++i){
    const it = rows[i]||{};
    const tool = it.tool||'';
    const status = it.status||'';
    const started = it.started||'';
    const input = it.input_path||'';
    const run = it.run_dir||'';
    const log = it.log||'';
    const meta = it.meta||'';
    const cmd = it.command||'';
    const args = it.args||'';
    const exitCode = (it.exit_code!==undefined) ? String(it.exit_code) : '';

    let st = status ? status : 'unknown';
    if(st && st!=='running' && st!=='stopping' && exitCode!==''){
      st += ' (exit '+exitCode+')';
    }

    let links='';
    if(log) links += '<a href="'+esc(log)+'" target="_blank" rel="noopener"><code>run.log</code></a>';
    if(meta) links += (links?' | ':'') + '<a href="'+esc(meta)+'" target="_blank" rel="noopener"><code>meta</code></a>';
    if(cmd) links += (links?' | ':'') + '<a href="'+esc(cmd)+'" target="_blank" rel="noopener"><code>command</code></a>';
    if(run){
      const runHref = (!String(run).endsWith('/')) ? (run + '/') : run;
      links += (links?' | ':'') + '<a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>dir</code></a>';
    }

    let actions = '';
    if(args){
      actions += '<button class="btn" data-tool="'+esc(tool)+'" data-args="'+esc(args)+'" onclick="loadHistoryArgs(this)">Load args</button> ';
    }
    if(input){
      actions += '<button class="btn" data-input="'+esc(input)+'" onclick="selectHistoryInput(this)">Select input</button> ';
    }
    if(run){
      const disabled = (status==='running' || status==='stopping' || status==='queued') ? 'disabled' : '';
      actions += '<button class="btn" data-run-dir="'+esc(run)+'" onclick="browseHistoryRun(this)">Browse</button> ';
      actions += '<button class="btn" data-run-dir="'+esc(run)+'" onclick="openNotesFromHistory(this)">Notes</button> ';
    actions += '<button class="btn" data-run="'+esc(run)+'" '+disabled+' onclick="deleteHistoryRun(this)">Delete</button> ';
    }
    if(!actions) actions = '<span class="small">(no actions)</span>';

    html += '<tr>'+
      '<td><code>'+esc(tool)+'</code></td>'+
      '<td>'+esc(st)+'</td>'+
      '<td>'+esc(started)+'</td>'+
      '<td><span title="'+esc(input)+'">'+esc(input?input.split('/').pop():'' )+'</span></td>'+
      '<td>'+links+'<br>'+actions+'</td>'+
    '</tr>';
  }
  html += '</tbody></table>';
  hv.innerHTML = html;
}

async function refreshHistory(force){
  if(!(qeegApiOk && qeegApiToken)) return;
  initRunsPanel();
  const hv=document.getElementById('runsHistoryView');
  if(!hv) return;
  const now = Date.now();
  if(!force && runsHistoryFetchedAt && (now - runsHistoryFetchedAt) < 2500){
    return;
  }
  hv.innerHTML = '<span class="small">Loading history…</span>';
  try{
    const r=await apiFetch('/api/history?limit=50');
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'history failed');
    runsHistory = (j && j.runs) ? j.runs : [];
    runsHistoryFetchedAt = Date.now();
    renderHistory();
  }catch(e){
    hv.innerHTML = '<span class="small">Error loading history: '+esc(e&&e.message?e.message:String(e))+'</span>';
  }
}

function findToolSectionByName(tool){
  const els = document.querySelectorAll('.tool');
  for(const el of els){
    if(el && el.dataset && el.dataset.tool===tool) return el;
  }
  return null;
}

function loadHistoryArgs(btn){
  const tool = btn && btn.dataset ? (btn.dataset.tool||'') : '';
  const args = btn && btn.dataset ? (btn.dataset.args||'') : '';
  if(!tool){ alert('Missing tool in history row.'); return; }
  const sec = findToolSectionByName(tool);
  if(!sec){ alert('Tool not found in this page: '+tool); return; }
  const inp = sec.querySelector('input[id$="_args"]');
  if(inp){
    inp.value = String(args||'');
    try{ inp.focus(); inp.select(); }catch(e){}
  }
  try{ sec.scrollIntoView({behavior:uiScrollBehavior(), block:'start'}); }catch(e){}
}

function selectHistoryInput(btn){
  const input = btn && btn.dataset ? (btn.dataset.input||'') : '';
  if(!input) return;
  // We don't know if it's a file or dir from the history API; default to file.
  setSelectedInput(String(input), 'file');
}

function browseHistoryRun(btn){
  const runDir = btn && btn.dataset ? (btn.dataset.runDir||'') : '';
  if(!runDir){
    alert('Missing run dir.');
    return;
  }
  browseInWorkspace(String(runDir));
}

async function detectApi(){
  try{
    const r=await fetch('/api/status',{cache:'no-store'});
    if(!r.ok) throw new Error('bad');
    const j=await r.json();
    qeegApiOk = !!(j && j.ok);
    qeegApiToken = (j && j.token) ? j.token : '';
  }catch(e){
    qeegApiOk=false;
    qeegApiToken='';
  }
  setApiUi();
  if(qeegApiOk && qeegApiToken){
    startRunsPoll();
    initFsBrowser();
    initRunsPanel();
    maybeInitServerPresets();
  } else {
    stopRunsPoll();
    stopHistoryPoll();
    fxSetRunningCount(0);
    toolLive = {};
    updateLiveBadges();
    presetsServerEnabled=false;
    presetsSource='local';
    updatePresetsStatus();
  }
}

function stopRunsPoll(){
  if(runsTimer){ clearInterval(runsTimer); runsTimer=null; }
}

function startRunsPoll(){
  if(runsTimer) return;
  refreshRuns();
  runsTimer = setInterval(refreshRuns, 2500);
}

function liveToLabel(live){
  if(!live) return '';
  const r = Number(live.running||0);
  const q = Number(live.queued||0);
  const s = Number(live.stopping||0);
  const parts=[];
  if(r>0) parts.push(r+' running');
  if(q>0) parts.push(q+' queued');
  if(s>0) parts.push(s+' stopping');
  return parts.join(' • ');
}

function liveToHint(live){
  if(!live) return '';
  const r = Number(live.running||0);
  const q = Number(live.queued||0);
  const s = Number(live.stopping||0);
  let out='';
  if(r>0) out += (out?' ':'') + 'R'+r;
  if(q>0) out += (out?' ':'') + 'Q'+q;
  if(s>0) out += (out?' ':'') + 'S'+s;
  return out;
}

function updateBadgeEl(el, live, kind){
  if(!el) return;
  if(!live || (!live.running && !live.queued && !live.stopping)){
    el.textContent='';
    el.title='';
    el.classList.add('hidden');
    el.classList.remove('running','queued','stopping');
    return;
  }
  const label = liveToLabel(live);
  if(kind==='nav'){
    const n = Number(live.running||0) + Number(live.queued||0) + Number(live.stopping||0);
    el.textContent = String(n);
    el.title = label;
  } else {
    el.textContent = label;
    el.title = label;
  }
  el.classList.remove('hidden');
  // Pick a dominant status class for lightweight color cues.
  el.classList.toggle('running', !!live.running);
  el.classList.toggle('queued', !live.running && !!live.queued);
  el.classList.toggle('stopping', !live.running && !live.queued && !!live.stopping);
}

function updateLiveBadges(){
  const secs = document.querySelectorAll('section.tool[data-tool]');
  for(const sec of secs){
    const tool = sec && sec.dataset ? (sec.dataset.tool||'') : '';
    const id = sec ? (sec.id||'') : '';
    if(!tool || !id) continue;
    const live = toolLive[tool];
    updateBadgeEl(document.getElementById(id+'_live'), live, 'tool');
    updateBadgeEl(document.getElementById('nav_'+id+'_live'), live, 'nav');
  }
}

function setToolLiveFromRuns(runs){
  const m={};
  for(const it of (runs||[])){
    const tool = it && it.tool ? String(it.tool) : '';
    if(!tool) continue;
    const st = it && it.status ? String(it.status) : '';
    if(!m[tool]) m[tool] = {running:0, queued:0, stopping:0};
    if(st==='running') m[tool].running++;
    else if(st==='queued') m[tool].queued++;
    else if(st==='stopping') m[tool].stopping++;
  }
  toolLive = m;
  updateLiveBadges();
}

async function refreshRuns(){
  if(!(qeegApiOk && qeegApiToken)) return;
  initRunsPanel();
  const rp=document.getElementById('runsSessionView');
  if(!rp) return;
  try{
    const r=await apiFetch('/api/runs');
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'runs failed');
    const runs = (j && j.runs) ? j.runs : [];
    // Drive the optional procedural animations with a tiny "activity" signal:
    // number of running jobs (0 = idle).
    let runningCount = 0;
    for(const it of runs){
      const st = it.status||'';
      if(st==='running' || st==='stopping') runningCount++;
    }
    // Update live per-tool badges (sidebar + tool headers + palette hints).
    setToolLiveFromRuns(runs);
    fxSetRunningCount(runningCount);
    if(runs.length===0){
      rp.innerHTML = '<span class="small">No runs yet.</span>';
      fxSetRunningCount(0);
      return;
    }
    let html = '<table><thead><tr><th>ID</th><th>Tool</th><th>Status</th><th>Started</th><th>Links</th></tr></thead><tbody>';
    // newest first
    for(let i=runs.length-1, shown=0; i>=0 && shown<10; --i,++shown){
      const it = runs[i];
      const id = it.id;
      const tool = it.tool||'';
      const status = it.status||'';
      const started = it.started||'';
      const log = it.log||'';
      const run = it.run_dir||'';
      let links = '';
      if(log) links += '<a href="'+esc(log)+'" target="_blank" rel="noopener"><code>'+esc(log)+'</code></a>';
      if(run){
        const runHref = (!run.endsWith('/')) ? (run + '/') : run;
        links += (links?'<br>':'') + '<a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>'+esc(run)+'</code></a>';
        links += ' <button class="btn" title="Open this run folder in Workspace browser" onclick="browseInWorkspace(\\''+escJs(run)+'\\')">Browse</button>';
      }
      html += '<tr><td>'+id+'</td><td><code>'+esc(tool)+'</code></td><td>'+esc(status)+'</td><td>'+esc(started)+'</td><td>'+links+'</td></tr>';
    }
    html += '</tbody></table>';
    rp.innerHTML = html;
  }catch(e){
    rp.innerHTML = '<span class="small">Error loading runs: '+esc(e&&e.message?e.message:String(e))+'</span>';
    fxSetRunningCount(0);
  }
}

async function pollJob(statusEl, runBtn, stopBtn){
  if(!statusEl) return;
  const id = parseInt(statusEl.dataset.jobId||'0',10);
  if(!id) return;
  try{
    const r=await apiFetch('/api/job/'+id);
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'job query failed');
    const st = j.status||'';
    if(statusEl && statusEl.dataset){
      statusEl.dataset.runDir = (j && j.run_dir!==undefined) ? String(j.run_dir||'') : (statusEl.dataset.runDir||'');
    }
    const code = (j.exit_code!==undefined) ? j.exit_code : '';

    // Use a stable base HTML so we don't append duplicate status lines on every poll.
    let base = (statusEl.dataset && statusEl.dataset.baseHtml) ? statusEl.dataset.baseHtml : '';
    if(!base){
      // If we're still in the initial "Starting…" state, build a clean base.
      if((statusEl.textContent||'').trim()==='Starting…'){
        base = (st==='queued') ? ('Queued <b>'+esc(j.tool||'')+'</b>') : ('Started <b>'+esc(j.tool||'')+'</b>');
        if(j.log) base += ' — log: <a href="'+esc(j.log)+'" target="_blank" rel="noopener"><code>'+esc(j.log)+'</code></a>';
        if(j.run_dir){
          const runHref = (!String(j.run_dir).endsWith('/')) ? (j.run_dir + '/') : j.run_dir;
          base += '<br>Run dir: <a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>'+esc(j.run_dir)+'</code></a>';
        }
      }else{
        const cur = (statusEl.innerHTML||'');
        const ix = cur.indexOf('<br>Status:');
        base = (ix>=0) ? cur.slice(0, ix) : cur;
      }
      if(statusEl.dataset) statusEl.dataset.baseHtml = base;
    }

    let q='';
    if(st==='queued' && j && j.queue_pos && j.queue_len){
      q = ' ('+esc(String(j.queue_pos))+'/'+esc(String(j.queue_len))+')';
    }

    let html = base + '<br>Status: <b>'+esc(st)+'</b>' + q;
    if(st!=='running' && st!=='stopping' && st!=='queued') html += ' (exit '+esc(String(code))+')';
    statusEl.innerHTML = html;

    if(stopBtn) stopBtn.disabled = !(st==='running' || st==='stopping' || st==='queued');
    if(runBtn && (st!=='running' && st!=='stopping' && st!=='queued')) runBtn.disabled = false;

    if(st==='running' || st==='stopping' || st==='queued'){
      const delay = (st==='queued') ? 2000 : 1500;
      setTimeout(()=>{pollJob(statusEl, runBtn, stopBtn);}, delay);
    } else {
      refreshRuns();
      maybeAutoRefreshOutputs(statusEl);
      // Stop any live log tail poller for this job when it finishes.
      stopLogFollowForStatus(statusEl);
    }
  }catch(e){
    setTimeout(()=>{pollJob(statusEl, runBtn, stopBtn);}, 2500);
  }
}

async function runTool(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const tool=btn.getAttribute('data-tool');
  const argsId=btn.getAttribute('data-args-id');
  const statusId=btn.getAttribute('data-status-id');
  const stopId=btn.getAttribute('data-stop-id');
  const logwrapId=btn.getAttribute('data-logwrap-id');
  const logId=btn.getAttribute('data-log-id');
  const inp=document.getElementById(argsId);
  const status=document.getElementById(statusId);
  const stopBtn = stopId ? document.getElementById(stopId) : null;
  const logWrap = logwrapId ? document.getElementById(logwrapId) : null;
  const logCode = logId ? document.getElementById(logId) : null;
  const args=(inp&&inp.value)?inp.value:'';

  // If a previous run is still being tailed, stop that polling before we
  // clear the job id.
  stopLogFollowForStatus(status);

  if(status){ status.textContent='Starting…'; status.dataset.jobId=''; status.dataset.runDir=''; if(status.dataset) status.dataset.baseHtml=''; }
  if(logCode) logCode.textContent='';
  if(logWrap) logWrap.classList.add('hidden');
  if(stopBtn){ stopBtn.disabled=true; stopBtn.dataset.jobId=''; }
  btn.disabled=true;
  try{
    const r=await apiFetch('/api/run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tool:tool,args:args})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'run failed');
    const id = j.id;
    if(status){ status.dataset.jobId = String(id); status.dataset.runDir = (j && j.run_dir) ? String(j.run_dir) : ''; }
    if(stopBtn){ stopBtn.disabled=false; stopBtn.dataset.jobId = String(id); }
    if(logWrap) logWrap.classList.remove('hidden');
    if(status){
      const st = (j && j.status) ? String(j.status) : 'running';
      let base = (st==='queued') ? ('Queued <b>'+esc(tool)+'</b>') : ('Started <b>'+esc(tool)+'</b>');
      if(j.log) base += ' — log: <a href="'+esc(j.log)+'" target="_blank" rel="noopener"><code>'+esc(j.log)+'</code></a>';
      if(j.run_dir){
        const runHref = (!String(j.run_dir).endsWith('/')) ? (j.run_dir + '/') : j.run_dir;
        base += '<br>Run dir: <a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>'+esc(j.run_dir)+'</code></a>';
      }
      if(status.dataset) status.dataset.baseHtml = base;
      let q='';
      if(st==='queued' && j && j.queue_pos && j.queue_len){
        q = ' ('+esc(String(j.queue_pos))+'/'+esc(String(j.queue_len))+')';
      }
      status.innerHTML = base + '<br>Status: <b>'+esc(st)+'</b>' + q;
    }

    pollJob(status, btn, stopBtn);
    if(logCode){
      try{
        const lr=await apiFetch('/api/log/'+id);
        logCode.textContent = await lr.text();
      }catch(_){ }
    }
    refreshRuns();
  }catch(e){
    if(status) status.textContent='Error: '+(e&&e.message?e.message:String(e));
  }finally{
    if(!(status && status.dataset.jobId)) btn.disabled = !(qeegApiOk && qeegApiToken);
  }
}

async function stopJob(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  const id = status && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
  if(!id) return;
  btn.disabled=true;
  try{
    const r=await apiFetch('/api/kill/'+id,{method:'POST'});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'kill failed');
    if(status){
      let base = (status.dataset && status.dataset.baseHtml) ? status.dataset.baseHtml : '';
      if(!base){
        const cur = (status.innerHTML||'');
        const ix = cur.indexOf('<br>Status:');
        base = (ix>=0) ? cur.slice(0, ix) : cur;
      }
      if(status.dataset) status.dataset.baseHtml = base;
      const st = (j && j.status) ? String(j.status) : 'stopping';
      let q='';
      if(st==='queued' && j && j.queue_pos && j.queue_len){
        q = ' ('+esc(String(j.queue_pos))+'/'+esc(String(j.queue_len))+')';
      }
      status.innerHTML = base + '<br>Status: <b>'+esc(st)+'</b>' + q;
    }
    refreshRuns();
    pollJob(status, null, btn);
  }catch(e){
    if(status) status.textContent='Error stopping job: '+(e&&e.message?e.message:String(e));
  }
}

function toggleLog(btn){
  const wrapId=btn.getAttribute('data-logwrap-id');
  const wrap = document.getElementById(wrapId);
  if(!wrap) return;

  // When opening the panel, start a lightweight "follow" tail. When closing,
  // stop polling.
  const willShow = wrap.classList.contains('hidden');
  wrap.classList.toggle('hidden');
  if(willShow){
    startLogFollow(btn);
  } else {
    stopLogFollow(btn);
  }
}

async function refreshLog(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const statusId=btn.getAttribute('data-status-id');
  const logId=btn.getAttribute('data-log-id');
  const status=document.getElementById(statusId);
  const code=document.getElementById(logId);
  const id = status && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
  if(!id || !code) return;
  // Hard refresh: reset follow state and re-fetch the tail.
  stopLogFollow(btn);
  try{
    // Prefer the incremental API if available.
    const r=await apiFetch('/api/log2/'+id+'?offset=0&max=65536');
    if(r.ok){
      const j=await r.json();
      if(j && j.ok){
        code.textContent = String(j.text||'');
        // Re-seed follow offset if the panel is open.
        const wrapId=btn.getAttribute('data-logwrap-id');
        const wrap = wrapId ? document.getElementById(wrapId) : null;
        if(wrap && !wrap.classList.contains('hidden')){
          const statusId=btn.getAttribute('data-status-id');
          const status=document.getElementById(statusId);
          const jobId = status && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
          if(jobId) seedLogFollow(jobId, code, j.offset||0, j.size||0);
        }
        return;
      }
    }
    // Fallback: plain /api/log tail.
    const r2=await apiFetch('/api/log/'+id);
    code.textContent = await r2.text();
  }catch(e){
    code.textContent='Error loading log: '+(e&&e.message?e.message:String(e));
  }
}

// ---- Live log tail (when the panel is open) ----
const logFollowers = new Map(); // jobId -> {timer,offset,lastSize,mode,code,pre}

function stopLogFollowForStatus(statusEl){
  const id = statusEl && statusEl.dataset && statusEl.dataset.jobId ? parseInt(statusEl.dataset.jobId,10) : 0;
  if(id && logFollowers.has(id)){
    const st = logFollowers.get(id);
    if(st && st.timer) clearInterval(st.timer);
    logFollowers.delete(id);
  }
}

function stopLogFollow(btn){
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  stopLogFollowForStatus(status);
}

function seedLogFollow(jobId, codeEl, offset, size){
  if(!jobId || !codeEl) return;
  const pre = codeEl.parentElement;
  const st = logFollowers.get(jobId) || {};
  st.offset = Number(offset||0);
  st.lastSize = Number(size||0);
  st.code = codeEl;
  st.pre = pre;
  st.mode = st.mode || 'delta';
  logFollowers.set(jobId, st);
}

function clampLogText(codeEl){
  if(!codeEl) return;
  const maxChars = 220000;
  const s = codeEl.textContent || '';
  if(s.length > maxChars){
    codeEl.textContent = s.slice(s.length - maxChars);
  }
}

function preNearBottom(preEl){
  if(!preEl) return true;
  const d = preEl.scrollHeight - preEl.scrollTop - preEl.clientHeight;
  return d < 40;
}

function scrollPreToBottom(preEl){
  if(!preEl) return;
  preEl.scrollTop = preEl.scrollHeight;
}

async function logFollowTick(jobId){
  const st = logFollowers.get(jobId);
  if(!st) return;
  const code = st.code;
  const pre = st.pre;
  if(!code) return;
  const keepBottom = preNearBottom(pre);

  // If the incremental endpoint fails, fall back to replacing the tail.
  if(st.mode !== 'tail'){
    try{
      const off = Number(st.offset||0);
      const r = await apiFetch('/api/log2/'+jobId+'?offset='+encodeURIComponent(String(off))+'&max=65536');
      if(!r.ok) throw new Error('bad');
      const j = await r.json();
      if(!(j && j.ok)) throw new Error('bad');
      const size = Number(j.size||0);
      const next = Number(j.offset||0);
      const text = String(j.text||'');

      // If the file shrank (rotation/truncation), reset.
      if(isFinite(st.lastSize) && size < st.lastSize){
        code.textContent = '';
        st.offset = 0;
      }

      // First fetch or reset: replace. Otherwise append.
      if(!st.offset || j.truncated){
        code.textContent = text;
      } else if(text){
        code.textContent += text;
      }

      st.offset = next;
      st.lastSize = size;
      clampLogText(code);

      if(keepBottom) scrollPreToBottom(pre);
      return;
    } catch(e){
      st.mode = 'tail';
      // fall through
    }
  }

  // Tail fallback
  try{
    const r2 = await apiFetch('/api/log/'+jobId);
    if(r2.ok){
      code.textContent = await r2.text();
      clampLogText(code);
      if(keepBottom) scrollPreToBottom(pre);
    }
  }catch(_){
    // ignore
  }
}

function startLogFollow(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const statusId=btn.getAttribute('data-status-id');
  const logId=btn.getAttribute('data-log-id');
  const status=document.getElementById(statusId);
  const code=document.getElementById(logId);
  const wrapId=btn.getAttribute('data-logwrap-id');
  const wrap = wrapId ? document.getElementById(wrapId) : null;
  const jobId = status && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
  if(!jobId || !code) return;

  // If we already follow this job, no-op.
  if(logFollowers.has(jobId)) return;

  // Seed and do an immediate fetch.
  seedLogFollow(jobId, code, 0, 0);
  logFollowTick(jobId);

  // Only poll while the panel is visible.
  const timer = setInterval(()=>{
    if(wrap && wrap.classList.contains('hidden')) return;
    logFollowTick(jobId);
  }, 1100);
  const st = logFollowers.get(jobId) || {};
  st.timer = timer;
  logFollowers.set(jobId, st);
}


function joinPath(a,b){
  a=String(a||'').replace(/\\/g,'/');
  b=String(b||'').replace(/\\/g,'/');
  while(a.endsWith('/')) a=a.slice(0,-1);
  while(b.startsWith('/')) b=b.slice(1);
  if(!a) return b;
  if(!b) return a;
  return a+'/'+b;
}
function safeRel(p){
  p=String(p||'').replace(/\\/g,'/').replace(/^\/+/,'');
  if(p.includes('..')) return '';
  return p;
}
function encodePath(p){
  // Encode a relative path safely (per-segment) for use in href/fetch.
  p = String(p||'').replace(/\\/g,'/');
  const parts = p.split('/').map(seg => encodeURIComponent(seg));
  return parts.join('/');
}
function pathExt(p){
  const s=String(p||'');
  const i=s.lastIndexOf('.');
  if(i<0) return '';
  return s.slice(i+1).toLowerCase();
}
function isTextExt(ext){
  return ext==='txt'||ext==='log'||ext==='csv'||ext==='tsv'||ext==='json'||ext==='md'||ext==='svg';
}
function isImageExt(ext){
  return ext==='png'||ext==='jpg'||ext==='jpeg'||ext==='webp'||ext==='bmp'||ext==='gif'||ext==='svg';
}

async function refreshOutputsForStatus(statusEl){
  if(!(qeegApiOk && qeegApiToken)) return;
  if(!statusEl) return;
  const outId = statusEl.dataset ? (statusEl.dataset.outId||'') : '';
  const out = outId ? document.getElementById(outId) : null;
  if(!out) return;

  const id = statusEl.dataset && statusEl.dataset.jobId ? parseInt(statusEl.dataset.jobId,10) : 0;
  if(!id){
    out.innerHTML = '<span class="small">No job yet.</span>';
    return;
  }
  out.textContent = 'Loading…';
  try{
    const jr=await apiFetch('/api/job/'+id);
    const job=await jr.json();
    if(!jr.ok) throw new Error(job&&job.error?job.error:'job query failed');
    const metaPath = job.meta || '';
    const runDir = job.run_dir || '';
    const runHref = runDir ? (!String(runDir).endsWith('/') ? (runDir + '/') : runDir) : '';
    let meta=null;
    if(metaPath){
      const mr=await fetch(metaPath,{cache:'no-store'});
      meta=await mr.json();
    }
    const outdir = (meta && meta.OutputDir) ? String(meta.OutputDir) : runDir;
    const outputs = (meta && meta.Outputs && Array.isArray(meta.Outputs)) ? meta.Outputs : [];
    let html='';
    if(runHref){
      html += '<div class="small">Run dir: <a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>'+esc(runDir)+'</code></a> '+
              '<button class="btn" title="Open this run folder in Workspace browser" onclick="browseInWorkspace(\\''+escJs(runDir)+'\\')">Browse</button> '+
              '<button class="btn" title="Set Workspace selection to this run folder" onclick="selectPathAuto(\\''+escJs(runDir)+'\\')">Use run dir</button></div>';
    }
    if(metaPath){
      html += '<div class="small">Meta: <a href="'+esc(metaPath)+'" target="_blank" rel="noopener"><code>'+esc(metaPath)+'</code></a></div>';
    }
    if(!outputs || outputs.length===0){
      html += '<div class="small" style="margin-top:8px">No outputs listed yet.</div>';
      out.innerHTML = html;
      return;
    }
    html += '<table style="margin-top:8px"><thead><tr><th>Output</th><th>Actions</th></tr></thead><tbody>';
    for(const rel0 of outputs){
      const rel = safeRel(rel0);
      if(!rel) continue;
      const full = joinPath(outdir, rel);
      const href = encodePath(full);
      const ext = pathExt(rel);
      let actions = '<a href="'+esc(href)+'" target="_blank" rel="noopener">Open</a>';
      if(isTextExt(ext) || isImageExt(ext)){
        actions += ' <button class="btn" onclick="openPreview(\\''+escJs(href)+'\\',\\''+escJs(rel)+'\\')">Preview</button>';
      }
      actions += ' <button class="btn" onclick="copyPath(\\''+escJs(full)+'\\')">Copy</button>';
      actions += ' <button class="btn" title="Set Workspace selection to this output" onclick="selectPathAuto(\\''+escJs(full)+'\\')">Use</button>';
      html += '<tr><td><code>'+esc(rel)+'</code></td><td>'+actions+'</td></tr>';
    }
    html += '</tbody></table>';
    out.innerHTML = html;
  }catch(e){
    out.innerHTML = '<span class="small">Error loading outputs: '+esc(e&&e.message?e.message:String(e))+'</span>';
  }
}

async function refreshOutputs(btn){
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  if(!status) return;
  refreshOutputsForStatus(status);
}

function browseRunDirFromStatus(btn){
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  const runDir = status && status.dataset ? (status.dataset.runDir||'') : '';
  if(!runDir){
    alert('No run dir yet for this job. Run the tool first.');
    return;
  }
  browseInWorkspace(runDir);
}


async function downloadZip(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  const id = status && status.dataset && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
  if(!id){
    alert('No job yet. Run the tool first.');
    return;
  }
  const prev = btn.textContent;
  btn.disabled=true;
  btn.textContent='Downloading…';
  try{
    const r=await apiFetch('/api/zip/'+id);
    if(!r.ok){
      let msg='download failed';
      try{
        const j=await r.json();
        if(j && j.error) msg=String(j.error);
      }catch(_){ }
      throw new Error(msg);
    }
    const blob=await r.blob();
    let fname='run_'+id+'.zip';
    const cd = (r.headers.get('Content-Disposition')||r.headers.get('content-disposition')||'');
    const m = /filename\*=UTF-8''([^;]+)|filename="?([^";]+)"?/i.exec(cd);
    if(m){
      if(m[1]){
        try{ fname=decodeURIComponent(m[1]); }catch(_){ fname=m[1]; }
      }else if(m[2]){
        fname=m[2];
      }
    }
    const url=URL.createObjectURL(blob);
    const a=document.createElement('a');
    a.href=url;
    a.download=fname;
    document.body.appendChild(a);
    a.click();
    setTimeout(()=>{ URL.revokeObjectURL(url); a.remove(); }, 1500);
  }catch(e){
    alert('Error downloading zip: '+(e&&e.message?e.message:String(e)));
  }finally{
    btn.disabled=false;
    btn.textContent=prev;
  }
}



async function deleteRunDir(runDir){
  if(!(qeegApiOk && qeegApiToken)) throw new Error('API not connected');
  runDir = String(runDir||'');
  const r = await apiFetch('/api/delete_run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({run_dir:runDir})});
  let j=null;
  try{ j = await r.json(); }catch(e){ j=null; }
  if(!r.ok){
    const msg = (j && j.error) ? String(j.error) : 'delete failed';
    const detail = (j && j.detail) ? String(j.detail) : '';
    throw new Error(detail ? (msg+' — '+detail) : msg);
  }
  // Refresh visible lists.
  try{ refreshRuns(); }catch(e){}
  try{ if(runsViewMode==='history') refreshHistory(true); }catch(e){}
  return j;
}

async function deleteRun(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const statusId=btn.getAttribute('data-status-id');
  const status=document.getElementById(statusId);
  const id = status && status.dataset && status.dataset.jobId ? parseInt(status.dataset.jobId,10) : 0;
  let runDir = status && status.dataset ? (status.dataset.runDir||'') : '';

  if(!runDir && id){
    try{
      const jr=await apiFetch('/api/job/'+id);
      if(jr.ok){
        const j=await jr.json();
        runDir = j && j.run_dir ? String(j.run_dir) : '';
      }
    }catch(e){}
  }

  if(!runDir){
    alert('No run dir found for this job.');
    return;
  }
  if(!confirm('Delete run folder "'+runDir+'"? This removes all artifacts under it.')) return;

  const prev = btn.textContent;
  btn.disabled=true;
  btn.textContent='Deleting…';
  try{
    await deleteRunDir(runDir);

    // Clear tool panel state.
    if(status){
      stopLogFollowForStatus(status);
      status.dataset.jobId='';
      status.dataset.runDir='';
      if(status.dataset) status.dataset.baseHtml='';
      status.innerHTML = 'Run deleted: <code>'+esc(runDir)+'</code>';
    }

    const runBtnId = statusId ? statusId.replace(/_status$/,'_runbtn') : '';
    const stopBtnId = statusId ? statusId.replace(/_status$/,'_stopbtn') : '';
    const runBtn = runBtnId ? document.getElementById(runBtnId) : null;
    const stopBtn = stopBtnId ? document.getElementById(stopBtnId) : null;
    if(stopBtn) stopBtn.disabled = true;
    if(runBtn) runBtn.disabled = !(qeegApiOk && qeegApiToken);

    // If an outputs panel is open, replace with a simple message.
    if(status && status.dataset){
      const outId = status.dataset.outId || '';
      const out = outId ? document.getElementById(outId) : null;
      if(out) out.innerHTML = '<span class="small">Run deleted.</span>';
    }
  }catch(e){
    alert('Error deleting run: '+(e&&e.message?e.message:String(e)));
  }finally{
    btn.disabled=false;
    btn.textContent=prev;
  }
}

async function deleteHistoryRun(btn){
  if(!(qeegApiOk && qeegApiToken)) return;
  const run = btn && btn.dataset ? (btn.dataset.run||'') : '';
  if(!run) return;
  if(!confirm('Delete run folder "'+run+'"?')) return;
  const prev = btn.textContent;
  btn.disabled=true;
  btn.textContent='Deleting…';
  try{
    await deleteRunDir(run);
    // Force-refresh history view after deletion.
    await refreshHistory(true);
  }catch(e){
    alert('Error deleting run: '+(e&&e.message?e.message:String(e)));
  }finally{
    btn.disabled=false;
    btn.textContent=prev;
  }
}

function toggleOutputs(btn){
  const wrapId=btn.getAttribute('data-outwrap-id');
  const wrap = document.getElementById(wrapId);
  if(!wrap) return;
  wrap.classList.toggle('hidden');
  if(!wrap.classList.contains('hidden')){
    const statusId=btn.getAttribute('data-status-id');
    const status=document.getElementById(statusId);
    if(status) refreshOutputsForStatus(status);
  }
}


let flagsLastFocus=null;
let flagsCtx={tool:'', argsId:'', helpId:'', injectSelId:'', items:[], pathFlags:new Set()};

function parseHelpOptions(helpText){
  const lines = String(helpText||'').split(/\r?\n/);
  const items=[];
  let last=null;
  for(const raw0 of lines){
    if(raw0==null) continue;
    const raw = String(raw0).replace(/\r/g,'');
    if(!raw.trim()) continue;

    const isOpt = /^\s*(-\w|--\w)/.test(raw);
    if(isOpt){
      const parts = raw.trim().split(/\s{2,}/);
      const left = parts[0] || raw.trim();
      const desc = parts.slice(1).join('  ') || '';
      const mLong = left.match(/--[A-Za-z0-9][A-Za-z0-9_-]*/);
      const mShort = left.match(/-\w/);
      const flag = mLong ? mLong[0] : (mShort ? mShort[0] : '');
      const item = {spec:left, flag:flag, desc:desc};
      items.push(item);
      last=item;
      continue;
    }

    // Wrapped description lines: append to the previous option.
    if(last && /^\s+/.test(raw)){
      const extra = raw.trim();
      if(extra){
        last.desc = last.desc ? (last.desc + ' ' + extra) : extra;
      }
    }
  }

  // De-dup (best-effort).
  const seen = new Set();
  const out = [];
  for(const it of items){
    const key = String(it.spec||'') + '|' + String(it.desc||'');
    if(seen.has(key)) continue;
    seen.add(key);
    out.push(it);
  }
  return out;
}

function getPathFlagsFromSelect(selId){
  const set = new Set();
  const sel = document.getElementById(selId);
  if(!sel) return set;
  for(const o of sel.options){
    const v = (o && o.value) ? String(o.value) : '';
    if(v && v.startsWith('--')) set.add(v);
  }
  return set;
}

function insertTextAtCursor(el, txt){
  if(!el) return;
  txt = String(txt||'');
  const start = el.selectionStart;
  const end = el.selectionEnd;
  if(typeof start === 'number' && typeof end === 'number'){
    const v = String(el.value||'');
    el.value = v.slice(0,start) + txt + v.slice(end);
    const p = start + txt.length;
    try{ el.setSelectionRange(p,p); }catch(_){}
  } else {
    el.value = String(el.value||'') + txt;
  }
  el.focus();
}

function flagInsert(argsId, flag){
  const el = document.getElementById(argsId);
  if(!el) return;
  const f = String(flag||'').trim();
  if(!f) return;
  insertTextAtCursor(el, f + ' ');
}

function flagUseSelection(argsId, flag){
  const el = document.getElementById(argsId);
  if(!el) return;
  const f = String(flag||'').trim();
  if(!f) return;
  if(!selectedInputPath){
    alert('Select a file or directory first (Workspace browser).');
    return;
  }
  el.value = setFlagValue(el.value, f, selectedInputPath);
  el.focus();
}

function renderFlagHelper(){
  const list = document.getElementById('flagsList');
  const meta = document.getElementById('flagsMeta');
  const searchEl = document.getElementById('flagsSearch');
  const titleEl = document.getElementById('flagsTitle');
  if(!list || !meta || !searchEl || !titleEl) return;

  titleEl.textContent = flagsCtx.tool ? ('Flags: ' + flagsCtx.tool) : 'Flags';

  const q = norm(searchEl.value||'');
  const items = (flagsCtx.items||[]);
  const pathFlags = flagsCtx.pathFlags || new Set();

  let shown = 0;
  let html = '<table><thead><tr><th>Flag</th><th>Description</th><th>Actions</th></tr></thead><tbody>';

  for(const it of items){
    const spec = String(it && it.spec ? it.spec : '');
    const flag = String(it && it.flag ? it.flag : '');
    const desc = String(it && it.desc ? it.desc : '');
    const hay = norm(spec + ' ' + flag + ' ' + desc);
    if(q && !hay.includes(q)) continue;
    ++shown;

    const isPath = !!(flag && pathFlags.has(flag));
    const pill = isPath ? ' <span class="pill">path</span>' : '';

    let actions = '';
    if(flag){
      actions += '<button class="btn" onclick="flagInsert(\\'' + escJs(flagsCtx.argsId) + '\\',\\'' + escJs(flag) + '\\')">Insert</button> ';
      if(isPath){
        actions += '<button class="btn" onclick="flagUseSelection(\\'' + escJs(flagsCtx.argsId) + '\\',\\'' + escJs(flag) + '\\')">Use selection</button> ';
      }
      actions += '<button class="btn" onclick="navigator.clipboard.writeText(\\'' + escJs(flag) + '\\').then(()=>{showToast(\'Copied\');},()=>{})">Copy</button>';
    } else {
      actions = '<span class="small">—</span>';
    }

    html += '<tr><td><code>' + esc(spec || flag) + '</code>' + pill + '</td><td>' + esc(desc) + '</td><td>' + actions + '</td></tr>';
  }

  html += '</tbody></table>';
  if(shown === 0){
    html = '<span class="small">No matching flags.</span>';
  }

  list.innerHTML = html;
  meta.textContent = String(shown) + ' / ' + String(items.length) + ' shown' + (selectedInputPath ? (' · selection: ' + selectedInputType) : '');
}

function openFlagHelper(btn){
  const back = document.getElementById('flagsBackdrop');
  const list = document.getElementById('flagsList');
  const meta = document.getElementById('flagsMeta');
  const searchEl = document.getElementById('flagsSearch');
  if(!back || !list || !meta || !searchEl) return;

  flagsLastFocus = document.activeElement;

  const tool = (btn && btn.dataset && btn.dataset.tool) ? String(btn.dataset.tool) : '';
  const argsId = (btn && btn.dataset && btn.dataset.argsId) ? String(btn.dataset.argsId) : '';
  const helpId = (btn && btn.dataset && btn.dataset.helpId) ? String(btn.dataset.helpId) : '';
  const injectSelId = (btn && btn.dataset && btn.dataset.injectSelId) ? String(btn.dataset.injectSelId) : '';

  flagsCtx = {tool:tool, argsId:argsId, helpId:helpId, injectSelId:injectSelId, items:[], pathFlags:getPathFlagsFromSelect(injectSelId)};

  back.classList.remove('hidden');
  searchEl.value = '';
  searchEl.focus();
  searchEl.oninput = renderFlagHelper;

  const helpEl = helpId ? document.getElementById(helpId) : null;
  const helpText = helpEl ? (helpEl.textContent||'') : '';
  if(!helpText.trim()){
    list.innerHTML = '<span class="small">No embedded help found for this tool. Generate the dashboard with <code>--bin-dir</code> and help embedding enabled, or run the tool in a terminal with <code>--help</code>.</span>';
    meta.textContent = '';
    return;
  }

  flagsCtx.items = parseHelpOptions(helpText);
  if(!flagsCtx.items.length){
    list.innerHTML = '<span class="small">Could not parse options from the embedded help output.</span>';
    meta.textContent = '';
    return;
  }

  renderFlagHelper();
}

function closeFlagHelper(){
  const back = document.getElementById('flagsBackdrop');
  if(back) back.classList.add('hidden');

  const searchEl = document.getElementById('flagsSearch');
  if(searchEl) searchEl.oninput = null;

  if(flagsLastFocus && flagsLastFocus.focus){
    try{ flagsLastFocus.focus(); }catch(_){}
  }
  flagsLastFocus = null;
}

// ----- CSV/TSV preview helpers (table + optional heatmap) -----

let csvPreviewCtx = null;

function parseDelimitedPreview(text, delim, maxRows, maxCols){
  text = String(text||'');
  delim = String(delim||',');
  maxRows = Number(maxRows||200);
  maxCols = Number(maxCols||60);
  if(!isFinite(maxRows) || maxRows<=0) maxRows = 200;
  if(!isFinite(maxCols) || maxCols<=0) maxCols = 60;

  const rows = [];
  let row = [];
  let field = '';
  let inQuotes = false;
  let i = 0;
  let truncated = false;
  while(i < text.length){
    const c = text[i];
    if(inQuotes){
      if(c === '"'){
        // Escaped quote
        if(i + 1 < text.length && text[i+1] === '"'){
          field += '"';
          i += 2;
          continue;
        }
        inQuotes = false;
        i += 1;
        continue;
      }
      field += c;
      i += 1;
      continue;
    }

    if(c === '"'){
      inQuotes = true;
      i += 1;
      continue;
    }

    if(c === delim){
      row.push(field);
      field = '';
      i += 1;
      continue;
    }

    if(c === '\n'){
      row.push(field);
      field = '';
      rows.push(row);
      row = [];
      i += 1;
      if(rows.length >= maxRows){
        truncated = (i < text.length);
        break;
      }
      continue;
    }

    if(c === '\r'){
      // swallow; handle CRLF by ignoring \r and letting \n terminate the row.
      i += 1;
      continue;
    }

    field += c;
    i += 1;
  }

  // Flush last field/row.
  if(!truncated){
    if(field.length || row.length){
      row.push(field);
      rows.push(row);
    }
  }

  // Trim columns for preview.
  let maxSeen = 0;
  const trimmed = rows.map(r=>{
    r = Array.isArray(r) ? r : [];
    if(r.length > maxSeen) maxSeen = r.length;
    if(r.length > maxCols) return r.slice(0, maxCols);
    return r;
  });
  const colTrunc = (maxSeen > maxCols);
  return {rows:trimmed, truncated:truncated, colTrunc:colTrunc, cols:maxSeen};
}

function isNumericCell(s){
  s = String(s||'').trim();
  if(!s) return false;
  const v = Number(s);
  return isFinite(v);
}

function detectNumericMatrix(rows){
  // Best-effort: detect matrices like coherence_matrix_*.csv.
  // Common shape: header row with column labels, first column with row labels.
  if(!Array.isArray(rows) || rows.length < 2) return null;
  const r0 = rows[0] || [];
  if(r0.length < 2) return null;

  // Infer whether first row is a header (non-numeric tokens).
  let headerRow = false;
  for(let j = 0; j < r0.length; ++j){
    const v = String(r0[j]||'').trim();
    if(!v) continue;
    if(!isNumericCell(v)) { headerRow = true; break; }
  }

  let rowLabels = [];
  let colLabels = [];
  let rowStart = headerRow ? 1 : 0;
  let colStart = 0;

  // If headerRow, prefer col labels (skip first cell if empty).
  if(headerRow){
    colStart = 1;
    colLabels = r0.slice(colStart).map(x=>String(x||''));
  }

  // Infer row labels if first column (below header) has non-numeric entries.
  let hasRowLabels = false;
  for(let i = rowStart; i < rows.length; ++i){
    const rr = rows[i] || [];
    const v = String(rr[0]||'').trim();
    if(!v) continue;
    if(!isNumericCell(v)) { hasRowLabels = true; break; }
  }
  if(hasRowLabels){
    colStart = Math.max(colStart, 1);
    rowLabels = rows.slice(rowStart).map(r=>String((r||[])[0]||''));
  } else {
    rowLabels = rows.slice(rowStart).map((_,i)=>String(i));
  }

  const data = [];
  let nCols = 0;
  let minV = Infinity;
  let maxV = -Infinity;

  for(let i = rowStart; i < rows.length; ++i){
    const rr = rows[i] || [];
    const row = [];
    for(let j = colStart; j < rr.length; ++j){
      const s = String(rr[j]||'').trim();
      if(!s) return null;
      const v = Number(s);
      if(!isFinite(v)) return null;
      row.push(v);
      if(v < minV) minV = v;
      if(v > maxV) maxV = v;
      if(row.length > 256) return null;
    }
    if(i === rowStart) nCols = row.length;
    if(row.length !== nCols) return null;
    data.push(row);
    if(data.length > 256) return null;
  }

  if(!data.length || !nCols) return null;

  // Fill labels if missing.
  if(!colLabels.length){
    colLabels = new Array(nCols).fill(0).map((_,i)=>String(i));
  } else if(colLabels.length !== nCols) {
    // If labels are present but don't match, drop.
    colLabels = new Array(nCols).fill(0).map((_,i)=>String(i));
  }
  if(rowLabels.length !== data.length){
    rowLabels = new Array(data.length).fill(0).map((_,i)=>String(i));
  }

  // Avoid degenerate range.
  if(!isFinite(minV) || !isFinite(maxV)) return null;
  if(maxV - minV < 1e-12) { maxV = minV + 1e-12; }

  return {data:data, rowLabels:rowLabels, colLabels:colLabels, min:minV, max:maxV};
}

function detectQeegCsv(rows, label){
  // Heuristic detection for common QEEG CSV outputs.
  //
  // 1) Wide bandpowers table: channel, delta/theta/alpha/beta/gamma (optionally *_z).
  // 2) Generic per-channel metrics table: channel + mostly-numeric columns (ratios, features).
  if(!Array.isArray(rows) || rows.length < 2) return null;
  const header = rows[0] || [];
  if(!Array.isArray(header) || header.length < 2) return null;

  const map = {};
  for(let j=0;j<header.length;++j){
    const key = norm(String(header[j]||'').trim());
    if(key && map[key]===undefined) map[key] = j;
  }  let channelIdx = map['channel'];
  if(channelIdx===undefined){
    const alts = ['chan','electrode','sensor'];
    for(const a of alts){ if(map[a]!==undefined){ channelIdx = map[a]; break; } }
  }
  if(channelIdx===undefined) return null;

  // Wide bandpowers (qeeg_bandpower_cli / qeeg_map_cli).
  const want = ['delta','theta','alpha','beta','gamma'];
  const bands = [];
  for(const b of want){
    if(map[b]!==undefined){
      const zKey = b + "_z";
      const zIdx = (map[zKey]!==undefined) ? map[zKey] : -1;
      bands.push({name:b, idx:map[b], zIdx:zIdx});
    }
  }
  const haveZ = bands.some(b=>b.zIdx>=0);
  if(bands.length >= 3){
    return {kind:'bandpowers', channelIdx:channelIdx, bands:bands, haveZ:haveZ};
  }

  // Generic per-channel metrics table (e.g., bandratios.csv, spectral_features.csv).
  const metrics = [];
  const probeN = Math.min(rows.length-1, 40);
  for(let j=0;j<header.length;++j){
    if(j===channelIdx) continue;
    const name = String(header[j]||'').trim();
    if(!name) continue;
    let seen=0, num=0;
    for(let i=1;i<=probeN;++i){
      const rr = rows[i] || [];
      if(j>=rr.length) continue;
      const v = String(rr[j]||'').trim();
      if(!v) continue;
      ++seen;
      if(isNumericCell(v)) ++num;
    }
    if(seen>=3 && (num/seen) >= 0.7){
      metrics.push({name:name, idx:j});
    }
  }
  if(!metrics.length) return null;

  // Prefer ratio-like columns when present.
  let pref = metrics[0].name;
  const labelLow = norm(label||'');
  for(const m of metrics){
    const mn = norm(m.name);
    if(labelLow.includes('ratio') || labelLow.includes('bandratio') || mn.includes('over') || mn.includes('ratio') || mn.includes('tbr')){
      pref = m.name;
      break;
    }
  }

  return {kind:'channel_metrics', channelIdx:channelIdx, metrics:metrics, prefMetric:pref};
}

function detectTimeSeriesCsv(rows, label){
  // Heuristic detection for simple time series tables.
  //
  // Expected shapes:
  //   - bandpower_timeseries.csv: t_end_sec, <band>_<channel>...
  //   - artifact_gate_timeseries.csv: t_end_sec, gate...
  //
  // We look for a numeric time column + at least one mostly-numeric value column.
  if(!Array.isArray(rows) || rows.length < 3) return null;
  const header = rows[0] || [];
  if(!Array.isArray(header) || header.length < 2) return null;

  const map = {};
  for(let j=0;j<header.length;++j){
    const key = norm(String(header[j]||'').trim());
    if(key && map[key]===undefined) map[key] = j;
  }

  const candidates = [
    't_end_sec','t_end_s','t_mid_sec','t_mid_s','t_start_sec','t_start_s',
    'time_sec','time_s','t_sec','t_s','time','t',
    'seconds','sec','sample','index'
  ];

  let timeIdx = -1;
  for(const k of candidates){
    if(map[k]!==undefined){ timeIdx = map[k]; break; }
  }
  if(timeIdx < 0){
    // Fallback: use the first column if it looks numeric.
    timeIdx = 0;
  }
  const timeName = String(header[timeIdx]||'time');

  const probeN = Math.min(rows.length-1, 80);

  // Validate time column numeric-ness.
  let seenT = 0, numT = 0;
  for(let i=1;i<=probeN;++i){
    const rr = rows[i] || [];
    if(timeIdx >= rr.length) continue;
    const v = String(rr[timeIdx]||'').trim();
    if(!v) continue;
    ++seenT;
    if(isNumericCell(v)) ++numT;
  }
  if(seenT < 3 || (numT/Math.max(1,seenT)) < 0.8) return null;

  // Find value columns.
  const cols = [];
  for(let j=0;j<header.length;++j){
    if(j===timeIdx) continue;
    const name = String(header[j]||'').trim();
    if(!name) continue;

    let seen=0, num=0;
    for(let i=1;i<=probeN;++i){
      const rr = rows[i] || [];
      if(j>=rr.length) continue;
      const v = String(rr[j]||'').trim();
      if(!v) continue;
      ++seen;
      if(isNumericCell(v)) ++num;
    }
    if(seen>=3 && (num/seen) >= 0.8){
      cols.push({name:name, idx:j});
    }
  }

  if(cols.length < 1) return null;

  // Pick a default column (best-effort).
  const labelLow = norm(label||'');
  let defIdx = cols[0].idx;
  if(labelLow.includes('bandpower_timeseries') || labelLow.includes('bandpower')){
    for(const c of cols){
      const n = norm(c.name);
      if(n.includes('alpha')) { defIdx = c.idx; break; }
    }
  }
  if(labelLow.includes('artifact') || labelLow.includes('gate')){
    for(const c of cols){
      const n = norm(c.name);
      if(n.includes('gate') || n.includes('artifact')) { defIdx = c.idx; break; }
    }
  }

  return {kind:'timeseries', timeIdx:timeIdx, timeName:timeName, cols:cols, defaultSel:[defIdx]};
}

function csvSetView(mode){
  if(!csvPreviewCtx) return;
  mode = (mode==='heat') ? 'heat' : (mode==='raw' ? 'raw' : (mode==='qeeg' ? 'qeeg' : (mode==='series' ? 'series' : 'table')));
  csvPreviewCtx.view = mode;
  try{ localStorage.setItem('qeeg_csv_view', mode); }catch(e){}
  renderCsvPreview();
}

function renderCsvPreview(){
  const body=document.getElementById('previewBody');
  if(!body || !csvPreviewCtx) return;
  const rows = csvPreviewCtx.rows || [];
  const cols = csvPreviewCtx.cols || 0;
  const maxRows = csvPreviewCtx.maxRows || 200;
  const maxCols = csvPreviewCtx.maxCols || 60;
  const truncated = !!csvPreviewCtx.truncated;
  const colTrunc = !!csvPreviewCtx.colTrunc;
  const raw = csvPreviewCtx.raw || '';

  const canHeat = !!csvPreviewCtx.matrix;
  const canQeeg = !!csvPreviewCtx.qeeg;
  const canSeries = !!csvPreviewCtx.series;

  let view = csvPreviewCtx.view || 'table';
  if(view==='heat' && !canHeat) view='table';
  if(view==='qeeg' && !canQeeg) view='table';
  if(view==='series' && !canSeries) view='table';
  csvPreviewCtx.view = view;

  let heatBtn = '<button class="btn" id="csvViewHeatBtn" type="button" '+(canHeat?'':'disabled')+'>Heatmap</button>';
  let qeegBtn = '<button class="btn" id="csvViewQeegBtn" type="button" '+(canQeeg?'':'disabled')+'>QEEG</button>';
  let seriesBtn = '<button class="btn" id="csvViewSeriesBtn" type="button" '+(canSeries?'':'disabled')+'>Series</button>';

  let meta = 'Rows: <b>'+String(rows.length)+'</b>' + (truncated ? (' (showing first '+String(maxRows)+')') : '') + ' · Cols: <b>'+String(cols)+'</b>' + (colTrunc ? (' (showing first '+String(maxCols)+')') : '');
  body.innerHTML =
    '<div class="csv-controls">'+
      '<span class="small">'+meta+'</span>'+
      '<input class="input" id="csvFilterInput" placeholder="filter rows/channels" style="max-width:240px" value="'+esc(csvPreviewCtx.filter||'')+'">'+
      '<button class=\"btn\" id=\"csvViewTableBtn\" type=\"button\">Table</button>'+
      seriesBtn+
      qeegBtn+
      heatBtn+
      '<button class=\"btn\" id=\"csvViewRawBtn\" type=\"button\">Raw</button>'+
    '</div>'+
    '<div id=\"csvViewTable\" class=\"csv-viewwrap\"></div>'+
    '<div id=\"csvViewSeries\" class=\"csv-viewwrap hidden\"></div>'+
    '<div id=\"csvViewQeeg\" class=\"csv-viewwrap hidden\"></div>'+
    '<div id=\"csvViewHeat\" class=\"csv-viewwrap hidden\"></div>'+
    '<div id=\"csvViewRaw\" class=\"hidden\"><pre><code>'+esc(raw)+'</code></pre></div>';

  const fi=document.getElementById('csvFilterInput');
  if(fi){
    fi.oninput = ()=>{ if(!csvPreviewCtx) return; csvPreviewCtx.filter = fi.value||''; renderCsvPreviewTableOnly(); if(csvPreviewCtx.view==='qeeg') renderCsvPreviewQeegOnly(); };
  }
  const bt=document.getElementById('csvViewTableBtn');
  const bs=document.getElementById('csvViewSeriesBtn');
  const bq=document.getElementById('csvViewQeegBtn');
  const bh=document.getElementById('csvViewHeatBtn');
  const br=document.getElementById('csvViewRawBtn');
  if(bt) bt.onclick = ()=>csvSetView('table');
  if(bs) bs.onclick = ()=>csvSetView('series');
  if(bq) bq.onclick = ()=>csvSetView('qeeg');
  if(bh) bh.onclick = ()=>csvSetView('heat');
  if(br) br.onclick = ()=>csvSetView('raw');

  // Activate view.
  const vTable=document.getElementById('csvViewTable');
  const vSeries=document.getElementById('csvViewSeries');
  const vQeeg=document.getElementById('csvViewQeeg');
  const vHeat=document.getElementById('csvViewHeat');
  const vRaw=document.getElementById('csvViewRaw');
  if(vTable) vTable.classList.toggle('hidden', view!=='table');
  if(vSeries) vSeries.classList.toggle('hidden', view!=='series');
  if(vQeeg) vQeeg.classList.toggle('hidden', view!=='qeeg');
  if(vHeat) vHeat.classList.toggle('hidden', view!=='heat');
  if(vRaw) vRaw.classList.toggle('hidden', view!=='raw');

  // Render current view.
  renderCsvPreviewTableOnly();
  if(view==='series') renderCsvPreviewSeriesOnly();
  if(view==='qeeg') renderCsvPreviewQeegOnly();
  if(view==='heat') renderCsvPreviewHeatOnly();
  if(bt) bt.classList.toggle('active', view==='table');
  if(bs) bs.classList.toggle('active', view==='series');
  if(bq) bq.classList.toggle('active', view==='qeeg');
  if(bh) bh.classList.toggle('active', view==='heat');
  if(br) br.classList.toggle('active', view==='raw');
}

function renderCsvPreviewTableOnly(){
  if(!csvPreviewCtx) return;
  const wrap=document.getElementById('csvViewTable');
  if(!wrap) return;
  const rows = Array.isArray(csvPreviewCtx.rows) ? csvPreviewCtx.rows : [];
  const q = norm(csvPreviewCtx.filter||'');
  if(!rows.length){
    wrap.innerHTML = '<span class="small">(no rows)</span>';
    return;
  }
  let shown = 0;
  let html = '<table class="csv-table"><thead><tr>';
  const header = rows[0] || [];
  for(let j=0;j<header.length;++j){
    html += '<th>'+esc(String(header[j]||''))+'</th>';
  }
  html += '</tr></thead><tbody>';
  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    if(q){
      let hit = false;
      for(const c of r){
        if(norm(String(c||'')).includes(q)) { hit = true; break; }
      }
      if(!hit) continue;
    }
    ++shown;
    html += '<tr>';
    for(let j=0;j<header.length;++j){
      const v = (j < r.length) ? r[j] : '';
      const isNum = isNumericCell(v);
      html += '<td'+(isNum?' class="num"':'')+'>'+esc(String(v||''))+'</td>';
    }
    html += '</tr>';
    if(shown > 500){
      html += '<tr><td colspan="'+String(header.length)+'" class="small">… (filtered view truncated)</td></tr>';
      break;
    }
  }
  html += '</tbody></table>';
  if(shown===0 && q){
    html = '<span class="small">No rows match the filter.</span>';
  }
  wrap.innerHTML = html;
}

function drawHeatmap(canvas, matrix){
  if(!canvas || !matrix || !matrix.data) return;
  const ctx = canvas.getContext('2d');
  if(!ctx) return;
  const data = matrix.data;
  const nR = data.length;
  const nC = data[0].length;
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0,0,W,H);
  const minV = matrix.min;
  const maxV = matrix.max;
  const inv = 1.0 / (maxV - minV);
  const cw = W / nC;
  const ch = H / nR;
  // Simple grayscale mapping (keeps things dependency-free).
  for(let i=0;i<nR;++i){
    const row = data[i];
    for(let j=0;j<nC;++j){
      const v = row[j];
      const t = clamp01((v - minV) * inv);
      const g = Math.floor(t * 255);
      ctx.fillStyle = 'rgb('+g+','+g+','+g+')';
      ctx.fillRect(j*cw, i*ch, cw+0.5, ch+0.5);
    }
  }
}

function renderCsvPreviewHeatOnly(){
  if(!csvPreviewCtx || !csvPreviewCtx.matrix) return;
  const wrap=document.getElementById('csvViewHeat');
  if(!wrap) return;
  const m = csvPreviewCtx.matrix;
  const nR = m.data.length;
  const nC = m.data[0].length;
  const size = Math.min(720, Math.max(260, Math.floor(18 * Math.max(nR, nC))));
  wrap.innerHTML =
    '<div style="display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:8px">'+
      '<span class="small">Numeric matrix: '+String(nR)+'×'+String(nC)+' · range ['+String(m.min.toFixed(4))+', '+String(m.max.toFixed(4))+']</span>'+
      '<span class="small" id="csvHeatHover"></span>'+
    '</div>'+
    '<canvas id="csvHeatCanvas" class="csv-heat" width="'+String(size)+'" height="'+String(size)+'"></canvas>';
  const canvas = document.getElementById('csvHeatCanvas');
  if(!canvas) return;
  drawHeatmap(canvas, m);
  const hover = document.getElementById('csvHeatHover');
  canvas.onmousemove = (ev)=>{
    if(!hover) return;
    const rect = canvas.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const y = ev.clientY - rect.top;
    const j = Math.floor((x / rect.width) * nC);
    const i = Math.floor((y / rect.height) * nR);
    if(i>=0 && i<nR && j>=0 && j<nC){
      const v = m.data[i][j];
      const rl = m.rowLabels[i] || String(i);
      const cl = m.colLabels[j] || String(j);
      hover.textContent = rl + ' × ' + cl + ' = ' + String(v);
    }
  };
  canvas.onmouseleave = ()=>{ if(hover) hover.textContent=''; };
}





function csvSeriesPalette(){
  const accent = qeegCssVar('--accent') || 'rgba(106,166,255,0.9)';
  return [
    accent,
    'rgba(255,120,140,0.9)',
    'rgba(120,255,170,0.9)',
    'rgba(255,205,120,0.9)',
    'rgba(180,140,255,0.9)',
    'rgba(140,240,255,0.9)',
  ];
}

function drawTimeSeriesChart(canvas, xs, series, opts){
  if(!canvas) return;
  const ctx = canvas.getContext('2d');
  if(!ctx) return;

  xs = Array.isArray(xs) ? xs : [];
  series = Array.isArray(series) ? series : [];
  opts = opts || {};

  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0,0,W,H);

  const title = String(opts.title||'');
  const xLabel = String(opts.xLabel||'t');
  const hoverIndex = (opts.hoverIndex===undefined || opts.hoverIndex===null) ? -1 : Number(opts.hoverIndex);

  let minX = Infinity, maxX = -Infinity;
  for(const x of xs){
    const v = Number(x);
    if(!isFinite(v)) continue;
    if(v < minX) minX = v;
    if(v > maxX) maxX = v;
  }

  let minY = Infinity, maxY = -Infinity;
  for(const s of series){
    const ys = Array.isArray(s.ys) ? s.ys : [];
    for(const y of ys){
      const v = Number(y);
      if(!isFinite(v)) continue;
      if(v < minY) minY = v;
      if(v > maxY) maxY = v;
    }
  }

  if(!isFinite(minX) || !isFinite(maxX) || !isFinite(minY) || !isFinite(maxY)){
    ctx.fillStyle = 'rgba(255,255,255,0.65)';
    ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
    ctx.fillText('No numeric time series to plot.', 20, 30);
    return;
  }

  // Include a baseline for common signals.
  minY = Math.min(minY, 0);
  maxY = Math.max(maxY, 0);
  if(maxX - minX < 1e-12) maxX = minX + 1e-12;
  if(maxY - minY < 1e-12) maxY = minY + 1e-12;

  const padL = 64;
  const padR = 18;
  const padT = 26;
  const padB = 42;
  const iw = W - padL - padR;
  const ih = H - padT - padB;
  if(iw <= 10 || ih <= 10) return;

  const xFor = (x)=> padL + (x - minX) * iw / (maxX - minX);
  const yFor = (y)=> padT + (maxY - y) * ih / (maxY - minY);

  // Title
  ctx.fillStyle = 'rgba(255,255,255,0.75)';
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'left';
  ctx.fillText(title, 10, 16);

  // Grid
  ctx.strokeStyle = 'rgba(255,255,255,0.10)';
  ctx.lineWidth = 1;
  const gridN = 5;
  for(let g=0; g<=gridN; ++g){
    const t = g / gridN;
    const y = padT + t * ih;
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + iw, y);
    ctx.stroke();
  }

  // Axes
  ctx.strokeStyle = 'rgba(255,255,255,0.18)';
  ctx.beginPath();
  ctx.moveTo(padL, padT);
  ctx.lineTo(padL, padT + ih);
  ctx.lineTo(padL + iw, padT + ih);
  ctx.stroke();

  // Labels
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'right';
  ctx.fillText(String(maxY.toFixed(4)), padL - 6, padT + 10);
  ctx.fillText(String(minY.toFixed(4)), padL - 6, padT + ih);
  ctx.textAlign = 'left';
  ctx.fillText(String(minX.toFixed(2)), padL, padT + ih + 18);
  ctx.textAlign = 'right';
  ctx.fillText(String(maxX.toFixed(2)), padL + iw, padT + ih + 18);
  ctx.textAlign = 'center';
  ctx.fillText(xLabel, padL + iw/2, H - 6);

  // Series
  for(let si=0; si<series.length; ++si){
    const s = series[si] || {};
    const ys = Array.isArray(s.ys) ? s.ys : [];
    const color = String(s.color||'rgba(255,255,255,0.8)');

    ctx.strokeStyle = color;
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    let started = false;
    for(let i=0;i<xs.length && i<ys.length;++i){
      const x = Number(xs[i]);
      const y = Number(ys[i]);
      if(!isFinite(x) || !isFinite(y)){
        started = false;
        continue;
      }
      const px = xFor(x);
      const py = yFor(y);
      if(!started){
        ctx.moveTo(px, py);
        started = true;
      } else {
        ctx.lineTo(px, py);
      }
    }
    ctx.stroke();
  }

  // Legend
  ctx.textAlign = 'left';
  ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  let lx = padL + 8;
  let ly = padT + 12;
  for(const s of series){
    const name = String(s.name||'');
    if(!name) continue;
    ctx.fillStyle = String(s.color||'rgba(255,255,255,0.7)');
    ctx.fillRect(lx, ly-8, 10, 10);
    ctx.fillStyle = 'rgba(255,255,255,0.75)';
    ctx.fillText(name.length>38 ? (name.slice(0,38)+'…') : name, lx + 14, ly);
    ly += 16;
    if(ly > padT + ih - 8) break;
  }

  // Hover marker
  if(isFinite(hoverIndex) && hoverIndex >= 0 && hoverIndex < xs.length){
    const x = Number(xs[Math.floor(hoverIndex)]);
    if(isFinite(x)){
      const px = xFor(x);
      ctx.strokeStyle = 'rgba(255,255,255,0.22)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(px, padT);
      ctx.lineTo(px, padT + ih);
      ctx.stroke();

      for(const s of series){
        const ys = Array.isArray(s.ys) ? s.ys : [];
        const y = Number(ys[Math.floor(hoverIndex)]);
        if(!isFinite(y)) continue;
        const py = yFor(y);
        ctx.fillStyle = String(s.color||'rgba(255,255,255,0.9)');
        ctx.beginPath();
        ctx.arc(px, py, 3.2, 0, Math.PI*2);
        ctx.fill();
      }
    }
  }
}

function renderCsvPreviewSeriesOnly(){
  if(!csvPreviewCtx || !csvPreviewCtx.series) return;
  const wrap=document.getElementById('csvViewSeries');
  if(!wrap) return;

  const rows = Array.isArray(csvPreviewCtx.rows) ? csvPreviewCtx.rows : [];
  if(rows.length < 2){
    wrap.innerHTML = '<span class="small">(no rows)</span>';
    return;
  }

  const s = csvPreviewCtx.series;
  const header = rows[0] || [];
  const timeIdx = s.timeIdx;
  const timeName = String(s.timeName||'time');

  if(!Array.isArray(s.selected) || !s.selected.length){
    s.selected = Array.isArray(s.defaultSel) ? s.defaultSel.slice(0,1) : [];
  }

  // Build series dropdown options.
  let optsHtml = '';
  for(const c of (s.cols||[])){
    const nm = String(c.name||'');
    const idx = Number(c.idx);
    optsHtml += '<option value="'+esc(String(idx))+'">'+esc(nm)+'</option>';
  }

  const truncNote = csvPreviewCtx.truncated ? '<span class="pill">truncated preview</span>' : '';

  wrap.innerHTML =
    '<div style="padding:10px">'+
      '<div class="qeeg-controls">'+
        '<span class="small">Detected: <b>time series</b> '+truncNote+' · x=<code>'+esc(timeName)+'</code></span>'+
        '<span class="small">Tip: select 1–4 series to overlay (Ctrl/Cmd-click).</span>'+
      '</div>'+
      '<div class="qeeg-controls" style="align-items:flex-start">'+
        '<div style="display:flex;flex-direction:column;gap:6px">'+
          '<label class="small">Series</label>'+
          '<select class="input" id="csvSeriesSelect" multiple size="7" style="min-width:340px;max-width:560px">'+optsHtml+'</select>'+
        '</div>'+
        '<div style="display:flex;flex-direction:column;gap:6px">'+
          '<label class="small">Actions</label>'+
          '<button class="btn" id="csvSeriesClearBtn" type="button">Clear</button>'+
          '<span class="small" id="csvSeriesMeta"></span>'+
          '<span class="small" id="csvSeriesHover"></span>'+
        '</div>'+
      '</div>'+
      '<canvas id="csvSeriesCanvas" class="qeeg-chart" width="940" height="360"></canvas>'+
    '</div>';

  const sel = document.getElementById('csvSeriesSelect');
  const clearBtn = document.getElementById('csvSeriesClearBtn');
  const meta = document.getElementById('csvSeriesMeta');
  const hover = document.getElementById('csvSeriesHover');
  const canvas = document.getElementById('csvSeriesCanvas');
  if(!sel || !canvas) return;

  // Restore selections
  const selectedSet = new Set((s.selected||[]).map(x=>String(x)));
  for(const opt of sel.options){
    if(selectedSet.has(String(opt.value))) opt.selected = true;
  }

  function getSelectedIdx(){
    const out = [];
    for(const opt of sel.selectedOptions){
      const v = Number(opt.value);
      if(isFinite(v)) out.push(v);
    }
    // Limit overlays to keep it readable.
    return out.slice(0, 6);
  }

  function buildData(selectedIdx, hoverIndex){
    // Extract x
    const xs = [];
    const N = rows.length - 1;
    for(let i=1;i<rows.length;++i){
      const r = rows[i] || [];
      const tx = (timeIdx < r.length) ? Number(String(r[timeIdx]||'').trim()) : NaN;
      xs.push(isFinite(tx) ? tx : NaN);
    }

    // Extract y series
    const pal = csvSeriesPalette();
    const series = [];
    for(let si=0; si<selectedIdx.length; ++si){
      const col = selectedIdx[si];
      let name = String(header[col]||('col'+String(col)));
      const ys = new Array(N).fill(NaN);
      for(let i=1;i<rows.length;++i){
        const r = rows[i] || [];
        const v = (col < r.length) ? Number(String(r[col]||'').trim()) : NaN;
        ys[i-1] = isFinite(v) ? v : NaN;
      }
      series.push({name:name, ys:ys, color:pal[si % pal.length]});
    }

    return {xs:xs, series:series};
  }

  function csvSeriesXRange(xs){
  xs = Array.isArray(xs) ? xs : [];
  let minX = Infinity;
  let maxX = -Infinity;
  for(const x of xs){
    const v = Number(x);
    if(!isFinite(v)) continue;
    if(v < minX) minX = v;
    if(v > maxX) maxX = v;
  }
  if(!isFinite(minX) || !isFinite(maxX)){
    minX = 0.0;
    maxX = Math.max(1.0, xs.length - 1);
  }
  if(Math.abs(maxX - minX) < 1e-12) maxX = minX + 1e-12;
  return {min:minX, max:maxX};
}

  // Draw + hover
  let lastHover = -1;

  function redraw(hoverIndex){
    const idxs = getSelectedIdx();
    s.selected = idxs.slice();
    if(!idxs.length){
      if(meta) meta.textContent = 'No series selected.';
      drawTimeSeriesChart(canvas, [0,1], [], {title:'(no series)', xLabel:timeName});
      return;
    }
    const d = buildData(idxs, hoverIndex);
    // Compute meta
    if(meta) meta.textContent = String(idxs.length) + ' series · points: ' + String(d.xs.length);
    drawTimeSeriesChart(canvas, d.xs, d.series, {title: String(csvPreviewCtx.label||'') , xLabel: timeName, hoverIndex: hoverIndex});

    // Update hover label
    if(hover && hoverIndex>=0 && hoverIndex<d.xs.length){
      const t = d.xs[Math.floor(hoverIndex)];
      let msg = 't=' + (isFinite(t) ? String(Number(t).toFixed(3)) : 'NaN');
      for(const ss of d.series){
        const y = ss.ys[Math.floor(hoverIndex)];
        if(isFinite(y)) msg += ' · ' + String(ss.name) + '=' + String(Number(y).toFixed(4));
      }
      hover.textContent = msg;
    }else if(hover){
      hover.textContent = '';
    }

    return d;
  }

  let lastData = redraw(-1);

  sel.onchange = ()=>{ lastData = redraw(lastHover); };
  if(clearBtn){
    clearBtn.onclick = ()=>{
      for(const opt of sel.options) opt.selected = false;
      // pick first default
      const def = (Array.isArray(s.defaultSel) && s.defaultSel.length) ? String(s.defaultSel[0]) : (sel.options.length ? String(sel.options[0].value) : '');
      for(const opt of sel.options){ if(String(opt.value)===def) opt.selected = true; }
      lastData = redraw(-1);
    };
  }

  // Hover behavior
  canvas.onmousemove = (ev)=>{
    if(!lastData || !Array.isArray(lastData.xs) || !lastData.xs.length) return;
    const rect = canvas.getBoundingClientRect();
    const x = (ev.clientX - rect.left) / rect.width;
    const idx = Math.max(0, Math.min(lastData.xs.length-1, Math.round(x * (lastData.xs.length-1))));
    lastHover = idx;
    lastData = redraw(idx);
  };
  canvas.onmouseleave = ()=>{ lastHover = -1; lastData = redraw(-1); };
}

function qeegBandNote(){
  // Common EEG band naming conventions (approx).
  return 'Bands (Hz): δ 0.5–4 · θ 4–7 · α 8–12 · β 13–30 · γ >30';
}


// Approximate 2D scalp positions for common 10-20/10-10 channel names.
// Coordinates are on a unit circle where (0,-1) is frontal/nasion and (0,1) is occipital.
const QEEG_POS = {
  'FP1':[-0.35,-0.92], 'FPZ':[0.0,-0.98], 'FP2':[0.35,-0.92],
  'AF7':[-0.55,-0.80], 'AF3':[-0.25,-0.78], 'AFZ':[0.0,-0.80], 'AF4':[0.25,-0.78], 'AF8':[0.55,-0.80],
  'F7':[-0.82,-0.62], 'F5':[-0.62,-0.62], 'F3':[-0.42,-0.62], 'F1':[-0.18,-0.62], 'FZ':[0.0,-0.62], 'F2':[0.18,-0.62], 'F4':[0.42,-0.62], 'F6':[0.62,-0.62], 'F8':[0.82,-0.62],
  'FT7':[-0.92,-0.34], 'FC5':[-0.66,-0.34], 'FC3':[-0.42,-0.34], 'FC1':[-0.18,-0.34], 'FCZ':[0.0,-0.34], 'FC2':[0.18,-0.34], 'FC4':[0.42,-0.34], 'FC6':[0.66,-0.34], 'FT8':[0.92,-0.34],
  'T7':[-1.00,0.0], 'C5':[-0.66,0.0], 'C3':[-0.42,0.0], 'C1':[-0.18,0.0], 'CZ':[0.0,0.0], 'C2':[0.18,0.0], 'C4':[0.42,0.0], 'C6':[0.66,0.0], 'T8':[1.00,0.0],
  'TP7':[-0.92,0.34], 'CP5':[-0.66,0.34], 'CP3':[-0.42,0.34], 'CP1':[-0.18,0.34], 'CPZ':[0.0,0.34], 'CP2':[0.18,0.34], 'CP4':[0.42,0.34], 'CP6':[0.66,0.34], 'TP8':[0.92,0.34],
  'P7':[-0.82,0.62], 'P5':[-0.62,0.62], 'P3':[-0.42,0.62], 'P1':[-0.18,0.62], 'PZ':[0.0,0.62], 'P2':[0.18,0.62], 'P4':[0.42,0.62], 'P6':[0.62,0.62], 'P8':[0.82,0.62],
  'PO7':[-0.55,0.80], 'PO3':[-0.25,0.80], 'POZ':[0.0,0.84], 'PO4':[0.25,0.80], 'PO8':[0.55,0.80],
  'O1':[-0.35,0.94], 'OZ':[0.0,0.98], 'O2':[0.35,0.94],
};

function qeegNormChanName(ch){
  ch = String(ch||'').trim();
  if(!ch) return '';
  // Common prefixes
  ch = ch.replace(/^EEG\s+/i,'');
  ch = ch.replace(/^CH\s+/i,'');
  // Split on common separators (bipolar labels etc.) and take the first token.
  const parts = ch.split(/[-:,\s]+/).filter(x=>x);
  if(parts.length) ch = parts[0];
  ch = ch.toUpperCase();
  // Legacy temporal labels
  if(ch==='T3') ch='T7';
  if(ch==='T4') ch='T8';
  if(ch==='T5') ch='P7';
  if(ch==='T6') ch='P8';
  return ch;
}

function qeegChanPos(ch){
  const key = qeegNormChanName(ch);
  if(!key) return null;
  const p = QEEG_POS[key];
  if(!p) return null;
  return {key:key, x:p[0], y:p[1]};
}

function qeegBuildTopomapData(rows, channelIdx, valueIdx, filter){
  const q = norm(filter||'');
  const map = {};
  const unmapped = [];
  let total = 0;

  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    if(channelIdx >= r.length || valueIdx >= r.length) continue;
    const chRaw = String(r[channelIdx]||'').trim();
    if(!chRaw) continue;
    if(q && !norm(chRaw).includes(q)) continue;
    const v = Number(String(r[valueIdx]||'').trim());
    if(!isFinite(v)) continue;
    ++total;
    const pos = qeegChanPos(chRaw);
    if(!pos){
      unmapped.push(chRaw);
      continue;
    }
    if(!map[pos.key]){
      map[pos.key] = {key:pos.key, x:pos.x, y:pos.y, sum:v, n:1};
    }else{
      map[pos.key].sum += v;
      map[pos.key].n += 1;
    }
  }

  const pts = [];
  let minV = Infinity;
  let maxV = -Infinity;
  for(const k in map){
    const p = map[k];
    const vv = p.n ? (p.sum / p.n) : NaN;
    if(!isFinite(vv)) continue;
    pts.push({key:p.key, x:p.x, y:p.y, v:vv});
    if(vv < minV) minV = vv;
    if(vv > maxV) maxV = vv;
  }
  pts.sort((a,b)=>String(a.key).localeCompare(String(b.key)));
  if(!isFinite(minV) || !isFinite(maxV)){
    minV = 0; maxV = 0;
  }
  return {pts:pts, min:minV, max:maxV, unmapped:unmapped, total:total};
}

function qeegParseCssColorToRgb(s){
  s = String(s||'').trim();
  if(!s) return null;
  // #rrggbb
  let m = s.match(/^#([0-9a-fA-F]{6})$/);
  if(m){
    const hex = m[1];
    const r = parseInt(hex.slice(0,2),16);
    const g = parseInt(hex.slice(2,4),16);
    const b = parseInt(hex.slice(4,6),16);
    return [r,g,b];
  }
  // rgb(r,g,b)
  m = s.match(/^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$/i);
  if(m){
    return [Number(m[1]), Number(m[2]), Number(m[3])];
  }
  return null;
}

function qeegLerp(a,b,t){return a + (b-a)*t;}
function qeegLerpRgb(c0,c1,t){
  return [
    Math.round(qeegLerp(c0[0],c1[0],t)),
    Math.round(qeegLerp(c0[1],c1[1],t)),
    Math.round(qeegLerp(c0[2],c1[2],t)),
  ];
}

function qeegDrawTopomap(canvas, topo, opts){
  if(!canvas || !topo || !Array.isArray(topo.pts) || topo.pts.length === 0) return;
  opts = opts || {};
  const ctx = canvas.getContext('2d');
  if(!ctx) return;

  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0,0,W,H);

  const pad = 18;
  const size = Math.min(W, H) - pad*2;
  const cx = Math.floor(W/2);
  const cy = Math.floor(H/2);
  const R = Math.floor(size/2);
  if(R <= 10) return;

  const bg = [18,26,45];
  const accent = qeegParseCssColorToRgb(qeegCssVar('--accent')) || [106,166,255];
  const blue = [90,160,255];
  const red = [255,90,120];

  let vmin = topo.min;
  let vmax = topo.max;
  let diverge = !!opts.diverge;

  if(diverge){
    const m = Math.max(Math.abs(vmin), Math.abs(vmax));
    vmin = -m;
    vmax = m;
  }
  if(!isFinite(vmin) || !isFinite(vmax) || Math.abs(vmax - vmin) < 1e-12){
    vmax = vmin + 1e-12;
  }

  // Offscreen buffer for faster per-pixel work.
  const N = Math.max(120, Math.min(220, Math.floor(R*1.15)));
  const buf = document.createElement('canvas');
  buf.width = N;
  buf.height = N;
  const bctx = buf.getContext('2d');
  if(!bctx) return;
  const img = bctx.createImageData(N, N);
  const data = img.data;

  const eps = 1e-4;
  const pwr = 2.0;

  for(let yi=0; yi<N; ++yi){
    const y = (yi/(N-1))*2 - 1;
    for(let xi=0; xi<N; ++xi){
      const x = (xi/(N-1))*2 - 1;
      const rr = x*x + y*y;
      const idx = (yi*N + xi)*4;
      if(rr > 1.0){
        data[idx+0] = 0;
        data[idx+1] = 0;
        data[idx+2] = 0;
        data[idx+3] = 0;
        continue;
      }

      let wsum = 0.0;
      let vsum = 0.0;
      for(const p of topo.pts){
        const dx = x - p.x;
        const dy = y - p.y;
        const d2 = dx*dx + dy*dy;
        const w = 1.0 / Math.pow(d2 + eps, pwr/2.0);
        wsum += w;
        vsum += w * p.v;
      }
      let v = (wsum > 0) ? (vsum/wsum) : 0.0;
      if(!isFinite(v)) v = 0.0;
      // clamp
      if(v < vmin) v = vmin;
      if(v > vmax) v = vmax;

      let rgb;
      if(diverge){
        const m = Math.max(Math.abs(vmin), Math.abs(vmax));
        const t = clamp01(Math.abs(v) / (m || 1e-12));
        rgb = (v >= 0) ? qeegLerpRgb(bg, red, t) : qeegLerpRgb(bg, blue, t);
      }else{
        const t = clamp01((v - vmin) / (vmax - vmin));
        rgb = qeegLerpRgb(bg, accent, t);
      }

      data[idx+0] = rgb[0];
      data[idx+1] = rgb[1];
      data[idx+2] = rgb[2];
      data[idx+3] = 255;
    }
  }

  bctx.putImageData(img, 0, 0);

  // Draw into main canvas.
  ctx.save();
  ctx.translate(cx - R, cy - R);
  ctx.drawImage(buf, 0, 0, 2*R, 2*R);
  ctx.restore();

  // Head outline.
  ctx.strokeStyle = 'rgba(255,255,255,0.25)';
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(cx, cy, R, 0, Math.PI*2);
  ctx.stroke();

  // Electrode markers.
  const hl = qeegNormChanName(opts.highlight||'');
  for(const p of topo.pts){
    const px = cx + p.x * R;
    const py = cy + p.y * R;
    const isHl = hl && (qeegNormChanName(p.key) === hl);
    ctx.fillStyle = isHl ? 'rgba(255,255,255,0.95)' : 'rgba(255,255,255,0.70)';
    ctx.beginPath();
    ctx.arc(px, py, isHl ? 4.2 : 3.0, 0, Math.PI*2);
    ctx.fill();
  }
}

function qeegTopomapHoverAt(canvas, topo, ev){
  if(!canvas || !topo || !Array.isArray(topo.pts) || !topo.pts.length) return null;
  const rect = canvas.getBoundingClientRect();
  const x = (ev.clientX - rect.left) / rect.width;
  const y = (ev.clientY - rect.top) / rect.height;
  const W = canvas.width;
  const H = canvas.height;
  const pad = 18;
  const size = Math.min(W, H) - pad*2;
  const cx = W/2;
  const cy = H/2;
  const R = size/2;
  const nx = ((x*W) - cx) / R;
  const ny = ((y*H) - cy) / R;

  // Find nearest electrode within a reasonable radius.
  let best = null;
  let bestD = 1e9;
  for(const p of topo.pts){
    const dx = nx - p.x;
    const dy = ny - p.y;
    const d2 = dx*dx + dy*dy;
    if(d2 < bestD){ bestD = d2; best = p; }
  }
  if(!best) return null;
  const d = Math.sqrt(bestD);
  if(d > 0.16) return null; // click/hover radius
  return best;
}

function qeegCollectChannels(rows, channelIdx, filter){
  const out = [];
  const q = norm(filter||'');
  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    const ch = (channelIdx < r.length) ? String(r[channelIdx]||'').trim() : '';
    if(!ch) continue;
    if(q && !norm(ch).includes(q)) continue;
    out.push(ch);
  }
  return out;
}

function qeegFindRow(rows, channelIdx, channelName){
  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    const ch = (channelIdx < r.length) ? String(r[channelIdx]||'').trim() : '';
    if(ch === channelName) return r;
  }
  return null;
}

function qeegMean(rows, colIdx){
  let sum = 0.0;
  let n = 0;
  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    if(colIdx >= r.length) continue;
    const v = Number(String(r[colIdx]||'').trim());
    if(!isFinite(v)) continue;
    sum += v;
    ++n;
  }
  return n ? (sum / n) : NaN;
}

function qeegGetBandValues(rows, q, selChannel, metric){
  const out = [];
  for(const b of (q.bands||[])){
    let idx = b.idx;
    if(metric === 'z'){
      if(b.zIdx >= 0) idx = b.zIdx;
      else { out.push(NaN); continue; }
    }
    let v = NaN;
    if(selChannel === '__mean__'){
      v = qeegMean(rows, idx);
    } else {
      const rr = qeegFindRow(rows, q.channelIdx, selChannel);
      if(rr && idx < rr.length){
        const x = Number(String(rr[idx]||'').trim());
        v = isFinite(x) ? x : NaN;
      }
    }
    out.push(v);
  }
  return out;
}

function qeegGetBandValueMap(rows, q, selChannel){
  const m = {};
  const vals = qeegGetBandValues(rows, q, selChannel, 'power');
  for(let i=0;i<(q.bands||[]).length;++i){
    const bn = String(q.bands[i].name||'');
    m[bn] = vals[i];
  }
  return m;
}

function qeegRatiosFromMap(m){
  function safeRatio(a,b){
    const x = m[a];
    const y = m[b];
    if(!isFinite(x) || !isFinite(y) || Math.abs(y) < 1e-20) return NaN;
    return x / y;
  }
  const out = [];
  if(('theta' in m) && ('beta' in m)) out.push({name:'theta/beta', v:safeRatio('theta','beta')});
  if(('alpha' in m) && ('theta' in m)) out.push({name:'alpha/theta', v:safeRatio('alpha','theta')});
  if(('beta' in m) && ('alpha' in m)) out.push({name:'beta/alpha', v:safeRatio('beta','alpha')});
  if(('delta' in m) && ('alpha' in m)) out.push({name:'delta/alpha', v:safeRatio('delta','alpha')});
  return out;
}

function qeegCssVar(name){
  try{
    const v = getComputedStyle(document.documentElement).getPropertyValue(name);
    return String(v||'').trim();
  }catch(e){
    return '';
  }
}

function qeegDrawBandBars(canvas, labels, values, title){
  if(!canvas) return;
  const ctx = canvas.getContext('2d');
  if(!ctx) return;
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0,0,W,H);

  const padL = 52;
  const padR = 18;
  const padT = 26;
  const padB = 42;
  const iw = W - padL - padR;
  const ih = H - padT - padB;
  if(iw <= 10 || ih <= 10) return;

  let minV = Infinity;
  let maxV = -Infinity;
  for(const v of values){
    if(!isFinite(v)) continue;
    if(v < minV) minV = v;
    if(v > maxV) maxV = v;
  }
  if(!isFinite(minV) || !isFinite(maxV)){
    ctx.fillStyle = 'rgba(255,255,255,0.65)';
    ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
    ctx.fillText('No numeric values to plot.', padL, padT + 10);
    return;
  }
  // Include 0 baseline.
  minV = Math.min(minV, 0);
  maxV = Math.max(maxV, 0);
  if(maxV - minV < 1e-12) maxV = minV + 1e-12;

  const yFor = (v)=> padT + (maxV - v) * ih / (maxV - minV);
  const y0 = yFor(0);

  // Title
  ctx.fillStyle = 'rgba(255,255,255,0.75)';
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'left';
  ctx.fillText(String(title||''), 10, 16);

  // Baseline
  ctx.strokeStyle = 'rgba(255,255,255,0.15)';
  ctx.beginPath();
  ctx.moveTo(padL, y0);
  ctx.lineTo(padL + iw, y0);
  ctx.stroke();

  const accent = qeegCssVar('--accent') || 'rgba(106,166,255,0.9)';
  const n = labels.length;
  const bw = iw / Math.max(1, n);
  for(let i=0;i<n;++i){
    const v = values[i];
    const x = padL + i*bw + bw*0.15;
    const w = bw*0.70;
    const y = isFinite(v) ? yFor(v) : y0;
    const top = Math.min(y, y0);
    const h = Math.abs(y - y0);

    ctx.fillStyle = accent;
    ctx.globalAlpha = isFinite(v) ? 0.85 : 0.15;
    ctx.fillRect(x, top, w, Math.max(0.5, h));
    ctx.globalAlpha = 1.0;

    // Label
    ctx.fillStyle = 'rgba(255,255,255,0.75)';
    ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
    ctx.textAlign = 'center';
    ctx.fillText(String(labels[i]||''), x + w/2, H - 18);
  }

  // Y min/max hints.
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'right';
  ctx.fillText(String(maxV.toFixed(4)), padL - 6, padT + 10);
  ctx.fillText(String(minV.toFixed(4)), padL - 6, padT + ih);
}

function qeegDrawChannelBars(canvas, labels, values, title){
  if(!canvas) return;
  const ctx = canvas.getContext('2d');
  if(!ctx) return;
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0,0,W,H);

  const padL = 140;
  const padR = 18;
  const padT = 26;
  const padB = 18;
  const iw = W - padL - padR;
  const ih = H - padT - padB;
  if(iw <= 10 || ih <= 10) return;

  let minV = Infinity;
  let maxV = -Infinity;
  for(const v of values){
    if(!isFinite(v)) continue;
    if(v < minV) minV = v;
    if(v > maxV) maxV = v;
  }
  if(!isFinite(minV) || !isFinite(maxV)){
    ctx.fillStyle = 'rgba(255,255,255,0.65)';
    ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
    ctx.fillText('No numeric values to plot.', padL, padT + 10);
    return;
  }
  // Include 0 baseline.
  minV = Math.min(minV, 0);
  maxV = Math.max(maxV, 0);
  if(maxV - minV < 1e-12) maxV = minV + 1e-12;

  const xFor = (v)=> padL + (v - minV) * iw / (maxV - minV);
  const x0 = xFor(0);

  // Title
  ctx.fillStyle = 'rgba(255,255,255,0.75)';
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'left';
  ctx.fillText(String(title||''), 10, 16);

  // Baseline
  ctx.strokeStyle = 'rgba(255,255,255,0.15)';
  ctx.beginPath();
  ctx.moveTo(x0, padT);
  ctx.lineTo(x0, padT + ih);
  ctx.stroke();

  const accent = qeegCssVar('--accent') || 'rgba(106,166,255,0.9)';
  const n = labels.length;
  const rowH = ih / Math.max(1, n);

  for(let i=0;i<n;++i){
    const y = padT + i*rowH;
    const v = values[i];
    const x = isFinite(v) ? xFor(v) : x0;
    const left = Math.min(x, x0);
    const w = Math.abs(x - x0);

    // Bar
    ctx.fillStyle = accent;
    ctx.globalAlpha = isFinite(v) ? 0.75 : 0.15;
    ctx.fillRect(left, y + rowH*0.2, Math.max(0.5, w), Math.max(1.0, rowH*0.6));
    ctx.globalAlpha = 1.0;

    // Label
    ctx.fillStyle = 'rgba(255,255,255,0.75)';
    ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
    ctx.textAlign = 'right';
    const lab = String(labels[i]||'');
    ctx.fillText(lab.length>18 ? (lab.slice(0,18)+'…') : lab, padL - 6, y + rowH*0.72);
  }

  // X min/max hints.
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace';
  ctx.textAlign = 'left';
  ctx.fillText(String(minV.toFixed(4)), padL, H - 6);
  ctx.textAlign = 'right';
  ctx.fillText(String(maxV.toFixed(4)), padL + iw, H - 6);
}

function renderCsvPreviewQeegOnly(){
  if(!csvPreviewCtx || !csvPreviewCtx.qeeg) return;
  const wrap=document.getElementById('csvViewQeeg');
  if(!wrap) return;

  const rows = Array.isArray(csvPreviewCtx.rows) ? csvPreviewCtx.rows : [];
  const q = csvPreviewCtx.qeeg;

  // If the preview is truncated, keep the warning visible.
  const truncNote = csvPreviewCtx.truncated ? '<span class="pill">truncated preview</span>' : '';

  if(q.kind === 'bandpowers'){
    if(!q.selChannel) q.selChannel = '__mean__';
    if(!q.metric) q.metric = 'power';

    let channels = qeegCollectChannels(rows, q.channelIdx, csvPreviewCtx.filter);
    if(q.selChannel && q.selChannel !== '__mean__'){
      let found = false;
      for(const ch of channels){ if(ch === q.selChannel){ found = true; break; } }
      if(!found) channels = [q.selChannel].concat(channels);
    }

    let chOpts = '<option value="__mean__">(mean across channels)</option>';
    for(const ch of channels){
      chOpts += '<option value="'+esc(ch)+'">'+esc(ch)+'</option>';
    }

    let metricOpts = '<option value="power">Power</option>';
    if(q.haveZ) metricOpts += '<option value="z">Z-score</option>';

    wrap.innerHTML =
      '<div style="padding:10px">'+
        '<div class="qeeg-controls">'+
          '<span class="small">Detected: <b>bandpowers</b> '+truncNote+'</span>'+
          '<span class="small">'+esc(qeegBandNote())+'</span>'+
        '</div>'+
        '<div class="qeeg-controls">'+
          '<label class="small">Channel</label>'+
          '<select class="input" id="qeegBandChSel" style="max-width:260px">'+chOpts+'</select>'+
          '<label class="small">Value</label>'+
          '<select class="input" id="qeegBandMetricSel" style="max-width:180px">'+metricOpts+'</select>'+
        '</div>'+
        '<div class="qeeg-grid two">'+
          '<div>'+
            '<canvas id="qeegBandCanvas" class="qeeg-chart" width="860" height="260"></canvas>'+
            '<div id="qeegBandRatios" class="qeeg-kv"></div>'+
            '<div class="small" style="margin-top:8px">Tip: use <b>Power</b> for ratios. Z-scores require <code>*_z</code> columns.</div>'+
          '</div>'+
          '<div>'+ 
            '<div class="qeeg-controls">'+
              '<label class="small">Topomap band</label>'+
              '<select class="input" id="qeegTopoBandSel" style="max-width:180px"></select>'+
              '<label class="small" style="margin-left:8px">|z|≥</label>'+
              '<input class="input" id="qeegTopoThrInput" type="number" min="0" max="10" step="0.25" style="max-width:92px" value="2">'+
              '<span class="small" id="qeegTopoHover"></span>'+
            '</div>'+
            '<canvas id="qeegTopoCanvas" class="qeeg-chart qeeg-topo" width="420" height="420"></canvas>'+
            '<div id="qeegTopoMeta" class="qeeg-topo-meta"></div>'+
            '<div id="qeegTopoOutliers" class="qeeg-kv"></div>'+
            '<div class="small" style="margin-top:8px">Click an electrode to set the channel picker.</div>'+
          '</div>'+
        '</div>'+
      '</div>';

    const sel=document.getElementById('qeegBandChSel');
    const ms=document.getElementById('qeegBandMetricSel');
    if(sel){
      sel.value = q.selChannel;
      if(sel.value !== q.selChannel) q.selChannel = sel.value || '__mean__';
      sel.onchange = ()=>{ q.selChannel = sel.value || '__mean__'; renderCsvPreviewQeegOnly(); };
    }
    if(ms){
      ms.value = q.metric;
      if(ms.value !== q.metric) q.metric = ms.value || 'power';
      ms.onchange = ()=>{ q.metric = ms.value || 'power'; renderCsvPreviewQeegOnly(); };
    }

    const labels = (q.bands||[]).map(b=>String(b.name||''));
    const vals = qeegGetBandValues(rows, q, q.selChannel, q.metric);
    const title = (q.metric==='z' ? 'Bandpower Z' : 'Bandpower') + ' · ' + (q.selChannel==='__mean__' ? 'mean' : q.selChannel);

    const canvas=document.getElementById('qeegBandCanvas');
    qeegDrawBandBars(canvas, labels, vals, title);

    const ratioWrap=document.getElementById('qeegBandRatios');
    if(ratioWrap){
      if(q.metric !== 'power'){
        ratioWrap.innerHTML = '<span class="small">(ratios hidden in Z-score view)</span>';
      } else {
        const m = qeegGetBandValueMap(rows, q, q.selChannel);
        const ratios = qeegRatiosFromMap(m);
        if(!ratios.length){
          ratioWrap.innerHTML = '<span class="small">No common ratios available for these bands.</span>';
        } else {
          let html='';
          for(const r of ratios){
            const vv = (isFinite(r.v) ? String(r.v.toFixed(6)) : 'NaN');
            html += '<span class="pill">'+esc(r.name)+': <code>'+esc(vv)+'</code></span>';
          }
          ratioWrap.innerHTML = html;
        }
      }
    }

    // ---- Topomap (band selected) ----
    if(!q.topoBand){
      // Prefer alpha if available.
      let tb = '';
      for(const b of (q.bands||[])){
        if(norm(b.name)==='alpha'){ tb = b.name; break; }
      }
      q.topoBand = tb || ((q.bands && q.bands.length) ? q.bands[0].name : '');
    }
    if(q.topoThr===undefined || q.topoThr===null){
      q.topoThr = 2.0;
    }

    const tbSel = document.getElementById('qeegTopoBandSel');
    if(tbSel){
      let opts='';
      for(const b of (q.bands||[])){
        opts += '<option value="'+esc(String(b.name||''))+'">'+esc(String(b.name||''))+'</option>';
      }
      tbSel.innerHTML = opts;
      tbSel.value = q.topoBand;
      if(tbSel.value !== q.topoBand) q.topoBand = tbSel.value;
      tbSel.onchange = ()=>{ q.topoBand = tbSel.value || q.topoBand; renderCsvPreviewQeegOnly(); };
    }

    const thr = document.getElementById('qeegTopoThrInput');
    if(thr){
      thr.value = String(q.topoThr);
      thr.disabled = (q.metric !== 'z');
      thr.onchange = ()=>{
        const v = Number(thr.value||'2');
        if(isFinite(v)) q.topoThr = Math.max(0, Math.min(10, v));
        renderCsvPreviewQeegOnly();
      };
    }

    // Find the selected band index (power or z column).
    let band = null;
    for(const b of (q.bands||[])){
      if(String(b.name||'') === String(q.topoBand||'')) { band = b; break; }
    }
    if(!band && q.bands && q.bands.length) band = q.bands[0];

    const topoCanvas = document.getElementById('qeegTopoCanvas');
    const topoMeta = document.getElementById('qeegTopoMeta');
    const topoHover = document.getElementById('qeegTopoHover');
    const outWrap = document.getElementById('qeegTopoOutliers');

    if(topoCanvas && band){
      const idx = (q.metric==='z' && band.zIdx>=0) ? band.zIdx : band.idx;
      const topo = qeegBuildTopomapData(rows, q.channelIdx, idx, csvPreviewCtx.filter);
      const diverge = (q.metric==='z');
      qeegDrawTopomap(topoCanvas, topo, {diverge:diverge, highlight:(q.selChannel && q.selChannel!=='__mean__') ? q.selChannel : ''});

      if(topoMeta){
        const m1 = '<span class="small">Mapped: <b>'+String(topo.pts.length)+'</b> / '+String(topo.total)+'</span>';
        const m2 = '<span class="small">Range: <b>'+String(topo.min.toFixed(4))+'</b> … <b>'+String(topo.max.toFixed(4))+'</b></span>';
        topoMeta.innerHTML = m1 + m2;
      }

      if(outWrap){
        if(q.metric !== 'z'){
          outWrap.innerHTML = '<span class="small">(Outliers shown only in Z-score view.)</span>';
        }else{
          const thrV = Number(q.topoThr||2.0);
          const outs = [];
          for(const p of topo.pts){
            if(isFinite(p.v) && Math.abs(p.v) >= thrV){ outs.push(p); }
          }
          outs.sort((a,b)=>Math.abs(b.v)-Math.abs(a.v));
          if(!outs.length){
            outWrap.innerHTML = '<span class="small">No channels exceed |z| ≥ '+esc(String(thrV))+' for '+esc(String(q.topoBand||''))+'.</span>';
          }else{
            let html='';
            const lim = Math.min(18, outs.length);
            for(let i=0;i<lim;++i){
              const p = outs[i];
              html += '<span class="pill">'+esc(p.key)+': <code>'+esc(String(p.v.toFixed(3)))+'</code></span>';
            }
            if(outs.length > lim) html += '<span class="small">… +'+String(outs.length-lim)+' more</span>';
            outWrap.innerHTML = html;
          }
        }
      }

      if(topoHover){
        topoCanvas.onmousemove = (ev)=>{
          const hit = qeegTopomapHoverAt(topoCanvas, topo, ev);
          if(hit){ topoHover.textContent = hit.key + ' = ' + String(hit.v.toFixed(4)); }
          else { topoHover.textContent = ''; }
        };
        topoCanvas.onmouseleave = ()=>{ topoHover.textContent=''; };
        topoCanvas.onclick = (ev)=>{
          const hit = qeegTopomapHoverAt(topoCanvas, topo, ev);
          if(hit){
            q.selChannel = hit.key;
            renderCsvPreviewQeegOnly();
          }
        };
      }
    }

    return;
  }

  // Generic per-channel metrics
  if(!q.metricName) q.metricName = q.prefMetric || (q.metrics && q.metrics.length ? q.metrics[0].name : '');
  if(!q.sort) q.sort = 'desc';
  if(!q.limit) q.limit = 48;
  if(!q.view) q.view = 'both';

  // Build metric select
  let mOpts = '';
  for(const m of (q.metrics||[])){
    const nm = String(m.name||'');
    mOpts += '<option value="'+esc(nm)+'">'+esc(nm)+'</option>';
  }

  const sortLabel = (q.sort==='asc') ? 'asc' : (q.sort==='none' ? 'none' : 'desc');

  wrap.innerHTML =
    '<div style="padding:10px">'+
      '<div class="qeeg-controls">'+
        '<span class="small">Detected: <b>channel metrics</b> '+truncNote+'</span>'+
        '<span class="small">(Plot a metric across channels; optional scalp topomap.)</span>'+
      '</div>'+
      '<div class="qeeg-controls">'+
        '<label class="small">Metric</label>'+
        '<select class="input" id="qeegMetricSel" style="max-width:320px">'+mOpts+'</select>'+
        '<button class="btn" id="qeegSortBtn" type="button">Sort: '+esc(sortLabel)+'</button>'+
        '<label class="small">Limit</label>'+
        '<input class="input" id="qeegLimitInput" type="number" min="5" max="200" step="1" style="max-width:110px" value="'+esc(String(q.limit||48))+'">'+
        '<span style="flex:1"></span>'+
        '<button class="btn" id="qeegViewBars" type="button">Bars</button>'+
        '<button class="btn" id="qeegViewTopo" type="button">Topomap</button>'+
        '<button class="btn" id="qeegViewBoth" type="button">Both</button>'+
      '</div>'+
      '<div id="qeegMetricsWrap" class="qeeg-grid two">'+
        '<div id="qeegMetricBarsWrap">'+
          '<canvas id="qeegMetricCanvas" class="qeeg-chart" width="860" height="520"></canvas>'+
          '<div id="qeegMetricMeta" class="small" style="margin-top:8px"></div>'+
        '</div>'+
        '<div id="qeegMetricTopoWrap">'+
          '<div class="qeeg-controls">'+
            '<span class="small" id="qeegMetricTopoHover"></span>'+
          '</div>'+
          '<canvas id="qeegMetricTopoCanvas" class="qeeg-chart qeeg-topo" width="420" height="420"></canvas>'+
          '<div id="qeegMetricTopoMeta" class="qeeg-topo-meta"></div>'+
          '<div class="small" style="margin-top:8px">Tip: click an electrode to filter the table + plots by that channel.</div>'+
        '</div>'+
      '</div>'+
    '</div>';

  const ms=document.getElementById('qeegMetricSel');
  const sb=document.getElementById('qeegSortBtn');
  const li=document.getElementById('qeegLimitInput');
  if(ms){
    ms.value = q.metricName;
    ms.onchange = ()=>{ q.metricName = ms.value || q.metricName; renderCsvPreviewQeegOnly(); };
  }
  if(sb){
    sb.onclick = ()=>{
      q.sort = (q.sort==='desc') ? 'asc' : (q.sort==='asc' ? 'none' : 'desc');
      renderCsvPreviewQeegOnly();
    };
  }
  if(li){
    li.onchange = ()=>{
      const v = Number(li.value||48);
      if(isFinite(v)) q.limit = Math.max(5, Math.min(200, Math.floor(v)));
      renderCsvPreviewQeegOnly();
    };
  }

  // Resolve metric index.
  let metricIdx = -1;
  for(const m of (q.metrics||[])){
    if(String(m.name||'') === String(q.metricName||'')) { metricIdx = m.idx; break; }
  }
  if(metricIdx < 0 && q.metrics && q.metrics.length){
    metricIdx = q.metrics[0].idx;
    q.metricName = q.metrics[0].name;
  }

  // Collect data
  const channels = [];
  const values = [];
  const fq = norm(csvPreviewCtx.filter||'');
  for(let i=1;i<rows.length;++i){
    const r = rows[i] || [];
    const ch = (q.channelIdx < r.length) ? String(r[q.channelIdx]||'').trim() : '';
    if(!ch) continue;
    if(fq && !norm(ch).includes(fq)) continue;
    if(metricIdx < 0 || metricIdx >= r.length) continue;
    const v = Number(String(r[metricIdx]||'').trim());
    if(!isFinite(v)) continue;
    channels.push(ch);
    values.push(v);
  }

  // Sort (optional)
  if(q.sort === 'desc' || q.sort === 'asc'){
    const idxs = values.map((v,i)=>[v,i]);
    idxs.sort((a,b)=> (q.sort==='asc' ? (a[0]-b[0]) : (b[0]-a[0])));
    const ch2 = [];
    const v2 = [];
    for(const [v,i] of idxs){ ch2.push(channels[i]); v2.push(values[i]); }
    channels.length = 0; values.length = 0;
    for(const x of ch2) channels.push(x);
    for(const x of v2) values.push(x);
  }

  const lim = Math.max(5, Math.min(200, Math.floor(Number(q.limit||48))));
  const trunc = channels.length > lim;
  const plotCh = channels.slice(0, lim);
  const plotV = values.slice(0, lim);

  // Resize canvas height for readability.
  const canvas=document.getElementById('qeegMetricCanvas');
  if(canvas){
    const h = Math.min(920, Math.max(280, 26*plotCh.length + 70));
    canvas.height = h;
  }

  const title = String(q.metricName||'metric') + ' · ' + String(plotCh.length) + ' channels';
  qeegDrawChannelBars(canvas, plotCh, plotV, title);

  const meta=document.getElementById('qeegMetricMeta');
  if(meta){
    meta.textContent = 'Plotted ' + String(plotCh.length) + ' channel rows' + (trunc ? (' (limited to ' + String(lim) + ')') : '') + (csvPreviewCtx.filter ? (' · filter: "' + String(csvPreviewCtx.filter) + '"') : '');
  }

  // ---- Channel-metric topomap ----
  if(!q.view) q.view = 'both'; // 'bars' | 'topo' | 'both'
  const vb=document.getElementById('qeegViewBars');
  const vt=document.getElementById('qeegViewTopo');
  const v2=document.getElementById('qeegViewBoth');
  const barsWrap=document.getElementById('qeegMetricBarsWrap');
  const topoWrap=document.getElementById('qeegMetricTopoWrap');
  if(vb && vt && v2 && barsWrap && topoWrap){
    vb.classList.toggle('active', q.view==='bars');
    vt.classList.toggle('active', q.view==='topo');
    v2.classList.toggle('active', q.view==='both');
    barsWrap.classList.toggle('hidden', q.view==='topo');
    topoWrap.classList.toggle('hidden', q.view==='bars');
    vb.onclick = ()=>{ q.view='bars'; renderCsvPreviewQeegOnly(); };
    vt.onclick = ()=>{ q.view='topo'; renderCsvPreviewQeegOnly(); };
    v2.onclick = ()=>{ q.view='both'; renderCsvPreviewQeegOnly(); };
  }

  const topoCanvas=document.getElementById('qeegMetricTopoCanvas');
  const topoMeta=document.getElementById('qeegMetricTopoMeta');
  const topoHover=document.getElementById('qeegMetricTopoHover');

  if(topoCanvas && metricIdx>=0){
    const topo = qeegBuildTopomapData(rows, q.channelIdx, metricIdx, csvPreviewCtx.filter);
    const diverge = (norm(String(q.metricName||'')).includes('z') || (topo.min < 0 && topo.max > 0));
    qeegDrawTopomap(topoCanvas, topo, {diverge:diverge});
    if(topoMeta){
      topoMeta.innerHTML = '<span class="small">Mapped: <b>'+String(topo.pts.length)+'</b> / '+String(topo.total)+'</span>'+
        '<span class="small">Range: <b>'+String(topo.min.toFixed(4))+'</b> … <b>'+String(topo.max.toFixed(4))+'</b></span>';
    }
    if(topoHover){
      topoCanvas.onmousemove = (ev)=>{
        const hit = qeegTopomapHoverAt(topoCanvas, topo, ev);
        if(hit){ topoHover.textContent = hit.key + ' = ' + String(hit.v.toFixed(4)); }
        else { topoHover.textContent = ''; }
      };
      topoCanvas.onmouseleave = ()=>{ topoHover.textContent=''; };
      topoCanvas.onclick = (ev)=>{
        const hit = qeegTopomapHoverAt(topoCanvas, topo, ev);
        if(hit){
          // Set the CSV filter to this channel.
          csvPreviewCtx.filter = hit.key;
          const fi = document.getElementById('csvFilterInput');
          if(fi) fi.value = csvPreviewCtx.filter;
          renderCsvPreviewTableOnly();
          renderCsvPreviewQeegOnly();
        }
      };
    }
  }
}
async function openPreview(url, label){
  const back=document.getElementById('previewBackdrop');
  const title=document.getElementById('previewTitle');
  const body=document.getElementById('previewBody');
  const link=document.getElementById('previewOpenLink');
  if(!back || !title || !body || !link) return;
  back.classList.remove('hidden');
  title.textContent = label ? ('Preview: '+label) : 'Preview';
  link.href = url || '#';
  body.innerHTML = '<span class="small">Loading…</span>';

  const ext = pathExt(label||url);
  if(isImageExt(ext)){
    body.innerHTML = '<div style="text-align:center"><img src="'+esc(url)+'" style="max-width:100%;height:auto;border-radius:12px;border:1px solid var(--border);background:#000"></div>';
    return;
  }
  if(!isTextExt(ext)){
    body.innerHTML = '<span class="small">No preview for this file type. Use “Open”.</span>';
    return;
  }
  try{
    const r=await fetch(url,{cache:'no-store'});
    let t=await r.text();
    const max=200000;
    const byteTrunc = (t.length > max);
    const clip = byteTrunc ? t.slice(0, max) : t;

    // Special-case CSV/TSV as an interactive table (and show a heatmap when the
    // file looks like a numeric matrix).
    if(ext==='csv' || ext==='tsv'){
      const delim = (ext==='tsv') ? '\t' : ',';
      const parsed = parseDelimitedPreview(clip, delim, 1200, 120);

      let view = 'table';
      try{
        const vv = localStorage.getItem('qeeg_csv_view') || '';
        if(vv==='heat' || vv==='raw' || vv==='table' || vv==='qeeg' || vv==='series') view = vv;
      }catch(e){}

      csvPreviewCtx = {
        ext: ext,
        url: url,
        label: label || url,
        raw: byteTrunc ? (clip + '\n… (truncated)') : clip,
        rows: parsed.rows,
        cols: parsed.cols,
        truncated: !!(parsed.truncated || byteTrunc),
        colTrunc: !!parsed.colTrunc,
        maxRows: 1200,
        maxCols: 120,
        filter: '',
        view: view,
        matrix: null,
        qeeg: null,
        series: null,
      };
      csvPreviewCtx.matrix = detectNumericMatrix(csvPreviewCtx.rows);
      csvPreviewCtx.qeeg = detectQeegCsv(csvPreviewCtx.rows, csvPreviewCtx.label);
      csvPreviewCtx.series = detectTimeSeriesCsv(csvPreviewCtx.rows, csvPreviewCtx.label);
      renderCsvPreview();
      return;
    }

    // Reset CSV context for non-tabular previews.
    csvPreviewCtx = null;

    if(ext==='json'){
      try{ t=JSON.stringify(JSON.parse(t), null, 2); }catch(_){}
    }
    if(t.length>max) t = t.slice(0,max) + '\n… (truncated)';
    body.innerHTML = '<pre><code>'+esc(t)+'</code></pre>';
  }catch(e){
    body.innerHTML = '<span class="small">Error loading preview: '+esc(e&&e.message?e.message:String(e))+'</span>';
  }
}

function closePreview(){
  const back=document.getElementById('previewBackdrop');
  if(back) back.classList.add('hidden');
  csvPreviewCtx = null;
}

// ----- Run notes (per-run note.md) -----
function openNotesFromStatus(btn){
  const sid = btn && btn.dataset ? (btn.dataset.statusId||'') : '';
  const st = sid ? document.getElementById(sid) : null;
  const runDir = (st && st.dataset) ? (st.dataset.runDir||'') : '';
  if(!runDir){
    alert('No run dir yet for this job. Run the tool first.');
    return;
  }
  openNotesForRunDir(runDir, btn);
}

function openNotesFromHistory(btn){
  const runDir = btn && btn.dataset ? (btn.dataset.runDir||'') : '';
  if(!runDir){
    alert('Missing run dir.');
    return;
  }
  openNotesForRunDir(runDir, btn);
}

function noteStatus(msg){
  const st=document.getElementById('noteStatus');
  if(st) st.textContent = String(msg||'');
}

function noteSetDirty(d){
  notesDirty = !!d;
  const b=document.getElementById('noteSaveBtn');
  if(b) b.disabled = !notesDirty || !(qeegApiOk && qeegApiToken) || !notesRunDir;
  const st=document.getElementById('noteStatus');
  if(notesDirty){
    noteStatus('Unsaved changes');
  }else{
    if(st && String(st.textContent||'') === 'Unsaved changes'){
      noteStatus('Saved');
    }
  }
}

function setNotesView(view){
  view = (view==='preview') ? 'preview' : 'edit';
  notesView = view;
  try{ localStorage.setItem('qeeg_notes_view', notesView); }catch(e){}
  const t1=document.getElementById('noteTabEdit');
  const t2=document.getElementById('noteTabPreview');
  if(t1) t1.classList.toggle('active', view==='edit');
  if(t2) t2.classList.toggle('active', view==='preview');
  const ta=document.getElementById('noteText');
  const pv=document.getElementById('notePreview');
  if(ta) ta.classList.toggle('hidden', view!=='edit');
  if(pv) pv.classList.toggle('hidden', view!=='preview');
  if(view==='preview') renderNotesPreview();
}

function closeNotes(){
  const back=document.getElementById('noteBackdrop');
  if(back && !back.classList.contains('hidden')){
    if(notesDirty){
      if(!confirm('Discard unsaved note changes?')) return;
    }
    back.classList.add('hidden');
  }
  notesRunDir='';
  notesDirty=false;
  try{ if(notesLastFocus) notesLastFocus.focus(); }catch(e){}
  notesLastFocus=null;
}

function mdSafeHref(u){
  u = String(u||'').trim();
  if(!u) return '';
  const low = u.toLowerCase();
  if(low.startsWith('javascript:') || low.startsWith('data:')) return '';
  if(low.startsWith('http://') || low.startsWith('https://') || low.startsWith('mailto:') || low.startsWith('/')) return u;
  // Allow simple relative paths without a scheme.
  if(low.indexOf(':')===-1 && !low.startsWith('//')) return u;
  return '';
}

function mdInline(escLine){
  let s = String(escLine||'');
  // Inline code
  s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
  // Bold
  s = s.replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
  // Italic (single asterisks)
  s = s.replace(/(^|[^*])\*([^*]+)\*([^*]|$)/g, function(_m, a, b, c){
    return a + '<i>' + b + '</i>' + c;
  });
  // Links
  s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, function(_m, text, url){
    const href = mdSafeHref(url);
    if(!href) return text;
    return '<a href="' + href + '" target="_blank" rel="noopener">' + text + '</a>';
  });
  return s;
}

function mdToHtml(md){
  md = String(md||'');
  // Escape first to keep this XSS-safe, then apply lightweight markdown transforms.
  const lines = esc(md).split('\n');
  let out='';
  let inCode=false;
  let inList=false;
  for(let i=0;i<lines.length;i++){
    const raw = lines[i];
    const t = raw.trim();

    if(t.startsWith('```')){
      if(inList){ out += '</ul>'; inList=false; }
      if(!inCode){ out += '<pre><code>'; inCode=true; }
      else { out += '</code></pre>'; inCode=false; }
      continue;
    }

    if(inCode){
      out += raw + '\n';
      continue;
    }

    const h = raw.match(/^(#{1,4})\s+(.*)$/);
    if(h){
      if(inList){ out += '</ul>'; inList=false; }
      const lvl = h[1].length;
      out += '<h'+lvl+'>' + mdInline(h[2]) + '</h'+lvl+'>';
      continue;
    }

    const li = raw.match(/^\s*[-*]\s+(.*)$/);
    if(li){
      if(!inList){ out += '<ul>'; inList=true; }
      out += '<li>' + mdInline(li[1]) + '</li>';
      continue;
    }

    if(inList){ out += '</ul>'; inList=false; }

    if(t===''){
      out += '<div style="height:6px"></div>';
    }else{
      out += '<p>' + mdInline(raw) + '</p>';
    }
  }
  if(inList) out += '</ul>';
  if(inCode) out += '</code></pre>';
  return out;
}

function renderNotesPreview(){
  const pv=document.getElementById('notePreview');
  const ta=document.getElementById('noteText');
  if(!pv || !ta) return;
  pv.innerHTML = mdToHtml(ta.value||'');
}

async function openNotesForRunDir(runDir, focusEl){
  if(!(qeegApiOk && qeegApiToken)){
    alert('Run notes are available only when served via qeeg_ui_server_cli.');
    return;
  }
  notesLastFocus = focusEl || null;
  notesRunDir = String(runDir||'');
  noteSetDirty(false);

  const back=document.getElementById('noteBackdrop');
  if(back) back.classList.remove('hidden');

  const rd=document.getElementById('noteRunDir');
  if(rd) rd.textContent = notesRunDir;

  const open=document.getElementById('noteOpenLink');
  if(open){
    const href = notesRunDir + (String(notesRunDir).endsWith('/') ? '' : '/') + 'note.md';
    open.href = encodePath(href);
  }

  const ta=document.getElementById('noteText');
  if(ta){ ta.value=''; ta.disabled=true; }
  noteStatus('Loading…');

  // Restore preferred view.
  try{
    const v = localStorage.getItem('qeeg_notes_view') || '';
    if(v==='preview' || v==='edit') notesView = v;
  }catch(e){}
  setNotesView(notesView);

  try{
    const r = await apiFetch('/api/note?run_dir=' + encodeURIComponent(notesRunDir));
    const j = await r.json();
    if(!r.ok) throw new Error(j && j.error ? j.error : 'note failed');
    const txt = (j && j.text!==undefined) ? String(j.text||'') : '';
    if(ta){ ta.value = txt; ta.disabled=false; }
    notesDirty = false;
    if(j && j.exists){
      noteStatus(j.truncated ? 'Loaded (truncated)' : 'Loaded');
    }else{
      noteStatus('New note');
    }
    noteSetDirty(false);
    if(notesView==='preview') renderNotesPreview();
  }catch(e){
    if(ta){ ta.value=''; ta.disabled=false; }
    noteStatus('Error: ' + (e && e.message ? e.message : String(e)));
  }

  if(ta){
    ta.oninput = ()=>{
      noteSetDirty(true);
      if(notesView==='preview') renderNotesPreview();
    };
    try{ ta.focus(); }catch(e){}
  }
}

async function saveNotes(){
  if(!(qeegApiOk && qeegApiToken)) return;
  if(!notesRunDir) return;
  const ta=document.getElementById('noteText');
  const text = ta ? String(ta.value||'') : '';
  noteStatus('Saving…');
  try{
    const r = await apiFetch('/api/note', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({run_dir:notesRunDir, text:text})});
    const j = await r.json();
    if(!r.ok) throw new Error(j && j.error ? j.error : 'save failed');
    noteSetDirty(false);
    noteStatus('Saved');
  }catch(e){
    noteStatus('Error: ' + (e && e.message ? e.message : String(e)));
  }
}


// ---- Batch runner (multi-input queue helper) ----
let batchOpen=false;
let batchTool='';
let batchArgsId='';
let batchInjectSelId='';
let batchDir='';
let batchEntries=[];
let batchSelected=new Set();
let batchVisible=[];
let batchSubmitting=false;
let batchLastFocus=null;

function parentDirOf(p){
  p = String(p||'').replace(/\\/g,'/');
  while(p.endsWith('/')) p = p.slice(0,-1);
  const ix = p.lastIndexOf('/');
  if(ix<0) return '';
  return p.slice(0, ix);
}

function showBatchBackdrop(show){
  const b = document.getElementById('batchBackdrop');
  if(!b) return;
  b.classList.toggle('hidden', !show);
}

function closeBatch(){
  if(!batchOpen) return;
  if(batchSubmitting){
    if(!confirm('Batch submission is in progress. Close anyway?')) return;
  }
  batchOpen=false;
  batchTool='';
  batchArgsId='';
  batchInjectSelId='';
  batchDir='';
  batchEntries=[];
  batchSelected = new Set();
  batchVisible=[];
  batchSubmitting=false;
  const st = document.getElementById('batchStatus');
  if(st) st.textContent='';
  const pr = document.getElementById('batchProgress');
  if(pr) pr.textContent='';
  showBatchBackdrop(false);
  if(batchLastFocus){
    try{ batchLastFocus.focus(); }catch(e){}
  }
  batchLastFocus=null;
}

function openBatchRunner(btn){
  if(!(qeegApiOk && qeegApiToken)){
    alert('Batch runner requires qeeg_ui_server_cli (local).');
    return;
  }
  const tool = btn && btn.dataset ? (btn.dataset.tool||'') : '';
  const argsId = btn && btn.dataset ? (btn.dataset.argsId||'') : '';
  const injectSelId = btn && btn.dataset ? (btn.dataset.injectSelId||'') : '';
  if(!tool || !argsId){
    alert('Missing tool metadata for batch run.');
    return;
  }

  batchLastFocus = btn;
  batchOpen=true;
  batchTool = tool;
  batchArgsId = argsId;
  batchInjectSelId = injectSelId;
  batchSelected = new Set();
  batchVisible = [];
  batchSubmitting = false;

  // Derive directory from the Workspace selection.
  let dir = '';
  if(selectedInputPath){
    if(selectedInputType==='dir') dir = selectedInputPath;
    else dir = parentDirOf(selectedInputPath);
  }
  batchDir = dir;

  const toolEl=document.getElementById('batchTool');
  if(toolEl) toolEl.textContent = batchTool;

  const dirEl=document.getElementById('batchDir');
  if(dirEl) dirEl.textContent = batchDir ? batchDir : '.';

  // Seed args template from the tool card args field.
  const srcArgs = document.getElementById(argsId);
  const args = (srcArgs && srcArgs.value!==undefined) ? String(srcArgs.value||'') : '';
  const argsEl=document.getElementById('batchArgs');
  if(argsEl) argsEl.value = args;

  // Copy inject-flag options from the tool card.
  const srcSel = injectSelId ? document.getElementById(injectSelId) : null;
  const sel = document.getElementById('batchInjectFlag');
  if(sel){
    if(srcSel){
      sel.innerHTML = srcSel.innerHTML;
      sel.value = srcSel.value || '';
      sel.dataset.defaultFile = srcSel.dataset.defaultFile || '--input';
      sel.dataset.defaultDir = srcSel.dataset.defaultDir || '';
    }else{
      sel.innerHTML = '<option value="">Auto (recommended)</option><option value="--input">--input</option>';
      sel.value = '';
      sel.dataset.defaultFile='--input';
      sel.dataset.defaultDir='';
    }
  }

  const flt=document.getElementById('batchFilter');
  if(flt){
    flt.value = '';
    flt.oninput = ()=>renderBatchTable();
  }
  const ext=document.getElementById('batchExt');
  if(ext){
    ext.onchange = ()=>renderBatchTable();
  }

  const status=document.getElementById('batchStatus');
  if(status) status.textContent='Loading…';
  const prog=document.getElementById('batchProgress');
  if(prog) prog.textContent='';

  showBatchBackdrop(true);
  try{ if(flt) flt.focus(); }catch(e){}

  batchRefresh();
}

function getBatchInjectFlag(){
  const sel = document.getElementById('batchInjectFlag');
  if(!sel) return '--input';
  let flag = sel.value || '';
  if(!flag){
    flag = sel.dataset.defaultFile || '--input';
  }
  if(!flag) flag='--input';
  return String(flag);
}

async function batchRefresh(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const status=document.getElementById('batchStatus');
  if(status) status.textContent='Loading…';
  try{
    const r = await apiFetch('/api/list', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({dir:batchDir, show_hidden:!!fsShowHidden, sort:'name', desc:false})});
    const j = await r.json();
    if(!r.ok) throw new Error(j && j.error ? j.error : 'list failed');
    const entries = (j && j.entries) ? j.entries : [];
    batchEntries = entries.filter(e=>e && e.type==='file');

    // Remove selections that no longer exist.
    const keep = new Set();
    for(const e of batchEntries){
      if(e && e.path && batchSelected.has(String(e.path))) keep.add(String(e.path));
    }
    batchSelected = keep;

    renderBatchTable();
    if(status) status.textContent='Ready.';
  }catch(e){
    if(status) status.textContent='Error: '+(e && e.message ? e.message : String(e));
    batchEntries=[];
    batchSelected=new Set();
    renderBatchTable();
  }
}

function batchMatchesFilters(entry){
  const name = (entry && entry.name) ? String(entry.name) : '';
  const path = (entry && entry.path) ? String(entry.path) : name;
  const qEl = document.getElementById('batchFilter');
  const extEl = document.getElementById('batchExt');
  const q = norm(qEl ? qEl.value : '');
  const ext = extEl ? String(extEl.value||'') : '';
  if(ext && !name.toLowerCase().endsWith(ext.toLowerCase())) return false;
  if(q){
    const n = norm(name);
    const p = norm(path);
    // Support a tiny "*.ext" shorthand.
    if(q.startsWith('*.') && q.length>2){
      const ex = q.slice(1); // ".ext"
      if(!name.toLowerCase().endsWith(ex.toLowerCase())) return false;
    }else{
      if(!n.includes(q) && !p.includes(q)) return false;
    }
  }
  return true;
}

function batchToggle(cb){
  const path = cb && cb.dataset ? (cb.dataset.path||'') : '';
  if(!path) return;
  if(cb.checked) batchSelected.add(path);
  else batchSelected.delete(path);
  updateBatchCount();
}

function updateBatchCount(){
  const el = document.getElementById('batchCount');
  if(!el) return;
  const shown = batchVisible ? batchVisible.length : 0;
  const sel = batchSelected ? batchSelected.size : 0;
  el.textContent = 'Showing ' + shown + ' file(s) • Selected ' + sel;
}

function renderBatchTable(){
  const tbl = document.getElementById('batchTable');
  if(!tbl) return;

  const entries = (batchEntries||[]).filter(batchMatchesFilters);
  batchVisible = entries.map(e=>String(e.path||e.name||'')).filter(p=>p);
  updateBatchCount();

  if(entries.length===0){
    tbl.innerHTML = '<thead><tr><th>Files</th></tr></thead><tbody><tr><td><span class="small">No files found.</span></td></tr></tbody>';
    return;
  }

  let html = '<thead><tr>'+
    '<th style="width:42px"><input type="checkbox" id="batchCheckAll" aria-label="Select all" onclick="batchToggleAll(this)"></th>'+
    '<th>Name</th><th>Size</th><th>Modified</th>'+
    '</tr></thead><tbody>';

  for(const e of entries){
    const name = (e && e.name) ? String(e.name) : '';
    const path = (e && e.path) ? String(e.path) : name;
    const size = (e && e.size) ? Number(e.size) : 0;
    const mtime = (e && e.mtime) ? Number(e.mtime) : 0;
    const checked = batchSelected.has(path) ? 'checked' : '';
    html += '<tr>'+
      '<td><input type="checkbox" data-path="'+esc(path)+'" '+checked+' onchange="batchToggle(this)"></td>'+
      '<td><code>'+esc(name)+'</code><div class="small" style="word-break:break-all">'+esc(path)+'</div></td>'+
      '<td>'+esc(humanSize(size))+'</td>'+
      '<td>'+esc(fmtLocalTimeSec(mtime))+'</td>'+
      '</tr>';
  }
  html += '</tbody>';
  tbl.innerHTML = html;

  // Update the "all" checkbox state.
  const allCb = document.getElementById('batchCheckAll');
  if(allCb){
    let all=true;
    for(const e of entries){
      const p = String(e.path||e.name||'');
      if(p && !batchSelected.has(p)){ all=false; break; }
    }
    allCb.checked = all;
  }
}

function batchToggleAll(cb){
  const checked = !!(cb && cb.checked);
  // Select/deselect currently visible entries only (respects filters).
  const entries = (batchEntries||[]).filter(batchMatchesFilters);
  for(const e of entries){
    const p = String(e && e.path ? e.path : (e && e.name ? e.name : ''));
    if(!p) continue;
    if(checked) batchSelected.add(p);
    else batchSelected.delete(p);
  }
  renderBatchTable();
}

function batchSelectAll(){
  const cb = document.getElementById('batchCheckAll');
  if(cb){ cb.checked=true; batchToggleAll(cb); }
}

function batchClearSelection(){
  batchSelected = new Set();
  renderBatchTable();
}

function insertAtCursor(el, text){
  if(!el) return;
  text = String(text||'');
  const start = el.selectionStart;
  const end = el.selectionEnd;
  if(start!==undefined && end!==undefined){
    const v = String(el.value||'');
    el.value = v.slice(0,start) + text + v.slice(end);
    const pos = start + text.length;
    try{ el.setSelectionRange(pos,pos); }catch(e){}
  }else{
    el.value = String(el.value||'') + text;
  }
  try{ el.focus(); }catch(e){}
}

function batchInsertInput(){
  const el = document.getElementById('batchArgs');
  if(!el) return;
  insertAtCursor(el, '{input}');
}

function batchUseInjectFlag(){
  const el = document.getElementById('batchArgs');
  if(!el) return;
  const flag = getBatchInjectFlag();
  el.value = setFlagValue(el.value, flag, '{input}');
  try{ el.focus(); }catch(e){}
}

function fileStem(name){
  name = String(name||'');
  const ix = name.lastIndexOf('.');
  if(ix>0) return name.slice(0, ix);
  return name;
}

function expandBatchTemplate(tpl, entry, index){
  tpl = String(tpl||'');
  const name = (entry && entry.name) ? String(entry.name) : '';
  const path = (entry && entry.path) ? String(entry.path) : name;
  const stem = fileStem(name);
  // Replace non-path placeholders.
  tpl = tpl.replace(/\{name\}/g, name);
  tpl = tpl.replace(/\{stem\}/g, stem);
  tpl = tpl.replace(/\{index\}/g, String(index));
  // Replace {input} with quoted path.
  tpl = tpl.replace(/\{input\}/g, quoteArgIfNeeded(path));
  return tpl;
}

async function runBatch(){
  if(batchSubmitting) return;
  if(!(qeegApiOk && qeegApiToken)) return;
  const tool = batchTool;
  if(!tool) return;

  const argsEl = document.getElementById('batchArgs');
  const tplRaw = argsEl ? String(argsEl.value||'') : '';
  const paths = Array.from(batchSelected || []);
  paths.sort();

  const status = document.getElementById('batchStatus');
  const prog = document.getElementById('batchProgress');
  const runBtn = document.getElementById('batchRunBtn');

  if(paths.length===0){
    alert('Select one or more files first.');
    return;
  }

  const maxBatch = 200;
  if(paths.length > maxBatch){
    if(!confirm('You selected '+paths.length+' files. Submitting a very large batch can overwhelm the server log/history. Continue?')){
      return;
    }
  }

  // Build an entry map for metadata (name/stats) to populate placeholders.
  const meta = new Map();
  for(const e of (batchEntries||[])){
    if(e && e.path) meta.set(String(e.path), e);
  }

  batchSubmitting=true;
  if(runBtn) runBtn.disabled=true;
  if(status) status.textContent='Submitting…';
  if(prog) prog.innerHTML = '';

  let okCount=0;
  let outHtml = '<div class="small">Submitting '+paths.length+' job(s)…</div>';

  const flag = getBatchInjectFlag();
  const hasInputPlaceholder = tplRaw.includes('{input}');

  for(let i=0; i<paths.length; ++i){
    const p = paths[i];
    const e = meta.get(p) || {name:(p.split('/').pop()||p), path:p};
    let args = tplRaw;

    // Always expand placeholders (including {input} if present).
    args = expandBatchTemplate(args, e, i+1);

    // If user didn't include {input}, inject the selected file path into the chosen flag.
    if(!hasInputPlaceholder){
      args = setFlagValue(args, flag, p);
    }

    if(status) status.textContent = 'Submitting '+(i+1)+'/'+paths.length+'…';
    try{
      const r = await apiFetch('/api/run', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({tool:tool, args:args})});
      const j = await r.json();
      if(!r.ok) throw new Error(j && j.error ? j.error : 'run failed');
      okCount++;
      const id = j.id;
      const runDir = j.run_dir || '';
      const log = j.log || '';
      outHtml += '<div style="margin-top:6px"><b>#'+esc(String(i+1))+'</b> <code>'+esc(p)+'</code> → job <b>'+esc(String(id))+'</b>';
      if(runDir){
        const runHref = (!String(runDir).endsWith('/')) ? (runDir + '/') : runDir;
        outHtml += ' — <a href="'+esc(runHref)+'" target="_blank" rel="noopener"><code>'+esc(runDir)+'</code></a>';
      }
      if(log){
        outHtml += ' — <a href="'+esc(log)+'" target="_blank" rel="noopener"><code>log</code></a>';
      }
      outHtml += '</div>';
      if(prog) prog.innerHTML = outHtml;
    }catch(e2){
      const msg = (e2 && e2.message) ? e2.message : String(e2);
      outHtml += '<div style="margin-top:6px;color:var(--bad)"><b>Error</b> submitting '+esc(p)+': '+esc(msg)+'</div>';
      if(prog) prog.innerHTML = outHtml;
      // Stop on first error (best-effort safety).
      break;
    }
  }

  batchSubmitting=false;
  if(runBtn) runBtn.disabled=false;
  if(status) status.textContent = 'Submitted '+okCount+'/'+paths.length+' job(s).';
  refreshRuns();
}

document.addEventListener('keydown', (e)=>{
  const k = e.key || '';
  if((e.ctrlKey || e.metaKey) && (k==='k' || k==='K')){
    e.preventDefault();
    openCmdPalette();
    return;
  }
  if(k==='Escape'){
    closePreview(); closeFlagHelper(); closeNotes(); closeBatch(); closeCmdPalette(); closeSidebar();
  }
});

function maybeAutoRefreshOutputs(statusEl){
  if(!statusEl || !statusEl.dataset) return;
  const wrapId = statusEl.dataset.outwrapId||'';
  if(!wrapId) return;
  const wrap = document.getElementById(wrapId);
  if(!wrap || wrap.classList.contains('hidden')) return;
  refreshOutputsForStatus(statusEl);
}

loadUiState();
initSidebar();
initPresets();
initInjectSelects();
initCmdPalette();
initSelectionBar();
initArgsDrop();
updateSelectedInputUi();
initFx();
detectApi();
setInterval(detectApi, 5000);
</script>
)JS";

  o << "</body></html>\n";
}

} // namespace

void write_qeeg_tools_ui_html(const UiDashboardArgs& args) {
  if (args.root.empty()) {
    throw std::runtime_error("write_qeeg_tools_ui_html: args.root is required");
  }
  if (args.output_html.empty()) {
    throw std::runtime_error("write_qeeg_tools_ui_html: args.output_html is required");
  }

  const std::filesystem::path out_html = std::filesystem::u8path(args.output_html);
  const std::filesystem::path out_dir = out_html.parent_path();
  if (!out_dir.empty()) {
    ensure_directory(out_dir.u8string());
  }

  std::ofstream f(out_html, std::ios::binary);
  if (!f) {
    throw std::runtime_error("Failed to open output HTML for writing: " + args.output_html);
  }

  write_html(f, args);
}

} // namespace qeeg
