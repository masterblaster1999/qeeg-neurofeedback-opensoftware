#include "qeeg/cli_input.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/subprocess.hpp"
#include "qeeg/ui_dashboard.hpp"
#include "qeeg/utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_spec;
  std::string outdir{"out_pipeline"};

  // Helpful when the initial input is CSV with no time column.
  double fs_csv{0.0};

  // Where to find tools.
  std::string bin_dir;   // If set, resolve qeeg_*_cli executables from this directory.
  std::string toolbox;   // If set, run tools as: <toolbox> <tool> [args...]

  // Workflow knobs.
  bool skip_preprocess{false};
  bool skip_bandratios{false};

  std::string preprocess_ext{"csv"};

  // Extra args appended to each step (parsed using split_commandline_args).
  std::string preprocess_args;
  std::string bandpower_args;
  std::string bandratios_args;

  // Optional post-processing steps.
  bool with_topomaps{false};
  bool topomaps_annotate{false};
  std::string topomap_montage{"builtin:standard_1020_19"};
  std::string topomap_args;

  bool with_region_summary{false};
  std::string region_args;

  // Optional: write a local HTML dashboard that links to this workspace.
  bool write_ui{false};
  bool ui_embed_help{false};
  bool ui_open{false};
  std::string ui_title{"QEEG Pipeline Workspace"};

  bool dry_run{false};
};

