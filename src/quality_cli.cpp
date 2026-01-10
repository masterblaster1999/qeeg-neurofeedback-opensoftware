#include "qeeg/line_noise.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  double fs_csv{0.0};

  size_t max_channels{8};
  size_t nperseg{1024};
  double overlap{0.5};
  double min_ratio{3.0};

  bool json{false};
};

static void print_help() {
  std::cout
      << "qeeg_quality_cli\n\n"
      << "Quick, dependency-light signal quality checks for EEG recordings.\n"
      << "Currently reports an estimate of 50/60 Hz power-line interference strength\n"
      << "to help choose a notch filter frequency.\n\n"
      << "Usage:\n"
      << "  qeeg_quality_cli --input session.edf\n"
      << "  qeeg_quality_cli --input session.txt --fs 256\n\n"
      << "Options:\n"
      << "  --input PATH             Input EDF/BDF/CSV/ASCII\n"
      << "  --fs HZ                  Sampling rate for CSV/TXT inputs (if no time column)\n"
      << "  --max-channels N         Use at most N channels for detection (default: 8; 0=all)\n"
      << "  --nperseg N              Welch segment length (default: 1024)\n"
      << "  --overlap FRAC           Welch overlap fraction in [0,1) (default: 0.5)\n"
      << "  --min-ratio R            Minimum median peak/baseline ratio to recommend notch (default: 3)\n"
      << "  --json                   Output machine-readable JSON\n"
      << "  -h, --help               Show help\n";
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
    } else if (arg == "--max-channels" && i + 1 < argc) {
      a.max_channels = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--min-ratio" && i + 1 < argc) {
      a.min_ratio = to_double(argv[++i]);
    } else if (arg == "--json") {
      a.json = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string json_number(double x) {
  if (!std::isfinite(x)) return "null";
  std::ostringstream oss;
  oss << std::setprecision(17) << x;
  return oss.str();
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      return 1;
    }
    if (args.overlap < 0.0 || args.overlap >= 1.0) {
      throw std::runtime_error("--overlap must be in [0,1)");
    }
    if (!(args.min_ratio >= 0.0)) {
      throw std::runtime_error("--min-ratio must be >= 0");
    }

    const EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    const double duration_sec = (rec.fs_hz > 0.0) ? (static_cast<double>(rec.n_samples()) / rec.fs_hz) : 0.0;

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    const LineNoiseEstimate ln = detect_line_noise_50_60(rec, wopt, args.max_channels, args.min_ratio);

    if (args.json) {
      std::cout << "{\n";
      std::cout << "  \"fs_hz\": " << json_number(rec.fs_hz) << ",\n";
      std::cout << "  \"n_channels\": " << rec.n_channels() << ",\n";
      std::cout << "  \"n_samples\": " << rec.n_samples() << ",\n";
      std::cout << "  \"duration_sec\": " << json_number(duration_sec) << ",\n";
      std::cout << "  \"line_noise\": {\n";
      std::cout << "    \"median_ratio_50\": " << json_number(ln.cand50.ratio) << ",\n";
      std::cout << "    \"median_ratio_60\": " << json_number(ln.cand60.ratio) << ",\n";
      std::cout << "    \"recommended_notch_hz\": " << json_number(ln.recommended_hz) << ",\n";
      std::cout << "    \"strength_ratio\": " << json_number(ln.strength_ratio) << ",\n";
      std::cout << "    \"channels_used\": " << ln.n_channels_used << "\n";
      std::cout << "  }\n";
      std::cout << "}\n";
      return 0;
    }

    std::cout << "qeeg_quality_cli\n\n";
    std::cout << "Input: " << args.input_path << "\n";
    std::cout << "Sampling rate (Hz): " << rec.fs_hz << "\n";
    std::cout << "Channels: " << rec.n_channels() << "\n";
    std::cout << "Samples: " << rec.n_samples() << "\n";
    std::cout << "Duration (sec): " << std::fixed << std::setprecision(3) << duration_sec << "\n\n";

    std::cout << "Line noise (median peak/baseline ratio across up to " << ln.n_channels_used << " channels):\n";
    std::cout << "  50 Hz ratio: " << std::setprecision(3) << ln.cand50.ratio << "\n";
    std::cout << "  60 Hz ratio: " << std::setprecision(3) << ln.cand60.ratio << "\n";
    if (ln.recommended_hz > 0.0) {
      std::cout << "  Recommended notch: " << ln.recommended_hz << " Hz (ratio=" << ln.strength_ratio
                << ")\n";
    } else {
      std::cout << "  Recommended notch: none (ratios below --min-ratio)\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
