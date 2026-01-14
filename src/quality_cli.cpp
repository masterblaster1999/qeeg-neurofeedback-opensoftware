#include "qeeg/csv_io.hpp"
#include "qeeg/line_noise.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/robust_stats.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <cmath>
#include <cstddef>
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
  double fs_csv{0.0};

  // If provided, write JSON/CSV reports and a *_run_meta.json under this directory.
  // If empty, behaves like the original version (stdout only).
  std::string outdir;

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
      << "  --outdir DIR             If set, write quality_report.json + per-channel CSV + run_meta\n"
      << "  --max-channels N         Use at most N channels for detection (default: 8; 0=all)\n"
      << "  --nperseg N              Welch segment length (default: 1024)\n"
      << "  --overlap FRAC           Welch overlap fraction in [0,1) (default: 0.5)\n"
      << "  --min-ratio R            Minimum median peak/baseline ratio to recommend notch (default: 3)\n"
      << "  --json                   Output machine-readable JSON to stdout\n"
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
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
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

struct PerChannelLineNoise {
  std::string channel;
  LineNoiseCandidate c50{};
  LineNoiseCandidate c60{};
};

struct LineNoiseDetail {
  LineNoiseEstimate summary{};
  std::vector<PerChannelLineNoise> per_channel;

  // Extra medians for debugging/interpretability (PSD density units).
  double median_peak_mean_50{0.0};
  double median_baseline_mean_50{0.0};
  double median_peak_mean_60{0.0};
  double median_baseline_mean_60{0.0};
};

static LineNoiseDetail compute_line_noise_detail(const EEGRecording& rec,
                                                const WelchOptions& opt,
                                                size_t max_channels,
                                                double min_ratio) {
  LineNoiseDetail d;

  if (!(rec.fs_hz > 0.0)) return d;
  const double nyq = 0.5 * rec.fs_hz;
  if (!(nyq > 1.0)) return d;

  const size_t n_ch = rec.n_channels();
  if (n_ch == 0) return d;

  const size_t use_ch = std::min(n_ch, (max_channels == 0 ? n_ch : max_channels));
  if (use_ch == 0) return d;

  d.summary.n_channels_used = use_ch;

  const bool can50 = (50.0 + 0.5 < nyq);
  const bool can60 = (60.0 + 0.5 < nyq);

  std::vector<double> ratios50;
  std::vector<double> ratios60;
  std::vector<double> peaks50;
  std::vector<double> bases50;
  std::vector<double> peaks60;
  std::vector<double> bases60;

  ratios50.reserve(use_ch);
  ratios60.reserve(use_ch);
  peaks50.reserve(use_ch);
  bases50.reserve(use_ch);
  peaks60.reserve(use_ch);
  bases60.reserve(use_ch);

  d.per_channel.reserve(use_ch);

  for (size_t ch = 0; ch < use_ch; ++ch) {
    PerChannelLineNoise row;
    row.channel = (ch < rec.channel_names.size()) ? rec.channel_names[ch] : ("ch" + std::to_string(ch));

    // Always set freq_hz for readability.
    row.c50.freq_hz = 50.0;
    row.c60.freq_hz = 60.0;

    if (ch < rec.data.size() && !rec.data[ch].empty()) {
      const PsdResult psd = welch_psd(rec.data[ch], rec.fs_hz, opt);
      if (can50) row.c50 = estimate_line_noise_candidate(psd, 50.0);
      if (can60) row.c60 = estimate_line_noise_candidate(psd, 60.0);
    }

    if (row.c50.ratio > 0.0) {
      ratios50.push_back(row.c50.ratio);
      peaks50.push_back(row.c50.peak_mean);
      bases50.push_back(row.c50.baseline_mean);
    }
    if (row.c60.ratio > 0.0) {
      ratios60.push_back(row.c60.ratio);
      peaks60.push_back(row.c60.peak_mean);
      bases60.push_back(row.c60.baseline_mean);
    }

    d.per_channel.push_back(row);
  }

  const double med50 = ratios50.empty() ? 0.0 : median_inplace(&ratios50);
  const double med60 = ratios60.empty() ? 0.0 : median_inplace(&ratios60);

  d.summary.cand50.freq_hz = 50.0;
  d.summary.cand50.ratio = (std::isfinite(med50) ? std::max(0.0, med50) : 0.0);
  d.summary.cand60.freq_hz = 60.0;
  d.summary.cand60.ratio = (std::isfinite(med60) ? std::max(0.0, med60) : 0.0);

  d.median_peak_mean_50 = peaks50.empty() ? 0.0 : median_inplace(&peaks50);
  d.median_baseline_mean_50 = bases50.empty() ? 0.0 : median_inplace(&bases50);
  d.median_peak_mean_60 = peaks60.empty() ? 0.0 : median_inplace(&peaks60);
  d.median_baseline_mean_60 = bases60.empty() ? 0.0 : median_inplace(&bases60);

  const double best = std::max(d.summary.cand50.ratio, d.summary.cand60.ratio);
  if (best >= min_ratio && best > 0.0) {
    if (d.summary.cand60.ratio > d.summary.cand50.ratio) {
      d.summary.recommended_hz = 60.0;
      d.summary.strength_ratio = d.summary.cand60.ratio;
    } else {
      d.summary.recommended_hz = 50.0;
      d.summary.strength_ratio = d.summary.cand50.ratio;
    }
  }

  return d;
}