static void print_help() {
  std::cout
      << "qeeg_pipeline_cli\n\n"
      << "Run a small multi-step qEEG workflow by chaining existing qeeg_*_cli tools.\n\n"
      << "This is intended to improve *CLI file cross integration*: instead of manually\n"
      << "copying filenames between commands, this tool creates a workspace directory\n"
      << "and runs a consistent pipeline inside it.\n\n"
      << "Default workflow (basic):\n"
      << "  1) qeeg_preprocess_cli   -> 01_preprocess/preprocessed.<ext>\n"
      << "  2) qeeg_bandpower_cli    -> 02_bandpower/bandpowers.csv\n"
      << "  3) qeeg_bandratios_cli   -> 03_bandratios/bandratios.csv (optional)\n"
      << "Optional post-processing:\n"
      << "  4) qeeg_topomap_cli      -> 04_topomaps_bandpower/* (optional; --topomaps)\n"
      << "  5) qeeg_topomap_cli      -> 05_topomaps_bandratios/* (optional; --topomaps)\n"
      << "  6) qeeg_region_summary_cli -> 06_regions_bandpower/* (optional; --region-summary)\n"
      << "  7) qeeg_region_summary_cli -> 07_regions_bandratios/* (optional; --region-summary)\n"
      << "  8) qeeg_ui.html          -> Workspace dashboard (optional; --write-ui)\n\n"
      << "Usage:\n"
      << "  qeeg_pipeline_cli --input file.edf --outdir out_work\n"
      << "  qeeg_pipeline_cli --input out_preprocess --skip-preprocess --outdir out_work\n"
      << "  qeeg_pipeline_cli --input raw.csv --fs 250 --outdir out_work --bandpower-args \"--nperseg 256\"\n\n"
      << "Tool discovery:\n"
      << "  --bin-dir DIR    Resolve qeeg_*_cli executables from DIR (plus .exe on Windows).\n"
      << "  --toolbox PATH   Run tools via a multicall toolbox (recommended for offline bundles):\n"
      << "                 PATH qeeg_preprocess_cli ...\n"
      << "                 PATH qeeg_bandpower_cli ...\n"
      << "                 PATH qeeg_bandratios_cli ...\n"
      << "               If not provided, the environment variable QEEG_TOOLBOX is used when set.\n\n"
      << "Options:\n"
      << "  --input SPEC            Input recording (file/dir/*_run_meta.json)\n"
      << "  --outdir DIR            Workspace output directory (default: out_pipeline)\n"
      << "  --fs HZ                 Sampling-rate hint for CSV/ASCII inputs (default: 0)\n"
      << "  --skip-preprocess       Skip step 1 and run bandpower directly on --input\n"
      << "  --skip-bandratios       Skip step 3 (bandratios)\n"
      << "  --preprocess-ext EXT    Output extension for preprocess step (default: csv).\n"
      << "                         Common: csv|edf|bdf|vhdr\n"
      << "  --preprocess-args STR   Extra args appended to qeeg_preprocess_cli\n"
      << "  --bandpower-args STR    Extra args appended to qeeg_bandpower_cli\n"
      << "  --bandratios-args STR   Extra args appended to qeeg_bandratios_cli\n"
      << "  --topomaps              Run qeeg_topomap_cli on bandpowers (and bandratios if present).\n"
      << "  --topomaps-annotate     Like --topomaps, but also passes --annotate to qeeg_topomap_cli.\n"
      << "  --topomap-montage SPEC  Montage for topomaps (default: builtin:standard_1020_19).\n"
      << "  --topomap-args STR      Extra args appended to qeeg_topomap_cli (e.g. \"--robust\").\n"
      << "  --region-summary        Run qeeg_region_summary_cli on bandpowers (and bandratios if present).\n"
      << "  --region-args STR       Extra args appended to qeeg_region_summary_cli.\n"
      << "  --write-ui              Generate <outdir>/qeeg_ui.html linking to the workspace outputs.\n"
      << "  --ui-help               When used with --write-ui, embed tool --help outputs in the UI.\n"
      << "  --ui-title TEXT         Title for the generated UI (default: QEEG Pipeline Workspace).\n"
      << "  --open-ui               Open the generated UI in your default browser (implies --write-ui).\n"
      << "  --dry-run               Print commands without executing or writing outputs\n"
      << "  -h, --help              Show help\n";
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
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_spec = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--bin-dir" && i + 1 < argc) {
      a.bin_dir = argv[++i];
    } else if (arg == "--toolbox" && i + 1 < argc) {
      a.toolbox = argv[++i];
    } else if (arg == "--skip-preprocess") {
      a.skip_preprocess = true;
    } else if (arg == "--skip-bandratios") {
      a.skip_bandratios = true;
    } else if (arg == "--preprocess-ext" && i + 1 < argc) {
      a.preprocess_ext = argv[++i];
    } else if (arg == "--preprocess-args" && i + 1 < argc) {
      a.preprocess_args = argv[++i];
    } else if (arg == "--bandpower-args" && i + 1 < argc) {
      a.bandpower_args = argv[++i];
    } else if (arg == "--bandratios-args" && i + 1 < argc) {
      a.bandratios_args = argv[++i];
    } else if (arg == "--topomaps") {
      a.with_topomaps = true;
    } else if (arg == "--topomaps-annotate") {
      a.with_topomaps = true;
      a.topomaps_annotate = true;
    } else if (arg == "--topomap-montage" && i + 1 < argc) {
      a.with_topomaps = true;
      a.topomap_montage = argv[++i];
    } else if (arg == "--topomap-args" && i + 1 < argc) {
      a.with_topomaps = true;
      a.topomap_args = argv[++i];
    } else if (arg == "--region-summary") {
      a.with_region_summary = true;
    } else if (arg == "--region-args" && i + 1 < argc) {
      a.with_region_summary = true;
      a.region_args = argv[++i];
    } else if (arg == "--write-ui") {
      a.write_ui = true;
    } else if (arg == "--ui-help") {
      a.write_ui = true;
      a.ui_embed_help = true;
    } else if (arg == "--ui-title" && i + 1 < argc) {
      a.write_ui = true;
      a.ui_title = argv[++i];
    } else if (arg == "--open-ui") {
      a.write_ui = true;
      a.ui_open = true;
    } else if (arg == "--dry-run") {
      a.dry_run = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }

  // Environment fallback.
  if (a.toolbox.empty()) {
    const char* env = std::getenv("QEEG_TOOLBOX");
    if (env && *env) {
      a.toolbox = env;
    }
  }

  return a;
}

static std::filesystem::path resolve_exe_from_bin_dir(const std::filesystem::path& bin_dir,
                                                      const std::string& tool) {
  if (bin_dir.empty()) return {};
  std::filesystem::path p = bin_dir / tool;
  if (std::filesystem::exists(p)) return p;
  std::filesystem::path pe = p;
  pe += ".exe";
  if (std::filesystem::exists(pe)) return pe;
  return {};
}

