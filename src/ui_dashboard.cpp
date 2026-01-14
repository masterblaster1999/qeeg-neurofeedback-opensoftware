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
     "qeeg_export_brainvision_cli --input session.edf --outdir out_bv"},

    {"qeeg_export_bids_cli", "Inspect & Convert",
     "Export a raw recording to a BIDS-like EEG layout (channels/events sidecars).",
     "qeeg_export_bids_cli --input session.edf --bids-root bids --sub 01 --task rest"},

    {"qeeg_export_derivatives_cli", "Inspect & Convert",
     "Package qeeg outputs (map/spec/qc/iaf/nf/microstates) into a BIDS derivatives layout.",
     "qeeg_export_derivatives_cli --bids-root bids --pipeline qeeg --sub 01 --task rest --map-outdir out_map"},

    {"qeeg_preprocess_cli", "Preprocess & Clean",
     "Apply simple offline preprocessing (CAR/notch/bandpass) and export cleaned signals.",
     "qeeg_preprocess_cli --input session.edf --output cleaned.csv --notch 50 --bandpass 1 40"},

    {"qeeg_clean_cli", "Preprocess & Clean",
     "Basic cleaning utilities (artifact-related helpers; depends on build).",
     "qeeg_clean_cli --help"},

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
     "qeeg_bandratios_cli --bandpowers out_bp/bandpowers.csv --outdir out_ratios --ratio theta/beta"},

    {"qeeg_spectral_features_cli", "Spectral & Maps",
     "Spectral summary table per channel (entropy, SEF95, peak frequency, ...).",
     "qeeg_spectral_features_cli --input session.edf --outdir out_spec"},

    {"qeeg_reference_cli", "Spectral & Maps",
     "Build a reference CSV (mean/std) for z-scoring bandpowers/topomaps.",
     "qeeg_reference_cli --input-dir dataset --out reference.csv"},

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
     "qeeg_ui_cli --root . --output qeeg_ui.html"},

    {"qeeg_ui_server_cli", "UI",
     "Serve the dashboard locally and enable one-click runs via a small local-only HTTP API.",
     "qeeg_ui_server_cli --root . --bin-dir ./build --port 8765 --open"},
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
  rewrite_flag_value("--output", false);
  rewrite_flag_value("--events-out", false);
  rewrite_flag_value("--events-out-tsv", false);
  rewrite_flag_value("--channel-map-template", false);

  return join_cmd_tokens(toks);
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
    << "    .sidebar h1{font-size:16px;margin:16px 16px 8px} .sidebar .meta{color:var(--muted);margin:0 16px 12px;font-size:12px;line-height:1.4}\n"
    << "    .search{padding:0 16px 12px} .search input{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--border);background:var(--panel2);color:var(--text)}\n"
    << "    .group{margin:10px 0 16px} .group-title{padding:8px 16px;color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}\n"
    << "    .nav a{display:flex;gap:8px;align-items:center;padding:8px 16px;border-left:3px solid transparent;font-size:13px}\n"
    << "    .nav a:hover{background:rgba(255,255,255,0.04)}\n"
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
    << "    .btn[disabled]{opacity:.55;cursor:not-allowed}\n"
    << "    .input{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--border);background:var(--panel);color:var(--text);font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, \"Liberation Mono\", \"Courier New\", monospace;font-size:12px;box-sizing:border-box}\n"
    << "    .statusline{margin-top:10px;color:var(--muted);font-size:12px;line-height:1.35;word-break:break-word}\n"
    << "    .statusline b{color:var(--text)}\n"
    << "    .outputs a{display:block;padding:4px 0;font-size:13px;word-break:break-all}\n"
    << "    details{margin-top:10px} summary{cursor:pointer;color:var(--accent);font-size:13px}\n"
    << "    .hidden{display:none !important}\n"
    << "    .pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);color:var(--muted);font-size:12px;margin-left:8px}\n"
    << "    .pill.run{color:var(--good)}\n"
    << "    table{width:100%;border-collapse:collapse}\n"
    << "    th,td{padding:6px 8px;border-bottom:1px solid var(--border);font-size:12px;vertical-align:top}\n"
    << "    th{color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}\n"
    << "    .small{font-size:12px;color:var(--muted)}\n"
    << "    .logtail{max-height:220px}\n"
    << "  </style>\n"
    << "</head>\n";

  o << "<body>\n";
  o << "<div class=\"layout\">\n";

  // Sidebar.
  o << "  <aside class=\"sidebar\">\n";
  o << "    <h1>" << html_escape(args.title) << "</h1>\n";
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
      o << "<span>" << html_escape(t.name) << "</span>";
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
  o << "      <div style=\"margin-top:10px;color:var(--muted);font-size:12px\">Selected input: <code id=\"selectedInput\">(none)</code></div>\n";
  o << "      <div class=\"row\" style=\"margin-top:12px\">\n";
  o << "        <div class=\"card\" style=\"flex:1 1 420px\">\n";
  o << "          <h4>Recent UI runs</h4>\n";
  o << "          <div id=\"runsPanel\" class=\"small\">(available when served via <code>qeeg_ui_server_cli</code>)</div>\n";
  o << "        </div>\n";
  o << "        <div class=\"card\" style=\"flex:1 1 420px\">\n";
  o << "          <h4>Workspace browser</h4>\n";
  o << "          <div id=\"fsPanel\" class=\"small\">(available when served via <code>qeeg_ui_server_cli</code>)</div>\n";
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
        const std::string default_args = infer_ui_run_args_from_example(t.name, t.example);

        o << "        <div class=\"card\">\n";
        o << "          <h4>Run (optional)</h4>\n";
        o << "          <div style=\"color:var(--muted);font-size:12px;margin-bottom:8px\">";
        o << "Requires <code>qeeg_ui_server_cli</code> (local).\n";
        o << "          </div>\n";
        o << "          <input class=\"input\" id=\"" << args_id << "\" data-default=\"" << html_escape(default_args) << "\" placeholder=\"Args (e.g. --input file.edf --outdir out)\" value=\"" << html_escape(default_args) << "\">\n";
        o << "          <div style=\"display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap\">\n";
        o << "            <button class=\"btn run-btn\" id=\"" << runbtn_id << "\" data-tool=\"" << html_escape(t.name) << "\" data-args-id=\"" << args_id << "\" data-status-id=\"" << status_id << "\" data-stop-id=\"" << stopbtn_id << "\" data-logwrap-id=\"" << logwrap_id << "\" data-log-id=\"" << log_id << "\" disabled onclick=\"runTool(this)\">Run</button>\n";
        // Copy full command (includes tool name).
        const std::string full_id = id + "_full";
        const std::string preset_id = id + "_preset";
        o << "            <button class=\"btn\" onclick=\"copyFullCmd('" << full_id << "','" << args_id << "','" << html_escape(t.name) << "')\">Copy full command</button>\n";
        o << "            <button class=\"btn\" id=\"" << stopbtn_id << "\" data-status-id=\"" << status_id << "\" disabled onclick=\"stopJob(this)\">Stop</button>\n";
        o << "            <button class=\"btn\" data-status-id=\"" << status_id << "\" data-logwrap-id=\"" << logwrap_id << "\" data-log-id=\"" << log_id << "\" onclick=\"toggleLog(this)\">Tail log</button>\n";
        o << "            <button class=\"btn use-input-btn\" data-args-id=\"" << args_id << "\" disabled onclick=\"useSelectedInput(this)\">Use selected file</button>\n";
        o << "            <span id=\"" << full_id << "\" class=\"hidden\"></span>\n";
        o << "          </div>\n";
        o << "          <div style=\"display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap\">\n";
        o << "            <select class=\"input preset-select\" id=\"" << preset_id << "\" data-tool=\"" << html_escape(t.name) << "\" data-args-id=\"" << args_id << "\" style=\"max-width:240px\"></select>\n";
        o << "            <button class=\"btn\" onclick=\"savePreset(\'" << html_escape(t.name) << "\',\'" << args_id << "\',\'" << preset_id << "\')\">Save preset</button>\n";
        o << "            <button class=\"btn\" onclick=\"deletePreset(\'" << html_escape(t.name) << "\',\'" << preset_id << "\')\">Delete</button>\n";
        o << "            <button class=\"btn\" onclick=\"resetArgs(\'" << args_id << "\')\">Reset</button>\n";
        o << "          </div>\n";
        o << "          <div class=\"statusline\" id=\"" << status_id << "\">Server not detected. Start: <code>qeeg_ui_server_cli --root . --bin-dir ./build</code></div>\n";
        o << "          <div id=\"" << logwrap_id << "\" class=\"hidden\" style=\"margin-top:10px\">\n";
        o << "            <div style=\"display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap\">\n";
        o << "              <div class=\"small\">Log tail (latest ~64KB)</div>\n";
        o << "              <button class=\"btn\" data-status-id=\"" << status_id << "\" data-log-id=\"" << log_id << "\" onclick=\"refreshLog(this)\">Refresh</button>\n";
        o << "            </div>\n";
        o << "            <pre class=\"logtail\"><code id=\"" << log_id << "\"></code></pre>\n";
        o << "          </div>\n";
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
            o << "          <a href=\"" << html_escape(href) << "\">" << html_escape(href) << "</a>\n";
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
          o << "        <pre><code>" << html_escape(help) << "</code></pre>\n";
        }
        o << "      </details>\n";
      }

      o << "    </section>\n";
    }
  }

  o << "  </main>\n";
  o << "</div>\n";

  // JS: search + copy + optional run API.
  o << R"JS(<script>
