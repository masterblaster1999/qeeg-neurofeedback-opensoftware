#include "qeeg/biquad.hpp"
#include "qeeg/channel_map.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/pattern.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/resample.hpp"
#include "qeeg/triggers.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string output_csv;
  std::string channel_map_path;
  std::string channel_map_template_out;
  std::string events_out_csv;
  std::string events_out_tsv;
  std::vector<std::string> extra_events;
  std::string nf_outdir;
  double fs_csv{0.0};
  double resample_hz{0.0};
  bool resample_antialias{false};
  double resample_antialias_cutoff_hz{0.0};
  bool resample_hold_auto{true};
  std::vector<std::string> resample_hold_patterns;
  bool write_time{true};
};

static void print_help() {
  std::cout
      << "qeeg_convert_cli\n\n"
      << "Convert EEG recordings to a simple, analysis-friendly CSV.\n"
      << "Intended for interoperability with BioTrace+/NeXus exports (EDF/BDF/ASCII).\n\n"
      << "Usage:\n"
      << "  qeeg_convert_cli --input <path> --output <out.csv> [options]\n"
      << "  qeeg_convert_cli --input <path> --channel-map-template <map.csv> [options]\n\n"
      << "Input formats:\n"
      << "  .edf/.edf+/.bdf/.bdf+   (recommended for BioTrace+ exports)\n"
      << "  .csv/.txt/.tsv/.asc     (ASCII exports)\n\n"
      << "Options:\n"
      << "  --fs <Hz>                    Sampling rate for CSV/TXT inputs (if no time column).\n"
      << "  --resample <Hz>              Resample channels to <Hz> before writing data outputs.\n"
      << "  --resample-antialias         When downsampling (target < input), apply a low-pass filter\n"
      << "                               before resampling continuous channels (helps reduce aliasing).\n"
      << "  --resample-antialias-cutoff <Hz>\n"
      << "                               Cutoff for antialias low-pass (default: 0.45 * target_fs).\n"
      << "  --resample-hold <glob>       Resample matching channels using zero-order hold (repeatable).\n"
      << "                               Useful for discrete trigger/status channels (avoids spurious codes).\n"
      << "  --no-resample-hold-auto      Disable auto-detection of trigger-like channels for hold resampling.\n"
      << "  --channel-map <path>         CSV mapping file to rename/drop channels.\n"
      << "                               Format: old,new   (or old=new). Use new=DROP to drop.\n"
      << "  --channel-map-template <path>Write a template mapping CSV for this recording (old,new).\n"
      << "  --events-out <path>          Write annotations/events to CSV (onset_sec,duration_sec,text).\n"
      << "  --events-out-tsv <path>      Write annotations/events to TSV (onset,duration,trial_type).\n"
      << "  --extra-events <file.{csv|tsv}> Merge additional events before writing (repeatable).\n"
      << "  --nf-outdir <dir|file>       Convenience: also merge nf_derived_events.tsv/.csv from a qeeg_nf_cli --outdir.\n"
      << "  --no-time                    Do not write a leading time column.\n"
      << "  -h, --help                   Show help.\n";
}

static void ensure_parent_dir(const std::string& path) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  const std::filesystem::path parent = p.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_path = argv[++i];
    } else if ((arg == "--output" || arg == "--out") && i + 1 < argc) {
      a.output_csv = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if ((arg == "--resample" || arg == "--resample-hz") && i + 1 < argc) {
      a.resample_hz = to_double(argv[++i]);
    } else if (arg == "--resample-antialias") {
      a.resample_antialias = true;
    } else if (arg == "--resample-antialias-cutoff" && i + 1 < argc) {
      a.resample_antialias_cutoff_hz = to_double(argv[++i]);
    } else if (arg == "--resample-hold" && i + 1 < argc) {
      a.resample_hold_patterns.push_back(argv[++i]);
    } else if (arg == "--no-resample-hold-auto") {
      a.resample_hold_auto = false;
    } else if (arg == "--channel-map" && i + 1 < argc) {
      a.channel_map_path = argv[++i];
    } else if (arg == "--channel-map-template" && i + 1 < argc) {
      a.channel_map_template_out = argv[++i];
    } else if (arg == "--events-out" && i + 1 < argc) {
      a.events_out_csv = argv[++i];
    } else if (arg == "--events-out-tsv" && i + 1 < argc) {
      a.events_out_tsv = argv[++i];
    } else if (arg == "--extra-events" && i + 1 < argc) {
      a.extra_events.push_back(argv[++i]);
    } else if (arg == "--nf-outdir" && i + 1 < argc) {
      a.nf_outdir = argv[++i];
    } else if (arg == "--no-time") {
      a.write_time = false;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }

  if (a.input_path.empty()) {
    print_help();
    throw std::runtime_error("Missing required --input");
  }

  if (a.output_csv.empty() && a.channel_map_template_out.empty() && a.events_out_csv.empty() && a.events_out_tsv.empty()) {
    print_help();
    throw std::runtime_error("Provide --output, --channel-map-template, and/or an --events-out option");
  }

  return a;
}