static void write_line_noise_per_channel_csv(const std::string& path, const LineNoiseDetail& ln) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open for write: " + path);

  f << "channel,ratio_50,peak_mean_50,baseline_mean_50,ratio_60,peak_mean_60,baseline_mean_60\n";
  for (const auto& row : ln.per_channel) {
    f << csv_escape(row.channel) << ","
      << row.c50.ratio << "," << row.c50.peak_mean << "," << row.c50.baseline_mean << ","
      << row.c60.ratio << "," << row.c60.peak_mean << "," << row.c60.baseline_mean
      << "\n";
  }
}

static void emit_json_report(std::ostream& os,
                            const EEGRecording& rec,
                            double duration_sec,
                            const Args& args,
                            const WelchOptions& wopt,
                            const LineNoiseDetail& ln) {
  os << "{\n";
  os << "  \"fs_hz\": " << json_number(rec.fs_hz) << ",\n";
  os << "  \"n_channels\": " << rec.n_channels() << ",\n";
  os << "  \"n_samples\": " << rec.n_samples() << ",\n";
  os << "  \"duration_sec\": " << json_number(duration_sec) << ",\n";

  os << "  \"params\": {\n";
  os << "    \"max_channels\": " << args.max_channels << ",\n";
  os << "    \"nperseg\": " << wopt.nperseg << ",\n";
  os << "    \"overlap\": " << json_number(wopt.overlap_fraction) << ",\n";
  os << "    \"min_ratio\": " << json_number(args.min_ratio) << "\n";
  os << "  },\n";

  os << "  \"line_noise\": {\n";
  os << "    \"median_ratio_50\": " << json_number(ln.summary.cand50.ratio) << ",\n";
  os << "    \"median_ratio_60\": " << json_number(ln.summary.cand60.ratio) << ",\n";
  os << "    \"recommended_notch_hz\": " << json_number(ln.summary.recommended_hz) << ",\n";
  os << "    \"strength_ratio\": " << json_number(ln.summary.strength_ratio) << ",\n";
  os << "    \"channels_used\": " << ln.summary.n_channels_used << ",\n";
  os << "    \"median_peak_mean_50\": " << json_number(ln.median_peak_mean_50) << ",\n";
  os << "    \"median_baseline_mean_50\": " << json_number(ln.median_baseline_mean_50) << ",\n";
  os << "    \"median_peak_mean_60\": " << json_number(ln.median_peak_mean_60) << ",\n";
  os << "    \"median_baseline_mean_60\": " << json_number(ln.median_baseline_mean_60) << "\n";
  os << "  },\n";

  os << "  \"per_channel\": [\n";
  for (size_t i = 0; i < ln.per_channel.size(); ++i) {
    const auto& row = ln.per_channel[i];
    os << "    {\n";
    os << "      \"channel\": \"" << json_escape(row.channel) << "\",\n";
    os << "      \"cand50\": {\"ratio\": " << json_number(row.c50.ratio)
       << ", \"peak_mean\": " << json_number(row.c50.peak_mean)
       << ", \"baseline_mean\": " << json_number(row.c50.baseline_mean) << "},\n";
    os << "      \"cand60\": {\"ratio\": " << json_number(row.c60.ratio)
       << ", \"peak_mean\": " << json_number(row.c60.peak_mean)
       << ", \"baseline_mean\": " << json_number(row.c60.baseline_mean) << "}\n";
    os << "    }";
    if (i + 1 < ln.per_channel.size()) os << ",";
    os << "\n";
  }
  os << "  ]\n";

  os << "}\n";
}

