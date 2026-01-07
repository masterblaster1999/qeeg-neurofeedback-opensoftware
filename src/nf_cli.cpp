#include "qeeg/bandpower.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/online_bandpower.hpp"
#include "qeeg/online_coherence.hpp"
#include "qeeg/online_artifacts.hpp"
#include "qeeg/online_pac.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/wav_writer.hpp"
#include "qeeg/osc.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_nf"};
  std::string band_spec; // empty => default
  std::string metric_spec{"alpha:Pz"};

  bool demo{false};
  double fs_csv{0.0};
  double demo_seconds{60.0};

  bool average_reference{false};

  // Optional preprocessing filters (applied causally during playback)
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};

  // Online estimation params
  double window_seconds{2.0};
  double update_seconds{0.25};
  size_t nperseg{512};
  double overlap{0.5};

  // Neurofeedback threshold params
  double baseline_seconds{10.0};
  double target_reward_rate{0.6};
  double adapt_eta{0.10};
  double reward_rate_window_seconds{5.0};
  bool no_adaptation{false};

  // Playback
  double chunk_seconds{0.10};

  // Debug exports
  bool export_bandpowers{false};   // bandpower mode only
  bool export_coherence{false};    // coherence mode only

  // Optional artifact gating (time-domain robust outlier detection)
  bool artifact_gate{false};
  double artifact_ptp_z{6.0};
  double artifact_rms_z{6.0};
  double artifact_kurtosis_z{6.0};
  int artifact_min_bad_channels{1};
  bool export_artifacts{false};

  // Optional audio feedback (writes a simple reward tone WAV)
  // If audio_wav is a filename without any path separators, it will be written inside --outdir.
  std::string audio_wav; // empty => disabled
  int audio_rate{44100};
  double audio_tone_hz{440.0};
  double audio_gain{0.20};
  double audio_attack_sec{0.005};
  double audio_release_sec{0.010};

  // Optional OSC/UDP output (for integrating with external apps).
  // Enabled when --osc-port is set to a value > 0.
  std::string osc_host{"127.0.0.1"};
  int osc_port{0};
  std::string osc_prefix{"/qeeg"};
  std::string osc_mode{"state"}; // 'state' (one message) or 'split' (several)

  // PAC estimator params (PAC mode only)
  size_t pac_bins{18};
  double pac_trim{0.10};
  bool pac_zero_phase{false};
};