static std::vector<std::string> build_tool_argv(const Args& a,
                                                const std::string& tool,
                                                const std::vector<std::string>& tool_args) {
  std::vector<std::string> argv;
  argv.reserve(tool_args.size() + 2);

  if (!a.toolbox.empty()) {
    argv.push_back(a.toolbox);
    argv.push_back(tool);
    for (const auto& s : tool_args) argv.push_back(s);
    return argv;
  }

  if (!a.bin_dir.empty()) {
    const std::filesystem::path exe = resolve_exe_from_bin_dir(std::filesystem::u8path(a.bin_dir), tool);
    if (!exe.empty()) {
      argv.push_back(exe.u8string());
      for (const auto& s : tool_args) argv.push_back(s);
      return argv;
    }
  }

  // Fall back to PATH lookup.
  argv.push_back(tool);
  for (const auto& s : tool_args) argv.push_back(s);
  return argv;
}

static std::string argv_to_string(const std::vector<std::string>& argv) {
  std::string out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) out += ' ';
    // Best-effort quoting for readability.
    const std::string& a = argv[i];
    const bool need_quote = (a.find(' ') != std::string::npos) || (a.find('\t') != std::string::npos);
    if (need_quote) {
      out += '"' + a + '"';
    } else {
      out += a;
    }
  }
  return out;
}

static int run_step(const Args& a,
                    const std::string& step_name,
                    const std::string& tool,
                    const std::vector<std::string>& tool_args,
                    const std::string& cwd) {
  const std::vector<std::string> argv = build_tool_argv(a, tool, tool_args);
  std::cerr << "[pipeline] " << step_name << ": " << argv_to_string(argv) << "\n";
  if (a.dry_run) return 0;
  const SubprocessResult r = run_subprocess(argv, cwd);
  return r.exit_code;
}