function copyText(id){const el=document.getElementById(id); if(!el) return; const t=el.innerText||el.textContent||''; navigator.clipboard.writeText(t).then(()=>{},()=>{});}
function copyFullCmd(_ignored, argsId, tool){const el=document.getElementById(argsId); const args=(el&&el.value)?el.value:''; const cmd=(tool+' '+args).trim(); navigator.clipboard.writeText(cmd).then(()=>{},()=>{});}
const search=document.getElementById('search');
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

let qeegApiOk=false;
let qeegApiToken='';
let runsTimer=null;
let selectedInputPath='';
let fsCurrentDir='';
let fsEntries=[];


function esc(s){
  return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
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


function loadUiState(){
  try{
    selectedInputPath = localStorage.getItem('qeeg_selected_input') || '';
    fsCurrentDir = localStorage.getItem('qeeg_fs_dir') || '';
  }catch(e){}
}

function updateSelectedInputUi(){
  const el=document.getElementById('selectedInput');
  if(el){
    el.textContent = selectedInputPath ? selectedInputPath : '(none)';
  }
  document.querySelectorAll('.use-input-btn').forEach(b=>{
    b.disabled = !selectedInputPath;
  });
}

function setSelectedInput(p){
  selectedInputPath = p || '';
  try{ localStorage.setItem('qeeg_selected_input', selectedInputPath); }catch(e){}
  updateSelectedInputUi();
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
    alert('Select an input file first (Workspace browser).');
    return;
  }
  el.value = setFlagValue(el.value, '--input', selectedInputPath);
}

