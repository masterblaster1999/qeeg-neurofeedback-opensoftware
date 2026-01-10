#include "qeeg/channel_map.hpp"
#include "qeeg/channel_qc.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/interpolate.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/types.hpp"
#include "qeeg/utils.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  std::string input_path;
  std::string outdir;
  double fs_csv{0.0};

  std::string channel_map_path;

  // Montage
  std::string montage_path; // empty => builtin 10-20 19

  // QC thresholds
  double flatline_ptp{1.0};
  double flatline_scale{0.0};
  double flatline_scale_factor{0.02};
  double noisy_scale_factor{10.0};
  double artifact_bad_frac{0.30};
  double min_abs_corr{0.0};
  size_t max_samples_robust{50000};

  // Artifact window scoring parameters
  double window_seconds{1.0};
  double step_seconds{0.5};
  double baseline_seconds{10.0};
  double ptp_z{6.0};
  double rms_z{6.0};
  double kurtosis_z{6.0};
  size_t min_bad_channels{1};

  // Optional fixes
  bool interpolate{false};
  bool drop_bad{false};

  // Optional export
  std::string output_path;   // .edf or .csv
  std::string events_out_csv;

  // EDF writer
  double record_duration_seconds{1.0};
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-channel-qc"};
  std::string phys_dim{"uV"};
  bool plain_edf{false};
  int annotation_spr{0};

  // CSV writer
  bool write_time{true};
};

static void print_help() {
  std::cout
      << "qeeg_channel_qc_cli\n\n"
      << "Detect likely bad channels (flatline/noisy/artifact-heavy) and optionally drop/interpolate.\n"
      << "Designed for pragmatic cleanup of EDF/BDF/ASCII exports before qEEG feature extraction.\n\n"
      << "Usage:\n"
      << "  qeeg_channel_qc_cli --input <in.edf|in.bdf|in.csv|in.txt> --outdir <out> [options]\n\n"
      << "Core options:\n"
      << "  --channel-map <map.csv>      Remap/drop channels before QC (e.g., ExG1->C3).\n"
      << "  --montage <path.csv>         Montage CSV (name,x,y). If omitted, uses builtin 10-20 19ch.\n"
      << "  --interpolate                Interpolate bad channels using spherical spline + montage.\n"
      << "  --drop-bad                   Drop bad channels (no montage required).\n"
      << "  --output <out.edf|out.csv>   Optional export after interpolation/drop.\n"
      << "  --events-out <events.csv>    Optional events sidecar CSV export.\n\n"
      << "QC thresholds (defaults are conservative, tune per dataset):\n"
      << "  --flatline-ptp <X>           Flatline if peak-to-peak < X (default 1.0).\n"
      << "  --flatline-scale <X>         Flatline if robust scale < X (default 0 = disabled).\n"
      << "  --flatline-scale-factor <F>  Flatline if scale < F*median_scale (default 0.02).\n"
      << "  --noisy-scale-factor <F>     Noisy if scale > F*median_scale (default 10).\n"
      << "  --artifact-bad-frac <F>      Bad if flagged in >=F of artifact windows (default 0.30; 0 disables).\n"
      << "  --min-abs-corr <C>           Bad if |corr(ch, mean)| < C (default 0 disables).\n"
      << "  --max-samples-robust <N>     Downsample cap for robust stats/corr (default 50000).\n\n"
      << "Artifact window params (used for artifact-bad-frac):\n"
      << "  --window <sec>               (default 1.0)\n"
      << "  --step <sec>                 (default 0.5)\n"
      << "  --baseline <sec>             (default 10)\n"
      << "  --ptp-z <Z>                  (default 6)\n"
      << "  --rms-z <Z>                  (default 6)\n"
      << "  --kurtosis-z <Z>             (default 6)\n"
      << "  --min-bad-ch <N>             (default 1)\n\n"
      << "CSV input:\n"
      << "  --fs <Hz>                    Sampling rate hint if there is no time column.\n"
      << "  --no-time                    Do not write a leading time column when exporting CSV.\n\n"
      << "EDF output options (when --output ends with .edf):\n"
      << "  --record-duration <sec>      EDF record duration (default 1.0; 0 writes a single record).\n"
      << "  --patient-id <text>          (default 'X')\n"
      << "  --recording-id <text>        (default 'qeeg-channel-qc')\n"
      << "  --phys-dim <text>            (default 'uV')\n"
      << "  --plain-edf                  Force classic EDF (no EDF+ annotations channel).\n"
      << "  --annotation-spr <N>         Override annotation samples/record (0 = auto).\n\n"
      << "Other:\n"
      << "  -h, --help                   Show help.\n";
}

