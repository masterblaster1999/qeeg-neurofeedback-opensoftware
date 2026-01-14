#include "qeeg/bmp_writer.hpp"
#include "qeeg/fft.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/spectrogram.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out"};
  std::string channel;  // channel name (case-insensitive); empty => first channel

  // CSV inputs
  double fs_csv{0.0};

  // STFT params
  double window_sec{2.0};
  double step_sec{0.25};
  size_t nfft{0};
  double maxfreq_hz{40.0};

  // Output scaling
  double dynrange_db{60.0};
  double vmax_db{std::numeric_limits<double>::quiet_NaN()}; // NaN => auto (p95)
  bool export_csv{true};
  bool csv_long{false};

  bool colorbar{false}; // add vmin/vmax colorbar to BMP output

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
    << "qeeg_spectrogram_cli (STFT spectrogram)\n\n"
    << "Usage:\n"
    << "  qeeg_spectrogram_cli --input file.edf --channel Cz --outdir out\n"
    << "  qeeg_spectrogram_cli --input file.csv --fs 250 --channel Cz --outdir out\n\n"
    << "Options:\n"
    << "  --input PATH            Input EDF/BDF/CSV\n"
    << "  --fs HZ                 Sampling rate for CSV (optional if first column is time)\n"
    << "  --outdir DIR            Output directory (default: out)\n"
    << "  --channel NAME          Channel name (case-insensitive); default: first\n"
    << "  --window S              Window length in seconds (default: 2.0)\n"
    << "  --step S                Step size in seconds (default: 0.25)\n"
    << "  --nfft N                FFT size (power of two; default: next pow2 >= window)\n"
    << "  --maxfreq HZ            Maximum displayed frequency (default: 40)\n"
    << "  --dynrange-db DB        Display dynamic range below vmax (default: 60)\n"
    << "  --vmax-db DB            Fix vmax in dB (default: auto ~95th percentile)\n"
    << "  --no-csv                Do not export CSV\n"
    << "  --csv-long              Export long-format CSV (time,freq,power_db)\n"
    << "  --colorbar              Add a vertical colorbar (vmin/vmax) to the BMP\n"
    << "  --average-reference     Apply common average reference across channels\n"
    << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q             Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --zero-phase            Offline: forward-backward filtering (less phase distortion)\n"
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
    } else if (arg == "--channel" && i + 1 < argc) {
      a.channel = argv[++i];
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_sec = to_double(argv[++i]);
    } else if (arg == "--step" && i + 1 < argc) {
      a.step_sec = to_double(argv[++i]);
    } else if (arg == "--nfft" && i + 1 < argc) {
      a.nfft = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--maxfreq" && i + 1 < argc) {
      a.maxfreq_hz = to_double(argv[++i]);
    } else if (arg == "--dynrange-db" && i + 1 < argc) {
      a.dynrange_db = to_double(argv[++i]);
    } else if (arg == "--vmax-db" && i + 1 < argc) {
      a.vmax_db = to_double(argv[++i]);
    } else if (arg == "--no-csv") {
      a.export_csv = false;
    } else if (arg == "--csv-long") {
      a.csv_long = true;
    } else if (arg == "--colorbar") {
      a.colorbar = true;
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
  if (want.empty()) return 0;

  const std::string lw = normalize_channel_name(want);
  for (size_t i = 0; i < names.size(); ++i) {
    if (normalize_channel_name(names[i]) == lw) return static_cast<int>(i);
  }

  // Accept numeric index (0-based or 1-based) for convenience.
  bool all_digits = !want.empty();
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

static double percentile_inplace(std::vector<double>* v, double p01) {
  if (!v || v->empty()) return std::numeric_limits<double>::quiet_NaN();
  if (p01 <= 0.0) {
    return *std::min_element(v->begin(), v->end());
  }
  if (p01 >= 1.0) {
    return *std::max_element(v->begin(), v->end());
  }
  const size_t n = v->size();
  size_t k = static_cast<size_t>(std::floor(p01 * static_cast<double>(n - 1)));
  std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(k), v->end());
  return (*v)[k];
}