static void emit_text_report(std::ostream& os,
                            const EEGRecording& rec,
                            double duration_sec,
                            const Args& args,
                            const LineNoiseDetail& ln) {
  os << "qeeg_quality_cli\n\n";
  os << "Input: " << args.input_path << "\n";
  os << "Sampling rate (Hz): " << rec.fs_hz << "\n";
  os << "Channels: " << rec.n_channels() << "\n";
  os << "Samples: " << rec.n_samples() << "\n";
  os << "Duration (sec): " << std::fixed << std::setprecision(3) << duration_sec << "\n\n";

  os << "Welch params:\n";
  os << "  nperseg: " << args.nperseg << "\n";
  os << "  overlap: " << args.overlap << "\n";
  os << "  max_channels: " << args.max_channels << "\n";
  os << "  min_ratio: " << args.min_ratio << "\n\n";

  os << "Line noise (median peak/baseline ratio across up to " << ln.summary.n_channels_used << " channels):\n";
  os << "  50 Hz ratio: " << std::setprecision(3) << ln.summary.cand50.ratio << "\n";
  os << "  60 Hz ratio: " << std::setprecision(3) << ln.summary.cand60.ratio << "\n";
  if (ln.summary.recommended_hz > 0.0) {
    os << "  Recommended notch: " << ln.summary.recommended_hz << " Hz (ratio=" << ln.summary.strength_ratio
       << ")\n";
  } else {
    os << "  Recommended notch: none (ratios below --min-ratio)\n";
  }
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

    const LineNoiseDetail ln = compute_line_noise_detail(rec, wopt, args.max_channels, args.min_ratio);

    // Optional file outputs (for qeeg_ui_cli linking + reproducibility).
    if (!args.outdir.empty()) {
      ensure_directory(args.outdir);

      const std::string json_path = args.outdir + "/quality_report.json";
      {
        std::ofstream jf(json_path);
        if (!jf) throw std::runtime_error("Failed to write " + json_path);
        emit_json_report(jf, rec, duration_sec, args, wopt, ln);
      }

      const std::string txt_path = args.outdir + "/quality_summary.txt";
      {
        std::ofstream tf(txt_path);
        if (!tf) throw std::runtime_error("Failed to write " + txt_path);
        emit_text_report(tf, rec, duration_sec, args, ln);
        tf << "\n";
      }

      const std::string csv_path = args.outdir + "/line_noise_per_channel.csv";
      write_line_noise_per_channel_csv(csv_path, ln);

      const std::string meta_path = args.outdir + "/quality_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("quality_run_meta.json");
      outs.push_back("quality_report.json");
      outs.push_back("quality_summary.txt");
      outs.push_back("line_noise_per_channel.csv");
      if (!write_run_meta_json(meta_path, "qeeg_quality_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write " << meta_path << "\n";
      }
    }

    if (args.json) {
      // Important: in --json mode, keep stdout machine-readable (no extra lines).
      emit_json_report(std::cout, rec, duration_sec, args, wopt, ln);
      return 0;
    }

    emit_text_report(std::cout, rec, duration_sec, args, ln);

    if (!args.outdir.empty()) {
      std::cout << "\nOutputs written to: " << args.outdir << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