static bool is_flag(const std::string& a, const char* s1, const char* s2 = nullptr) {
  if (a == s1) return true;
  if (s2 && a == s2) return true;
  return false;
}

static std::string require_value(int& i, int argc, char** argv, const std::string& flag) {
  if (i + 1 >= argc) throw std::runtime_error("Missing value for " + flag);
  return std::string(argv[++i]);
}

static void ensure_dir(const std::string& path) {
  if (path.empty()) return;
  std::filesystem::create_directories(std::filesystem::u8path(path));
}

static void ensure_parent_dir(const std::string& path) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  const std::filesystem::path parent = p.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
}

static void write_bad_channels_txt(const std::string& path,
                                   const EEGRecording& rec,
                                   const std::vector<size_t>& bad_idx) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open for write: " + path);
  for (size_t i : bad_idx) {
    if (i < rec.channel_names.size()) {
      f << rec.channel_names[i] << "\n";
    }
  }
}

static void write_channel_qc_csv(const std::string& path, const ChannelQCResult& qc) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open for write: " + path);

  f << "channel,min,max,ptp,mean,stddev,robust_scale,artifact_bad_window_fraction,abs_corr_with_mean,flatline,noisy,artifact_often_bad,corr_low,bad,reasons\n";
  for (const auto& r : qc.channels) {
    f << csv_escape(r.channel) << ","
      << r.min_value << "," << r.max_value << "," << r.ptp << ","
      << r.mean << "," << r.stddev << "," << r.robust_scale << ","
      << r.artifact_bad_window_fraction << "," << r.abs_corr_with_mean << ","
      << (r.flatline ? 1 : 0) << "," << (r.noisy ? 1 : 0) << "," << (r.artifact_often_bad ? 1 : 0) << ","
      << (r.corr_low ? 1 : 0) << "," << (r.bad ? 1 : 0) << ","
      << csv_escape(r.reasons)
      << "\n";
  }
}

