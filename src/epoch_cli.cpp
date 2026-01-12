#include "qeeg/annotations.hpp"
#include "qeeg/baseline.hpp"
#include "qeeg/bandpower.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/pattern.hpp"
#include "qeeg/welch_psd.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_epochs"};

  // If provided, override embedded EDF+/BDF+ annotations with this table.
  // Supported formats: qeeg events CSV (onset_sec,duration_sec,text) or BIDS events TSV (onset,duration,trial_type).
  std::string events_path;

  // Additional events tables to merge (repeatable). Useful for overlaying/combining
  // multiple sources (e.g., BIDS events.tsv + nf_cli derived segments).
  std::vector<std::string> extra_events_paths;

  // If provided, auto-merge nf_cli derived events (nf_derived_events.tsv/.csv) from this output folder.
  std::string nf_outdir;

  std::string band_spec; // empty => default
  size_t nperseg{1024};
  double overlap{0.5};

  // Epoch extraction
  double offset_sec{0.0};
  double window_sec{0.0}; // if >0, override event duration

  // Optional baseline normalization (per-event, per-channel, per-band).
  // If baseline_sec > 0, we compute bandpower on a baseline window that ends
  // at (epoch_start_sec - baseline_gap_sec).
  double baseline_sec{0.0};
  double baseline_gap_sec{0.0};
  std::string baseline_mode{"rel"}; // ratio|rel|logratio|db

  // Event selection (choose at most one; if multiple are specified, the last one wins)
  std::string event_glob;
  std::string event_regex;
  std::optional<std::regex> event_regex_compiled;
  std::string event_contains;
  bool case_sensitive{false};
  bool include_empty_text{false};
  size_t max_events{0}; // 0 = all

  // Optional preprocessing
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};

  double fs_csv{0.0};
};