function resetArgs(argsId){
  const el=document.getElementById(argsId);
  if(!el) return;
  const d = el.getAttribute('data-default') || '';
  el.value = d;
}

function presetsKey(tool){ return 'qeeg_presets_'+tool; }
function loadPresets(tool){
  try{
    const s = localStorage.getItem(presetsKey(tool));
    return s ? (JSON.parse(s) || {}) : {};
  }catch(e){ return {}; }
}
function savePresets(tool, obj){
  try{ localStorage.setItem(presetsKey(tool), JSON.stringify(obj||{})); }catch(e){}
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

function fsSetDir(d){
  fsCurrentDir = d || '';
  try{ localStorage.setItem('qeeg_fs_dir', fsCurrentDir); }catch(e){}
}

function fsEnter(d){
  fsSetDir(d);
  refreshFs();
}

function fsUp(){
  if(!fsCurrentDir) return;
  const parts = fsCurrentDir.split('/').filter(p=>p);
  if(parts.length===0) return;
  parts.pop();
  fsEnter(parts.join('/'));
}

function copyPath(p){
  navigator.clipboard.writeText(String(p||'')).then(()=>{},()=>{});
}

function selectFromFs(p){
  setSelectedInput(String(p||''));
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
      '<button class="btn" id="fsRefreshBtn">Refresh</button>'+
      '<span class="small">Dir: <code id="fsDirLabel"></code></span>'+
      '<input class="input" id="fsFilter" style="max-width:220px" placeholder="filter (e.g., .edf)">'+
    '</div>'+
    '<div id="fsList" class="small"></div>';

  const up=document.getElementById('fsUpBtn'); if(up) up.onclick=fsUp;
  const rf=document.getElementById('fsRefreshBtn'); if(rf) rf.onclick=refreshFs;
  const flt=document.getElementById('fsFilter'); if(flt) flt.addEventListener('input', renderFs);

  refreshFs();
}

async function refreshFs(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const lbl=document.getElementById('fsDirLabel');
  if(lbl) lbl.textContent = fsCurrentDir ? fsCurrentDir : '.';
  const list=document.getElementById('fsList');
  if(list) list.textContent = 'Loading…';
  try{
    const r=await apiFetch('/api/list',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dir:fsCurrentDir})});
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

  let html = '<table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Actions</th></tr></thead><tbody>';
  for(const e of entries){
    const name = (e && e.name) ? String(e.name) : '';
    const path = (e && e.path) ? String(e.path) : name;
    const type = (e && e.type) ? String(e.type) : '';
    const size = (e && e.size) ? Number(e.size) : 0;
    const pjs = escJs(path);
    let actions = '';
    if(type==='dir'){
      actions = '<button class="btn" onclick="fsEnter(\\''+pjs+'\\')">Open</button>';
    }else{
      actions =
        '<button class="btn" onclick="selectFromFs(\\''+pjs+'\\')">Select</button> '+
        '<button class="btn" onclick="copyPath(\\''+pjs+'\\')">Copy</button> '+
        '<a href="'+encodeURI(path)+'" target="_blank">Open</a>';
    }
    const nameCell = (type==='dir')
      ? '<a href="#" onclick="fsEnter(\\''+pjs+'\\');return false;">📁 '+esc(name)+'</a>'
      : esc(name);
    html += '<tr><td>'+nameCell+'</td><td>'+esc(type)+'</td><td>'+(type==='file'?esc(humanSize(size)):'')+'</td><td>'+actions+'</td></tr>';
  }
  html += '</tbody></table>';
  list.innerHTML = html;
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
  if(rp && !(qeegApiOk && qeegApiToken)){
    rp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
  }
  const fp = document.getElementById('fsPanel');
  if(fp && !(qeegApiOk && qeegApiToken)){
    fp.innerHTML = '(available when served via <code>qeeg_ui_server_cli</code>)';
  }
  updateSelectedInputUi();
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
  } else {
    stopRunsPoll();
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

async function refreshRuns(){
  if(!(qeegApiOk && qeegApiToken)) return;
  const rp=document.getElementById('runsPanel');
  if(!rp) return;
  try{
    const r=await apiFetch('/api/runs');
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'runs failed');
    const runs = (j && j.runs) ? j.runs : [];
    if(runs.length===0){
      rp.innerHTML = '<span class="small">No runs yet.</span>';
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
      if(log) links += '<a href="'+esc(log)+'"><code>'+esc(log)+'</code></a>';
      if(run) links += (links?'<br>':'') + '<a href="'+esc(run)+'"><code>'+esc(run)+'</code></a>';
      html += '<tr><td>'+id+'</td><td><code>'+esc(tool)+'</code></td><td>'+esc(status)+'</td><td>'+esc(started)+'</td><td>'+links+'</td></tr>';
    }
    html += '</tbody></table>';
    rp.innerHTML = html;
  }catch(e){
    rp.innerHTML = '<span class="small">Error loading runs: '+esc(e&&e.message?e.message:String(e))+'</span>';
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
    const code = (j.exit_code!==undefined) ? j.exit_code : '';
    let html = statusEl.innerHTML;
    if((statusEl.textContent||'').trim()==='Starting…'){
      html = 'Started <b>'+esc(j.tool||'')+'</b>';
      if(j.log) html += ' — log: <a href="'+esc(j.log)+'"><code>'+esc(j.log)+'</code></a>';
      if(j.run_dir) html += '<br>Run dir: <a href="'+esc(j.run_dir)+'"><code>'+esc(j.run_dir)+'</code></a>';
    }
    html += '<br>Status: <b>'+esc(st)+'</b>';
    if(st!=='running' && st!=='stopping') html += ' (exit '+esc(String(code))+')';
    statusEl.innerHTML = html;
    if(stopBtn) stopBtn.disabled = !(st==='running' || st==='stopping');
    if(runBtn && (st!=='running' && st!=='stopping')) runBtn.disabled = false;
    if(st==='running' || st==='stopping'){
      setTimeout(()=>{pollJob(statusEl, runBtn, stopBtn);}, 1500);
    } else {
      refreshRuns();
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
  if(status){ status.textContent='Starting…'; status.dataset.jobId=''; }
  if(logCode) logCode.textContent='';
  if(logWrap) logWrap.classList.add('hidden');
  if(stopBtn){ stopBtn.disabled=true; stopBtn.dataset.jobId=''; }
  btn.disabled=true;
  try{
    const r=await apiFetch('/api/run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tool:tool,args:args})});
    const j=await r.json();
    if(!r.ok) throw new Error(j&&j.error?j.error:'run failed');
    const id = j.id;
    if(status){ status.dataset.jobId = String(id); }
    if(stopBtn){ stopBtn.disabled=false; stopBtn.dataset.jobId = String(id); }
    if(logWrap) logWrap.classList.remove('hidden');
    if(status){
      let html='Started <b>'+esc(tool)+'</b>';
      if(j.log) html += ' — log: <a href="'+esc(j.log)+'"><code>'+esc(j.log)+'</code></a>';
      if(j.run_dir) html += '<br>Run dir: <a href="'+esc(j.run_dir)+'"><code>'+esc(j.run_dir)+'</code></a>';
      status.innerHTML=html;
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
      status.innerHTML = (status.innerHTML||'') + '<br>Status: <b>stopping</b>';
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
  wrap.classList.toggle('hidden');
  if(!wrap.classList.contains('hidden')){
    const logId=btn.getAttribute('data-log-id');
    if(logId){
      const code=document.getElementById(logId);
      if(code && code.textContent===''){
        refreshLog(btn);
      }
    }
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
  try{
    const r=await apiFetch('/api/log/'+id);
    code.textContent = await r.text();
  }catch(e){
    code.textContent='Error loading log: '+(e&&e.message?e.message:String(e));
  }
}

loadUiState();
initPresets();
updateSelectedInputUi();
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