static void write_csv_wide(const std::string& path,
                           const std::vector<double>& times,
                           const std::vector<double>& freqs,
                           const std::vector<double>& db,
                           size_t nframes,
                           size_t nfreq) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open output CSV: " + path);

  f << "time_sec";
  for (size_t k = 0; k < nfreq; ++k) {
    f << "," << freqs[k];
  }
  f << "\n";

  for (size_t t = 0; t < nframes; ++t) {
    f << times[t];
    for (size_t k = 0; k < nfreq; ++k) {
      f << "," << db[t * nfreq + k];
    }
    f << "\n";
  }
}

static void write_csv_long(const std::string& path,
                           const std::vector<double>& times,
                           const std::vector<double>& freqs,
                           const std::vector<double>& db,
                           size_t nframes,
                           size_t nfreq) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open output CSV: " + path);

  f << "time_sec,freq_hz,power_db\n";
  for (size_t t = 0; t < nframes; ++t) {
    for (size_t k = 0; k < nfreq; ++k) {
      f << times[t] << "," << freqs[k] << "," << db[t * nfreq + k] << "\n";
    }
  }
}

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.window_sec <= 0.0) throw std::runtime_error("--window must be > 0");
    if (args.step_sec <= 0.0) throw std::runtime_error("--step must be > 0");

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");
    if (rec.n_channels() == 0 || rec.n_samples() < 8) throw std::runtime_error("Recording too small");

    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    preprocess_recording_inplace(rec, popt);

    int ch = find_channel_index(rec.channel_names, args.channel);
    if (ch < 0) {
      throw std::runtime_error("Channel not found: " + args.channel);
    }

    const std::string ch_name = rec.channel_names[static_cast<size_t>(ch)];
    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";
    std::cout << "Spectrogram channel: " << ch_name << "\n";

    // Build options
    SpectrogramOptions sopt;
    sopt.nperseg = static_cast<size_t>(std::llround(args.window_sec * rec.fs_hz));
    sopt.hop = static_cast<size_t>(std::llround(args.step_sec * rec.fs_hz));
    sopt.nfft = args.nfft;

    SpectrogramResult spec = stft_spectrogram_psd(rec.data[static_cast<size_t>(ch)], rec.fs_hz, sopt);

    // Keep frequencies up to maxfreq
    size_t nfreq_keep = spec.n_freq;
    if (args.maxfreq_hz > 0.0) {
      for (size_t k = 0; k < spec.n_freq; ++k) {
        if (spec.freqs_hz[k] > args.maxfreq_hz) { nfreq_keep = k; break; }
      }
      if (nfreq_keep < 2) nfreq_keep = std::min<size_t>(2, spec.n_freq);
    }

    // Convert to dB
    const double eps = 1e-20;
    std::vector<double> db(spec.n_frames * nfreq_keep, 0.0);
    std::vector<double> all_vals;
    all_vals.reserve(spec.n_frames * nfreq_keep);
    for (size_t t = 0; t < spec.n_frames; ++t) {
      for (size_t k = 0; k < nfreq_keep; ++k) {
        double v = spec.at(t, k);
        double d = 10.0 * std::log10(v + eps);
        db[t * nfreq_keep + k] = d;
        all_vals.push_back(d);
      }
    }

    double vmax = args.vmax_db;
    if (!std::isfinite(vmax)) {
      // Robust auto scaling: use p95
      vmax = percentile_inplace(&all_vals, 0.95);
    }
    double vmin = vmax - args.dynrange_db;
    if (!(vmax > vmin)) {
      vmin = vmax - 1.0;
    }

    // Render BMP (time on x, low freq at bottom)
    const int width = static_cast<int>(spec.n_frames);
    const int height = static_cast<int>(nfreq_keep);
    std::vector<RGB> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y) {
      // y=0 is top, corresponds to highest kept frequency
      size_t k = static_cast<size_t>(height - 1 - y);
      for (int x = 0; x < width; ++x) {
        double d = db[static_cast<size_t>(x) * nfreq_keep + k];
        double t01 = (d - vmin) / (vmax - vmin);
        pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = colormap_heat(t01);
      }
    }

    std::string safe_ch = ch_name;
    for (char& c : safe_ch) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
    }

    const std::string bmp_path = args.outdir + "/spectrogram_" + safe_ch + ".bmp";
    if (args.colorbar) {
      // Add a vmin/vmax colorbar directly into the BMP for easier interpretation.
      VerticalColorbarOptions opt;
      write_bmp24_with_vertical_colorbar(bmp_path, width, height, pixels, vmin, vmax, opt);
    } else {
      write_bmp24(bmp_path, width, height, pixels);
    }
    std::cout << "Wrote: " << bmp_path << "\n";

    if (args.export_csv) {
      // Write only kept freqs
      std::vector<double> freqs_keep(spec.freqs_hz.begin(), spec.freqs_hz.begin() + static_cast<std::ptrdiff_t>(nfreq_keep));
      const std::string csv_path = args.outdir + "/spectrogram_" + safe_ch + ".csv";
      if (args.csv_long) {
        write_csv_long(csv_path, spec.times_sec, freqs_keep, db, spec.n_frames, nfreq_keep);
      } else {
        write_csv_wide(csv_path, spec.times_sec, freqs_keep, db, spec.n_frames, nfreq_keep);
      }
      std::cout << "Wrote: " << csv_path << "\n";
    }

    // Write a small metadata file for reproducibility
    {
      const std::string meta_path = args.outdir + "/spectrogram_" + safe_ch + "_meta.txt";
      std::ofstream m(meta_path);
      if (m) {
        m << "channel=" << ch_name << "\n";
        m << "fs_hz=" << rec.fs_hz << "\n";
        m << "window_sec=" << args.window_sec << "\n";
        m << "step_sec=" << args.step_sec << "\n";
        m << "nperseg=" << sopt.nperseg << "\n";
        m << "hop=" << sopt.hop << "\n";
        m << "nfft=" << ((sopt.nfft == 0) ? next_power_of_two(sopt.nperseg) : sopt.nfft) << "\n";
        m << "maxfreq_hz=" << args.maxfreq_hz << "\n";
        m << "vmin_db=" << vmin << "\n";
        m << "vmax_db=" << vmax << "\n";
        m << "dynrange_db=" << args.dynrange_db << "\n";
        m << "average_reference=" << (args.average_reference ? 1 : 0) << "\n";
        m << "notch_hz=" << args.notch_hz << "\n";
        m << "notch_q=" << args.notch_q << "\n";
        m << "bandpass_low_hz=" << args.bandpass_low_hz << "\n";
        m << "bandpass_high_hz=" << args.bandpass_high_hz << "\n";
        m << "zero_phase=" << (args.zero_phase ? 1 : 0) << "\n";
      }
      std::cout << "Wrote: " << meta_path << "\n";
    }

    // Run manifest for qeeg_ui_cli / qeeg_ui_server_cli discovery.
    {
      const std::string meta_path = args.outdir + "/spectrogram_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("spectrogram_" + safe_ch + ".bmp");
      if (args.export_csv) {
        outs.push_back("spectrogram_" + safe_ch + ".csv");
      }
      outs.push_back("spectrogram_" + safe_ch + "_meta.txt");
      outs.push_back("spectrogram_run_meta.json");

      if (!write_run_meta_json(meta_path, "qeeg_spectrogram_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write spectrogram_run_meta.json to: " << meta_path << "\n";
      } else {
        std::cout << "Wrote: " << meta_path << "\n";
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