static void print_help() {
  std::cout
    << "qeeg_epoch_cli (event/epoch bandpower feature extraction)\n\n"
    << "Usage:\n"
    << "  qeeg_epoch_cli --input file.edf --outdir out_epochs\n"
    << "  qeeg_epoch_cli --input file.edf --outdir out_epochs --event-contains Stim --window 1.0\n"
    << "  qeeg_epoch_cli --input file.edf --nf-outdir nf_out --outdir out_epochs --event-contains NF:Reward\n"
    << "  qeeg_epoch_cli --input file.csv --fs 250 --events events.csv --outdir out_epochs\n\n"
    << "Outputs:\n"
    << "  events.csv                (event_id + onset_sec + duration_sec + text)\n"
    << "  events_table.csv          (qeeg events table: onset_sec,duration_sec,text)\n"
    << "  events_table.tsv          (BIDS-style events table: onset,duration,trial_type)\n"
    << "  epoch_bandpowers.csv      (long format: one row per event x channel x band)\n"
    << "  epoch_bandpowers_summary.csv (mean across processed epochs)\n"
    << "  (optional) epoch_bandpowers_norm.csv (baseline-normalized values; when --baseline is used)\n"
    << "  (optional) epoch_bandpowers_norm_summary.csv (mean baseline-normalized values; when --baseline is used)\n\n"
    << "Options:\n"
    << "  --input PATH             Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                  Sampling rate for CSV\n"
    << "  --events PATH            Optional events table (CSV or TSV). Overrides embedded EDF+/BDF+ annotations.\n"
    << "  --extra-events PATH      Additional events table(s) to merge (repeatable).\n"
    << "  --nf-outdir PATH         Auto-merge nf_cli derived events (nf_derived_events.tsv/.csv) from this folder.\n"
    << "  --outdir DIR             Output directory (default: out_epochs)\n"
    << "  --bands SPEC             Band spec, e.g. 'alpha:8-12,beta:13-30' (default: built-in EEG bands)\n"
    << "  --nperseg N              Welch segment length (default: 1024)\n"
    << "  --overlap FRAC           Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --offset SEC             Epoch start offset relative to event onset (default: 0)\n"
    << "  --window SEC             Fixed epoch window length. If omitted, uses event duration.\n"
    << "  --baseline SEC           Baseline duration ending at epoch start (default: 0; disabled)\n"
    << "  --baseline-gap SEC       Gap between baseline end and epoch start (default: 0)\n"
    << "  --baseline-mode MODE     Baseline normalization: ratio|rel|logratio|db (default: rel)\n"
    << "  --event-glob PATTERN      Only keep events whose text matches PATTERN (* and ? wildcards)\n"
    << "  --event-regex REGEX       Only keep events whose text matches REGEX (ECMAScript; std::regex_search)\n"
    << "  --event-contains STR      Only keep events whose text contains STR\n"
    << "  --case-sensitive          Make --event-contains matching case-sensitive\n"
    << "  --include-empty           Include events with empty text (not recommended)\n"
    << "  --max-events N            Process at most N matching events (default: all)\n"
    << "  --average-reference       Apply common average reference across channels\n"
    << "  --notch HZ                Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q               Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI          Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase              Offline: forward-backward filtering (less phase distortion)\n"
    << "  -h, --help                Show this help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_path = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--events" && i + 1 < argc) {
      a.events_path = argv[++i];
    } else if (arg == "--extra-events" && i + 1 < argc) {
      a.extra_events_paths.push_back(argv[++i]);
    } else if (arg == "--nf-outdir" && i + 1 < argc) {
      a.nf_outdir = argv[++i];
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--offset" && i + 1 < argc) {
      a.offset_sec = to_double(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_sec = to_double(argv[++i]);
    } else if (arg == "--baseline" && i + 1 < argc) {
      a.baseline_sec = to_double(argv[++i]);
    } else if (arg == "--baseline-gap" && i + 1 < argc) {
      a.baseline_gap_sec = to_double(argv[++i]);
    } else if (arg == "--baseline-mode" && i + 1 < argc) {
      a.baseline_mode = argv[++i];
    } else if (arg == "--event-glob" && i + 1 < argc) {
      a.event_glob = argv[++i];
      a.event_regex.clear();
      a.event_regex_compiled.reset();
      a.event_contains.clear();
    } else if ((arg == "--event-regex" || arg == "--event-re") && i + 1 < argc) {
      a.event_regex = argv[++i];
      a.event_glob.clear();
      a.event_contains.clear();
      a.event_regex_compiled.reset();
    } else if (arg == "--event-contains" && i + 1 < argc) {
      a.event_contains = argv[++i];
      a.event_glob.clear();
      a.event_regex.clear();
      a.event_regex_compiled.reset();
    } else if (arg == "--case-sensitive") {
      a.case_sensitive = true;
    } else if (arg == "--include-empty") {
      a.include_empty_text = true;
    } else if (arg == "--max-events" && i + 1 < argc) {
      a.max_events = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--average-reference") {
      a.average_reference = true;
    } else if (arg == "--notch" && i + 1 < argc) {
      a.notch_hz = to_double(argv[++i]);
    } else if (arg == "--notch-q" && i + 1 < argc) {
      a.notch_q = to_double(argv[++i]);
    } else if (arg == "--bandpass" && i + 2 < argc) {
      a.bandpass_low_hz = to_double(argv[++i]);
      a.bandpass_high_hz = to_double(argv[++i]);
    } else if (arg == "--zero-phase") {
      a.zero_phase = true;
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  if (!a.event_regex.empty()) {
    a.event_regex_compiled = compile_regex(a.event_regex, a.case_sensitive);
  }

  return a;
}

static bool event_text_matches(const AnnotationEvent& ev, const Args& a) {
  if (!a.include_empty_text && trim(ev.text).empty()) return false;

  if (a.event_regex_compiled.has_value()) {
    return std::regex_search(ev.text, *a.event_regex_compiled);
  }
  if (!a.event_glob.empty()) {
    return wildcard_match(ev.text, a.event_glob, a.case_sensitive);
  }
  if (!a.event_contains.empty()) {
    if (a.case_sensitive) {
      return ev.text.find(a.event_contains) != std::string::npos;
    }
    return to_lower(ev.text).find(to_lower(a.event_contains)) != std::string::npos;
  }
  return true;
}

static size_t time_to_index_floor(double t_sec, double fs_hz) {
  if (t_sec <= 0.0) return 0;
  return static_cast<size_t>(std::floor(t_sec * fs_hz + 1e-9));
}

static size_t time_to_index_ceil(double t_sec, double fs_hz) {
  if (t_sec <= 0.0) return 0;
  return static_cast<size_t>(std::ceil(t_sec * fs_hz - 1e-9));
}

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    if (a.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (ends_with(to_lower(a.input_path), ".csv") && a.fs_csv <= 0.0) {
      throw std::runtime_error("CSV input requires --fs");
    }

    EEGRecording rec = read_recording_auto(a.input_path, a.fs_csv);

    // --- Events: base + optional overrides + optional merges ---
    std::vector<AnnotationEvent> events = rec.events;
    if (!a.events_path.empty()) {
      events = read_events_table(a.events_path);
    }

    std::vector<AnnotationEvent> extra;
    for (const auto& p : a.extra_events_paths) {
      const auto e = read_events_table(p);
      extra.insert(extra.end(), e.begin(), e.end());
    }

    if (!a.nf_outdir.empty()) {
      const auto nf_tbl = find_nf_derived_events_table(a.nf_outdir);
      if (nf_tbl.has_value()) {
        const auto e = read_events_table(*nf_tbl);
        extra.insert(extra.end(), e.begin(), e.end());
      } else {
        const auto d = normalize_nf_outdir_path(a.nf_outdir);
        std::cerr << "Warning: --nf-outdir provided but no nf_derived_events.tsv/.csv found in: "
                  << d.u8string() << "\n";
      }
    }

    merge_events(&events, extra);

    if (events.empty()) {
      throw std::runtime_error(
        "No events found. If your file is CSV, provide --events. If EDF/BDF, make sure the file is EDF+/BDF+ with an Annotations channel.\n"
        "You can also merge nf_cli-derived segments via --nf-outdir.");
    }

    // Optional preprocessing (offline).
    PreprocessOptions popt;
    popt.average_reference = a.average_reference;
    popt.notch_hz = a.notch_hz;
    popt.notch_q = a.notch_q;
    popt.bandpass_low_hz = a.bandpass_low_hz;
    popt.bandpass_high_hz = a.bandpass_high_hz;
    popt.zero_phase = a.zero_phase;

    if (popt.average_reference || popt.notch_hz > 0.0 || (popt.bandpass_low_hz > 0.0 && popt.bandpass_high_hz > 0.0)) {
      preprocess_recording_inplace(rec, popt);
    }

    const std::vector<BandDefinition> bands = parse_band_spec(a.band_spec);

    WelchOptions wopt;
    wopt.nperseg = a.nperseg;
    wopt.overlap_fraction = a.overlap;

    ensure_directory(a.outdir);

    // Export event lists for interoperability.
    {
      // Richer table with stable row ids.
      std::ofstream fe(a.outdir + "/events.csv");
      fe << "event_id,onset_sec,duration_sec,text\n";
      for (size_t i = 0; i < events.size(); ++i) {
        fe << i << "," << events[i].onset_sec << "," << events[i].duration_sec << "," << csv_escape(events[i].text) << "\n";
      }

      // Standard tables for other tools (qeeg CSV and BIDS TSV).
      write_events_csv(a.outdir + "/events_table.csv", events);
      write_events_tsv(a.outdir + "/events_table.tsv", events);
    }

    std::ofstream fb(a.outdir + "/epoch_bandpowers.csv");
    fb << "event_id,onset_sec,duration_sec,epoch_start_sec,epoch_end_sec,text,channel,band,power\n";

    // For summary mean across epochs.
    struct Acc {
      double sum{0.0};
      size_t n{0};
    };
    std::unordered_map<std::string, Acc> accum; // key = channel|band

    const bool do_baseline = (a.baseline_sec > 0.0);
    BaselineNormMode baseline_mode = BaselineNormMode::RelativeChange;
    if (do_baseline) {
      if (a.baseline_gap_sec < 0.0) {
        throw std::runtime_error("--baseline-gap must be >= 0");
      }
      if (!parse_baseline_norm_mode(a.baseline_mode, &baseline_mode)) {
        throw std::runtime_error("Invalid --baseline-mode: " + a.baseline_mode + " (expected ratio|rel|logratio|db)");
      }
    }

    const std::string baseline_mode_str = baseline_mode_name(baseline_mode);

    std::ofstream fnorm;
    std::unordered_map<std::string, Acc> accum_norm; // key = channel|band
    if (do_baseline) {
      fnorm.open(a.outdir + "/epoch_bandpowers_norm.csv");
      fnorm << "event_id,onset_sec,duration_sec,epoch_start_sec,epoch_end_sec,baseline_start_sec,baseline_end_sec,text,channel,band,epoch_power,baseline_power,mode,norm_value\n";
    }

    const double fs = rec.fs_hz;
    const size_t total_samples = rec.n_samples();
    const double total_dur = (fs > 0.0) ? (static_cast<double>(total_samples) / fs) : 0.0;

    size_t n_used_events = 0;
    for (size_t ei = 0; ei < events.size(); ++ei) {
      const AnnotationEvent& ev = events[ei];
      if (!event_text_matches(ev, a)) continue;
      if (a.max_events > 0 && n_used_events >= a.max_events) break;

      const double start_sec = ev.onset_sec + a.offset_sec;
      double win_sec = 0.0;
      if (a.window_sec > 0.0) {
        win_sec = a.window_sec;
      } else {
        win_sec = ev.duration_sec;
      }
      if (win_sec <= 0.0) continue;
      double end_sec = start_sec + win_sec;

      // Clamp.
      if (start_sec >= total_dur) continue;
      if (end_sec <= 0.0) continue;
      const double start_c = std::max(0.0, start_sec);
      const double end_c = std::min(total_dur, end_sec);
      if (end_c <= start_c) continue;

      const size_t i0 = time_to_index_floor(start_c, fs);
      const size_t i1 = std::min(total_samples, time_to_index_ceil(end_c, fs));
      if (i1 <= i0 + 1) continue;

      const double nan = std::numeric_limits<double>::quiet_NaN();
      double baseline_start_c = 0.0;
      double baseline_end_c = 0.0;
      size_t ib0 = 0;
      size_t ib1 = 0;
      bool baseline_valid = false;
      if (do_baseline) {
        const double baseline_end = start_c - a.baseline_gap_sec;
        const double baseline_start = baseline_end - a.baseline_sec;
        baseline_start_c = std::max(0.0, baseline_start);
        baseline_end_c = std::min(total_dur, std::max(0.0, baseline_end));
        if (baseline_end_c > baseline_start_c) {
          ib0 = time_to_index_floor(baseline_start_c, fs);
          ib1 = std::min(total_samples, time_to_index_ceil(baseline_end_c, fs));
          baseline_valid = (ib1 > ib0 + 1);
        }
      }

      // For each channel: Welch PSD + integrate bands.
      for (size_t ch = 0; ch < rec.channel_names.size(); ++ch) {
        std::vector<float> seg;
        seg.assign(rec.data[ch].begin() + static_cast<std::ptrdiff_t>(i0),
                   rec.data[ch].begin() + static_cast<std::ptrdiff_t>(i1));
        if (seg.empty()) continue;

        const PsdResult psd = welch_psd(seg, fs, wopt);

        PsdResult psd_baseline;
        bool have_baseline = false;
        if (do_baseline && baseline_valid) {
          std::vector<float> seg_base;
          seg_base.assign(rec.data[ch].begin() + static_cast<std::ptrdiff_t>(ib0),
                          rec.data[ch].begin() + static_cast<std::ptrdiff_t>(ib1));
          if (seg_base.size() > 1) {
            psd_baseline = welch_psd(seg_base, fs, wopt);
            have_baseline = true;
          }
        }

        for (const auto& b : bands) {
          const double p = integrate_bandpower(psd, b.fmin_hz, b.fmax_hz);
          fb << ei << "," << ev.onset_sec << "," << ev.duration_sec << "," << start_c << "," << end_c << ","
             << csv_escape(ev.text) << "," << rec.channel_names[ch] << "," << b.name << "," << p << "\n";

          const std::string key = to_lower(rec.channel_names[ch]) + "|" + to_lower(b.name);
          Acc& ac = accum[key];
          ac.sum += p;
          ac.n += 1;

          if (do_baseline) {
            double p_base = nan;
            double norm = nan;
            if (have_baseline) {
              p_base = integrate_bandpower(psd_baseline, b.fmin_hz, b.fmax_hz);
              norm = baseline_normalize(p, p_base, baseline_mode);
            }
            fnorm << ei << "," << ev.onset_sec << "," << ev.duration_sec << "," << start_c << "," << end_c
                  << "," << baseline_start_c << "," << baseline_end_c << "," << csv_escape(ev.text) << ","
                  << rec.channel_names[ch] << "," << b.name << "," << p << "," << p_base << "," << baseline_mode_str
                  << "," << norm << "\n";

            if (std::isfinite(norm)) {
              Acc& an = accum_norm[key];
              an.sum += norm;
              an.n += 1;
            }
          }
        }
      }

      ++n_used_events;
    }

    // Summary CSV
    {
      std::ofstream fsu(a.outdir + "/epoch_bandpowers_summary.csv");
      fsu << "channel,band,mean_power,n_epochs\n";
      // Stable-ish output ordering: sort keys.
      std::vector<std::string> keys;
      keys.reserve(accum.size());
      for (const auto& kv : accum) keys.push_back(kv.first);
      std::sort(keys.begin(), keys.end());
      for (const auto& k : keys) {
        const auto& ac = accum[k];
        if (ac.n == 0) continue;
        const auto parts = split(k, '|');
        const std::string ch = (parts.size() >= 1) ? parts[0] : k;
        const std::string band = (parts.size() >= 2) ? parts[1] : "";
        fsu << ch << "," << band << "," << (ac.sum / static_cast<double>(ac.n)) << "," << ac.n << "\n";
      }
    }

    // Optional baseline-normalized summary CSV
    if (do_baseline) {
      std::ofstream fsu(a.outdir + "/epoch_bandpowers_norm_summary.csv");
      fsu << "channel,band,mode,mean_value,n_epochs\n";

      std::vector<std::string> keys;
      keys.reserve(accum_norm.size());
      for (const auto& kv : accum_norm) keys.push_back(kv.first);
      std::sort(keys.begin(), keys.end());
      for (const auto& k : keys) {
        const auto& ac = accum_norm[k];
        if (ac.n == 0) continue;
        const auto parts = split(k, '|');
        const std::string ch = (parts.size() >= 1) ? parts[0] : k;
        const std::string band = (parts.size() >= 2) ? parts[1] : "";
        fsu << ch << "," << band << "," << baseline_mode_str << "," << (ac.sum / static_cast<double>(ac.n))
            << "," << ac.n << "\n";
      }
    }

    std::cout << "Loaded " << rec.channel_names.size() << " channels, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Found " << events.size() << " events (exported to events.csv, events_table.csv, events_table.tsv)\n";
    std::cout << "Processed " << n_used_events << " matching events\n";
    std::cout << "Wrote epoch_bandpowers.csv and epoch_bandpowers_summary.csv to: " << a.outdir << "\n";
    if (do_baseline) {
      std::cout << "Wrote epoch_bandpowers_norm.csv and epoch_bandpowers_norm_summary.csv (mode="
                << baseline_mode_str << ") to: " << a.outdir << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