static std::vector<std::string> split_extra_args(const std::string& s) {
  if (trim(s).empty()) return {};
  return split_commandline_args(s);
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    if (trim(args.input_spec).empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (trim(args.outdir).empty()) {
      throw std::runtime_error("--outdir must be non-empty");
    }

    const std::filesystem::path root = std::filesystem::u8path(args.outdir);
    if (!args.dry_run) {
      ensure_directory(root.u8string());
    }

    const std::filesystem::path pre_dir = root / "01_preprocess";
    const std::filesystem::path bp_dir = root / "02_bandpower";
    const std::filesystem::path br_dir = root / "03_bandratios";

    const std::filesystem::path topo_bp_dir = root / "04_topomaps_bandpower";
    const std::filesystem::path topo_br_dir = root / "05_topomaps_bandratios";
    const std::filesystem::path regions_bp_dir = root / "06_regions_bandpower";
    const std::filesystem::path regions_br_dir = root / "07_regions_bandratios";

    if (!args.dry_run) {
      if (!args.skip_preprocess) ensure_directory(pre_dir.u8string());
      ensure_directory(bp_dir.u8string());
      if (!args.skip_bandratios) ensure_directory(br_dir.u8string());

      if (args.with_topomaps) {
        ensure_directory(topo_bp_dir.u8string());
        if (!args.skip_bandratios) ensure_directory(topo_br_dir.u8string());
      }
      if (args.with_region_summary) {
        ensure_directory(regions_bp_dir.u8string());
        if (!args.skip_bandratios) ensure_directory(regions_br_dir.u8string());
      }
    }

    // Resolve the initial input to a concrete recording path for the pipeline run meta.
    // (Individual tools also resolve inputs, but keeping a resolved path here makes the
    // top-level manifest more useful.)
    const ResolvedInputPath resolved_in = resolve_input_recording_path(args.input_spec);
    if (!resolved_in.note.empty()) {
      std::cerr << "[pipeline] " << resolved_in.note << "\n";
    }

    // --- Step 1: preprocess ---
    std::filesystem::path preprocess_out;
    std::string bandpower_input_spec = args.input_spec;
    if (!args.skip_preprocess) {
      const std::string ext = to_lower(trim(args.preprocess_ext));
      if (ext.empty()) {
        throw std::runtime_error("--preprocess-ext must be non-empty");
      }
      preprocess_out = pre_dir / (std::string("preprocessed.") + ext);

      std::vector<std::string> pargs;
      pargs.push_back("--input");
      pargs.push_back(args.input_spec);
      if (args.fs_csv > 0.0) {
        pargs.push_back("--fs");
        pargs.push_back(std::to_string(args.fs_csv));
      }
      pargs.push_back("--output");
      pargs.push_back(preprocess_out.u8string());

      const auto extra = split_extra_args(args.preprocess_args);
      pargs.insert(pargs.end(), extra.begin(), extra.end());

      const int rc = run_step(args, "preprocess", "qeeg_preprocess_cli", pargs, root.u8string());
      if (rc != 0) {
        std::cerr << "[pipeline] preprocess failed with exit code " << rc << "\n";
        return rc;
      }

      // For downstream tools, passing the directory is preferred (enables run-meta chaining).
      bandpower_input_spec = pre_dir.u8string();
    }

    // --- Step 2: bandpower ---
    {
      std::vector<std::string> bpargs;
      bpargs.push_back("--input");
      bpargs.push_back(bandpower_input_spec);
      if (args.fs_csv > 0.0) {
        bpargs.push_back("--fs");
        bpargs.push_back(std::to_string(args.fs_csv));
      }
      bpargs.push_back("--outdir");
      bpargs.push_back(bp_dir.u8string());

      const auto extra = split_extra_args(args.bandpower_args);
      bpargs.insert(bpargs.end(), extra.begin(), extra.end());

      const int rc = run_step(args, "bandpower", "qeeg_bandpower_cli", bpargs, root.u8string());
      if (rc != 0) {
        std::cerr << "[pipeline] bandpower failed with exit code " << rc << "\n";
        return rc;
      }
    }

    // --- Step 3: bandratios ---
    if (!args.skip_bandratios) {
      std::vector<std::string> brargs;
      brargs.push_back("--bandpowers");
      brargs.push_back(bp_dir.u8string());
      brargs.push_back("--outdir");
      brargs.push_back(br_dir.u8string());

      const auto extra = split_extra_args(args.bandratios_args);
      brargs.insert(brargs.end(), extra.begin(), extra.end());

      const int rc = run_step(args, "bandratios", "qeeg_bandratios_cli", brargs, root.u8string());
      if (rc != 0) {
        std::cerr << "[pipeline] bandratios failed with exit code " << rc << "\n";
        return rc;
      }
    }

    // --- Optional: topomaps ---
    if (args.with_topomaps) {
      // Bandpowers -> topomaps
      {
        std::vector<std::string> targs;
        targs.push_back("--input");
        targs.push_back(bp_dir.u8string());
        targs.push_back("--outdir");
        targs.push_back(topo_bp_dir.u8string());
        if (!trim(args.topomap_montage).empty()) {
          targs.push_back("--montage");
          targs.push_back(args.topomap_montage);
        }
        if (args.topomaps_annotate) {
          targs.push_back("--annotate");
        }
        targs.push_back("--html-report");

        const auto extra = split_extra_args(args.topomap_args);
        targs.insert(targs.end(), extra.begin(), extra.end());

        const int rc = run_step(args, "topomaps_bandpower", "qeeg_topomap_cli", targs, root.u8string());
        if (rc != 0) {
          std::cerr << "[pipeline] topomaps (bandpower) failed with exit code " << rc << "\n";
          return rc;
        }
      }

      // Bandratios -> topomaps
      if (!args.skip_bandratios) {
        std::vector<std::string> targs;
        targs.push_back("--input");
        targs.push_back(br_dir.u8string());
        targs.push_back("--outdir");
        targs.push_back(topo_br_dir.u8string());
        if (!trim(args.topomap_montage).empty()) {
          targs.push_back("--montage");
          targs.push_back(args.topomap_montage);
        }
        if (args.topomaps_annotate) {
          targs.push_back("--annotate");
        }
        targs.push_back("--html-report");

        const auto extra = split_extra_args(args.topomap_args);
        targs.insert(targs.end(), extra.begin(), extra.end());

        const int rc = run_step(args, "topomaps_bandratios", "qeeg_topomap_cli", targs, root.u8string());
        if (rc != 0) {
          std::cerr << "[pipeline] topomaps (bandratios) failed with exit code " << rc << "\n";
          return rc;
        }
      }
    }

    // --- Optional: region summaries ---
    if (args.with_region_summary) {
      // Bandpowers -> region summary
      {
        std::vector<std::string> rargs;
        rargs.push_back("--input");
        rargs.push_back(bp_dir.u8string());
        rargs.push_back("--outdir");
        rargs.push_back(regions_bp_dir.u8string());
        rargs.push_back("--html-report");

        const auto extra = split_extra_args(args.region_args);
        rargs.insert(rargs.end(), extra.begin(), extra.end());

        const int rc = run_step(args, "region_summary_bandpower", "qeeg_region_summary_cli", rargs, root.u8string());
        if (rc != 0) {
          std::cerr << "[pipeline] region summary (bandpower) failed with exit code " << rc << "\n";
          return rc;
        }
      }

      // Bandratios -> region summary
      if (!args.skip_bandratios) {
        std::vector<std::string> rargs;
        rargs.push_back("--input");
        rargs.push_back(br_dir.u8string());
        rargs.push_back("--outdir");
        rargs.push_back(regions_br_dir.u8string());
        rargs.push_back("--html-report");

        const auto extra = split_extra_args(args.region_args);
        rargs.insert(rargs.end(), extra.begin(), extra.end());

        const int rc = run_step(args, "region_summary_bandratios", "qeeg_region_summary_cli", rargs, root.u8string());
        if (rc != 0) {
          std::cerr << "[pipeline] region summary (bandratios) failed with exit code " << rc << "\n";
          return rc;
        }
      }
    }

    // --- Optional: write a local UI dashboard ---
    if (args.write_ui && !args.dry_run) {
      try {
        qeeg::UiDashboardArgs u;
        u.root = root.u8string();
        u.output_html = (root / "qeeg_ui.html").u8string();
        u.title = args.ui_title;
        u.scan_run_meta = true;

        u.toolbox = args.toolbox;

        // If the caller provided --bin-dir, use it; otherwise, when we need to
        // embed help, default to the directory containing this executable.
        if (!trim(args.bin_dir).empty()) {
          u.bin_dir = args.bin_dir;
        } else if (args.ui_embed_help) {
          u.bin_dir = self_dir(argv).u8string();
        }

        u.embed_help = args.ui_embed_help;
        u.scan_bin_dir = !trim(u.bin_dir).empty();

        qeeg::write_qeeg_tools_ui_html(u);
        std::cout << "Wrote UI dashboard: " << u.output_html << "\n";
        if (args.ui_open) {
          try_open_browser(std::filesystem::u8path(u.output_html));
        }
      } catch (const std::exception& e) {
        std::cerr << "[pipeline] Warning: failed to write qeeg_ui.html: " << e.what() << "\n";
      }
    }

    // --- Write pipeline run meta ---
    if (!args.dry_run) {
      const std::string meta_name = "pipeline_run_meta.json";
      const std::string meta_path = (root / meta_name).u8string();

      std::vector<std::string> outs;
      outs.push_back(meta_name);

      if (!args.skip_preprocess) {
        outs.push_back("01_preprocess/preprocess_run_meta.json");
      }
      outs.push_back("02_bandpower/bandpower_run_meta.json");
      if (!args.skip_bandratios) {
        outs.push_back("03_bandratios/bandratios_run_meta.json");
      }

      if (args.with_topomaps) {
        outs.push_back("04_topomaps_bandpower/topomap_run_meta.json");
        if (!args.skip_bandratios) {
          outs.push_back("05_topomaps_bandratios/topomap_run_meta.json");
        }
      }

      if (args.with_region_summary) {
        outs.push_back("06_regions_bandpower/region_summary_run_meta.json");
        if (!args.skip_bandratios) {
          outs.push_back("07_regions_bandratios/region_summary_run_meta.json");
        }
      }

      if (args.write_ui) {
        outs.push_back("qeeg_ui.html");
      }

      if (!write_run_meta_json(meta_path, "qeeg_pipeline_cli", root.u8string(), resolved_in.path, outs)) {
        std::cerr << "[pipeline] Warning: failed to write pipeline_run_meta.json: " << meta_path << "\n";
      }
    }

    if (args.dry_run) {
      std::cout << "Dry run complete. Planned workspace: " << root.u8string() << "\n";
    } else {
      std::cout << "Wrote workspace: " << root.u8string() << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