struct ResampleReport {
  std::size_t hold_channels{0};
  std::string auto_hold_channel;
  bool antialias_applied{false};
  double antialias_cutoff_hz{0.0};
};

static bool match_any_hold_pattern(const std::string& name,
                                  const std::vector<std::string>& patterns) {
  for (const auto& p : patterns) {
    if (wildcard_match(name, p, /*case_sensitive=*/false)) return true;
  }
  return false;
}

static ResampleReport resample_recording_inplace(EEGRecording* rec,
                                                 double target_fs_hz,
                                                 const Args& args) {
  ResampleReport rep;
  if (!rec) return rep;
  if (!(target_fs_hz > 0.0)) return rep;
  if (!(rec->fs_hz > 0.0)) {
    throw std::runtime_error("Cannot resample: input sampling rate is not known (fs_hz <= 0)");
  }
  const size_t in_len = rec->n_samples();
  if (in_len == 0 || rec->n_channels() == 0) return rep;

  if (std::fabs(target_fs_hz - rec->fs_hz) < 1e-12) return rep;

  const long double out_len_f = static_cast<long double>(in_len) * (static_cast<long double>(target_fs_hz) / static_cast<long double>(rec->fs_hz));
  long long out_ll = static_cast<long long>(std::llround(out_len_f));
  if (out_ll < 1) out_ll = 1;
  const size_t out_len = static_cast<size_t>(out_ll);

  if (out_len == in_len) {
    rec->fs_hz = target_fs_hz;
    return rep;
  }

  // Decide which channels should be resampled using zero-order-hold.
  //
  // Motivation: Trigger/status channels are often discrete-valued, and linear interpolation
  // can create spurious intermediate values that later get decoded as false events.
  std::vector<bool> hold;
  hold.resize(rec->n_channels(), false);

  // 1) User-provided wildcard patterns.
  if (!args.resample_hold_patterns.empty()) {
    for (std::size_t ch = 0; ch < rec->n_channels(); ++ch) {
      if (match_any_hold_pattern(rec->channel_names[ch], args.resample_hold_patterns)) {
        hold[ch] = true;
      }
    }
  }

  // 2) Auto-detect a trigger-like channel (conservative heuristic).
  if (args.resample_hold_auto) {
    const auto tr = extract_events_from_triggers_auto(*rec);
    rep.auto_hold_channel = tr.used_channel;
    if (!tr.used_channel.empty()) {
      const std::string want = normalize_channel_name(tr.used_channel);
      for (std::size_t ch = 0; ch < rec->n_channels(); ++ch) {
        if (normalize_channel_name(rec->channel_names[ch]) == want) {
          hold[ch] = true;
          break;
        }
      }
    }
  }

  for (bool b : hold) {
    if (b) ++rep.hold_channels;
  }

  // Optional: antialias filtering for downsampling.
  const double in_fs = rec->fs_hz;
  const bool do_antialias = args.resample_antialias && (target_fs_hz < in_fs);
  std::vector<BiquadCoeffs> antialias_stages;
  if (do_antialias) {
    const double ny_in = 0.5 * in_fs;
    const double ny_out = 0.5 * target_fs_hz;

    double fc = args.resample_antialias_cutoff_hz;
    if (!(fc > 0.0)) {
      // 0.45*target_fs = 0.9*Nyquist_out
      fc = 0.45 * target_fs_hz;
    }

    // Clamp to keep the lowpass well below both Nyquist frequencies.
    if (ny_out > 0.0) {
      fc = std::min(fc, 0.9 * ny_out);
    }
    if (ny_in > 0.0) {
      fc = std::min(fc, 0.9 * ny_in);
    }

    if (fc > 0.0 && std::isfinite(fc)) {
      antialias_stages.push_back(design_lowpass(in_fs, fc, 0.7071067811865476));
      rep.antialias_applied = true;
      rep.antialias_cutoff_hz = fc;
    }
  }

  for (std::size_t ch = 0; ch < rec->n_channels(); ++ch) {
    auto& x = rec->data[ch];
    if (hold[ch]) {
      x = resample_hold(x, out_len);
      continue;
    }

    if (!antialias_stages.empty()) {
      // Forward-backward for ~zero phase distortion (offline conversion).
      filtfilt_inplace(&x, antialias_stages);
    }
    x = resample_linear(x, out_len);
  }

  rec->fs_hz = target_fs_hz;
  return rep;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_template_out.empty()) {
      write_channel_map_template(args.channel_map_template_out, rec);
    }

    if (!args.channel_map_path.empty()) {
      const ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    if (args.resample_hz > 0.0) {
      if (rec.fs_hz <= 0.0) {
        throw std::runtime_error("--resample requires a valid sampling rate (fs_hz). For CSV/TXT inputs, pass --fs <Hz> if no time column is present.");
      }
      const size_t in_len = rec.n_samples();
      const double in_fs = rec.fs_hz;
      const ResampleReport rep = resample_recording_inplace(&rec, args.resample_hz, args);
      const size_t out_len = rec.n_samples();
      std::cerr << "Resampled recording: fs " << in_fs << " -> " << rec.fs_hz
                << " Hz, samples " << in_len << " -> " << out_len;
      if (rep.hold_channels > 0) {
        std::cerr << ", hold_channels=" << rep.hold_channels;
        if (!rep.auto_hold_channel.empty()) {
          std::cerr << " (auto: " << rep.auto_hold_channel << ")";
        }
      }
      if (rep.antialias_applied) {
        std::cerr << ", antialias_cutoff_hz=" << rep.antialias_cutoff_hz;
      }
      std::cerr << "\n";
    }

    // Merge additional events (e.g., NF-derived segments) into the recording.
    // Supports qeeg events CSV as well as BIDS-style events.tsv.
    std::vector<std::string> extra_paths = args.extra_events;
    if (!args.nf_outdir.empty()) {
      const auto p = find_nf_derived_events_table(args.nf_outdir);
      if (p) {
        extra_paths.push_back(*p);
      } else {
        std::cerr << "Warning: --nf-outdir provided, but nf_derived_events.tsv/.csv was not found in: "
                  << args.nf_outdir << "\n"
                  << "         Did you run qeeg_nf_cli with --export-derived-events or --biotrace-ui?\n";
      }
    }

    std::vector<AnnotationEvent> extra_all;
    for (const auto& p : extra_paths) {
      const auto extra = read_events_table(p);
      extra_all.insert(extra_all.end(), extra.begin(), extra.end());
    }
    merge_events(&rec.events, extra_all);

    if (!args.output_csv.empty()) {
      if (rec.fs_hz <= 0.0) {
        throw std::runtime_error(
            "Invalid sampling rate (fs_hz). If converting CSV/TXT inputs, pass --fs <Hz>.");
      }
      if (rec.n_channels() == 0 || rec.n_samples() == 0) {
        throw std::runtime_error("Empty recording (no channels or no samples).");
      }

      ensure_parent_dir(args.output_csv);
      write_recording_csv(args.output_csv, rec, args.write_time);
    }

    if (!args.events_out_csv.empty()) {
      ensure_parent_dir(args.events_out_csv);
      write_events_csv(args.events_out_csv, rec);
    }

    if (!args.events_out_tsv.empty()) {
      ensure_parent_dir(args.events_out_tsv);
      write_events_tsv(args.events_out_tsv, rec);
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