static void print_help() {
  std::cout
    << "qeeg_nf_cli (first pass neurofeedback engine)\n\n"
    << "Usage:\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric alpha:Pz\n"
    << "  qeeg_nf_cli --input file.bdf --outdir out_nf --metric alpha/beta:Pz\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric coh:alpha:F3:F4\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric pac:theta:gamma:Cz\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric mvl:theta:gamma:Cz\n"
    << "  qeeg_nf_cli --demo --fs 250 --seconds 60 --outdir out_demo_nf\n\n"
    << "Options:\n"
    << "  --input PATH              Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                   Sampling rate for CSV (required for CSV); also used for --demo\n"
    << "  --outdir DIR              Output directory (default: out_nf)\n"
    << "  --bands SPEC              Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
    << "  --metric SPEC             Metric: 'alpha:Pz' (bandpower), 'alpha/beta:Pz' (ratio),\n"
    << "                           'coh:alpha:F3:F4' (magnitude-squared coherence),\n"
    << "                           'pac:PHASE:AMP:CH' (Tort MI), or 'mvl:PHASE:AMP:CH'\n"
    << "  --window S                Sliding window seconds (default: 2.0)\n"
    << "  --update S                Update interval seconds (default: 0.25)\n"
    << "  --nperseg N               Welch segment length (default: 512)\n"
    << "  --overlap FRAC            Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --baseline S              Baseline duration seconds for initial threshold (default: 10)\n"
    << "  --target-rate R           Target reward rate in (0,1) (default: 0.6)\n"
    << "  --eta E                   Adaptation speed (default: 0.10)\n"
    << "  --rate-window S           Reward-rate window seconds (default: 5)\n"
    << "  --no-adaptation            Disable adaptive thresholding (fixed threshold from baseline)\n"
    << "  --average-reference        Apply common average reference across channels\n"
    << "  --notch HZ                 Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q                Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI           Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --chunk S                 File playback chunk seconds (default: 0.10)\n"
    << "  --export-bandpowers        Write bandpower_timeseries.csv (bandpower/ratio modes)\n"
    << "  --export-coherence         Write coherence_timeseries.csv (coherence mode)\n"
    << "  --artifact-gate            Suppress reward/adaptation during detected artifacts\n"
    << "  --artifact-ptp-z Z         Artifact threshold: peak-to-peak robust z (<=0 disables; default: 6)\n"
    << "  --artifact-rms-z Z         Artifact threshold: RMS robust z (<=0 disables; default: 6)\n"
    << "  --artifact-kurtosis-z Z    Artifact threshold: excess kurtosis robust z (<=0 disables; default: 6)\n"
    << "  --artifact-min-bad-ch N    Artifact frame is bad if >=N channels flagged (default: 1)\n"
    << "  --export-artifacts         Write artifact_gate_timeseries.csv aligned to NF updates\n"
    << "  --audio-wav PATH           Optional: write a reward-tone WAV (mono PCM16)\n"
    << "  --audio-rate HZ            Audio sample rate (default: 44100)\n"
    << "  --audio-tone HZ            Reward tone frequency (default: 440)\n"
    << "  --audio-gain G             Reward tone gain in [0,1] (default: 0.2)\n"
    << "  --audio-attack S           Tone attack seconds (default: 0.005)\n"
    << "  --audio-release S          Tone release seconds (default: 0.010)\n"
    << "  --osc-host HOST            Optional: OSC/UDP destination host (default: 127.0.0.1)\n"
    << "  --osc-port PORT            Optional: OSC/UDP destination port (0 disables; e.g. 9000)\n"
    << "  --osc-prefix PATH          OSC address prefix (default: /qeeg)\n"
    << "  --osc-mode MODE            OSC mode: state|split (default: state)\n"
    << "  --pac-bins N              PAC: #phase bins for MI (default: 18)\n"
    << "  --pac-trim FRAC           PAC: edge trim fraction per window (default: 0.10)\n"
    << "  --pac-zero-phase          PAC: use zero-phase bandpass filters (default: off)\n"
    << "  --demo                    Generate synthetic recording instead of reading file\n"
    << "  --seconds S               Duration for --demo (default: 60)\n"
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
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
    } else if (arg == "--metric" && i + 1 < argc) {
      a.metric_spec = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_seconds = to_double(argv[++i]);
    } else if (arg == "--update" && i + 1 < argc) {
      a.update_seconds = to_double(argv[++i]);
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--baseline" && i + 1 < argc) {
      a.baseline_seconds = to_double(argv[++i]);
    } else if (arg == "--target-rate" && i + 1 < argc) {
      a.target_reward_rate = to_double(argv[++i]);
    } else if (arg == "--eta" && i + 1 < argc) {
      a.adapt_eta = to_double(argv[++i]);
    } else if (arg == "--rate-window" && i + 1 < argc) {
      a.reward_rate_window_seconds = to_double(argv[++i]);
    } else if (arg == "--no-adaptation") {
      a.no_adaptation = true;
    } else if (arg == "--average-reference") {
      a.average_reference = true;
    } else if (arg == "--notch" && i + 1 < argc) {
      a.notch_hz = to_double(argv[++i]);
    } else if (arg == "--notch-q" && i + 1 < argc) {
      a.notch_q = to_double(argv[++i]);
    } else if (arg == "--bandpass" && i + 2 < argc) {
      a.bandpass_low_hz = to_double(argv[++i]);
      a.bandpass_high_hz = to_double(argv[++i]);
    } else if (arg == "--chunk" && i + 1 < argc) {
      a.chunk_seconds = to_double(argv[++i]);
    } else if (arg == "--export-bandpowers") {
      a.export_bandpowers = true;
    } else if (arg == "--export-coherence") {
      a.export_coherence = true;
    } else if (arg == "--artifact-gate") {
      a.artifact_gate = true;
    } else if (arg == "--artifact-ptp-z" && i + 1 < argc) {
      a.artifact_ptp_z = to_double(argv[++i]);
    } else if (arg == "--artifact-rms-z" && i + 1 < argc) {
      a.artifact_rms_z = to_double(argv[++i]);
    } else if (arg == "--artifact-kurtosis-z" && i + 1 < argc) {
      a.artifact_kurtosis_z = to_double(argv[++i]);
    } else if (arg == "--artifact-min-bad-ch" && i + 1 < argc) {
      a.artifact_min_bad_channels = to_int(argv[++i]);
    } else if (arg == "--export-artifacts") {
      a.export_artifacts = true;
    } else if (arg == "--audio-wav" && i + 1 < argc) {
      a.audio_wav = argv[++i];
    } else if (arg == "--audio-rate" && i + 1 < argc) {
      a.audio_rate = to_int(argv[++i]);
    } else if (arg == "--audio-tone" && i + 1 < argc) {
      a.audio_tone_hz = to_double(argv[++i]);
    } else if (arg == "--audio-gain" && i + 1 < argc) {
      a.audio_gain = to_double(argv[++i]);
    } else if (arg == "--audio-attack" && i + 1 < argc) {
      a.audio_attack_sec = to_double(argv[++i]);
    } else if (arg == "--audio-release" && i + 1 < argc) {
      a.audio_release_sec = to_double(argv[++i]);
    } else if (arg == "--osc-host" && i + 1 < argc) {
      a.osc_host = argv[++i];
    } else if (arg == "--osc-port" && i + 1 < argc) {
      a.osc_port = to_int(argv[++i]);
    } else if (arg == "--osc-prefix" && i + 1 < argc) {
      a.osc_prefix = argv[++i];
    } else if (arg == "--osc-mode" && i + 1 < argc) {
      a.osc_mode = argv[++i];
    } else if (arg == "--pac-bins" && i + 1 < argc) {
      a.pac_bins = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--pac-trim" && i + 1 < argc) {
      a.pac_trim = to_double(argv[++i]);
    } else if (arg == "--pac-zero-phase") {
      a.pac_zero_phase = true;
    } else if (arg == "--demo") {
      a.demo = true;
    } else if (arg == "--seconds" && i + 1 < argc) {
      a.demo_seconds = to_double(argv[++i]);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static std::string resolve_out_path(const std::string& outdir, const std::string& path_or_name) {
  if (path_or_name.empty()) return path_or_name;
  // If it looks like a filename (no path separators), write inside outdir.
  if (path_or_name.find('/') == std::string::npos && path_or_name.find('\\') == std::string::npos) {
    return outdir + "/" + path_or_name;
  }
  return path_or_name;
}

static void write_reward_tone_wav_if_requested(const Args& args,
                                              const std::vector<int>& reward_flags) {
  if (args.audio_wav.empty()) return;
  if (args.audio_rate <= 0) throw std::runtime_error("--audio-rate must be > 0");
  if (args.audio_tone_hz <= 0.0) throw std::runtime_error("--audio-tone must be > 0");
  if (args.audio_gain < 0.0) throw std::runtime_error("--audio-gain must be >= 0");
  if (args.audio_attack_sec < 0.0) throw std::runtime_error("--audio-attack must be >= 0");
  if (args.audio_release_sec < 0.0) throw std::runtime_error("--audio-release must be >= 0");

  const std::string outpath = resolve_out_path(args.outdir, args.audio_wav);

  // One audio segment per NF update.
  const int sr = args.audio_rate;
  const size_t seg = std::max<size_t>(1, static_cast<size_t>(std::llround(args.update_seconds * sr)));

  const size_t attack = static_cast<size_t>(std::llround(args.audio_attack_sec * sr));
  const size_t release = static_cast<size_t>(std::llround(args.audio_release_sec * sr));

  std::vector<float> mono;
  mono.reserve(reward_flags.size() * seg);

  const double two_pi = 2.0 * std::acos(-1.0);
  const double phase_inc = two_pi * args.audio_tone_hz / static_cast<double>(sr);
  double phase = 0.0;

  // Generate contiguous runs of reward=1 as continuous tones with a simple attack/release envelope.
  for (size_t i = 0; i < reward_flags.size(); ) {
    if (reward_flags[i] == 0) {
      mono.insert(mono.end(), seg, 0.0f);
      ++i;
      // Reset phase so re-started beeps are phase-aligned (also avoids large phase accumulation).
      phase = 0.0;
      continue;
    }

    size_t j = i;
    while (j < reward_flags.size() && reward_flags[j] != 0) ++j;
    const size_t run_frames = j - i;
    const size_t run_samples = run_frames * seg;

    for (size_t k = 0; k < run_samples; ++k) {
      // Piecewise-linear envelope at the run boundaries.
      double env = 1.0;
      if (attack > 0 && k < attack) {
        env = static_cast<double>(k) / static_cast<double>(attack);
      }
      if (release > 0 && k + release > run_samples) {
        const size_t kr = run_samples - k;
        const double e2 = static_cast<double>(kr) / static_cast<double>(release);
        if (e2 < env) env = e2;
      }
      if (env < 0.0) env = 0.0;
      if (env > 1.0) env = 1.0;

      const float s = static_cast<float>(std::sin(phase) * (args.audio_gain * env));
      mono.push_back(s);
      phase += phase_inc;
      if (phase > two_pi) phase -= two_pi;
    }

    i = j;
  }

  write_wav_mono_pcm16(outpath, sr, mono);
  std::cout << "Wrote audio reward tone: " << outpath << "\n";
}


static std::string normalize_osc_prefix(std::string p) {
  p = trim(p);
  if (p.empty()) p = "/qeeg";
  if (!p.empty() && p[0] != '/') p = "/" + p;
  // Remove trailing slashes (but keep "/" if user explicitly wants it).
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

static void osc_send_info(OscUdpClient* osc, const std::string& prefix, const Args& args, double fs_hz) {
  if (!osc) return;
  try {
    OscMessage m1(prefix + "/metric_spec");
    m1.add_string(args.metric_spec);
    osc->send(m1);

    OscMessage m2(prefix + "/fs");
    m2.add_float32(static_cast<float>(fs_hz));
    osc->send(m2);
  } catch (...) {
    // best-effort
  }
}

static void osc_send_state(OscUdpClient* osc,
                           const std::string& prefix,
                           const std::string& mode,
                           double t_end_sec,
                           double metric,
                           double threshold,
                           int reward,
                           double reward_rate,
                           int have_threshold) {
  if (!osc) return;
  try {
    if (mode == "split") {
      OscMessage mt(prefix + "/time");
      mt.add_float32(static_cast<float>(t_end_sec));
      osc->send(mt);

      OscMessage mm(prefix + "/metric");
      mm.add_float32(static_cast<float>(metric));
      osc->send(mm);

      OscMessage mth(prefix + "/threshold");
      mth.add_float32(static_cast<float>(threshold));
      osc->send(mth);

      OscMessage mr(prefix + "/reward");
      mr.add_int32(reward);
      osc->send(mr);

      OscMessage mrr(prefix + "/reward_rate");
      mrr.add_float32(static_cast<float>(reward_rate));
      osc->send(mrr);

      OscMessage mht(prefix + "/have_threshold");
      mht.add_int32(have_threshold);
      osc->send(mht);

      return;
    }

    // Default: one state message per update.
    OscMessage msg(prefix + "/state");
    msg.add_float32(static_cast<float>(t_end_sec));
    msg.add_float32(static_cast<float>(metric));
    msg.add_float32(static_cast<float>(threshold));
    msg.add_int32(reward);
    msg.add_float32(static_cast<float>(reward_rate));
    msg.add_int32(have_threshold);
    osc->send(msg);
  } catch (...) {
    // best-effort
  }
}

static void osc_send_artifact(OscUdpClient* osc,
                              const std::string& prefix,
                              const OnlineArtifactFrame& fr) {
  if (!osc) return;
  try {
    OscMessage mr(prefix + "/artifact_ready");
    mr.add_int32(fr.baseline_ready ? 1 : 0);
    osc->send(mr);

    OscMessage ma(prefix + "/artifact");
    ma.add_int32((fr.baseline_ready && fr.bad) ? 1 : 0);
    osc->send(ma);

    OscMessage mb(prefix + "/artifact_bad_channels");
    mb.add_int32(static_cast<int>(fr.bad_channel_count));
    osc->send(mb);
  } catch (...) {
    // best-effort
  }
}

static OnlineArtifactFrame take_artifact_frame(std::deque<OnlineArtifactFrame>* q,
                                               double t_end_sec,
                                               double eps_sec) {
  OnlineArtifactFrame none;
  none.t_end_sec = t_end_sec;
  none.baseline_ready = false;
  none.bad = false;
  none.bad_channel_count = 0;
  if (!q) return none;

  while (!q->empty() && q->front().t_end_sec < t_end_sec - eps_sec) {
    q->pop_front();
  }
  if (q->empty()) return none;

  // If the next artifact frame matches closely, consume it.
  if (std::fabs(q->front().t_end_sec - t_end_sec) <= eps_sec) {
    OnlineArtifactFrame out = q->front();
    q->pop_front();
    return out;
  }

  // If artifact frame is slightly behind, consume it; otherwise leave it.
  if (q->front().t_end_sec <= t_end_sec + eps_sec) {
    OnlineArtifactFrame out = q->front();
    q->pop_front();
    return out;
  }

  return none;
}

static EEGRecording make_demo_recording(const Montage& montage, double fs_hz, double seconds) {
  if (fs_hz <= 0.0) throw std::runtime_error("--demo requires --fs > 0");
  if (seconds <= 0.0) seconds = 60.0;

  EEGRecording rec;
  rec.fs_hz = fs_hz;

  const std::vector<std::string> canonical = {
    "Fp1","Fp2","F7","F3","Fz","F4","F8",
    "T3","C3","Cz","C4","T4",
    "T5","P3","Pz","P4","T6","O1","O2"
  };

  for (const auto& ch : canonical) {
    if (montage.has(ch)) rec.channel_names.push_back(ch);
  }
  if (rec.channel_names.empty()) {
    rec.channel_names = montage.channel_names();
  }

  const size_t n = static_cast<size_t>(std::llround(seconds * fs_hz));
  rec.data.assign(rec.channel_names.size(), std::vector<float>(n, 0.0f));

  std::mt19937 rng(12345);
  std::normal_distribution<double> noise(0.0, 1.0);
  const double pi = std::acos(-1.0);

  for (size_t c = 0; c < rec.channel_names.size(); ++c) {
    Vec2 p;
    montage.get(rec.channel_names[c], &p);

    double frontal = std::max(0.0, p.y);
    double occip = std::max(0.0, -p.y);
    double left = std::max(0.0, -p.x);
    double right = std::max(0.0, p.x);

    // Make alpha strongest occipitally, theta strongest frontally.
    double a_delta = 4.0 * (0.2 + 0.8 * occip);
    double a_theta = 3.5 * (0.3 + 0.7 * frontal);
    double a_alpha = 8.0 * (0.2 + 0.8 * occip);
    double a_beta  = 2.0 * (0.5 + 0.5 * (left + right) * 0.5);

    a_alpha *= (1.0 + 0.2 * (right - left));
    a_theta *= (1.0 + 0.1 * (left - right));

    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / fs_hz;
      const double v =
          a_delta * std::sin(2.0 * pi * 2.0  * t) +
          a_theta * std::sin(2.0 * pi * 6.0  * t) +
          a_alpha * std::sin(2.0 * pi * 10.0 * t) +
          a_beta  * std::sin(2.0 * pi * 20.0 * t) +
          0.8 * noise(rng);
      rec.data[c][i] = static_cast<float>(v);
    }
  }

  return rec;
}

struct MetricSpec {
  enum class Type { Band, Ratio, Coherence, Pac };
  Type type{Type::Band};

  // Band (and coherence) selection.
  std::string band;

  // Ratio bands.
  std::string band_num;
  std::string band_den;

  // Band/ratio channel.
  std::string channel;

  // Coherence pair.
  std::string channel_a;
  std::string channel_b;

  // PAC (phase-amplitude coupling)
  PacMethod pac_method{PacMethod::ModulationIndex};
  std::string phase_band;
  std::string amp_band;
};

static MetricSpec parse_metric_spec(const std::string& s) {
  // Supported:
  //  - alpha:Pz
  //  - alpha/beta:Pz
  //  - band:alpha:Pz
  //  - ratio:alpha:beta:Pz
  //  - coh:alpha:F3:F4
  //  - coherence:alpha:F3:F4
  //  - pac:theta:gamma:Cz  (Tort MI)
  //  - mvl:theta:gamma:Cz  (mean vector length)
  const auto parts = split(trim(s), ':');
  if (parts.empty()) throw std::runtime_error("--metric: empty spec");

  // Long-form
  if (parts.size() >= 1) {
    const std::string head = to_lower(trim(parts[0]));
    if (head == "band") {
      if (parts.size() != 3) throw std::runtime_error("--metric band: expects band:NAME:CHANNEL");
      MetricSpec m;
      m.type = MetricSpec::Type::Band;
      m.band = trim(parts[1]);
      m.channel = trim(parts[2]);
      return m;
    }
    if (head == "ratio") {
      if (parts.size() != 4) throw std::runtime_error("--metric ratio: expects ratio:NUM:DEN:CHANNEL");
      MetricSpec m;
      m.type = MetricSpec::Type::Ratio;
      m.band_num = trim(parts[1]);
      m.band_den = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }
    if (head == "coh" || head == "coherence") {
      if (parts.size() != 4) throw std::runtime_error("--metric coh: expects coh:BAND:CH_A:CH_B");
      MetricSpec m;
      m.type = MetricSpec::Type::Coherence;
      m.band = trim(parts[1]);
      m.channel_a = trim(parts[2]);
      m.channel_b = trim(parts[3]);
      return m;
    }
    if (head == "pac" || head == "pacmi") {
      if (parts.size() != 4) throw std::runtime_error("--metric pac: expects pac:PHASE:AMP:CHANNEL");
      MetricSpec m;
      m.type = MetricSpec::Type::Pac;
      m.pac_method = PacMethod::ModulationIndex;
      m.phase_band = trim(parts[1]);
      m.amp_band = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }
    if (head == "mvl" || head == "pacmvl") {
      if (parts.size() != 4) throw std::runtime_error("--metric mvl: expects mvl:PHASE:AMP:CHANNEL");
      MetricSpec m;
      m.type = MetricSpec::Type::Pac;
      m.pac_method = PacMethod::MeanVectorLength;
      m.phase_band = trim(parts[1]);
      m.amp_band = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }
  }

  // Short-form (bandpower or ratio)
  if (parts.size() != 2) {
    throw std::runtime_error("--metric: expected 'alpha:Pz', 'alpha/beta:Pz', 'coh:alpha:F3:F4', or 'pac:theta:gamma:Cz'");
  }
  MetricSpec m;
  const std::string left = trim(parts[0]);
  m.channel = trim(parts[1]);
  const auto slash = left.find('/');
  if (slash == std::string::npos) {
    m.type = MetricSpec::Type::Band;
    m.band = left;
  } else {
    m.type = MetricSpec::Type::Ratio;
    m.band_num = trim(left.substr(0, slash));
    m.band_den = trim(left.substr(slash + 1));
  }
  return m;
}

static int find_channel_index(const std::vector<std::string>& channels, const std::string& name) {
  const std::string target = to_lower(trim(name));
  for (size_t i = 0; i < channels.size(); ++i) {
    if (to_lower(channels[i]) == target) return static_cast<int>(i);
  }
  return -1;
}

static int find_band_index(const std::vector<BandDefinition>& bands, const std::string& name) {
  const std::string target = to_lower(trim(name));
  for (size_t i = 0; i < bands.size(); ++i) {
    if (to_lower(bands[i].name) == target) return static_cast<int>(i);
  }
  return -1;
}

static BandDefinition resolve_band_token(const std::vector<BandDefinition>& bands,
                                        const std::string& token,
                                        const std::string& label) {
  // 1) Try name lookup.
  const int idx = find_band_index(bands, token);
  if (idx >= 0) return bands[static_cast<size_t>(idx)];

  // 2) Try explicit range "LO-HI".
  const std::string t = trim(token);
  const auto edges = split(t, '-');
  if (edges.size() == 2) {
    const double lo = to_double(edges[0]);
    const double hi = to_double(edges[1]);
    if (!(lo > 0.0 && hi > lo)) {
      throw std::runtime_error(label + " band range must satisfy 0 < LO < HI: " + token);
    }
    BandDefinition b;
    b.name = label;
    b.fmin_hz = lo;
    b.fmax_hz = hi;
    return b;
  }

  throw std::runtime_error(label + " band not found (name) and not a range (LO-HI): " + token);
}

static double compute_metric_band_or_ratio(const OnlineBandpowerFrame& fr,
                                          const MetricSpec& spec,
                                          int ch_idx,
                                          int b_idx,
                                          int b_num,
                                          int b_den) {
  const size_t c = static_cast<size_t>(ch_idx);
  if (spec.type == MetricSpec::Type::Band) {
    return fr.powers[static_cast<size_t>(b_idx)][c];
  }
  // Ratio
  const double eps = 1e-12;
  const double num = fr.powers[static_cast<size_t>(b_num)][c];
  const double den = fr.powers[static_cast<size_t>(b_den)][c];
  return (num + eps) / (den + eps);
}

static double median(std::vector<double> v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = v.size();
  const size_t mid = n / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  double m = v[mid];
  if ((n % 2) == 0) {
    std::nth_element(v.begin(), v.begin() + (mid - 1), v.end());
    m = 0.5 * (m + v[mid - 1]);
  }
  return m;
}

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    if (!args.demo && args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required (or use --demo)");
    }
    if (args.target_reward_rate <= 0.0 || args.target_reward_rate >= 1.0) {
      throw std::runtime_error("--target-rate must be in (0,1)");
    }
    if (args.adapt_eta < 0.0) {
      throw std::runtime_error("--eta must be >= 0");
    }
    if (args.artifact_min_bad_channels < 1) {
      throw std::runtime_error("--artifact-min-bad-ch must be >= 1");
    }

    ensure_directory(args.outdir);

    EEGRecording rec;
    if (args.demo) {
      Montage montage = Montage::builtin_standard_1020_19();
      rec = make_demo_recording(montage, args.fs_csv, args.demo_seconds);
    } else {
      rec = read_recording_auto(args.input_path, args.fs_csv);
    }

    if (rec.n_channels() < 1) throw std::runtime_error("Recording has no channels");
    if (rec.fs_hz <= 0.0) throw std::runtime_error("Invalid sampling rate");

    std::cout << "Loaded recording: " << rec.n_channels() << " channels, "
              << rec.n_samples() << " samples, fs=" << rec.fs_hz << " Hz\n";


    // Optional OSC output for integration with external tools (UDP is best-effort / unreliable).
    std::unique_ptr<OscUdpClient> osc_client;
    OscUdpClient* osc = nullptr;
    std::string osc_prefix;
    std::string osc_mode = to_lower(args.osc_mode);

    if (args.osc_port != 0) {
      if (args.osc_port < 0 || args.osc_port > 65535) {
        throw std::runtime_error("--osc-port must be 0 (disable) or in [1, 65535]");
      }
      if (osc_mode != "state" && osc_mode != "split") {
        throw std::runtime_error("--osc-mode must be 'state' or 'split'");
      }
      osc_prefix = normalize_osc_prefix(args.osc_prefix);
      osc_client = std::make_unique<OscUdpClient>(args.osc_host, args.osc_port);
      if (!osc_client->ok()) {
        std::cerr << "OSC disabled: " << osc_client->last_error() << "\n";
      } else {
        osc = osc_client.get();
        std::cout << "OSC/UDP output enabled: " << args.osc_host << ":" << args.osc_port
                  << " prefix=" << osc_prefix << " mode=" << osc_mode << "\n";
        osc_send_info(osc, osc_prefix, args, rec.fs_hz);
      }
    }

    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = false;

    const bool do_pre = popt.average_reference || popt.notch_hz > 0.0 ||
                        popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0;
    if (do_pre) {
      std::cout << "Streaming preprocessing (causal):\n";
      if (popt.average_reference) {
        std::cout << "  - CAR (average reference)\n";
      }
      if (popt.notch_hz > 0.0) {
        std::cout << "  - notch " << popt.notch_hz << " Hz (Q=" << popt.notch_q << ")\n";
      }
      if (popt.bandpass_low_hz > 0.0 || popt.bandpass_high_hz > 0.0) {
        std::cout << "  - bandpass " << popt.bandpass_low_hz << ".." << popt.bandpass_high_hz << " Hz\n";
      }
    }

    StreamingPreprocessor pre(rec.n_channels(), rec.fs_hz, popt);

    const std::vector<BandDefinition> bands = parse_band_spec(args.band_spec);
    const MetricSpec metric = parse_metric_spec(args.metric_spec);

    // Output
    std::ofstream out(args.outdir + "/nf_feedback.csv");
    if (!out) throw std::runtime_error("Failed to write nf_feedback.csv");

    const bool do_artifacts = args.artifact_gate || args.export_artifacts;

    out << "t_end_sec,metric,threshold,reward,reward_rate";
    if (do_artifacts) {
      out << ",artifact_ready,artifact,bad_channels";
    }
    if (metric.type == MetricSpec::Type::Band) {
      out << ",band,channel";
    } else if (metric.type == MetricSpec::Type::Ratio) {
      out << ",band_num,band_den,channel";
    } else if (metric.type == MetricSpec::Type::Coherence) {
      out << ",band,channel_a,channel_b";
    } else {
      out << ",phase_band,amp_band,channel,method";
    }
    out << "\n";

    std::ofstream out_bp;
    std::ofstream out_coh;

    // Thresholding state
    std::vector<double> baseline_values;
    baseline_values.reserve(256);
    bool have_threshold = false;
    double threshold = std::numeric_limits<double>::quiet_NaN();

    const size_t rate_window_frames = std::max<size_t>(
      1,
      sec_to_samples(args.reward_rate_window_seconds, 1.0 / args.update_seconds));
    std::deque<int> reward_hist;
    reward_hist.clear();
    auto reward_rate = [&]() {
      if (reward_hist.empty()) return 0.0;
      int sum = 0;
      for (int v : reward_hist) sum += v;
      return static_cast<double>(sum) / static_cast<double>(reward_hist.size());
    };

    // Optional audio export: one 0/1 flag per emitted NF update (including baseline frames).
    std::vector<int> audio_reward_flags;
    audio_reward_flags.reserve(1024);

    // Optional artifact engine (aligned to NF updates).
    std::unique_ptr<OnlineArtifactGate> artifact_gate;
    OnlineArtifactGate* art = nullptr;
    std::deque<OnlineArtifactFrame> art_queue;
    std::ofstream out_art;

    if (args.artifact_gate || args.export_artifacts) {
      OnlineArtifactOptions aopt;
      aopt.window_seconds = args.window_seconds;
      aopt.update_seconds = args.update_seconds;
      aopt.baseline_seconds = args.baseline_seconds;
      aopt.ptp_z = args.artifact_ptp_z;
      aopt.rms_z = args.artifact_rms_z;
      aopt.kurtosis_z = args.artifact_kurtosis_z;
      aopt.min_bad_channels = static_cast<size_t>(args.artifact_min_bad_channels);
      artifact_gate = std::make_unique<OnlineArtifactGate>(rec.channel_names, rec.fs_hz, aopt);
      art = artifact_gate.get();
      if (args.export_artifacts) {
        out_art.open(args.outdir + "/artifact_gate_timeseries.csv");
        if (!out_art) throw std::runtime_error("Failed to write artifact_gate_timeseries.csv");
        out_art << "t_end_sec,artifact_ready,artifact,bad_channels,max_ptp_z,max_rms_z,max_kurtosis_z\n";
      }
      std::cout << "Artifact engine enabled (gate=" << (args.artifact_gate ? "on" : "off")
                << ", export=" << (args.export_artifacts ? "on" : "off") << ")\n";
    }

    const size_t chunk_samples = std::max<size_t>(1, sec_to_samples(args.chunk_seconds, rec.fs_hz));
    std::vector<std::vector<float>> block(rec.n_channels());

    if (metric.type == MetricSpec::Type::Coherence) {
      // Resolve pair indices from the recording.
      const int ia = find_channel_index(rec.channel_names, metric.channel_a);
      const int ib = find_channel_index(rec.channel_names, metric.channel_b);
      if (ia < 0) throw std::runtime_error("Metric channel_a not found in recording: " + metric.channel_a);
      if (ib < 0) throw std::runtime_error("Metric channel_b not found in recording: " + metric.channel_b);
      if (ia == ib) throw std::runtime_error("coherence metric requires two different channels");

      OnlineCoherenceOptions opt;
      opt.window_seconds = args.window_seconds;
      opt.update_seconds = args.update_seconds;
      opt.welch.nperseg = args.nperseg;
      opt.welch.overlap_fraction = args.overlap;

      OnlineWelchCoherence eng(rec.channel_names, rec.fs_hz, bands, {{ia, ib}}, opt);

      // Resolve band index once we see a frame.
      int b_idx = -1;

      if (args.export_coherence) {
        out_coh.open(args.outdir + "/coherence_timeseries.csv");
        if (!out_coh) throw std::runtime_error("Failed to write coherence_timeseries.csv");
        out_coh << "t_end_sec";
        const std::string pair_name = metric.channel_a + "_" + metric.channel_b;
        for (const auto& b : bands) {
          out_coh << "," << b.name << "_" << pair_name;
        }
        out_coh << "\n";
      }

      for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
        const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
        const size_t len = end - pos;
        (void)len;
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          block[c].assign(rec.data[c].begin() + static_cast<std::ptrdiff_t>(pos),
                          rec.data[c].begin() + static_cast<std::ptrdiff_t>(end));
        }

        pre.process_block(&block);

        if (art) {
          const auto aframes = art->push_block(block);
          for (const auto& af : aframes) {
            art_queue.push_back(af);
            if (args.export_artifacts) {
              out_art << af.t_end_sec << "," << (af.baseline_ready ? 1 : 0)
                      << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                      << "," << af.bad_channel_count
                      << "," << af.max_ptp_z << "," << af.max_rms_z
                      << "," << af.max_kurtosis_z << "\n";
            }
          }
        }

        const auto frames = eng.push_block(block);
        for (const auto& fr : frames) {
          if (b_idx < 0) {
            b_idx = find_band_index(fr.bands, metric.band);
            if (b_idx < 0) throw std::runtime_error("Metric band not found: " + metric.band);
          }

          // Optional full export (all bands for the selected pair).
          if (args.export_coherence) {
            out_coh << fr.t_end_sec;
            for (size_t bi = 0; bi < fr.bands.size(); ++bi) {
              out_coh << "," << fr.coherences[bi][0];
            }
            out_coh << "\n";
          }

          const OnlineArtifactFrame af = take_artifact_frame(art ? &art_queue : nullptr,
                                                            fr.t_end_sec,
                                                            0.5 / rec.fs_hz);
          const bool artifact_hit = (args.artifact_gate && af.baseline_ready && af.bad);

          const double val = fr.coherences[static_cast<size_t>(b_idx)][0];
          if (!std::isfinite(val)) {
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);
            continue;
          }

          if (!have_threshold) {
            if (fr.t_end_sec <= args.baseline_seconds) {
              baseline_values.push_back(val);
            } else {
              threshold = median(baseline_values);
              if (!std::isfinite(threshold)) threshold = val;
              have_threshold = true;
              std::cout << "Initial threshold set to: " << threshold
                        << " (baseline=" << args.baseline_seconds << "s, n=" << baseline_values.size() << ")\n";
            }
            const double thr_send = have_threshold ? threshold : 0.0;
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                          have_threshold ? 1 : 0);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);
            continue;
          }

          if (artifact_hit) {
            const double rr = reward_rate();
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b;
            out << "\n";
            continue;
          }

          const bool reward = (val > threshold);
          audio_reward_flags.push_back(reward ? 1 : 0);
          reward_hist.push_back(reward ? 1 : 0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();

          if (!args.no_adaptation && args.adapt_eta > 0.0) {
            threshold *= std::exp(args.adapt_eta * (rr - args.target_reward_rate));
          }

          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                        (reward ? 1 : 0), rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);

          out << fr.t_end_sec << "," << val << "," << threshold << "," << (reward ? 1 : 0) << "," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b;
          out << "\n";
        }
      }

      write_reward_tone_wav_if_requested(args, audio_reward_flags);
      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    if (metric.type == MetricSpec::Type::Pac) {
      const int ic = find_channel_index(rec.channel_names, metric.channel);
      if (ic < 0) throw std::runtime_error("Metric channel not found in recording: " + metric.channel);

      const BandDefinition phase_band = resolve_band_token(bands, metric.phase_band, "phase");
      const BandDefinition amp_band   = resolve_band_token(bands, metric.amp_band, "amplitude");

      OnlinePacOptions opt_pac;
      opt_pac.window_seconds = args.window_seconds;
      opt_pac.update_seconds = args.update_seconds;
      opt_pac.pac.method = metric.pac_method;
      opt_pac.pac.n_phase_bins = args.pac_bins;
      opt_pac.pac.edge_trim_fraction = args.pac_trim;
      opt_pac.pac.zero_phase = args.pac_zero_phase;

      OnlinePAC eng(rec.fs_hz, phase_band, amp_band, opt_pac);

      for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
        const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          block[c].assign(rec.data[c].begin() + static_cast<std::ptrdiff_t>(pos),
                          rec.data[c].begin() + static_cast<std::ptrdiff_t>(end));
        }

        pre.process_block(&block);

        if (art) {
          const auto aframes = art->push_block(block);
          for (const auto& af : aframes) {
            art_queue.push_back(af);
            if (args.export_artifacts) {
              out_art << af.t_end_sec << "," << (af.baseline_ready ? 1 : 0)
                      << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                      << "," << af.bad_channel_count
                      << "," << af.max_ptp_z << "," << af.max_rms_z
                      << "," << af.max_kurtosis_z << "\n";
            }
          }
        }

        const auto frames = eng.push_block(block[static_cast<size_t>(ic)]);
        for (const auto& fr : frames) {
          const OnlineArtifactFrame af = take_artifact_frame(art ? &art_queue : nullptr,
                                                            fr.t_end_sec,
                                                            0.5 / rec.fs_hz);
          const bool artifact_hit = (args.artifact_gate && af.baseline_ready && af.bad);

          const double val = fr.value;
          if (!std::isfinite(val)) {
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);
            continue;
          }

          if (!have_threshold) {
            if (fr.t_end_sec <= args.baseline_seconds) {
              baseline_values.push_back(val);
            } else {
              threshold = median(baseline_values);
              if (!std::isfinite(threshold)) threshold = val;
              have_threshold = true;
              std::cout << "Initial threshold set to: " << threshold
                        << " (baseline=" << args.baseline_seconds << "s, n=" << baseline_values.size() << ")\n";
            }
            const double thr_send = have_threshold ? threshold : 0.0;
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                          have_threshold ? 1 : 0);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);
            continue;
          }

          if (artifact_hit) {
            const double rr = reward_rate();
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_flags.push_back(0);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            out << "," << metric.phase_band << "," << metric.amp_band << "," << metric.channel;
            out << "," << (metric.pac_method == PacMethod::ModulationIndex ? "mi" : "mvl");
            out << "\n";
            continue;
          }

          const bool reward = (val > threshold);
          audio_reward_flags.push_back(reward ? 1 : 0);
          reward_hist.push_back(reward ? 1 : 0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();

          if (!args.no_adaptation && args.adapt_eta > 0.0) {
            threshold *= std::exp(args.adapt_eta * (rr - args.target_reward_rate));
          }

          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                        (reward ? 1 : 0), rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);

          out << fr.t_end_sec << "," << val << "," << threshold << "," << (reward ? 1 : 0) << "," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          out << "," << metric.phase_band << "," << metric.amp_band << "," << metric.channel;
          out << "," << (metric.pac_method == PacMethod::ModulationIndex ? "mi" : "mvl");
          out << "\n";
        }
      }

      write_reward_tone_wav_if_requested(args, audio_reward_flags);
      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    // Bandpower / ratio modes.
    OnlineBandpowerOptions opt;
    opt.window_seconds = args.window_seconds;
    opt.update_seconds = args.update_seconds;
    opt.welch.nperseg = args.nperseg;
    opt.welch.overlap_fraction = args.overlap;

    OnlineWelchBandpower eng(rec.channel_names, rec.fs_hz, bands, opt);

    // We'll resolve band/channel indices once the first frame is emitted.
    int ch_idx = -1;
    int b_idx = -1;
    int b_num = -1;
    int b_den = -1;

    if (args.export_bandpowers) {
      out_bp.open(args.outdir + "/bandpower_timeseries.csv");
      if (!out_bp) throw std::runtime_error("Failed to write bandpower_timeseries.csv");
      out_bp << "t_end_sec";
      for (const auto& b : bands) {
        for (const auto& ch : rec.channel_names) {
          out_bp << "," << b.name << "_" << ch;
        }
      }
      out_bp << "\n";
    }

    for (size_t pos = 0; pos < rec.n_samples(); pos += chunk_samples) {
      const size_t end = std::min(rec.n_samples(), pos + chunk_samples);
      const size_t len = end - pos;
      (void)len;
      for (size_t c = 0; c < rec.n_channels(); ++c) {
        block[c].assign(rec.data[c].begin() + static_cast<std::ptrdiff_t>(pos),
                        rec.data[c].begin() + static_cast<std::ptrdiff_t>(end));
      }

      pre.process_block(&block);

      if (art) {
        const auto aframes = art->push_block(block);
        for (const auto& af : aframes) {
          art_queue.push_back(af);
          if (args.export_artifacts) {
            out_art << af.t_end_sec << "," << (af.baseline_ready ? 1 : 0)
                    << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                    << "," << af.bad_channel_count
                    << "," << af.max_ptp_z << "," << af.max_rms_z
                    << "," << af.max_kurtosis_z << "\n";
          }
        }
      }

      const auto frames = eng.push_block(block);
      for (const auto& fr : frames) {
        if (ch_idx < 0) {
          ch_idx = find_channel_index(fr.channel_names, metric.channel);
          if (ch_idx < 0) throw std::runtime_error("Metric channel not found in recording: " + metric.channel);
          if (metric.type == MetricSpec::Type::Band) {
            b_idx = find_band_index(fr.bands, metric.band);
            if (b_idx < 0) throw std::runtime_error("Metric band not found: " + metric.band);
          } else {
            b_num = find_band_index(fr.bands, metric.band_num);
            b_den = find_band_index(fr.bands, metric.band_den);
            if (b_num < 0) throw std::runtime_error("Metric numerator band not found: " + metric.band_num);
            if (b_den < 0) throw std::runtime_error("Metric denominator band not found: " + metric.band_den);
          }
        }

        const OnlineArtifactFrame af = take_artifact_frame(art ? &art_queue : nullptr,
                                                          fr.t_end_sec,
                                                          0.5 / rec.fs_hz);
        const bool artifact_hit = (args.artifact_gate && af.baseline_ready && af.bad);

        const double val = compute_metric_band_or_ratio(fr, metric, ch_idx, b_idx, b_num, b_den);
        if (!std::isfinite(val)) {
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_flags.push_back(0);
          continue;
        }

        if (!have_threshold) {
          if (fr.t_end_sec <= args.baseline_seconds) {
            baseline_values.push_back(val);
          } else {
            threshold = median(baseline_values);
            if (!std::isfinite(threshold)) threshold = val;
            have_threshold = true;
            std::cout << "Initial threshold set to: " << threshold
                      << " (baseline=" << args.baseline_seconds << "s, n=" << baseline_values.size() << ")\n";
          }
          const double thr_send = have_threshold ? threshold : 0.0;
          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                        have_threshold ? 1 : 0);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_flags.push_back(0);
          continue;
        }

        if (artifact_hit) {
          const double rr = reward_rate();
          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                        0, rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_flags.push_back(0);

          out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          if (metric.type == MetricSpec::Type::Band) {
            out << "," << metric.band << "," << metric.channel;
          } else {
            out << "," << metric.band_num << "," << metric.band_den << "," << metric.channel;
          }
          out << "\n";
          continue;
        }

        const bool reward = (val > threshold);
        audio_reward_flags.push_back(reward ? 1 : 0);
        reward_hist.push_back(reward ? 1 : 0);
        while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
        const double rr = reward_rate();

        if (!args.no_adaptation && args.adapt_eta > 0.0) {
          threshold *= std::exp(args.adapt_eta * (rr - args.target_reward_rate));
        }

        osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                      (reward ? 1 : 0), rr, 1);
        if (art) osc_send_artifact(osc, osc_prefix, af);

        out << fr.t_end_sec << "," << val << "," << threshold << "," << (reward ? 1 : 0) << "," << rr;
        if (do_artifacts) {
          out << "," << (af.baseline_ready ? 1 : 0)
              << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
              << "," << af.bad_channel_count;
        }
        if (metric.type == MetricSpec::Type::Band) {
          out << "," << metric.band << "," << metric.channel;
        } else {
          out << "," << metric.band_num << "," << metric.band_den << "," << metric.channel;
        }
        out << "\n";

        if (args.export_bandpowers) {
          out_bp << fr.t_end_sec;
          for (size_t b = 0; b < fr.bands.size(); ++b) {
            for (size_t c = 0; c < fr.channel_names.size(); ++c) {
              out_bp << "," << fr.powers[b][c];
            }
          }
          out_bp << "\n";
        }
      }
    }

    write_reward_tone_wav_if_requested(args, audio_reward_flags);
    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
