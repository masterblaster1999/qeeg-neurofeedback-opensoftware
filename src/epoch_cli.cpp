#include "qeeg/annotations.hpp"
#include "qeeg/bandpower.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_epochs"};

  // If provided, override embedded EDF+/BDF+ annotations with this CSV.
  std::string events_csv;

  std::string band_spec; // empty => default
  size_t nperseg{1024};
  double overlap{0.5};

  // Epoch extraction
  double offset_sec{0.0};
  double window_sec{0.0}; // if >0, override event duration

  // Event selection
  std::string event_glob;
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
    << "  qeeg_epoch_cli --input file.csv --fs 250 --events events.csv --outdir out_epochs\n\n"
    << "Outputs:\n"
    << "  events.csv\n"
    << "  epoch_bandpowers.csv (long format: one row per event x channel x band)\n"
    << "  epoch_bandpowers_summary.csv (mean across processed epochs)\n\n"
    << "Options:\n"
    << "  --input PATH             Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                  Sampling rate for CSV\n"
    << "  --events PATH            Optional events CSV (overrides embedded EDF+/BDF+ annotations)\n"
    << "  --outdir DIR             Output directory (default: out_epochs)\n"
    << "  --bands SPEC             Band spec, e.g. 'alpha:8-12,beta:13-30' (default: built-in EEG bands)\n"
    << "  --nperseg N              Welch segment length (default: 1024)\n"
    << "  --overlap FRAC           Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --offset SEC             Epoch start offset relative to event onset (default: 0)\n"
    << "  --window SEC             Fixed epoch window length. If omitted, uses event duration.\n"
    << "  --event-glob PATTERN      Only keep events whose text matches PATTERN (* and ? wildcards)\n"
    << "  --event-regex REGEX       Alias for --event-glob (NOTE: this is a glob, not a full regex)\n"
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
      a.events_csv = argv[++i];
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
    } else if ((arg == "--event-glob" || arg == "--event-regex") && i + 1 < argc) {
      a.event_glob = argv[++i];
    } else if (arg == "--event-contains" && i + 1 < argc) {
      a.event_contains = argv[++i];
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
  return a;
}

static std::string csv_escape(const std::string& s) {
  const bool need_quote = (s.find_first_of(",\"\n\r") != std::string::npos);
  if (!need_quote) return s;
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::vector<AnnotationEvent> load_events_csv(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open events CSV: " + path);

  std::vector<AnnotationEvent> out;
  std::string line;
  size_t lineno = 0;
  while (std::getline(f, line)) {
    ++lineno;
    std::string raw = line;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::string t = trim(raw);
    if (t.empty()) continue;
    if (starts_with(t, "#")) continue;

    // Skip common header.
    const std::string low = to_lower(t);
    if (lineno == 1 && (low.find("onset") != std::string::npos)) {
      continue;
    }

    // CSV parsing: onset,duration,text
    // - quoted fields are supported (e.g., text may contain commas)
    // - for robustness, if unquoted commas exist in text and produce >3 columns,
    //   we join the remainder back into the text field.
    std::vector<std::string> cols = split_csv_row(raw, ',');
    if (cols.size() < 2) {
      throw std::runtime_error("Events CSV parse error at line " + std::to_string(lineno) + ": expected onset,duration[,text]");
    }
    for (auto& c : cols) c = trim(c);

    const double onset = to_double(cols[0]);
    const double dur = to_double(cols[1]);
    std::string text;
    if (cols.size() >= 3) {
      text = cols[2];
      for (size_t i = 3; i < cols.size(); ++i) {
        text += ",";
        text += cols[i];
      }
      text = trim(text);
    }

    AnnotationEvent ev;
    ev.onset_sec = onset;
    ev.duration_sec = dur;
    ev.text = text;
    out.push_back(std::move(ev));
  }

  std::sort(out.begin(), out.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
    if (a.duration_sec != b.duration_sec) return a.duration_sec < b.duration_sec;
    return a.text < b.text;
  });
  return out;
}

static bool wildcard_match(const std::string& text_in, const std::string& pattern_in, bool case_sensitive) {
  // Simple glob-style matching supporting:
  //  - '*' : matches any sequence (including empty)
  //  - '?' : matches exactly one character
  std::string text = text_in;
  std::string pattern = pattern_in;
  if (!case_sensitive) {
    text = to_lower(std::move(text));
    pattern = to_lower(std::move(pattern));
  }

  size_t t = 0;
  size_t p = 0;
  size_t star = std::string::npos;
  size_t match = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      ++p;
      match = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      ++match;
      t = match;
    } else {
      return false;
    }
  }

  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

static bool event_text_matches(const AnnotationEvent& ev, const Args& a) {
  if (!a.include_empty_text && trim(ev.text).empty()) return false;

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
    std::vector<AnnotationEvent> events = rec.events;
    if (!a.events_csv.empty()) {
      events = load_events_csv(a.events_csv);
    }

    if (events.empty()) {
      throw std::runtime_error(
        "No events found. If your file is CSV, provide --events. If EDF/BDF, make sure the file is EDF+/BDF+ with an Annotations channel.");
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

    // Write events.csv (the (possibly overridden) event list)
    {
      std::ofstream fe(a.outdir + "/events.csv");
      fe << "event_id,onset_sec,duration_sec,text\n";
      for (size_t i = 0; i < events.size(); ++i) {
        fe << i << "," << events[i].onset_sec << "," << events[i].duration_sec << "," << csv_escape(events[i].text) << "\n";
      }
    }

    std::ofstream fb(a.outdir + "/epoch_bandpowers.csv");
    fb << "event_id,onset_sec,duration_sec,epoch_start_sec,epoch_end_sec,text,channel,band,power\n";

    // For summary mean across epochs.
    struct Acc {
      double sum{0.0};
      size_t n{0};
    };
    std::unordered_map<std::string, Acc> accum; // key = channel|band

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

      // For each channel: Welch PSD + integrate bands.
      for (size_t ch = 0; ch < rec.channel_names.size(); ++ch) {
        std::vector<float> seg;
        seg.assign(rec.data[ch].begin() + static_cast<std::ptrdiff_t>(i0),
                   rec.data[ch].begin() + static_cast<std::ptrdiff_t>(i1));
        if (seg.empty()) continue;

        const PsdResult psd = welch_psd(seg, fs, wopt);
        for (const auto& b : bands) {
          const double p = integrate_bandpower(psd, b.fmin_hz, b.fmax_hz);
          fb << ei << "," << ev.onset_sec << "," << ev.duration_sec << "," << start_c << "," << end_c << ","
             << csv_escape(ev.text) << "," << rec.channel_names[ch] << "," << b.name << "," << p << "\n";

          const std::string key = to_lower(rec.channel_names[ch]) + "|" + to_lower(b.name);
          Acc& ac = accum[key];
          ac.sum += p;
          ac.n += 1;
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

    std::cout << "Loaded " << rec.channel_names.size() << " channels, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Found " << events.size() << " events (exported to events.csv)\n";
    std::cout << "Processed " << n_used_events << " matching events\n";
    std::cout << "Wrote epoch_bandpowers.csv and epoch_bandpowers_summary.csv to: " << a.outdir << "\n";

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
