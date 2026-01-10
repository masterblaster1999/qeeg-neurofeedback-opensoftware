#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  double fs_csv{0.0};

  bool json{false};

  // Optional lists.
  bool channels{false};
  bool events{false};
  bool per_channel{false};

  // Scan loaded samples for basic sanity/stats.
  bool scan{true};

  // Limit how much we print (0 => no limit).
  std::size_t max_channels{64};
  std::size_t max_events{10};
};

static void print_help() {
  std::cout
    << "qeeg_info_cli\n\n"
    << "Print a quick summary of a recording (EDF/BDF/CSV): sampling rate, channel list,\n"
    << "sample counts, duration, and (optionally) parsed EDF+/BDF+ annotations.\n\n"
    << "Usage:\n"
    << "  qeeg_info_cli --input file.edf\n"
    << "  qeeg_info_cli --input file.bdf\n"
    << "  qeeg_info_cli --input file.csv --fs 250\n"
    << "  qeeg_info_cli --input file_with_time.csv\n\n"
    << "Options:\n"
    << "  --input PATH             Input EDF/BDF/CSV\n"
    << "  --fs HZ                  Sampling rate for CSV (optional if CSV has a time column)\n"
    << "  --channels               Print channel names (limited by --max-channels)\n"
    << "  --events                 Print annotation events (EDF+/BDF+ only; limited by --max-events)\n"
    << "  --scan                   Scan samples and report global min/max + non-finite counts (default)\n"
    << "  --no-scan                Skip sample scanning (faster output)\n"
    << "  --per-channel             Print per-channel stats (implies --scan)\n"
    << "  --max-channels N          Limit printed channels/stats (0 => all; default: 64)\n"
    << "  --max-events N            Limit printed events (0 => all; default: 10)\n"
    << "  --json                   Output JSON (useful for scripts)\n"
    << "  -h, --help               Show this help\n";
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
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--channels") {
      a.channels = true;
    } else if (arg == "--events") {
      a.events = true;
    } else if (arg == "--scan") {
      a.scan = true;
    } else if (arg == "--no-scan") {
      a.scan = false;
    } else if (arg == "--per-channel") {
      a.per_channel = true;
      a.scan = true;
    } else if (arg == "--max-channels" && i + 1 < argc) {
      a.max_channels = static_cast<std::size_t>(to_int(argv[++i]));
    } else if (arg == "--max-events" && i + 1 < argc) {
      a.max_events = static_cast<std::size_t>(to_int(argv[++i]));
    } else if (arg == "--json") {
      a.json = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string json_escape(const std::string& s) {
  std::ostringstream oss;
  for (char c : s) {
    switch (c) {
      case '\\': oss << "\\\\"; break;
      case '"': oss << "\\\""; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c))
              << std::dec << std::setw(0) << std::setfill(' ');
        } else {
          oss << c;
        }
        break;
    }
  }
  return oss.str();
}

static std::string json_number(double x) {
  if (!std::isfinite(x)) return "null";
  std::ostringstream oss;
  oss << std::setprecision(17) << x;
  return oss.str();
}

static std::string format_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) return "n/a";
  const long long total = static_cast<long long>(seconds + 0.5); // nearest second
  const long long h = total / 3600;
  const long long m = (total % 3600) / 60;
  const long long s = total % 60;
  std::ostringstream oss;
  if (h > 0) oss << h << "h";
  if (m > 0 || h > 0) oss << m << "m";
  oss << s << "s";
  return oss.str();
}

struct ChanStats {
  std::string name;
  std::size_t n{0};
  std::size_t nonfinite{0};
  double min_v{std::numeric_limits<double>::infinity()};
  double max_v{-std::numeric_limits<double>::infinity()};
  double mean{std::numeric_limits<double>::quiet_NaN()};
  double stdev{std::numeric_limits<double>::quiet_NaN()};
  double max_abs{0.0};
};

