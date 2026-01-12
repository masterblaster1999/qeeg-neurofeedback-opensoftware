#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/nf_session.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/robust_stats.hpp"
#include "qeeg/svg_utils.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out"};
  std::string output_name{"traces.svg"};

  // Channel selection: comma-separated list (case-insensitive).
  // Empty => first N channels.
  std::string channels;
  int default_n_channels{8};

  // Time window
  double start_sec{0.0};
  double duration_sec{10.0};

  // For CSV inputs
  double fs_csv{0.0};

  // Rendering
  int width_px{1200};
  int row_height_px{80};
  int margin_left_px{120};
  int margin_right_px{20};
  int margin_top_px{20};
  int margin_bottom_px{50};

  // Scaling
  bool autoscale{false};
  double uv_per_row{200.0}; // peak-to-peak range mapped to ~80% of row height

  // Performance
  int max_points{5000};

  // Events
  bool draw_events{true};
  bool draw_event_labels{true};
  int max_event_labels{40};

  // Optional extra events file(s) to overlay (CSV or TSV). This is useful for
  // plotting nf_cli-derived events (reward/artifacts) or BIDS events.tsv.
  std::vector<std::string> extra_events;

  // Convenience: point to an nf_cli output directory (created by --outdir).
  // If <dir>/nf_derived_events.tsv/.csv exists, it is auto-merged for drawing.
  std::string nf_outdir;

  // Segments (duration annotations) - BioTrace+ style
  bool draw_segments{true};
  bool draw_segment_labels{true};
  bool min_segment_sec_user_set{false};
  double min_segment_sec{0.5};
  int segment_band_px{14};
  int max_segment_labels{30};

  // Preprocess
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static void print_help() {
  std::cout
    << "qeeg_trace_plot_cli (stacked time-series trace plot to SVG)\n\n"
    << "Usage:\n"
    << "  qeeg_trace_plot_cli --input file.edf --outdir out --channels Cz,Fz,Pz\n"
    << "  qeeg_trace_plot_cli --input file.csv --fs 250 --outdir out --channels 1,2,3\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV (required)\n"
    << "  --fs HZ                 Sampling rate for CSV (optional if time column exists)\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --output NAME           Output SVG filename under outdir (default: traces.svg)\n"
    << "  --channels LIST         Comma-separated channel names or indices (default: first N)\n"
    << "  --n N                   If --channels is empty, plot the first N channels (default: 8)\n"
    << "  --start SEC             Start time in seconds (default: 0)\n"
    << "  --duration SEC          Duration in seconds (default: 10)\n"
    << "  --width PX              SVG width in pixels (default: 1200)\n"
    << "  --row-height PX         Per-channel row height (default: 80)\n"
    << "  --uv-per-row UV         Peak-to-peak uV range per channel row (default: 200)\n"
    << "  --autoscale             Auto-scale each channel row using a robust percentile\n"
    << "  --max-points N          Max points per channel polyline (default: 5000)\n"
    << "  --no-events             Do not draw EDF+/BDF+ events/annotations\n"
    << "  --no-event-labels       Draw event lines but omit text labels\n"
    << "  --max-event-labels N    Limit number of event labels (default: 40)\n"
    << "  --events FILE           Load additional events from a CSV/TSV and overlay them\n"
    << "                         (repeatable; supports qeeg events CSV or BIDS events.tsv)\n"
    << "  --nf-outdir DIR         Convenience: overlay nf_cli derived events from DIR/nf_derived_events.tsv/.csv\n"
    << "  --no-segments           Do not draw duration annotations as segment bars\n"
    << "  --min-segment-sec SEC   Minimum duration (s) to treat annotation as a segment (default: 0.5)\n"
    << "  --segment-band-px PX    Height of segment band in px (default: 14)\n"
    << "  --no-segment-labels     Draw segment bars but omit text labels\n"
    << "  --max-segment-labels N  Limit number of segment labels (default: 30)\n"
    << "  --average-reference     Apply common average reference across channels\n"
    << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q             Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase            Offline: forward-backward filtering\n"
    << "  -h, --help              Show this help\n";
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
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--output" && i + 1 < argc) {
      a.output_name = argv[++i];
    } else if (arg == "--channels" && i + 1 < argc) {
      a.channels = argv[++i];
    } else if (arg == "--n" && i + 1 < argc) {
      a.default_n_channels = to_int(argv[++i]);
    } else if (arg == "--start" && i + 1 < argc) {
      a.start_sec = to_double(argv[++i]);
    } else if (arg == "--duration" && i + 1 < argc) {
      a.duration_sec = to_double(argv[++i]);
    } else if (arg == "--width" && i + 1 < argc) {
      a.width_px = to_int(argv[++i]);
    } else if (arg == "--row-height" && i + 1 < argc) {
      a.row_height_px = to_int(argv[++i]);
    } else if (arg == "--uv-per-row" && i + 1 < argc) {
      a.uv_per_row = to_double(argv[++i]);
    } else if (arg == "--autoscale") {
      a.autoscale = true;
    } else if (arg == "--max-points" && i + 1 < argc) {
      a.max_points = to_int(argv[++i]);
    } else if (arg == "--no-events") {
      a.draw_events = false;
    } else if (arg == "--no-event-labels") {
      a.draw_event_labels = false;
    } else if (arg == "--events" && i + 1 < argc) {
      a.extra_events.push_back(argv[++i]);
    } else if (arg == "--nf-outdir" && i + 1 < argc) {
      a.nf_outdir = argv[++i];
    } else if (arg == "--max-event-labels" && i + 1 < argc) {
      a.max_event_labels = to_int(argv[++i]);
    } else if (arg == "--no-segments") {
      a.draw_segments = false;
    } else if (arg == "--min-segment-sec" && i + 1 < argc) {
      a.min_segment_sec = to_double(argv[++i]);
      a.min_segment_sec_user_set = true;
    } else if (arg == "--segment-band-px" && i + 1 < argc) {
      a.segment_band_px = to_int(argv[++i]);
    } else if (arg == "--no-segment-labels") {
      a.draw_segment_labels = false;
    } else if (arg == "--max-segment-labels" && i + 1 < argc) {
      a.max_segment_labels = to_int(argv[++i]);
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
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static int find_channel_index(const std::vector<std::string>& names, const std::string& want) {
  if (names.empty()) return -1;
  if (want.empty()) return -1;

  const std::string lw = normalize_channel_name(want);
  for (size_t i = 0; i < names.size(); ++i) {
    if (normalize_channel_name(names[i]) == lw) return static_cast<int>(i);
  }

  // Accept numeric index (0-based or 1-based)
  bool all_digits = true;
  for (char c : want) {
    if (!(c >= '0' && c <= '9')) { all_digits = false; break; }
  }
  if (all_digits) {
    int idx = to_int(want);
    if (idx >= 0 && idx < static_cast<int>(names.size())) return idx;
    if (idx >= 1 && idx <= static_cast<int>(names.size())) return idx - 1;
  }

  return -1;
}

static double choose_time_tick(double duration_sec) {
  if (!(duration_sec > 0.0)) return 1.0;
  if (duration_sec <= 5.0) return 0.5;
  if (duration_sec <= 12.0) return 1.0;
  if (duration_sec <= 30.0) return 2.0;
  if (duration_sec <= 90.0) return 5.0;
  if (duration_sec <= 300.0) return 10.0;
  return 30.0;
}

static std::vector<std::string> palette() {
  // A small, readable categorical palette (no external deps).
  return {
    "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
    "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf"
  };
}

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.duration_sec <= 0.0) throw std::runtime_error("--duration must be > 0");
    if (args.width_px < 300) throw std::runtime_error("--width too small");
    if (args.row_height_px < 20) throw std::runtime_error("--row-height too small");
    if (args.default_n_channels < 1) args.default_n_channels = 1;
    if (args.max_points < 200) args.max_points = 200;

    if (args.min_segment_sec < 0.0) args.min_segment_sec = 0.0;
    if (args.segment_band_px < 0) args.segment_band_px = 0;
    if (args.max_segment_labels < 0) args.max_segment_labels = 0;

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");
    if (rec.n_channels() == 0 || rec.n_samples() < 8) throw std::runtime_error("Recording too small");

    // Optional extra events overlay (CSV or TSV). These are merged into any
    // events parsed from the source file (EDF+/BDF+ annotations or CSV marker columns).
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


    // When overlaying an external events table (qeeg events CSV / BIDS events.tsv),
    // treat *any* duration > 0 as a segment by default. This is especially useful
    // for nf_cli derived segments (e.g., short reward bursts at the update rate).
    if (!extra_paths.empty() && !args.min_segment_sec_user_set) {
      args.min_segment_sec = 0.0;
    }

    std::vector<AnnotationEvent> extra_all;
    for (const auto& p : extra_paths) {
      const auto extra = read_events_table(p);
      extra_all.insert(extra_all.end(), extra.begin(), extra.end());
    }
    // Also normalizes + de-duplicates source events for deterministic rendering.
    merge_events(&rec.events, extra_all);

    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    preprocess_recording_inplace(rec, popt);

    const double file_dur = static_cast<double>(rec.n_samples()) / rec.fs_hz;
    const double start_sec = std::max(0.0, std::min(args.start_sec, std::max(0.0, file_dur - 1e-9)));
    const double end_sec = std::min(file_dur, start_sec + args.duration_sec);
    if (!(end_sec > start_sec)) throw std::runtime_error("Empty time window");

    size_t start_idx = static_cast<size_t>(std::llround(start_sec * rec.fs_hz));
    size_t end_idx = static_cast<size_t>(std::llround(end_sec * rec.fs_hz));
    if (start_idx >= rec.n_samples()) start_idx = rec.n_samples() - 1;
    if (end_idx > rec.n_samples()) end_idx = rec.n_samples();
    if (end_idx <= start_idx + 1) throw std::runtime_error("Selected time window too small");

    // Channel selection
    std::vector<int> ch_indices;
    std::vector<std::string> ch_names;

    if (!args.channels.empty()) {
      for (const std::string& tok : split(args.channels, ',')) {
        const std::string t = trim(tok);
        if (t.empty()) continue;
        int idx = find_channel_index(rec.channel_names, t);
        if (idx < 0) throw std::runtime_error("Channel not found: " + t);
        ch_indices.push_back(idx);
        ch_names.push_back(rec.channel_names[static_cast<size_t>(idx)]);
      }
    } else {
      const int n = std::min<int>(args.default_n_channels, static_cast<int>(rec.n_channels()));
      for (int i = 0; i < n; ++i) {
        ch_indices.push_back(i);
        ch_names.push_back(rec.channel_names[static_cast<size_t>(i)]);
      }
    }

    const int n_ch = static_cast<int>(ch_indices.size());
    if (n_ch <= 0) throw std::runtime_error("No channels selected");

    const int plot_width = args.width_px - args.margin_left_px - args.margin_right_px;
    const int plot_height = n_ch * args.row_height_px;
    const int height_px = args.margin_top_px + plot_height + args.margin_bottom_px;

    const int seg_band_px = (args.draw_segments ? std::max(0, args.segment_band_px) : 0);
    const int seg_y0 = args.margin_top_px + plot_height + 4;
    const int tick_label_y = args.margin_top_px + plot_height + seg_band_px + 20;

    const double duration = end_sec - start_sec;
    const auto colors = palette();

    auto to_x = [&](double t_sec) {
      const double u = (t_sec - start_sec) / duration;
      return static_cast<double>(args.margin_left_px) + u * static_cast<double>(plot_width);
    };

    // Begin SVG
    const std::string out_svg = args.outdir + "/" + args.output_name;
    std::ofstream f(out_svg);
    if (!f) throw std::runtime_error("Failed to open output: " + out_svg);

    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    f << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" "
      << "width=\"" << args.width_px << "\" height=\"" << height_px << "\" "
      << "viewBox=\"0 0 " << args.width_px << " " << height_px << "\">\n";

    f << "<rect x=\"0\" y=\"0\" width=\"" << args.width_px << "\" height=\"" << height_px
      << "\" fill=\"white\"/>\n";

    // Definitions (pattern fill for artifact segments)
    f << "<defs>\n";
    f << "  <pattern id=\"artifactHatch\" patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" patternTransform=\"rotate(45)\">\n";
    f << "    <line x1=\"0\" y1=\"0\" x2=\"0\" y2=\"6\" stroke=\"#cc0000\" stroke-width=\"2\" opacity=\"0.45\"/>\n";
    f << "  </pattern>\n";
    f << "</defs>\n";

    // Grid + time axis ticks
    const double tick = choose_time_tick(duration);
    const double t0 = std::floor(start_sec / tick) * tick;
    f << "<g stroke=\"#e6e6e6\" stroke-width=\"1\">\n";
    for (double t = t0; t <= end_sec + 1e-9; t += tick) {
      if (t < start_sec - 1e-9) continue;
      const double x = to_x(t);
      f << "<line x1=\"" << x << "\" y1=\"" << args.margin_top_px
        << "\" x2=\"" << x << "\" y2=\"" << (args.margin_top_px + plot_height) << "\"/>\n";
    }
    f << "</g>\n";

    // Horizontal separators (per channel row)
    f << "<g stroke=\"#f0f0f0\" stroke-width=\"1\">\n";
    for (int i = 0; i <= n_ch; ++i) {
      const int y = args.margin_top_px + i * args.row_height_px;
      f << "<line x1=\"" << args.margin_left_px << "\" y1=\"" << y
        << "\" x2=\"" << (args.margin_left_px + plot_width) << "\" y2=\"" << y << "\"/>\n";
    }
    f << "</g>\n";

    // Segment band (duration annotations) - BioTrace+ style
    if (args.draw_segments && seg_band_px > 0 && !rec.events.empty()) {
      auto is_artifact = [&](const std::string& s) {
        const std::string t = to_lower(s);
        return (t.find("artifact") != std::string::npos) || (t.find("artefact") != std::string::npos);
      };
      const std::vector<std::string> seg_colors = {
        "#93c5fd", "#a7f3d0", "#fcd34d", "#fca5a5", "#d8b4fe",
        "#fdba74", "#c4b5fd", "#f9a8d4", "#86efac", "#fde68a"
      };
      auto pick_color = [&](const std::string& lbl) {
        const size_t h = std::hash<std::string>{}(lbl);
        return seg_colors[h % seg_colors.size()];
      };

      f << "<g>\n";
      // Band outline
      f << "<rect x=\"" << args.margin_left_px << "\" y=\"" << seg_y0
        << "\" width=\"" << plot_width << "\" height=\"" << seg_band_px
        << "\" fill=\"none\" stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n";

      int nlab = 0;
      for (const auto& ev : rec.events) {
        if (!(ev.duration_sec > 0.0) || !std::isfinite(ev.duration_sec)) continue;
        if (ev.duration_sec < args.min_segment_sec) continue;
        const double s0 = ev.onset_sec;
        const double s1 = ev.onset_sec + ev.duration_sec;
        if (s1 < start_sec - 1e-9) continue;
        if (s0 > end_sec + 1e-9) continue;

        const double a0 = std::max(start_sec, s0);
        const double a1 = std::min(end_sec, s1);
        const double xA = to_x(a0);
        const double xB = to_x(a1);
        const double wseg = std::max(0.0, xB - xA);
        if (wseg < 0.5) continue;

        const bool art = is_artifact(ev.text);
        const std::string color = pick_color(ev.text);

        if (art) {
          f << "<rect x=\"" << xA << "\" y=\"" << seg_y0
            << "\" width=\"" << wseg << "\" height=\"" << seg_band_px
            << "\" fill=\"#ffcccc\" opacity=\"0.25\"/>\n";
          f << "<rect x=\"" << xA << "\" y=\"" << seg_y0
            << "\" width=\"" << wseg << "\" height=\"" << seg_band_px
            << "\" fill=\"url(#artifactHatch)\" opacity=\"0.85\"/>\n";
        } else {
          f << "<rect x=\"" << xA << "\" y=\"" << seg_y0
            << "\" width=\"" << wseg << "\" height=\"" << seg_band_px
            << "\" fill=\"" << color << "\" opacity=\"0.45\" stroke=\"#999\" stroke-width=\"0.5\"/>\n";
        }

        if (args.draw_segment_labels && nlab < args.max_segment_labels && wseg >= 50.0) {
          std::string txt = ev.text;
          if (txt.size() > 28) txt = txt.substr(0, 28) + "…";
          const double xc = xA + 0.5 * wseg;
          f << "<text x=\"" << xc << "\" y=\"" << (seg_y0 + seg_band_px - 3)
            << "\" text-anchor=\"middle\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#222\">"
            << svg_escape(txt) << "</text>\n";
          ++nlab;
        }
      }
      f << "</g>\n";
    }

    // Axis labels (time)
    f << "<g font-family=\"sans-serif\" font-size=\"12\" fill=\"#333\">\n";
    for (double t = t0; t <= end_sec + 1e-9; t += tick) {
      if (t < start_sec - 1e-9) continue;
      const double x = to_x(t);
      f << "<text x=\"" << x << "\" y=\"" << tick_label_y
        << "\" text-anchor=\"middle\">";
      f << std::fixed << std::setprecision(1) << t;
      f << "</text>\n";
    }
    f << "<text x=\"" << (args.margin_left_px + plot_width / 2) << "\" y=\"" << (height_px - 8)
      << "\" text-anchor=\"middle\">time (s)</text>\n";
    f << "</g>\n";

    // Event lines
    if (args.draw_events && !rec.events.empty()) {
      f << "<g stroke=\"#cc0000\" stroke-width=\"1\" opacity=\"0.65\">\n";
      for (const auto& ev : rec.events) {
        if (args.draw_segments && ev.duration_sec >= args.min_segment_sec) continue;
        if (ev.onset_sec < start_sec - 1e-9) continue;
        if (ev.onset_sec > end_sec + 1e-9) continue;
        const double x = to_x(ev.onset_sec);
        f << "<line x1=\"" << x << "\" y1=\"" << args.margin_top_px
          << "\" x2=\"" << x << "\" y2=\"" << (args.margin_top_px + plot_height) << "\"/>\n";
      }
      f << "</g>\n";

      if (args.draw_event_labels) {
        f << "<g font-family=\"sans-serif\" font-size=\"11\" fill=\"#cc0000\">\n";
        int nlab = 0;
        for (const auto& ev : rec.events) {
          if (args.draw_segments && ev.duration_sec >= args.min_segment_sec) continue;
          if (ev.onset_sec < start_sec - 1e-9) continue;
          if (ev.onset_sec > end_sec + 1e-9) continue;
          if (nlab >= args.max_event_labels) break;
          const double x = to_x(ev.onset_sec);
          std::string txt = ev.text;
          if (txt.size() > 40) txt = txt.substr(0, 40) + "…";
          f << "<text x=\"" << x << "\" y=\"" << (args.margin_top_px + 12)
            << "\" text-anchor=\"middle\">" << svg_escape(txt) << "</text>\n";
          ++nlab;
        }
        f << "</g>\n";
      }
    }

    // Traces
    f << "<g fill=\"none\" stroke-width=\"1\">\n";
    for (int i = 0; i < n_ch; ++i) {
      const int ch = ch_indices[static_cast<size_t>(i)];
      const std::string name = ch_names[static_cast<size_t>(i)];
      const int y0 = args.margin_top_px + i * args.row_height_px;
      const double y_mid = static_cast<double>(y0) + 0.5 * static_cast<double>(args.row_height_px);

      // Robust auto-scale: use the 99th percentile absolute deviation.
      double uv_per_row = args.uv_per_row;
      if (args.autoscale) {
        std::vector<double> v;
        v.reserve(end_idx - start_idx);
        for (size_t s = start_idx; s < end_idx; ++s) {
          v.push_back(static_cast<double>(rec.data[static_cast<size_t>(ch)][s]));
        }
        const double med = median_inplace(&v);
        for (double& x : v) x = std::fabs(x - med);
        const double q = quantile_inplace(&v, 0.99);
        uv_per_row = std::max(10.0, 2.2 * q); // peak-to-peak ~ 2*(1.1*q)
      }

      const double y_scale = (0.8 * static_cast<double>(args.row_height_px)) / std::max(1e-9, uv_per_row);

      // Decimation
      const size_t nwin = end_idx - start_idx;
      size_t step = 1;
      if (static_cast<int>(nwin) > args.max_points) {
        step = static_cast<size_t>(std::ceil(static_cast<double>(nwin) / static_cast<double>(args.max_points)));
      }

      const std::string stroke = colors[static_cast<size_t>(i) % colors.size()];
      f << "<polyline stroke=\"" << stroke << "\" points=\"";

      bool first = true;
      for (size_t s = start_idx; s < end_idx; s += step) {
        const double t = static_cast<double>(s) / rec.fs_hz;
        const double x = to_x(t);
        const double v_uv = static_cast<double>(rec.data[static_cast<size_t>(ch)][s]);
        const double y = y_mid - v_uv * y_scale;
        if (!std::isfinite(y)) continue;
        if (!first) f << ' ';
        f << std::fixed << std::setprecision(2) << x << ',' << y;
        first = false;
      }

      f << "\"/>\n";

      // Channel label
      f << "<text x=\"" << (args.margin_left_px - 8) << "\" y=\"" << (y_mid + 4)
        << "\" text-anchor=\"end\" font-family=\"sans-serif\" font-size=\"12\" fill=\"#111\">"
        << svg_escape(name) << "</text>\n";

      // Scale label (only when autoscale, to make per-row scaling explicit)
      if (args.autoscale) {
        f << "<text x=\"" << (args.margin_left_px + plot_width + 6) << "\" y=\"" << (y_mid + 4)
          << "\" text-anchor=\"start\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#555\">"
          << std::fixed << std::setprecision(0) << uv_per_row << " uVpp</text>\n";
      }
    }
    f << "</g>\n";

    // Title
    {
      std::string title = "Trace plot";
      title += " (" + args.input_path + ")";
      f << "<text x=\"" << (args.margin_left_px) << "\" y=\"" << 16
        << "\" font-family=\"sans-serif\" font-size=\"14\" fill=\"#111\">"
        << svg_escape(title) << "</text>\n";
    }

    f << "</svg>\n";

    std::cout << "Wrote: " << out_svg << "\n";

    // Metadata
    {
      const std::string meta = args.outdir + "/trace_plot_meta.txt";
      std::ofstream m(meta);
      if (m) {
        m << "input=" << args.input_path << "\n";
        m << "fs_hz=" << rec.fs_hz << "\n";
        m << "start_sec=" << start_sec << "\n";
        m << "end_sec=" << end_sec << "\n";
        m << "duration_sec=" << duration << "\n";
        m << "channels=" << (args.channels.empty() ? "(first N)" : args.channels) << "\n";
        m << "autoscale=" << (args.autoscale ? 1 : 0) << "\n";
        m << "uv_per_row=" << args.uv_per_row << "\n";
        m << "max_points=" << args.max_points << "\n";
        m << "events_drawn=" << (args.draw_events ? 1 : 0) << "\n";
        m << "average_reference=" << (args.average_reference ? 1 : 0) << "\n";
        m << "notch_hz=" << args.notch_hz << "\n";
        m << "notch_q=" << args.notch_q << "\n";
        m << "bandpass_low_hz=" << args.bandpass_low_hz << "\n";
        m << "bandpass_high_hz=" << args.bandpass_high_hz << "\n";
        m << "zero_phase=" << (args.zero_phase ? 1 : 0) << "\n";
      }
      std::cout << "Wrote: " << meta << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