static void write_summary_txt(const std::string& path,
                              const ChannelQCResult& qc,
                              const InterpolateReport* interp_rep,
                              bool dropped,
                              const std::string& exported_path) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("Failed to open for write: " + path);

  f << "qeeg_channel_qc_cli summary\n";
  f << "Channels: " << qc.channels.size() << "\n";
  f << "Bad channels: " << qc.bad_indices.size() << "\n\n";

  f << "QC thresholds:\n";
  f << "  flatline_ptp=" << qc.opt.flatline_ptp << "\n";
  f << "  flatline_scale=" << qc.opt.flatline_scale << "\n";
  f << "  flatline_scale_factor=" << qc.opt.flatline_scale_factor << "\n";
  f << "  noisy_scale_factor=" << qc.opt.noisy_scale_factor << "\n";
  f << "  artifact_bad_window_fraction=" << qc.opt.artifact_bad_window_fraction << "\n";
  f << "  min_abs_corr=" << qc.opt.min_abs_corr << "\n";
  f << "  max_samples_for_robust=" << qc.opt.max_samples_for_robust << "\n\n";

  if (dropped) {
    f << "Action: dropped bad channels\n";
  }
  if (interp_rep) {
    f << "Action: interpolated bad channels (spherical spline)\n";
    f << "  interpolated=" << interp_rep->interpolated.size() << "\n";
    f << "  skipped_no_position=" << interp_rep->skipped_no_position.size() << "\n";
    f << "  skipped_not_enough_good=" << interp_rep->skipped_not_enough_good.size() << "\n";
    f << "  good_used=" << interp_rep->good_used.size() << "\n";
  }

  if (!exported_path.empty()) {
    f << "Exported: " << exported_path << "\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    Args args;

    if (argc <= 1) {
      print_help();
      return 1;
    }

    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];

      if (is_flag(a, "-h", "--help")) {
        print_help();
        return 0;
      } else if (is_flag(a, "--input", "-i")) {
        args.input_path = require_value(i, argc, argv, a);
      } else if (a == "--outdir") {
        args.outdir = require_value(i, argc, argv, a);
      } else if (a == "--fs") {
        args.fs_csv = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--channel-map") {
        args.channel_map_path = require_value(i, argc, argv, a);
      } else if (a == "--montage") {
        args.montage_path = require_value(i, argc, argv, a);
      } else if (a == "--interpolate") {
        args.interpolate = true;
      } else if (a == "--drop-bad") {
        args.drop_bad = true;
      } else if (a == "--output") {
        args.output_path = require_value(i, argc, argv, a);
      } else if (a == "--events-out") {
        args.events_out_csv = require_value(i, argc, argv, a);
      } else if (a == "--flatline-ptp") {
        args.flatline_ptp = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--flatline-scale") {
        args.flatline_scale = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--flatline-scale-factor") {
        args.flatline_scale_factor = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--noisy-scale-factor") {
        args.noisy_scale_factor = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--artifact-bad-frac") {
        args.artifact_bad_frac = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--min-abs-corr") {
        args.min_abs_corr = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--max-samples-robust") {
        args.max_samples_robust = static_cast<size_t>(std::stoll(require_value(i, argc, argv, a)));
      } else if (a == "--window") {
        args.window_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--step") {
        args.step_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--baseline") {
        args.baseline_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--ptp-z") {
        args.ptp_z = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--rms-z") {
        args.rms_z = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--kurtosis-z") {
        args.kurtosis_z = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--min-bad-ch") {
        args.min_bad_channels = static_cast<size_t>(std::stoll(require_value(i, argc, argv, a)));
      } else if (a == "--record-duration") {
        args.record_duration_seconds = std::stod(require_value(i, argc, argv, a));
      } else if (a == "--patient-id") {
        args.patient_id = require_value(i, argc, argv, a);
      } else if (a == "--recording-id") {
        args.recording_id = require_value(i, argc, argv, a);
      } else if (a == "--phys-dim") {
        args.phys_dim = require_value(i, argc, argv, a);
      } else if (a == "--plain-edf") {
        args.plain_edf = true;
      } else if (a == "--annotation-spr") {
        args.annotation_spr = std::stoi(require_value(i, argc, argv, a));
      } else if (a == "--no-time") {
        args.write_time = false;
      } else {
        throw std::runtime_error("Unknown argument: " + a);
      }
    }

    if (args.input_path.empty() || args.outdir.empty()) {
      throw std::runtime_error("Missing required arguments. Need --input and --outdir.");
    }
    if (args.interpolate && args.drop_bad) {
      throw std::runtime_error("Choose only one of --interpolate or --drop-bad.");
    }

    ensure_dir(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    if (!args.channel_map_path.empty()) {
      ChannelMap m = load_channel_map_file(args.channel_map_path);
      apply_channel_map(&rec, m);
    }

    ChannelQCOptions qopt;
    qopt.flatline_ptp = args.flatline_ptp;
    qopt.flatline_scale = args.flatline_scale;
    qopt.flatline_scale_factor = args.flatline_scale_factor;
    qopt.noisy_scale_factor = args.noisy_scale_factor;
    qopt.artifact_bad_window_fraction = args.artifact_bad_frac;
    qopt.max_samples_for_robust = args.max_samples_robust;
    qopt.min_abs_corr = args.min_abs_corr;

    qopt.artifact_opt.window_seconds = args.window_seconds;
    qopt.artifact_opt.step_seconds = args.step_seconds;
    qopt.artifact_opt.baseline_seconds = args.baseline_seconds;
    qopt.artifact_opt.ptp_z = args.ptp_z;
    qopt.artifact_opt.rms_z = args.rms_z;
    qopt.artifact_opt.kurtosis_z = args.kurtosis_z;
    qopt.artifact_opt.min_bad_channels = args.min_bad_channels;

    const ChannelQCResult qc = evaluate_channel_qc(rec, qopt);

    const std::string qc_csv = (std::filesystem::u8path(args.outdir) / "channel_qc.csv").u8string();
    const std::string bad_txt = (std::filesystem::u8path(args.outdir) / "bad_channels.txt").u8string();
    const std::string summary_txt = (std::filesystem::u8path(args.outdir) / "qc_summary.txt").u8string();

    write_channel_qc_csv(qc_csv, qc);
    write_bad_channels_txt(bad_txt, rec, qc.bad_indices);

    bool dropped = false;
    InterpolateReport interp_rep;
    InterpolateReport* interp_rep_ptr = nullptr;

    if (args.drop_bad && !qc.bad_indices.empty()) {
      ChannelMap m;
      for (size_t i : qc.bad_indices) {
        if (i >= rec.channel_names.size()) continue;
        m.normalized_to_name[normalize_channel_name(rec.channel_names[i])] = "DROP";
      }
      apply_channel_map(&rec, m);
      dropped = true;
    }

    if (args.interpolate && !qc.bad_indices.empty()) {
      Montage montage;
      if (args.montage_path.empty()) {
        montage = Montage::builtin_standard_1020_19();
      } else {
        montage = Montage::load_csv(args.montage_path);
      }

      interp_rep = interpolate_bad_channels_spherical_spline(&rec, montage, qc.bad_indices);
      interp_rep_ptr = &interp_rep;
    }

    std::string exported;
    if (!args.output_path.empty() || args.drop_bad || args.interpolate) {
      // If the user requested an action but didn't provide an output path, write a default EDF into outdir.
      if (args.output_path.empty()) {
        args.output_path = (std::filesystem::u8path(args.outdir) / "qc_output.edf").u8string();
      }

      const std::string out_low = to_lower(args.output_path);
      if (ends_with(out_low, ".edf") || ends_with(out_low, ".edf+") || ends_with(out_low, ".rec")) {
        ensure_parent_dir(args.output_path);
        EDFWriterOptions wopts;
        wopts.record_duration_seconds = args.record_duration_seconds;
        wopts.patient_id = args.patient_id;
        wopts.recording_id = args.recording_id;
        wopts.physical_dimension = args.phys_dim;
        wopts.write_edfplus_annotations = !args.plain_edf;
        wopts.annotation_samples_per_record = args.annotation_spr;

        EDFWriter w;
        w.write(rec, args.output_path, wopts);
        exported = args.output_path;
      } else if (ends_with(out_low, ".csv")) {
        ensure_parent_dir(args.output_path);
        write_recording_csv(args.output_path, rec, args.write_time);
        exported = args.output_path;
      } else {
        throw std::runtime_error("Unsupported output extension (use .edf or .csv): " + args.output_path);
      }

      if (!args.events_out_csv.empty()) {
        ensure_parent_dir(args.events_out_csv);
        write_events_csv(args.events_out_csv, rec);
      }
    }

    write_summary_txt(summary_txt, qc, interp_rep_ptr, dropped, exported);

    std::cout << "Wrote: " << qc_csv << "\n";
    std::cout << "Wrote: " << bad_txt << "\n";
    std::cout << "Wrote: " << summary_txt << "\n";
    if (!exported.empty()) {
      std::cout << "Exported: " << exported << "\n";
    }
    if (!args.events_out_csv.empty()) {
      std::cout << "Wrote events: " << args.events_out_csv << "\n";
    }

    if (interp_rep_ptr) {
      std::cout << "Interpolated channels: " << interp_rep_ptr->interpolated.size() << "\n";
      if (!interp_rep_ptr->skipped_no_position.empty()) {
        std::cout << "Skipped (no montage position): " << interp_rep_ptr->skipped_no_position.size() << "\n";
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }
}