static ChanStats compute_channel_stats(const std::string& name, const std::vector<float>& x) {
  ChanStats s;
  s.name = name;
  s.n = x.size();

  // Welford for mean/std over finite samples.
  double m = 0.0;
  double m2 = 0.0;
  std::size_t k = 0;

  for (float fv : x) {
    const double v = static_cast<double>(fv);
    if (!std::isfinite(v)) {
      ++s.nonfinite;
      continue;
    }

    s.min_v = std::min(s.min_v, v);
    s.max_v = std::max(s.max_v, v);
    s.max_abs = std::max(s.max_abs, std::fabs(v));

    ++k;
    const double delta = v - m;
    m += delta / static_cast<double>(k);
    const double delta2 = v - m;
    m2 += delta * delta2;
  }

  if (k == 0) {
    s.min_v = std::numeric_limits<double>::quiet_NaN();
    s.max_v = std::numeric_limits<double>::quiet_NaN();
    s.mean = std::numeric_limits<double>::quiet_NaN();
    s.stdev = std::numeric_limits<double>::quiet_NaN();
  } else if (k == 1) {
    s.mean = m;
    s.stdev = 0.0;
  } else {
    s.mean = m;
    s.stdev = std::sqrt(m2 / static_cast<double>(k - 1)); // sample stdev
  }

  return s;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      return 1;
    }

    const EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    const std::size_t n_ch = rec.n_channels();
    const std::size_t n0 = rec.data.empty() ? 0 : rec.data[0].size();

    bool lengths_match = true;
    std::size_t min_len = std::numeric_limits<std::size_t>::max();
    std::size_t max_len = 0;
    for (const auto& ch : rec.data) {
      min_len = std::min(min_len, ch.size());
      max_len = std::max(max_len, ch.size());
      if (ch.size() != n0) lengths_match = false;
    }
    if (n_ch == 0) {
      min_len = 0;
      max_len = 0;
      lengths_match = true;
    }

    const double duration_sec = (rec.fs_hz > 0.0) ? (static_cast<double>(n0) / rec.fs_hz)
                                                  : std::numeric_limits<double>::quiet_NaN();

    // Optional scan stats.
    std::size_t nonfinite_total = 0;
    double global_min = std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();
    double global_max_abs = 0.0;

    std::vector<ChanStats> per;
    if (args.scan && args.per_channel) per.reserve(n_ch);

    if (args.scan) {
      for (std::size_t i = 0; i < n_ch; ++i) {
        const std::string name =
          (i < rec.channel_names.size() ? rec.channel_names[i] : ("ch" + std::to_string(i)));

        ChanStats st = compute_channel_stats(name, rec.data[i]);
        nonfinite_total += st.nonfinite;
        if (std::isfinite(st.min_v)) global_min = std::min(global_min, st.min_v);
        if (std::isfinite(st.max_v)) global_max = std::max(global_max, st.max_v);
        global_max_abs = std::max(global_max_abs, st.max_abs);

        if (args.per_channel) per.push_back(std::move(st));
      }
    }

    const bool have_finite_global = std::isfinite(global_min) && std::isfinite(global_max);

    if (args.json) {
      std::ostringstream oss;
      oss << "{";

      oss << "\"input\":\"" << json_escape(args.input_path) << "\"";
      oss << ",\"fs_hz\":" << json_number(rec.fs_hz);
      oss << ",\"n_channels\":" << n_ch;
      oss << ",\"n_samples\":" << n0;
      oss << ",\"duration_sec\":" << json_number(duration_sec);
      oss << ",\"events_count\":" << rec.events.size();

      oss << ",\"channel_lengths\":{";
      oss << "\"min\":" << min_len << ",\"max\":" << max_len
          << ",\"uniform\":" << (lengths_match ? "true" : "false") << "}";

      oss << ",\"scan_performed\":" << (args.scan ? "true" : "false");
      if (args.scan) {
        oss << ",\"nonfinite_total\":" << nonfinite_total;
        oss << ",\"global_min\":" << (have_finite_global ? json_number(global_min) : "null");
        oss << ",\"global_max\":" << (have_finite_global ? json_number(global_max) : "null");
        oss << ",\"global_max_abs\":" << json_number(global_max_abs);
      }

      // Optional channels list (can be large).
      if (args.channels || args.per_channel) {
        const std::size_t total = rec.channel_names.size();
        const std::size_t limit =
          (args.max_channels == 0) ? total : std::min(args.max_channels, total);

        oss << ",\"channels_truncated\":" << ((limit < total) ? "true" : "false");
        oss << ",\"channels\":[";
        for (std::size_t i = 0; i < limit; ++i) {
          if (i) oss << ",";
          oss << "\"" << json_escape(rec.channel_names[i]) << "\"";
        }
        oss << "]";
      }

      // Optional events list (EDF+/BDF+ annotations).
      if (args.events) {
        const std::size_t total = rec.events.size();
        const std::size_t limit =
          (args.max_events == 0) ? total : std::min(args.max_events, total);

        oss << ",\"events_truncated\":" << ((limit < total) ? "true" : "false");
        oss << ",\"events\":[";
        for (std::size_t i = 0; i < limit; ++i) {
          if (i) oss << ",";
          const auto& ev = rec.events[i];
          oss << "{"
              << "\"onset_sec\":" << json_number(ev.onset_sec) << ","
              << "\"duration_sec\":" << json_number(ev.duration_sec) << ","
              << "\"text\":\"" << json_escape(ev.text) << "\""
              << "}";
        }
        oss << "]";
      }

      // Optional per-channel stats.
      if (args.per_channel) {
        const std::size_t total = per.size();
        const std::size_t limit =
          (args.max_channels == 0) ? total : std::min(args.max_channels, total);

        oss << ",\"channel_stats_truncated\":" << ((limit < total) ? "true" : "false");
        oss << ",\"channel_stats\":[";
        for (std::size_t i = 0; i < limit; ++i) {
          if (i) oss << ",";
          const auto& st = per[i];
          oss << "{"
              << "\"name\":\"" << json_escape(st.name) << "\""
              << ",\"n_samples\":" << st.n
              << ",\"nonfinite\":" << st.nonfinite
              << ",\"min\":" << json_number(st.min_v)
              << ",\"max\":" << json_number(st.max_v)
              << ",\"mean\":" << json_number(st.mean)
              << ",\"stdev\":" << json_number(st.stdev)
              << ",\"max_abs\":" << json_number(st.max_abs)
              << "}";
        }
        oss << "]";
      }

      oss << "}\n";
      std::cout << oss.str();
      return 0;
    }

    // Human-readable output.
    std::cout << "Input: " << args.input_path << "\n";
    std::cout << "Sampling rate (Hz): " << std::setprecision(12) << rec.fs_hz << "\n";
    std::cout << "Channels: " << n_ch << "\n";
    std::cout << "Samples: " << n0 << "\n";
    if (std::isfinite(duration_sec)) {
      std::cout << "Duration (s): " << std::setprecision(6) << duration_sec
                << " (" << format_duration(duration_sec) << ")\n";
    } else {
      std::cout << "Duration (s): n/a\n";
    }
    std::cout << "Events: " << rec.events.size() << "\n";

    if (!lengths_match) {
      std::cout << "WARNING: channel sample counts are not uniform (min=" << min_len << ", max=" << max_len << ")\n";
    }

    if (args.channels) {
      const std::size_t total = rec.channel_names.size();
      const std::size_t limit =
        (args.max_channels == 0) ? total : std::min(args.max_channels, total);

      std::cout << "Channel names";
      if (limit < total) std::cout << " (showing first " << limit << ")";
      std::cout << ":\n";
      for (std::size_t i = 0; i < limit; ++i) {
        std::cout << "  - " << rec.channel_names[i] << "\n";
      }
    }

    if (args.events) {
      const std::size_t total = rec.events.size();
      const std::size_t limit =
        (args.max_events == 0) ? total : std::min(args.max_events, total);

      std::cout << "Events list";
      if (limit < total) std::cout << " (showing first " << limit << ")";
      std::cout << ":\n";
      for (std::size_t i = 0; i < limit; ++i) {
        const auto& ev = rec.events[i];
        std::cout << "  - onset=" << std::setprecision(6) << ev.onset_sec
                  << "s, dur=" << std::setprecision(6) << ev.duration_sec
                  << "s, text=\"" << ev.text << "\"\n";
      }
    }

    if (args.scan) {
      std::cout << "Data scan:\n";
      std::cout << "  non-finite samples: " << nonfinite_total << "\n";
      if (have_finite_global) {
        std::cout << "  global min/max: " << std::setprecision(10) << global_min << " / " << global_max << "\n";
      } else {
        std::cout << "  global min/max: n/a\n";
      }
      std::cout << "  global max |x|: " << std::setprecision(10) << global_max_abs << "\n";
    }

    if (args.per_channel) {
      const std::size_t total = per.size();
      const std::size_t limit =
        (args.max_channels == 0) ? total : std::min(args.max_channels, total);

      std::cout << "Per-channel stats";
      if (limit < total) std::cout << " (showing first " << limit << ")";
      std::cout << ":\n";
      std::cout << "  name,n_samples,nonfinite,min,max,mean,stdev,max_abs\n";
      for (std::size_t i = 0; i < limit; ++i) {
        const auto& st = per[i];
        std::cout << "  " << st.name << ","
                  << st.n << ","
                  << st.nonfinite << ",";
        if (std::isfinite(st.min_v)) {
          std::cout << std::setprecision(10) << st.min_v;
        } else {
          std::cout << "nan";
        }
        std::cout << ",";
        if (std::isfinite(st.max_v)) {
          std::cout << std::setprecision(10) << st.max_v;
        } else {
          std::cout << "nan";
        }
        std::cout << ",";
        if (std::isfinite(st.mean)) {
          std::cout << std::setprecision(10) << st.mean;
        } else {
          std::cout << "nan";
        }
        std::cout << ",";
        if (std::isfinite(st.stdev)) {
          std::cout << std::setprecision(10) << st.stdev;
        } else {
          std::cout << "nan";
        }
        std::cout << "," << std::setprecision(10) << st.max_abs << "\n";
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "qeeg_info_cli error: " << e.what() << "\n";
    return 1;
  }
}
