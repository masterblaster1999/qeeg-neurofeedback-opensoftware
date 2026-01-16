#include "qeeg/bandpower.hpp"
#include "qeeg/montage.hpp"
#include "qeeg/preprocess.hpp"
#include "qeeg/online_bandpower.hpp"
#include "qeeg/online_coherence.hpp"
#include "qeeg/online_artifacts.hpp"
#include "qeeg/online_pac.hpp"
#include "qeeg/nf_metric.hpp"
#include "qeeg/nf_metric_eval.hpp"
#include "qeeg/nf_threshold.hpp"
#include "qeeg/nf_protocols.hpp"
#include "qeeg/hysteresis_gate.hpp"
#include "qeeg/adaptive_threshold.hpp"
#include "qeeg/debounce.hpp"
#include "qeeg/robust_stats.hpp"
#include "qeeg/running_stats.hpp"
#include "qeeg/smoother.hpp"
#include "qeeg/reward_shaper.hpp"
#include "qeeg/feedback_value.hpp"
#include "qeeg/csv_io.hpp"
#include "qeeg/bids.hpp"
#include "qeeg/channel_qc_io.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/wav_writer.hpp"
#include "qeeg/osc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

using namespace qeeg;

struct Args {
  std::string input_path;
  std::string outdir{"out_nf"};
  std::string band_spec; // empty => default
  std::string metric_spec{"alpha:Pz"};

  // Optional: use a built-in neurofeedback protocol preset (see --list-protocols).
  // When set, the preset provides defaults for --metric/--bands and a few NF loop params
  // unless you explicitly override them on the command line.
  std::string protocol;
  std::string protocol_ch;
  std::string protocol_a;
  std::string protocol_b;

  struct ExplicitFlags {
    bool bands{false};
    bool metric{false};
    bool reward_direction{false};
    bool target_rate{false};
    bool baseline{false};
    bool window{false};
    bool update{false};
    bool metric_smooth{false};
  } explicit_set;


  // Optional: channel quality control (qeeg_channel_qc_cli output).
  // When provided:
  //  - bad channels are ignored by the artifact gate (to avoid "always bad" sessions due to a known dead channel)
  //  - bandpower_timeseries.csv masks bad channels as NaN for easier downstream analysis
  //  - by default, using a bad channel for the NF metric is treated as an error (override with --allow-bad-metric-channels)
  std::string channel_qc;
  bool allow_bad_metric_channels{false};

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

  // Bandpower scaling options (bandpower/ratio/asymmetry metrics only)
  // - --relative: band_power / total_power within a frequency range
  // - --log10: apply log10(max(eps, value)) after any optional relative normalization
  bool log10_power{false};
  bool relative_power{false};
  double relative_fmin_hz{0.0};
  double relative_fmax_hz{0.0};

  // Neurofeedback threshold params
  // Initial threshold is estimated from the first --baseline seconds unless overridden by --threshold.
  double baseline_seconds{10.0};
  // Optional: quantile in [0,1] used to derive the initial threshold from baseline values.
  // If not set, defaults to an "auto" quantile that roughly matches --target-rate:
  //   - RewardDirection::Above: q = 1 - target_rate
  //   - RewardDirection::Below: q = target_rate
  double baseline_quantile{std::numeric_limits<double>::quiet_NaN()};
  double initial_threshold{std::numeric_limits<double>::quiet_NaN()}; // NaN => use baseline
  RewardDirection reward_direction{RewardDirection::Above};
  double target_reward_rate{0.6};
  double adapt_eta{0.10};
  std::string adapt_mode{"exp"};
  double adapt_interval_seconds{0.0};
  double adapt_window_seconds{30.0};
  int adapt_min_samples{20};
  double reward_rate_window_seconds{5.0};
  bool no_adaptation{false};

  // Optional reward debouncing / hysteresis (in NF update frames).
  // Example: --reward-on-frames 2 --reward-off-frames 2
  // will require two consecutive raw rewards to switch ON, and two consecutive
  // non-rewards to switch OFF.
  int reward_on_frames{1};
  int reward_off_frames{1};

  // Optional numeric threshold hysteresis (metric units).
  //
  // When >0, qeeg_nf_cli uses a Schmitt-trigger style comparator to
  // reduce ON/OFF chatter when the metric hovers near the threshold.
  // Example (reward=above): ON when metric > thr + H, OFF when metric < thr - H.
  double threshold_hysteresis{0.0};

  // Optional reward shaping (time-domain) on top of metric thresholding.
  //
  // - dwell_seconds: require the raw reward condition to remain true for this long
  //   before reward can turn on.
  // - refractory_seconds: after reward turns off, require this long before it can
  //   turn on again.
  double dwell_seconds{0.0};
  double refractory_seconds{0.0};

  // Optional continuous feedback value derived from the metric.
  //
  // - feedback_mode=binary (default): reward_value is a 0/1 gate (same as reward).
  // - feedback_mode=continuous: reward_value is a graded value in [0,1] based on
  //   how far the metric is beyond threshold (direction-aware). This value can be
  //   exported, sent via OSC, and used for amplitude-modulated audio.
  //
  // feedback_span controls the metric delta that maps to full-scale feedback.
  // If not provided, qeeg_nf_cli will attempt to use a robust baseline scale (MAD).
  std::string feedback_mode{"binary"};
  double feedback_span{std::numeric_limits<double>::quiet_NaN()};

  // Optional training block schedule (useful for offline "real-time" practice).
  // When enabled (train>0 and rest>0), reinforcement is paused during rest blocks.
  double train_block_seconds{0.0};
  double rest_block_seconds{0.0};
  bool start_with_rest{false};

  // Playback
  double chunk_seconds{0.10};

  // Optional: pace offline playback to approximate real-time.
  //
  // If <= 0, processing runs as fast as possible.
  // If > 0, the tool will sleep so updates are emitted at (sim_time / playback_speed).
  //   playback_speed = 1.0 => real-time
  //   playback_speed = 2.0 => 2x real-time (faster)
  //   playback_speed = 0.5 => half-speed (slower)
  double playback_speed{0.0};

  // Optional: smooth the metric prior to thresholding/feedback.
  // This uses an exponential moving average with a time-constant (seconds).
  // If <= 0, smoothing is disabled.
  double metric_smooth_seconds{0.0};

  // Debug exports
  bool export_bandpowers{false};   // bandpower mode only
  bool export_coherence{false};    // coherence mode only

  // Optional artifact gating (time-domain robust outlier detection)
  bool artifact_gate{false};
  double artifact_ptp_z{6.0};
  double artifact_rms_z{6.0};
  double artifact_kurtosis_z{6.0};
  int artifact_min_bad_channels{1};
  std::vector<std::string> artifact_ignore_channels; // optional
  bool export_artifacts{false};

  // Optional static HTML UI export (BioTrace+ inspired).
  // Writes a self-contained 'biotrace_ui.html' into --outdir.
  bool biotrace_ui{false};

  // Optional derived events export (reward/artifact/baseline segments).
  // Writes 'nf_derived_events.csv' and 'nf_derived_events.tsv' into --outdir.
  bool export_derived_events{false};

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



static void print_protocol_list() {
  const auto presets = qeeg::built_in_nf_protocols();
  std::cout << "Built-in NF protocol presets (use with --protocol NAME):\n";
  for (const auto& p : presets) {
    std::cout << "  " << p.name;
    if (!p.title.empty()) std::cout << " — " << p.title;
    std::cout << "\n";
    if (!p.description.empty()) {
      std::cout << "      " << p.description << "\n";
    }
  }
  if (presets.empty()) {
    std::cout << "  (none)\n";
  }
}

static void print_protocol_help(const std::string& name) {
  const auto p = qeeg::find_nf_protocol_preset(name);
  if (!p.has_value()) {
    throw std::runtime_error("Unknown protocol preset: " + name);
  }

  std::cout << p->name;
  if (!p->title.empty()) std::cout << " — " << p->title;
  std::cout << "\n";
  if (!p->description.empty()) std::cout << "  " << p->description << "\n";

  std::cout << "\nDefaults:\n";
  std::cout << "  metric_template: " << p->metric_template << "\n";
  try {
    const std::string metric_rendered = qeeg::nf_render_protocol_metric(*p);
    std::cout << "  metric:          " << metric_rendered << "\n";
  } catch (const std::exception& e) {
    std::cout << "  metric:          (error: " << e.what() << ")\n";
  }
  if (!p->band_spec.empty()) {
    std::cout << "  bands:           " << p->band_spec << "\n";
  } else {
    std::cout << "  bands:           (default_eeg_bands)\n";
  }
  if (!p->default_channel.empty()) std::cout << "  default_channel: " << p->default_channel << "\n";
  if (!p->default_channel_a.empty()) std::cout << "  default_channel_a: " << p->default_channel_a << "\n";
  if (!p->default_channel_b.empty()) std::cout << "  default_channel_b: " << p->default_channel_b << "\n";

  std::cout << "  reward_direction: " << qeeg::reward_direction_name(p->reward_direction) << "\n";
  std::cout << "  target_reward_rate: " << p->target_reward_rate << "\n";
  std::cout << "  baseline_seconds: " << p->baseline_seconds << "\n";
  std::cout << "  window_seconds: " << p->window_seconds << "\n";
  std::cout << "  update_seconds: " << p->update_seconds << "\n";
  std::cout << "  metric_smooth_seconds: " << p->metric_smooth_seconds << "\n";

  std::cout << "\nOverride examples:\n";
  std::cout << "  qeeg_nf_cli --protocol " << p->name << " --protocol-ch Cz ...\n";
  std::cout << "  qeeg_nf_cli --protocol " << p->name << " --metric alpha:Pz ...  (explicit flags override preset defaults)\n";
}

static void apply_protocol_preset(Args* a) {
  if (!a) return;
  const std::string proto_name = qeeg::trim(a->protocol);
  if (proto_name.empty()) return;

  const auto p = qeeg::find_nf_protocol_preset(proto_name);
  if (!p.has_value()) {
    throw std::runtime_error("Unknown protocol preset: " + proto_name + ". Use --list-protocols to see available presets.");
  }

  // Canonicalize the stored name.
  a->protocol = p->name;

  // Apply defaults only when the user did NOT explicitly set the corresponding flag.
  // This makes presets a convenient starting point without preventing expert overrides.
  if (!a->explicit_set.metric) {
    a->metric_spec = qeeg::nf_render_protocol_metric(*p, a->protocol_ch, a->protocol_a, a->protocol_b);
  }
  if (!a->explicit_set.bands && !p->band_spec.empty()) {
    a->band_spec = qeeg::nf_render_protocol_bands(*p, a->protocol_ch, a->protocol_a, a->protocol_b);
  }
  if (!a->explicit_set.reward_direction) {
    a->reward_direction = p->reward_direction;
  }
  if (!a->explicit_set.target_rate) {
    a->target_reward_rate = p->target_reward_rate;
  }
  if (!a->explicit_set.baseline) {
    a->baseline_seconds = p->baseline_seconds;
  }
  if (!a->explicit_set.window) {
    a->window_seconds = p->window_seconds;
  }
  if (!a->explicit_set.update) {
    a->update_seconds = p->update_seconds;
  }
  if (!a->explicit_set.metric_smooth) {
    a->metric_smooth_seconds = p->metric_smooth_seconds;
  }
}

static void print_help() {
  std::cout
    << "qeeg_nf_cli (first pass neurofeedback engine)\n\n"
    << "Usage:\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric alpha:Pz\n"
    << "  qeeg_nf_cli --input file.bdf --outdir out_nf --metric alpha/beta:Pz\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric coh:alpha:F3:F4\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric imcoh:alpha:F3:F4\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric pac:theta:gamma:Cz\n"
    << "  qeeg_nf_cli --input file.edf --outdir out_nf --metric mvl:theta:gamma:Cz\n"
    << "  qeeg_nf_cli --demo --fs 250 --seconds 60 --outdir out_demo_nf\n\n"
    << "Options:\n"
    << "  --input PATH              Input EDF/BDF/CSV (CSV requires --fs)\n"
    << "  --fs HZ                   Sampling rate for CSV (optional if first column is time); also used for --demo\n"
    << "  --outdir DIR              Output directory (default: out_nf)\n"
    << "\nProtocol presets (optional):\n"
    << "  --list-protocols          List built-in NF protocol presets and exit\n"
    << "  --protocol NAME           Apply a built-in protocol preset (defaults for --metric/--bands/etc unless overridden)\n"
    << "  --protocol-help NAME      Show details for one preset and exit\n"
    << "  --protocol-ch CH          Override {ch} for single-channel presets\n"
    << "  --protocol-a CH_A         Override {a} for pair presets (coherence/asymmetry)\n"
    << "  --protocol-b CH_B         Override {b} for pair presets (coherence/asymmetry)\n\n"
    << "  --bands SPEC              Band spec, e.g. 'delta:0.5-4,theta:4-7,alpha:8-12'\n"
    << "                             IAF-relative convenience forms:\n"
    << "                               --bands iaf=10.2\n"
    << "                               --bands iaf:out_iaf   (reads out_iaf/iaf_band_spec.txt or out_iaf/iaf_summary.txt)\n"
    << "  --metric SPEC             Metric: 'alpha:Pz' (bandpower), 'alpha/beta:Pz' (ratio),\n"
    << "                           'asym:alpha:F4:F3' (asymmetry),\n"
    << "                           'coh:alpha:F3:F4' or 'msc:alpha:F3:F4' (magnitude-squared coherence),\n"
    << "                           'imcoh:alpha:F3:F4' (imaginary coherency),\n"
    << "                           'pac:PHASE:AMP:CH' (Tort MI), or 'mvl:PHASE:AMP:CH'\n"
    << "  --window S                Sliding window seconds (default: 2.0)\n"
    << "  --update S                Update interval seconds (default: 0.25)\n"
    << "  --metric-smooth S         Optional: EMA smooth the metric before thresholding (time constant seconds; default: 0/off)\n"
    << "  --nperseg N               Welch segment length (default: 512)\n"
    << "  --overlap FRAC            Welch overlap fraction in [0,1) (default: 0.5)\n"
    << "  --log10                   Use log10(power) instead of raw bandpower (bandpower/ratio/asymmetry metrics only)\n"
    << "  --relative                Use relative power: band_power / total_power (bandpower/ratio/asymmetry metrics only)\n"
    << "  --relative-range LO HI    Total-power integration range used for --relative.\n"
    << "                           Default: [min_band_fmin, max_band_fmax] from --bands.\n"
    << "  --baseline S              Baseline duration seconds for initial threshold (default: 10)\n"
    << "  --baseline-quantile Q     Baseline quantile in [0,1] for initial threshold.\n"
    << "                           Default: auto (matches --target-rate): above=>1-R, below=>R.\n"
    << "                           Set Q=0.5 to force median behavior.\n"
    << "  --threshold X             Set an explicit initial threshold (skips baseline threshold estimation)\n"
    << "  --reward-direction DIR    Reward direction: above|below (default: above)\n"
    << "  --target-rate R           Target reward rate in (0,1) (default: 0.6)\n"
    << "  --eta E                   Adaptation speed/gain (default: 0.10)\n"
    << "  --adapt-mode MODE          Adaptive threshold mode: exp|quantile (default: exp)\n"
    << "  --adapt-interval S         Only update threshold every S seconds (0 => every frame; default: 0)\n"
    << "  --adapt-window S           Quantile mode: rolling window seconds for threshold estimation (default: 30)\n"
    << "  --adapt-min-samples N      Quantile mode: minimum metric samples in the window before adapting (default: 20)\n"
    << "  --rate-window S           Reward-rate window seconds (default: 5)\n"
    << "  --reward-on-frames N      Debounce: require N consecutive reward frames to turn ON (default: 1)\n"
    << "  --reward-off-frames N     Debounce: require N consecutive non-reward frames to turn OFF (default: 1)\n"
    << "  --threshold-hysteresis H  Optional: numeric hysteresis band (metric units) around threshold to reduce chatter (default: 0/off)\n"
    << "  --dwell S                 Reward shaping: require raw reward for S seconds before turning ON (default: 0/off)\n"
    << "  --refractory S            Reward shaping: after reward turns OFF, enforce S seconds before it can turn ON again (default: 0/off)\n"
    << "  --feedback-mode MODE      Feedback value mode: binary|continuous (default: binary)\n"
    << "  --feedback-span X         Continuous mode: metric delta that maps to full-scale feedback (value==1).\n"
    << "                           If omitted, uses a robust baseline scale estimate (MAD) when available.\n"
    << "  --train-block S           Offline training: training block length in seconds (enables train/rest schedule when used with --rest-block)\n"
    << "  --rest-block S            Offline training: rest block length in seconds (reinforcement paused during rest blocks)\n"
    << "  --start-rest              Offline training: start the schedule with a rest block (default: start with train)\n"
    << "  --no-adaptation            Disable adaptive thresholding (fixed threshold from baseline)\n"
    << "  --average-reference        Apply common average reference across channels\n"
    << "  --notch HZ                 Apply a notch filter at HZ (e.g., 50 or 60)\n"
    << "  --notch-q Q                Notch Q factor (default: 30)\n"
    << "  --bandpass LO HI           Apply a simple bandpass (highpass LO then lowpass HI)\n"
    << "  --chunk S                 File playback chunk seconds (default: 0.10)\n"
    << "  --realtime                 Pace offline playback at 1x real-time (useful for OSC / interactive training)\n"
    << "  --speed X                  Pace offline playback at X times real-time (X>0; e.g. 2.0 is 2x speed)\n"
    << "  --export-bandpowers        Write bandpower_timeseries.csv (bandpower/ratio modes)\n"
    << "  --export-coherence         Write coherence_timeseries.csv or imcoh_timeseries.csv (coherence mode)\n"
    << "  --artifact-gate            Suppress reward/adaptation during detected artifacts\n"
    << "  --artifact-ptp-z Z         Artifact threshold: peak-to-peak robust z (<=0 disables; default: 6)\n"
    << "  --artifact-rms-z Z         Artifact threshold: RMS robust z (<=0 disables; default: 6)\n"
    << "  --artifact-kurtosis-z Z    Artifact threshold: excess kurtosis robust z (<=0 disables; default: 6)\n"
    << "  --artifact-min-bad-ch N    Artifact frame is bad if >=N channels flagged (default: 1)\n"
    << "  --artifact-ignore LIST     Comma-separated channel names to ignore for artifact gating\n"
    << "  --channel-qc PATH          Optional: qeeg_channel_qc_cli output (channel_qc.csv, bad_channels.txt, or qc outdir)\n"
    << "                           Used to ignore bad channels in artifact gating and mask bad channels as NaN in bandpower_timeseries.csv\n"
    << "                           Also writes bad_channels_used.txt for provenance\n"
    << "  --allow-bad-metric-channels  Run even if the NF metric uses channels marked bad by --channel-qc (default: error)\n"
    << "  --export-artifacts         Write artifact_gate_timeseries.csv aligned to NF updates\n"
    << "  --biotrace-ui              Write a self-contained BioTrace+ style HTML UI (biotrace_ui.html).\n"
    << "                           Also writes nf_derived_events.csv, nf_derived_events.tsv and nf_derived_events.json for interoperability.\n"
    << "  --export-derived-events    Write nf_derived_events.csv/.tsv/.json (baseline/reward/artifact segments) even if --biotrace-ui is off.\n"
    << "  --audio-wav PATH           Optional: write a reward-tone WAV (mono PCM16)\n"
    << "  --audio-rate HZ            Audio sample rate (default: 44100)\n"
    << "  --audio-tone HZ            Reward tone frequency (default: 440)\n"
    << "  --audio-gain G             Reward tone gain in [0,1] (default: 0.2)\n"
    << "  --audio-attack S           Tone attack seconds (default: 0.005)\n"
    << "  --audio-release S          Tone release seconds (default: 0.010)\n"
    << "  --osc-host HOST            Optional: OSC/UDP destination host (default: 127.0.0.1)\n"
    << "  --osc-port PORT            Optional: OSC/UDP destination port (0 disables; e.g. 9000)\n"
    << "  --osc-prefix PATH          OSC address prefix (default: /qeeg)\n"
    << "  --osc-mode MODE            OSC mode: state|split|bundle (default: state)\n"
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
    } else if (arg == "--list-protocols") {
      print_protocol_list();
      std::exit(0);
    } else if (arg == "--protocol-help" && i + 1 < argc) {
      print_protocol_help(argv[++i]);
      std::exit(0);
    } else if (arg == "--protocol" && i + 1 < argc) {
      a.protocol = argv[++i];
    } else if (arg == "--protocol-ch" && i + 1 < argc) {
      a.protocol_ch = argv[++i];
    } else if (arg == "--protocol-a" && i + 1 < argc) {
      a.protocol_a = argv[++i];
    } else if (arg == "--protocol-b" && i + 1 < argc) {
      a.protocol_b = argv[++i];
    } else if (arg == "--bands" && i + 1 < argc) {
      a.band_spec = argv[++i];
      a.explicit_set.bands = true;
    } else if (arg == "--metric" && i + 1 < argc) {
      a.metric_spec = argv[++i];
      a.explicit_set.metric = true;
    } else if (arg == "--channel-qc" && i + 1 < argc) {
      a.channel_qc = argv[++i];
    } else if (arg == "--allow-bad-metric-channels") {
      a.allow_bad_metric_channels = true;
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      a.window_seconds = to_double(argv[++i]);
      a.explicit_set.window = true;
    } else if (arg == "--update" && i + 1 < argc) {
      a.update_seconds = to_double(argv[++i]);
      a.explicit_set.update = true;
    } else if (arg == "--metric-smooth" && i + 1 < argc) {
      a.metric_smooth_seconds = to_double(argv[++i]);
      a.explicit_set.metric_smooth = true;
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if (arg == "--log10") {
      a.log10_power = true;
    } else if (arg == "--relative") {
      a.relative_power = true;
    } else if (arg == "--relative-range" && i + 2 < argc) {
      a.relative_power = true;
      a.relative_fmin_hz = to_double(argv[++i]);
      a.relative_fmax_hz = to_double(argv[++i]);
    } else if (arg == "--baseline" && i + 1 < argc) {
      a.baseline_seconds = to_double(argv[++i]);
      a.explicit_set.baseline = true;
    } else if (arg == "--baseline-quantile" && i + 1 < argc) {
      a.baseline_quantile = to_double(argv[++i]);
    } else if (arg == "--threshold" && i + 1 < argc) {
      a.initial_threshold = to_double(argv[++i]);
    } else if (arg == "--reward-direction" && i + 1 < argc) {
      a.reward_direction = parse_reward_direction(argv[++i]);
      a.explicit_set.reward_direction = true;
    } else if (arg == "--reward-below") {
      a.reward_direction = RewardDirection::Below;
      a.explicit_set.reward_direction = true;
    } else if (arg == "--reward-above") {
      a.reward_direction = RewardDirection::Above;
      a.explicit_set.reward_direction = true;
    } else if (arg == "--target-rate" && i + 1 < argc) {
      a.target_reward_rate = to_double(argv[++i]);
      a.explicit_set.target_rate = true;
    } else if (arg == "--eta" && i + 1 < argc) {
      a.adapt_eta = to_double(argv[++i]);
    } else if (arg == "--adapt-mode" && i + 1 < argc) {
      a.adapt_mode = argv[++i];
    } else if (arg == "--adapt-interval" && i + 1 < argc) {
      a.adapt_interval_seconds = to_double(argv[++i]);
    } else if (arg == "--adapt-window" && i + 1 < argc) {
      a.adapt_window_seconds = to_double(argv[++i]);
    } else if (arg == "--adapt-min-samples" && i + 1 < argc) {
      a.adapt_min_samples = to_int(argv[++i]);
    } else if (arg == "--rate-window" && i + 1 < argc) {
      a.reward_rate_window_seconds = to_double(argv[++i]);
    } else if (arg == "--reward-on-frames" && i + 1 < argc) {
      a.reward_on_frames = to_int(argv[++i]);
    } else if (arg == "--reward-off-frames" && i + 1 < argc) {
      a.reward_off_frames = to_int(argv[++i]);
    } else if ((arg == "--threshold-hysteresis" || arg == "--hysteresis") && i + 1 < argc) {
      a.threshold_hysteresis = to_double(argv[++i]);
    } else if (arg == "--dwell" && i + 1 < argc) {
      a.dwell_seconds = to_double(argv[++i]);
    } else if (arg == "--refractory" && i + 1 < argc) {
      a.refractory_seconds = to_double(argv[++i]);
    } else if (arg == "--feedback-mode" && i + 1 < argc) {
      a.feedback_mode = argv[++i];
    } else if (arg == "--feedback-span" && i + 1 < argc) {
      a.feedback_span = to_double(argv[++i]);
    } else if (arg == "--train-block" && i + 1 < argc) {
      a.train_block_seconds = to_double(argv[++i]);
    } else if (arg == "--rest-block" && i + 1 < argc) {
      a.rest_block_seconds = to_double(argv[++i]);
    } else if (arg == "--start-rest") {
      a.start_with_rest = true;
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
    } else if (arg == "--realtime") {
      a.playback_speed = 1.0;
    } else if (arg == "--speed" && i + 1 < argc) {
      a.playback_speed = to_double(argv[++i]);
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
    } else if (arg == "--artifact-ignore" && i + 1 < argc) {
      const auto parts = split(argv[++i], ',');
      for (const auto& p : parts) {
        const std::string t = trim(p);
        if (!t.empty()) a.artifact_ignore_channels.push_back(t);
      }
    } else if (arg == "--export-artifacts") {
      a.export_artifacts = true;
    } else if (arg == "--biotrace-ui") {
      a.biotrace_ui = true;
    } else if (arg == "--export-derived-events") {
      a.export_derived_events = true;
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


struct NfUiFrame {
  double t_end_sec{0.0};
  double metric{std::numeric_limits<double>::quiet_NaN()};
  double threshold{std::numeric_limits<double>::quiet_NaN()};
  int reward{0};
  // Optional continuous feedback value in [0,1] (ungated). In binary mode, this is 0/1.
  double feedback_raw{0.0};
  // Optional continuous reinforcement value in [0,1] (reward-gated). In binary mode, this is 0/1.
  double reward_value{0.0};
  double reward_rate{0.0};

  // Optional artifact info (only populated when the artifact engine is enabled).
  int artifact_ready{0};
  int artifact{0};
  int bad_channels{0};
};

// Build simple duration annotations from a binary state sampled at regular-ish
// NF update frames. This lets nf_cli export reward/artifact segments that can
// be consumed by other tools (trace_plot_cli, export_bids_cli, etc.).
struct BoolSegmentBuilder {
  std::string label;
  bool open{false};
  double start_sec{0.0};
  double last_end_sec{0.0};

  explicit BoolSegmentBuilder(std::string lbl) : label(std::move(lbl)) {}

  void update(bool active, double frame_start_sec, double frame_end_sec, std::vector<AnnotationEvent>* out) {
    if (!out) return;
    if (active) {
      if (!open) {
        open = true;
        start_sec = frame_start_sec;
      }
      last_end_sec = frame_end_sec;
    } else {
      if (open) {
        const double end_sec = last_end_sec;
        const double dur = end_sec - start_sec;
        if (dur > 0.0 && std::isfinite(dur) && std::isfinite(start_sec) && std::isfinite(end_sec)) {
          out->push_back({start_sec, dur, label});
        }
        open = false;
      }
    }
  }

  void finish(double end_sec, std::vector<AnnotationEvent>* out) {
    if (!out) return;
    if (!open) return;
    const double e = std::isfinite(end_sec) ? end_sec : last_end_sec;
    const double dur = e - start_sec;
    if (dur > 0.0 && std::isfinite(dur) && std::isfinite(start_sec)) {
      out->push_back({start_sec, dur, label});
    }
    open = false;
  }
};

// Optional pacing helper for offline playback.
//
// When enabled (speed>0), the caller can call wait_until(sim_time_sec) before
// emitting each update to approximate real-time behavior.
struct RealtimePacer {
  bool enabled{false};
  double speed{1.0}; // sim seconds per wall seconds
  bool started{false};
  std::chrono::steady_clock::time_point wall_start;
  double max_lag_sec{0.0};
  double total_sleep_sec{0.0};

  explicit RealtimePacer(double playback_speed) {
    if (std::isfinite(playback_speed) && playback_speed > 0.0) {
      enabled = true;
      speed = playback_speed;
    }
  }

  void wait_until(double sim_time_sec) {
    if (!enabled) return;
    if (!std::isfinite(sim_time_sec)) return;
    using clock = std::chrono::steady_clock;
    if (!started) {
      started = true;
      wall_start = clock::now();
    }

    const double scaled = sim_time_sec / speed;
    const auto target = wall_start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(scaled));
    const auto now = clock::now();
    if (target > now) {
      const auto d = target - now;
      total_sleep_sec += std::chrono::duration<double>(d).count();
      std::this_thread::sleep_for(d);
    } else {
      const double lag = std::chrono::duration<double>(now - target).count();
      if (lag > max_lag_sec) max_lag_sec = lag;
    }
  }
};

// Lightweight run summary accumulator for nf_cli.
struct NfSummaryStats {
  // Only counts frames where the metric value is finite.
  size_t baseline_frames{0};
  size_t training_frames{0};
  size_t rest_frames{0};
  size_t artifact_frames{0};
  size_t reward_frames{0};

  // Continuous feedback statistics (reward_value in [0,1]).
  //
  // In binary mode this is equivalent to reward.
  double reward_value_sum{0.0};
  double reward_value_max{0.0};

  // For continuous mode: the span (metric delta) used to map metric->reward_value.
  double feedback_span_used{std::numeric_limits<double>::quiet_NaN()};
  bool feedback_span_used_set{false};

  RunningStats metric_stats; // training (non-artifact) frames
  double metric_min{std::numeric_limits<double>::infinity()};
  double metric_max{-std::numeric_limits<double>::infinity()};

  double threshold_init{std::numeric_limits<double>::quiet_NaN()};
  bool threshold_init_set{false};

  void add_training_metric(double v) {
    metric_stats.add(v);
    if (std::isfinite(v)) {
      if (v < metric_min) metric_min = v;
      if (v > metric_max) metric_max = v;
    }
  }

  void add_reward_value(double v) {
    if (!std::isfinite(v)) return;
    reward_value_sum += v;
    if (v > reward_value_max) reward_value_max = v;
  }
};

static void write_biotrace_ui_html_if_requested(const Args& args,
                                                const EEGRecording& rec,
                                                const NfMetricSpec& metric,
                                                const std::vector<NfUiFrame>& frames,
                                                bool include_artifacts,
                                                const std::vector<AnnotationEvent>* extra_events = nullptr) {
  if (!args.biotrace_ui) return;
  (void)metric;
  if (frames.empty()) {
    std::cerr << "BioTrace UI: no frames captured (nothing to render)\n";
    return;
  }

  const std::string outpath = args.outdir + "/biotrace_ui.html";
  std::ofstream out(outpath);
  if (!out) throw std::runtime_error("Failed to write " + outpath);

  out << "<!doctype html>\n"
      << "<html lang=\"en\">\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\"/>\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
      << "  <title>BioTrace+ Style Neurofeedback UI</title>\n"
      << "  <style>\n"
      << "    :root { --bg:#0b1020; --panel:#111a33; --panel2:#0f172a; --text:#e5e7eb; --muted:#94a3b8; --accent:#38bdf8; --reward:#34d399; --warn:#f97316; --bad:#ef4444; }\n"
      << "    html, body { height:100%; margin:0; background:var(--bg); color:var(--text); font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial; }\n"
      << "    .topbar { height:52px; display:flex; align-items:center; gap:12px; padding:0 14px; background:linear-gradient(90deg,var(--panel),var(--panel2)); border-bottom:1px solid rgba(255,255,255,0.08); }\n"
      << "    .brand { font-weight:700; letter-spacing:0.2px; }\n"
      << "    .pill { padding:4px 10px; border:1px solid rgba(255,255,255,0.12); border-radius:999px; color:var(--muted); font-size:12px; }\n"
      << "    .layout { display:grid; grid-template-columns: 320px 1fr; height: calc(100% - 52px - 68px); }\n"
      << "    .side { padding:14px; background:var(--panel2); border-right:1px solid rgba(255,255,255,0.08); overflow:auto; }\n"
      << "    .main { padding:14px; }\n"
      << "    .card { background:rgba(17,26,51,0.6); border:1px solid rgba(255,255,255,0.08); border-radius:12px; padding:12px; margin-bottom:12px; }\n"
      << "    .row { display:flex; justify-content:space-between; gap:10px; }\n"
      << "    .k { color:var(--muted); font-size:12px; }\n"
      << "    .v { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; font-size:12px; }\n"
      << "    .big { font-size:22px; font-weight:800; }\n"
      << "    canvas { width:100%; height:420px; background:rgba(0,0,0,0.18); border:1px solid rgba(255,255,255,0.08); border-radius:12px; }\n"
      << "    .bottombar { height:68px; display:flex; align-items:center; gap:12px; padding:0 14px; background:linear-gradient(90deg,var(--panel2),var(--panel)); border-top:1px solid rgba(255,255,255,0.08); }\n"
      << "    button { background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.12); color:var(--text); border-radius:10px; padding:10px 12px; cursor:pointer; }\n"
      << "    button:hover { border-color:rgba(255,255,255,0.22); }\n"
      << "    input[type=range] { width:320px; }\n"
      << "    select { background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.12); color:var(--text); border-radius:10px; padding:10px 12px; }\n"
      << "    .hint { color:var(--muted); font-size:12px; }\n"
      << "    .bar { height:10px; background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.12); border-radius:999px; overflow:hidden; }\n"
      << "    .bar > div { height:100%; width:0%; background:linear-gradient(90deg, var(--reward), rgba(52,211,153,0.4)); }\n"
      << "    .evlist { max-height:240px; overflow:auto; }\n"
      << "    .evitem { padding:8px 10px; border:1px solid rgba(255,255,255,0.10); border-radius:10px; margin-top:8px; cursor:pointer; background:rgba(255,255,255,0.03); }\n"
      << "    .evitem:hover { border-color:rgba(255,255,255,0.22); }\n"
      << "    .evitem .t { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; color:var(--muted); font-size:12px; }\n"
      << "    .evitem .txt { font-size:13px; margin-top:2px; }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div class=\"topbar\">\n"
      << "    <div class=\"brand\">QEEG Neurofeedback — BioTrace+ Style UI</div>\n"
      << "    <div class=\"pill\" id=\"pillMetric\"></div>\n"
      << "    <div class=\"pill\" id=\"pillFs\"></div>\n"
      << "    <div class=\"pill\" id=\"pillUpdate\"></div>\n"
      << "  </div>\n"
      << "  <div class=\"layout\">\n"
      << "    <div class=\"side\">\n"
      << "      <div class=\"card\">\n"
      << "        <div class=\"k\">Current</div>\n"
      << "        <div class=\"big\" id=\"curMetric\">—</div>\n"
      << "        <div class=\"row\"><div class=\"k\">Threshold</div><div class=\"v\" id=\"curThreshold\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Reward</div><div class=\"v\" id=\"curReward\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Feedback</div><div class=\"v\" id=\"curFb\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Reinforcement</div><div class=\"v\" id=\"curRV\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Reward rate</div><div class=\"v\" id=\"curRR\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Artifacts</div><div class=\"v\" id=\"curArt\">—</div></div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div class=\"k\">Session</div>\n"
      << "        <div class=\"row\"><div class=\"k\">t</div><div class=\"v\" id=\"curT\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Frames</div><div class=\"v\" id=\"curIdx\">—</div></div>\n"
      << "        <div class=\"row\"><div class=\"k\">Reward %</div><div class=\"v\" id=\"curPct\">—</div></div>\n"
      << "        <div class=\"bar\" style=\"margin-top:8px\"><div id=\"barPct\"></div></div>\n"
      << "        <div class=\"row\" style=\"margin-top:8px\"><div class=\"k\">Segment</div><div class=\"v\" id=\"curSegment\">—</div></div>\n"
      << "        <div class=\"row\" style=\"margin-top:8px\"><div class=\"k\">Event</div><div class=\"v\" id=\"curEvent\">—</div></div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div class=\"k\">Segments</div>\n"
      << "        <div class=\"hint\">Click a segment to jump the scrubber.</div>\n"
      << "        <div class=\"evlist\" id=\"segmentList\"></div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div class=\"k\">Events</div>\n"
      << "        <div class=\"hint\">Click an event to jump the scrubber.</div>\n"
      << "        <div class=\"evlist\" id=\"eventList\"></div>\n"
      << "      </div>\n"
      << "      <div class=\"card\">\n"
      << "        <div class=\"k\">Notes</div>\n"
      << "        <div class=\"hint\">This file is generated by qeeg_nf_cli --biotrace-ui. It embeds the NF timeline so it can be opened directly in a browser (no server needed).</div>\n"
      << "      </div>\n"
      << "    </div>\n"
      << "    <div class=\"main\">\n"
      << "      <canvas id=\"plot\"></canvas>\n"
      << "      <div class=\"hint\" style=\"margin-top:10px\">Metric (line), Threshold (line). Reward frames are highlighted; artifact frames (if present) are shaded. Duration events are shown as a segment band near the bottom axis.</div>\n"
      << "    </div>\n"
      << "  </div>\n"
      << "  <div class=\"bottombar\">\n"
      << "    <button id=\"btnStart\" title=\"Go to start\">⏮</button>\n"
      << "    <button id=\"btnPlay\">▶︎ Play</button>\n"
      << "    <button id=\"btnStop\">■ Stop</button>\n"
      << "    <button id=\"btnEnd\" title=\"Go to end\">⏭</button>\n"
      << "    <span class=\"hint\">Scrub:</span>\n"
      << "    <input id=\"slider\" type=\"range\" min=\"0\" max=\"0\" value=\"0\" step=\"1\"/>\n"
      << "    <span class=\"hint\">Speed:</span>\n"
      << "    <select id=\"speed\">\n"
      << "      <option value=\"0.25\">0.25×</option>\n"
      << "      <option value=\"0.5\">0.5×</option>\n"
      << "      <option value=\"1\" selected>1×</option>\n"
      << "      <option value=\"2\">2×</option>\n"
      << "      <option value=\"4\">4×</option>\n"
      << "    </select>\n"
      << "    <span class=\"hint\">View:</span>\n"
      << "    <select id=\"viewMode\">\n"
      << "      <option value=\"overview\" selected>Overview</option>\n"
      << "      <option value=\"realtime\">Real-time</option>\n"
      << "    </select>\n"
      << "    <span class=\"hint\">Time axis:</span>\n"
      << "    <select id=\"winSec\">\n"
      << "      <option value=\"5\">5 s</option>\n"
      << "      <option value=\"10\">10 s</option>\n"
      << "      <option value=\"20\">20 s</option>\n"
      << "      <option value=\"30\" selected>30 s</option>\n"
      << "      <option value=\"60\">60 s</option>\n"
      << "      <option value=\"120\">120 s</option>\n"
      << "    </select>\n"
      << "    <span class=\"hint\">Y:</span>\n"
      << "    <select id=\"yMode\">\n"
      << "      <option value=\"global\" selected>Global</option>\n"
      << "      <option value=\"window\">Window</option>\n"
      << "    </select>\n"
      << "  </div>\n"
      << "  <script id=\"nfData\" type=\"application/json\">\n";

  out << std::setprecision(10);
  out << "{\n";
  out << "  \"meta\": {\n";
  out << "    \"protocol\": ";
  if (!args.protocol.empty()) out << "\"" << json_escape(args.protocol) << "\"";
  else out << "null";
  out << ",\n";
  out << "    \"metric_spec\": \"" << json_escape(args.metric_spec) << "\",\n";
  out << "    \"band_spec\": \"" << json_escape(args.band_spec) << "\",\n";
  out << "    \"reward_direction\": \"" << (args.reward_direction == RewardDirection::Above ? "above" : "below") << "\",\n";
  out << "    \"target_reward_rate\": " << args.target_reward_rate << ",\n";
  out << "    \"baseline_seconds\": " << args.baseline_seconds << ",\n";
  out << "    \"update_seconds\": " << args.update_seconds << ",\n";
  out << "    \"recording_fs_hz\": " << rec.fs_hz << ",\n";
  out << "    \"artifact_engine\": " << (include_artifacts ? 1 : 0) << "\n";
  out << "  },\n";
  out << "  \"frames\": [\n";

  auto write_num_or_null = [&](double v) {
    if (std::isfinite(v)) {
      out << v;
    } else {
      out << "null";
    }
  };

  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& fr = frames[i];
    out << "    {\"t\":";
    write_num_or_null(fr.t_end_sec);
    out << ",\"metric\":";
    write_num_or_null(fr.metric);
    out << ",\"threshold\":";
    write_num_or_null(fr.threshold);
    out << ",\"reward\":" << fr.reward;
    out << ",\"feedback_raw\":";
    write_num_or_null(fr.feedback_raw);
    out << ",\"reward_value\":";
    write_num_or_null(fr.reward_value);
    out << ",\"reward_rate\":";
    write_num_or_null(fr.reward_rate);
    out << ",\"artifact_ready\":" << (include_artifacts ? fr.artifact_ready : 0);
    out << ",\"artifact\":" << (include_artifacts ? fr.artifact : 0);
    out << ",\"bad_channels\":" << (include_artifacts ? fr.bad_channels : 0);
    out << "}";
    if (i + 1 < frames.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";

  // Optional annotation events from the source recording (if any). These are
  // rendered as vertical markers and listed in the sidebar.
  out << "  \"events\": [\n";
  const size_t max_events = 2000;
  size_t n_written = 0;
  bool first = true;
  std::vector<AnnotationEvent> merged_events = rec.events;
  if (extra_events && !extra_events->empty()) {
    merged_events.insert(merged_events.end(), extra_events->begin(), extra_events->end());
  }
  std::sort(merged_events.begin(), merged_events.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
    return a.duration_sec < b.duration_sec;
  });

  for (const auto& ev : merged_events) {
    if (n_written >= max_events) break;
    if (!std::isfinite(ev.onset_sec) || !std::isfinite(ev.duration_sec)) continue;
    if (!first) out << ",\n";
    first = false;
    out << "    {\"onset\":" << ev.onset_sec << ",\"duration\":" << ev.duration_sec
        << ",\"text\":\"" << json_escape(ev.text) << "\"}";
    ++n_written;
  }
  if (!first) out << "\n";
  out << "  ]\n";
  out << "}\n";
  out << "  </script>\n";

  // JS: render + basic playback.
  out << R"JS(
  <script>
  const data = JSON.parse(document.getElementById('nfData').textContent);
  const frames = data.frames || [];
  const events = data.events || [];

  const pillMetric = document.getElementById('pillMetric');
  const pillFs = document.getElementById('pillFs');
  const pillUpdate = document.getElementById('pillUpdate');

  const proto = (data.meta && data.meta.protocol) ? `Protocol: ${data.meta.protocol} | ` : "";
  pillMetric.textContent = `${proto}Metric: ${data.meta.metric_spec}`;
  pillFs.textContent = `Fs: ${Number(data.meta.recording_fs_hz).toFixed(3)} Hz`;
  pillUpdate.textContent = `Update: ${Number(data.meta.update_seconds).toFixed(3)} s`;

  const plot = document.getElementById('plot');
  const ctx = plot.getContext('2d');

  const curMetric = document.getElementById('curMetric');
  const curThreshold = document.getElementById('curThreshold');
  const curReward = document.getElementById('curReward');
  const curFb = document.getElementById('curFb');
  const curRV = document.getElementById('curRV');
  const curRR = document.getElementById('curRR');
  const curArt = document.getElementById('curArt');
  const curT = document.getElementById('curT');
  const curIdx = document.getElementById('curIdx');
  const curPct = document.getElementById('curPct');
  const curSegment = document.getElementById('curSegment');
  const curEvent = document.getElementById('curEvent');
  const barPct = document.getElementById('barPct');
  const eventList = document.getElementById('eventList');
  const segmentList = document.getElementById('segmentList');

  const slider = document.getElementById('slider');
  const btnStart = document.getElementById('btnStart');
  const btnPlay = document.getElementById('btnPlay');
  const btnStop = document.getElementById('btnStop');
  const btnEnd = document.getElementById('btnEnd');
  const speedSel = document.getElementById('speed');
  const viewModeSel = document.getElementById('viewMode');
  const winSel = document.getElementById('winSec');
  const yModeSel = document.getElementById('yMode');

  let idx = 0;
  let playing = false;
  let timer = null;

  // Events/segments: sorted by onset (seconds).
  // BioTrace+ can export both point markers and duration-based segments.
  const eventsSorted = (events || [])
    .map(e => ({ onset: Number(e.onset), duration: Number(e.duration), text: String(e.text ?? '') }))
    .filter(e => Number.isFinite(e.onset))
    .sort((a, b) => a.onset - b.onset);

  // Segment rule:
  //   - duration <= 0 => point marker
  //   - duration  > 0 => time segment
  //
  // This matches both qeeg events CSV and BIDS events.tsv conventions and
  // ensures short NF segments (e.g. 1-frame reward bursts) still render as segments.
  function isSegment(e) {
    return Number.isFinite(e.duration) && e.duration > 0;
  }
  function isArtifactLabel(txt) {
    const t = String(txt || '').toLowerCase();
    return t.includes('artifact') || t.includes('artefact');
  }
  function hash32(str) {
    // Simple deterministic string hash (FNV-1a-ish)
    let h = 2166136261 >>> 0;
    const s = String(str || '');
    for (let i = 0; i < s.length; ++i) {
      h ^= s.charCodeAt(i);
      h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  }
  function segFill(text) {
    const h = hash32(text) % 360;
    return `hsla(${h},70%,55%,0.22)`;
  }

  const segmentsSorted = eventsSorted
    .filter(isSegment)
    .map(e => ({ ...e, end: Number(e.onset) + Math.max(0, Number(e.duration)), artifact: isArtifactLabel(e.text) }));

  const pointEventsSorted = eventsSorted
    .filter(e => !isSegment(e));

  function indexFromTime(t) {
    if (frames.length === 0) return 0;
    if (!Number.isFinite(t)) return 0;
    let lo = 0, hi = frames.length - 1;
    while (lo < hi) {
      const mid = Math.floor((lo + hi) / 2);
      const tm = Number(frames[mid].t);
      if (tm < t) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  function fmtTime(t) {
    if (!Number.isFinite(t)) return '—';
    const s = Math.max(0, t);
    const m = Math.floor(s / 60);
    const ss = s - m * 60;
    return `${m}:${ss.toFixed(3).padStart(6, '0')}`;
  }

  function buildSegmentList() {
    if (!segmentList) return;
    segmentList.innerHTML = '';
    if (segmentsSorted.length === 0) {
      const d = document.createElement('div');
      d.className = 'hint';
      d.textContent = 'No segments (duration events) detected.';
      segmentList.appendChild(d);
      return;
    }
    const maxShow = 200;
    for (let i = 0; i < Math.min(maxShow, segmentsSorted.length); ++i) {
      const seg = segmentsSorted[i];
      const box = document.createElement('div');
      box.className = 'evitem';
      if (seg.artifact) {
        box.style.borderColor = 'rgba(239,68,68,0.35)';
      }
      box.addEventListener('click', () => setIndex(indexFromTime(seg.onset)));
      const t = document.createElement('div');
      t.className = 't';
      const dur = (Number.isFinite(seg.duration) ? seg.duration.toFixed(1) : '0.0');
      t.textContent = `${fmtTime(seg.onset)}  (dur ${dur}s)`;
      const txt = document.createElement('div');
      txt.className = 'txt';
      txt.textContent = seg.text || '(segment)';
      box.appendChild(t);
      box.appendChild(txt);
      segmentList.appendChild(box);
    }
    if (segmentsSorted.length > maxShow) {
      const more = document.createElement('div');
      more.className = 'hint';
      more.style.marginTop = '8px';
      more.textContent = `Showing first ${maxShow} of ${segmentsSorted.length} segments.`;
      segmentList.appendChild(more);
    }
  }

  function buildEventList() {
    if (!eventList) return;
    eventList.innerHTML = '';
    if (pointEventsSorted.length === 0) {
      const d = document.createElement('div');
      d.className = 'hint';
      d.textContent = 'No point events in source recording.';
      eventList.appendChild(d);
      return;
    }
    const maxShow = 200;
    for (let i = 0; i < Math.min(maxShow, pointEventsSorted.length); ++i) {
      const ev = pointEventsSorted[i];
      const box = document.createElement('div');
      box.className = 'evitem';
      box.addEventListener('click', () => setIndex(indexFromTime(ev.onset)));
      const t = document.createElement('div');
      t.className = 't';
      t.textContent = fmtTime(ev.onset);
      const txt = document.createElement('div');
      txt.className = 'txt';
      txt.textContent = ev.text || '(event)';
      box.appendChild(t);
      box.appendChild(txt);
      eventList.appendChild(box);
    }
    if (pointEventsSorted.length > maxShow) {
      const more = document.createElement('div');
      more.className = 'hint';
      more.style.marginTop = '8px';
      more.textContent = `Showing first ${maxShow} of ${pointEventsSorted.length} events.`;
      eventList.appendChild(more);
    }
  }

  buildSegmentList();
  buildEventList();

  let evPtr = 0;
  function currentEventLabel(t) {
    if (pointEventsSorted.length === 0 || !Number.isFinite(t)) return '—';
    while (evPtr + 1 < pointEventsSorted.length && pointEventsSorted[evPtr + 1].onset <= t) {
      ++evPtr;
    }
    const ev = pointEventsSorted[Math.max(0, Math.min(evPtr, pointEventsSorted.length - 1))];
    const dt = t - ev.onset;
    const gate = Math.max(0.25, 1.5 * Number(data.meta.update_seconds || 0.25));
    if (dt >= 0 && dt <= gate) return ev.text || '(event)';
    return '—';
  }

  let segPtr = 0;
  function currentSegmentLabel(t) {
    if (segmentsSorted.length === 0 || !Number.isFinite(t)) return '—';
    if (t < segmentsSorted[0].onset) return '—';
    while (segPtr + 1 < segmentsSorted.length && segmentsSorted[segPtr + 1].onset <= t) {
      ++segPtr;
    }
    // Check a small backward window to handle overlaps.
    for (let back = 0; back < 64; ++back) {
      const j = segPtr - back;
      if (j < 0) break;
      const s = segmentsSorted[j];
      if (t >= s.onset && t <= (s.end ?? (s.onset + s.duration))) {
        return s.text || '(segment)';
      }
    }
    return '—';
  }

  function finiteOrNaN(x) {
    return (x === null || x === undefined) ? NaN : Number(x);
  }

  function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const rect = plot.getBoundingClientRect();
    plot.width = Math.max(1, Math.floor(rect.width * dpr));
    plot.height = Math.max(1, Math.floor(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  window.addEventListener('resize', () => { resizeCanvas(); render(); });

  function computeYRangeFor(i0, i1) {
    let minY = Infinity;
    let maxY = -Infinity;
    if (frames.length === 0) {
      return { minY: 0, maxY: 1 };
    }
    const a = Math.max(0, Math.min(Number(i0) || 0, frames.length - 1));
    const b = Math.max(0, Math.min(Number(i1) || 0, frames.length - 1));
    const lo = Math.min(a, b);
    const hi = Math.max(a, b);
    for (let i = lo; i <= hi; ++i) {
      const f = frames[i];
      const m = finiteOrNaN(f.metric);
      const th = finiteOrNaN(f.threshold);
      if (Number.isFinite(m)) { minY = Math.min(minY, m); maxY = Math.max(maxY, m); }
      if (Number.isFinite(th)) { minY = Math.min(minY, th); maxY = Math.max(maxY, th); }
    }
    if (!Number.isFinite(minY) || !Number.isFinite(maxY)) {
      minY = 0; maxY = 1;
    }
    if (Math.abs(maxY - minY) < 1e-12) {
      maxY = minY + 1;
    }
    const pad = 0.10 * (maxY - minY);
    return { minY: minY - pad, maxY: maxY + pad };
  }

  const globalYR = computeYRangeFor(0, frames.length - 1);

  function render() {
    if (frames.length === 0) return;
    resizeCanvas();
    const w = plot.getBoundingClientRect().width;
    const h = plot.getBoundingClientRect().height;
    ctx.clearRect(0, 0, w, h);

    const padL = 50, padR = 18, padT = 14, padB = 32;
    const x0 = padL, x1 = w - padR, y0 = padT, y1 = h - padB;

    const t0 = Number(frames[0].t);
    const tN = Number(frames[frames.length - 1].t);
    const tcur = Number(frames[idx].t);

    const mode = viewModeSel ? String(viewModeSel.value) : 'overview';
    const winSec = Math.max(0.5, Number(winSel ? winSel.value : 30));
    const yMode = yModeSel ? String(yModeSel.value) : 'global';

    let tMin = t0;
    let tMax = tN;
    if (mode === 'realtime') {
      // BioTrace+ has a real-time mode (scrolling strip chart). We approximate it
      // by showing a trailing window ending at the current frame.
      tMax = Number.isFinite(tcur) ? tcur : tN;
      tMin = tMax - winSec;
      if (tMin < t0) {
        tMin = t0;
        tMax = Math.min(tN, t0 + winSec);
      }
      if (tMax > tN) {
        tMax = tN;
        tMin = Math.max(t0, tN - winSec);
      }
    }

    const tSpan = Math.max(1e-9, tMax - tMin);
    let iStart = indexFromTime(tMin);
    let iEnd = indexFromTime(tMax);
    if (iEnd > 0 && Number(frames[iEnd].t) > tMax) iEnd -= 1;
    iEnd = Math.max(iStart, Math.min(iEnd, frames.length - 1));

    const yr = (yMode === 'window') ? computeYRangeFor(iStart, iEnd) : globalYR;

    function xFromT(t) { return x0 + (t - tMin) / tSpan * (x1 - x0); }
    function yFromV(v) { return y1 - (v - yr.minY) / (yr.maxY - yr.minY) * (y1 - y0); }

    // Grid (strip-chart feel).
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1;
    for (let g = 1; g < 6; ++g) {
      const yg = y0 + g * (y1 - y0) / 6;
      ctx.beginPath(); ctx.moveTo(x0, yg); ctx.lineTo(x1, yg); ctx.stroke();
    }
    // Vertical grid at ~10 lines max.
    const step = Math.pow(10, Math.floor(Math.log10(Math.max(1e-9, tSpan))));
    const nice = (tSpan / step > 10) ? step * 2 : (tSpan / step > 5 ? step : step / 2);
    const start = Math.ceil(tMin / nice) * nice;
    for (let tt = start; tt < tMax; tt += nice) {
      const xg = xFromT(tt);
      ctx.beginPath(); ctx.moveTo(xg, y0); ctx.lineTo(xg, y1); ctx.stroke();
    }

    // Axes
    ctx.globalAlpha = 1.0;
    ctx.strokeStyle = 'rgba(255,255,255,0.20)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x0, y1);
    ctx.lineTo(x1, y1);
    ctx.stroke();

    // Artifact shading
    if (Number(data.meta.artifact_engine) === 1) {
      ctx.fillStyle = 'rgba(239,68,68,0.10)';
      let open = false;
      let xStart = 0;
      for (let i = iStart; i <= iEnd; ++i) {
        const f = frames[i];
        const bad = Number(f.artifact) === 1;
        const x = xFromT(Number(f.t));
        if (bad && !open) { open = true; xStart = x; }
        if (!bad && open) {
          open = false;
          ctx.fillRect(xStart, y0, Math.max(1, x - xStart), y1 - y0);
        }
      }
      if (open) {
        const xEnd = xFromT(Number(frames[iEnd].t));
        ctx.fillRect(xStart, y0, Math.max(1, xEnd - xStart), y1 - y0);
      }
    }

    // Segment band (BioTrace+ style): duration events rendered near the bottom axis.
    if (segmentsSorted.length > 0) {
      const segH = 10;
      const ySeg0 = y1 - segH;
      for (const seg of segmentsSorted) {
        if (!Number.isFinite(seg.onset) || !Number.isFinite(seg.end)) continue;
        const s0 = seg.onset;
        const s1 = seg.end;
        if (s1 < tMin || s0 > tMax) continue;
        const xa = xFromT(Math.max(tMin, s0));
        const xb = xFromT(Math.min(tMax, s1));
        const ww = Math.max(1, xb - xa);
        if (seg.artifact) {
          ctx.fillStyle = 'rgba(239,68,68,0.12)';
          ctx.fillRect(xa, ySeg0, ww, segH);
          ctx.strokeStyle = 'rgba(239,68,68,0.35)';
          ctx.lineWidth = 1;
          const step = 6;
          for (let x = xa - segH; x < xa + ww + segH; x += step) {
            ctx.beginPath();
            ctx.moveTo(x, ySeg0 + segH);
            ctx.lineTo(x + segH, ySeg0);
            ctx.stroke();
          }
        } else {
          ctx.fillStyle = segFill(seg.text);
          ctx.fillRect(xa, ySeg0, ww, segH);
        }
      }
      ctx.strokeStyle = 'rgba(255,255,255,0.16)';
      ctx.lineWidth = 1;
      ctx.strokeRect(x0, y1 - segH, x1 - x0, segH);
    }

    // Event markers
    if (pointEventsSorted.length > 0) {
      ctx.strokeStyle = 'rgba(148,163,184,0.22)';
      ctx.lineWidth = 1;
      for (const ev of pointEventsSorted) {
        if (!Number.isFinite(ev.onset)) continue;
        if (ev.onset < tMin || ev.onset > tMax) continue;
        const x = xFromT(ev.onset);
        ctx.beginPath(); ctx.moveTo(x, y0); ctx.lineTo(x, y1); ctx.stroke();
      }
    }

    // Threshold line
    ctx.strokeStyle = 'rgba(251,191,36,0.95)';
    ctx.lineWidth = 2;
    ctx.beginPath();
    let started = false;
    for (let i = iStart; i <= iEnd; ++i) {
      const f = frames[i];
      const th = finiteOrNaN(f.threshold);
      if (!Number.isFinite(th)) continue;
      const x = xFromT(Number(f.t));
      const y = yFromV(th);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else { ctx.lineTo(x, y); }
    }
    ctx.stroke();

    // Metric line
    ctx.strokeStyle = 'rgba(56,189,248,0.95)';
    ctx.lineWidth = 2;
    ctx.beginPath();
    started = false;
    for (let i = iStart; i <= iEnd; ++i) {
      const f = frames[i];
      const m = finiteOrNaN(f.metric);
      if (!Number.isFinite(m)) continue;
      const x = xFromT(Number(f.t));
      const y = yFromV(m);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else { ctx.lineTo(x, y); }
    }
    ctx.stroke();

    // Reward overlay (draw points)
    ctx.fillStyle = 'rgba(52,211,153,0.95)';
    for (let i = iStart; i <= iEnd; ++i) {
      if (Number(frames[i].reward) !== 1) continue;
      const m = finiteOrNaN(frames[i].metric);
      if (!Number.isFinite(m)) continue;
      const x = xFromT(Number(frames[i].t));
      const y = yFromV(m);
      ctx.fillRect(x - 1, y - 1, 2, 2);
    }

    // Current index marker
    const f = frames[idx];
    const xCur = xFromT(Number(f.t));
    ctx.strokeStyle = 'rgba(255,255,255,0.50)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(xCur, y0);
    ctx.lineTo(xCur, y1);
    ctx.stroke();

    // Axis labels
    ctx.fillStyle = 'rgba(148,163,184,0.9)';
    ctx.font = '12px ui-monospace, Menlo, Consolas, monospace';
    const modeLabel = (mode === 'realtime') ? 'RT' : 'OV';
    ctx.fillText(`t: ${tMin.toFixed(2)}..${tMax.toFixed(2)} s (${modeLabel})`, x0, h - 10);
    ctx.fillText(`y: ${yr.minY.toPrecision(4)}..${yr.maxY.toPrecision(4)}`, x0, 14);
  }

  function updateReadouts() {
    if (frames.length === 0) return;
    idx = Math.max(0, Math.min(idx, frames.length - 1));
    const f = frames[idx];
    const m = finiteOrNaN(f.metric);
    const th = finiteOrNaN(f.threshold);
    const fb = finiteOrNaN(f.feedback_raw);
    const rv = finiteOrNaN(f.reward_value);
    const rr = finiteOrNaN(f.reward_rate);
    curMetric.textContent = Number.isFinite(m) ? m.toPrecision(6) : '—';
    curThreshold.textContent = Number.isFinite(th) ? th.toPrecision(6) : '—';
    curRR.textContent = Number.isFinite(rr) ? rr.toFixed(3) : '—';
    const rOn = (Number(f.reward) === 1);
    curReward.textContent = rOn ? 'ON' : 'OFF';
    curReward.style.color = rOn ? 'var(--reward)' : 'var(--muted)';

    if (curFb) {
      curFb.textContent = Number.isFinite(fb) ? fb.toFixed(3) : '—';
      curFb.style.color = (Number.isFinite(fb) && fb > 0) ? 'var(--accent)' : 'var(--muted)';
    }

    if (curRV) {
      curRV.textContent = Number.isFinite(rv) ? rv.toFixed(3) : '—';
      curRV.style.color = (Number.isFinite(rv) && rv > 0) ? 'var(--reward)' : 'var(--muted)';
    }

    if (Number(data.meta.artifact_engine) === 1) {
      const ready = Number(f.artifact_ready) === 1;
      const bad = Number(f.artifact) === 1;
      const bc = Number(f.bad_channels) || 0;
      curArt.textContent = ready ? (bad ? `BAD (bad_ch=${bc})` : `OK (bad_ch=${bc})`) : 'warming up';
    } else {
      curArt.textContent = 'disabled';
    }

    const tcur = Number(f.t);
    curT.textContent = `${tcur.toFixed(3)} s`;
    curIdx.textContent = `${idx + 1} / ${frames.length}`;

    let sum = 0;
    for (let i = 0; i <= idx; ++i) sum += Number(frames[i].reward) === 1 ? 1 : 0;
    const pct = 100 * sum / Math.max(1, (idx + 1));
    curPct.textContent = `${pct.toFixed(1)}%`;

    if (barPct) {
      const p = Math.max(0, Math.min(100, pct));
      barPct.style.width = `${p.toFixed(1)}%`;
    }

    if (curSegment) {
      curSegment.textContent = currentSegmentLabel(tcur);
    }

    if (curEvent) {
      curEvent.textContent = currentEventLabel(tcur);
    }
  }

  function setIndex(i) {
    idx = Math.max(0, Math.min(i, frames.length - 1));
    slider.value = String(idx);
    updateReadouts();
    render();
  }

  function stop() {
    playing = false;
    if (timer) { clearInterval(timer); timer = null; }
    btnPlay.textContent = '▶︎ Play';
    setIndex(0);
  }

  function playToggle() {
    if (frames.length === 0) return;
    playing = !playing;
    if (!playing) {
      if (timer) { clearInterval(timer); timer = null; }
      btnPlay.textContent = '▶︎ Play';
      return;
    }
    btnPlay.textContent = '❚❚ Pause';
    const baseDtMs = 1000.0 * Number(data.meta.update_seconds || 0.25);
    const speed = Number(speedSel.value || 1.0);
    const dtMs = Math.max(10, Math.floor(baseDtMs / Math.max(1e-9, speed)));
    if (timer) { clearInterval(timer); timer = null; }
    timer = setInterval(() => {
      if (!playing) return;
      if (idx >= frames.length - 1) {
        playing = false;
        btnPlay.textContent = '▶︎ Play';
        clearInterval(timer);
        timer = null;
        return;
      }
      setIndex(idx + 1);
    }, dtMs);
  }

  slider.addEventListener('input', () => setIndex(Number(slider.value)));
  btnPlay.addEventListener('click', playToggle);
  btnStop.addEventListener('click', stop);
  speedSel.addEventListener('change', () => { if (playing) playToggle(), playToggle(); });

  if (btnStart) btnStart.addEventListener('click', () => setIndex(0));
  if (btnEnd) btnEnd.addEventListener('click', () => setIndex(Math.max(0, frames.length - 1)));

  if (viewModeSel) viewModeSel.addEventListener('change', () => render());
  if (winSel) winSel.addEventListener('change', () => render());
  if (yModeSel) yModeSel.addEventListener('change', () => render());

  slider.max = String(Math.max(0, frames.length - 1));
  slider.value = '0';
  setIndex(0);
  </script>
)JS";

  out << "</body>\n</html>\n";
  std::cout << "Wrote BioTrace+ style UI: " << outpath << "\n";
}


static void write_reward_tone_wav_if_requested(const Args& args,
                                              const std::vector<float>& reward_values) {
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

  std::vector<float> mono;
  mono.reserve(reward_values.size() * seg);

  const double two_pi = 2.0 * std::acos(-1.0);
  const double phase_inc = two_pi * args.audio_tone_hz / static_cast<double>(sr);
  double phase = 0.0;

  // Attack/release smoothing at the audio sample rate.
  //
  // This keeps the audio click-free even when the NF update interval is coarse.
  const double dt = 1.0 / static_cast<double>(sr);
  const auto alpha_from_tau = [&](double tau_sec) {
    if (!(std::isfinite(tau_sec) && tau_sec > 0.0)) return 1.0; // instantaneous
    return dt / (tau_sec + dt);
  };
  const double a_attack = alpha_from_tau(args.audio_attack_sec);
  const double a_release = alpha_from_tau(args.audio_release_sec);

  double env = 0.0;
  for (size_t i = 0; i < reward_values.size(); ++i) {
    double target = static_cast<double>(reward_values[i]);
    if (!std::isfinite(target)) target = 0.0;
    if (target < 0.0) target = 0.0;
    if (target > 1.0) target = 1.0;
    target *= args.audio_gain;

    for (size_t k = 0; k < seg; ++k) {
      const double a = (target > env) ? a_attack : a_release;
      env += (target - env) * a;
      const float s = static_cast<float>(std::sin(phase) * env);
      mono.push_back(s);
      phase += phase_inc;
      if (phase > two_pi) phase -= two_pi;
    }

    // Reset phase when we're effectively silent so restarted tones are phase-aligned.
    if (target == 0.0 && std::fabs(env) < 1e-6) phase = 0.0;
  }

  // Optional tail so the envelope can decay smoothly to silence.
  const size_t tail = static_cast<size_t>(std::llround(args.audio_release_sec * sr));
  for (size_t k = 0; k < tail; ++k) {
    const double target = 0.0;
    env += (target - env) * a_release;
    const float s = static_cast<float>(std::sin(phase) * env);
    mono.push_back(s);
    phase += phase_inc;
    if (phase > two_pi) phase -= two_pi;
  }

  write_wav_mono_pcm16(outpath, sr, mono);
  std::cout << "Wrote audio reward tone: " << outpath << "\n";
}

static void write_nf_summary_json(const Args& args,
                                 const EEGRecording& rec,
                                 const NfMetricSpec& metric,
                                 const NfSummaryStats& stats,
                                 double threshold_final,
                                 const AdaptiveThresholdController& adapt,
                                 const RealtimePacer& pacer,
                                 double wall_elapsed_sec) {
  (void)metric;
  const std::string outpath = args.outdir + "/nf_summary.json";
  std::ofstream out(outpath);
  if (!out) {
    std::cerr << "Warning: failed to write " << outpath << "\n";
    return;
  }

  const double file_dur_sec = static_cast<double>(rec.n_samples()) / rec.fs_hz;
  const double achieved_speed = (wall_elapsed_sec > 0.0) ? (file_dur_sec / wall_elapsed_sec)
                                                        : std::numeric_limits<double>::quiet_NaN();

  size_t valid_training_frames = stats.training_frames;
  if (valid_training_frames >= stats.artifact_frames) valid_training_frames -= stats.artifact_frames;
  else valid_training_frames = 0;
  if (valid_training_frames >= stats.rest_frames) valid_training_frames -= stats.rest_frames;
  else valid_training_frames = 0;
  const double overall_reward_rate = (valid_training_frames > 0)
                                      ? (static_cast<double>(stats.reward_frames) /
                                         static_cast<double>(valid_training_frames))
                                      : std::numeric_limits<double>::quiet_NaN();

  const double reward_value_mean = (valid_training_frames > 0)
                                    ? (stats.reward_value_sum / static_cast<double>(valid_training_frames))
                                    : std::numeric_limits<double>::quiet_NaN();
  const double reward_value_max = (valid_training_frames > 0)
                                   ? stats.reward_value_max
                                   : std::numeric_limits<double>::quiet_NaN();

  auto jnum_or_null = [&](double v) {
    if (std::isfinite(v)) out << v;
    else out << "null";
  };

  out << std::setprecision(10);
  out << "{\n";
  out << "  \"Tool\": \"qeeg_nf_cli\",\n";
  out << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";
  out << "  \"OutputDir\": \"" << json_escape(args.outdir) << "\",\n";
  out << "  \"protocol\": ";
  if (!args.protocol.empty()) out << "\"" << json_escape(args.protocol) << "\"";
  else out << "null";
  out << ",\n";
  out << "  \"input_path\": \"" << json_escape(args.input_path) << "\",\n";
  out << "  \"fs_hz\": " << rec.fs_hz << ",\n";
  out << "  \"file_duration_sec\": ";
  jnum_or_null(file_dur_sec);
  out << ",\n";
  out << "  \"wall_elapsed_sec\": ";
  jnum_or_null(wall_elapsed_sec);
  out << ",\n";
  out << "  \"playback_speed_arg\": " << args.playback_speed << ",\n";
  out << "  \"achieved_speed_x\": ";
  jnum_or_null(achieved_speed);
  out << ",\n";
  out << "  \"pacer_enabled\": " << (pacer.enabled ? "true" : "false") << ",\n";
  out << "  \"pacer_max_lag_sec\": ";
  jnum_or_null(pacer.max_lag_sec);
  out << ",\n";
  out << "  \"pacer_total_sleep_sec\": ";
  jnum_or_null(pacer.total_sleep_sec);
  out << ",\n";
  out << "  \"metric_spec\": \"" << json_escape(args.metric_spec) << "\",\n";
  out << "  \"metric_smooth_seconds\": " << args.metric_smooth_seconds << ",\n";
  out << "  \"threshold_hysteresis\": " << args.threshold_hysteresis << ",\n";
  out << "  \"dwell_seconds\": " << args.dwell_seconds << ",\n";
  out << "  \"refractory_seconds\": " << args.refractory_seconds << ",\n";
  out << "  \"feedback_mode\": \"" << json_escape(to_lower(args.feedback_mode)) << "\",\n";
  out << "  \"feedback_span_used\": ";
  jnum_or_null(stats.feedback_span_used);
  out << ",\n";
  out << "  \"train_block_seconds\": " << args.train_block_seconds << ",\n";
  out << "  \"rest_block_seconds\": " << args.rest_block_seconds << ",\n";
  out << "  \"start_with_rest\": " << (args.start_with_rest ? "true" : "false") << ",\n";
  out << "  \"baseline_seconds\": " << args.baseline_seconds << ",\n";
  out << "  \"target_reward_rate\": " << args.target_reward_rate << ",\n";
  out << "  \"adapt_mode\": \"" << json_escape(args.adapt_mode) << "\",\n";
  out << "  \"adapt_eta\": " << args.adapt_eta << ",\n";
  out << "  \"adapt_interval_seconds\": " << args.adapt_interval_seconds << ",\n";
  out << "  \"adapt_window_seconds\": " << args.adapt_window_seconds << ",\n";
  out << "  \"adapt_min_samples\": " << args.adapt_min_samples << ",\n";
  out << "  \"no_adaptation\": " << (args.no_adaptation ? "true" : "false") << ",\n";
  out << "  \"adapt_updates\": " << adapt.update_count() << ",\n";
  out << "  \"adapt_target_quantile\": " << adapt.target_quantile() << ",\n";
  out << "  \"adapt_last_desired_threshold\": ";
  jnum_or_null(adapt.last_desired_threshold());
  out << ",\n";
  out << "  \"adapt_history_size_final\": " << adapt.history_size() << ",\n";

  out << "  \"threshold_init\": ";
  jnum_or_null(stats.threshold_init);
  out << ",\n";
  out << "  \"threshold_final\": ";
  jnum_or_null(threshold_final);
  out << ",\n";

  out << "  \"frames\": {\n";
  out << "    \"baseline\": " << stats.baseline_frames << ",\n";
  out << "    \"training\": " << stats.training_frames << ",\n";
  out << "    \"rest\": " << stats.rest_frames << ",\n";
  out << "    \"artifact\": " << stats.artifact_frames << ",\n";
  out << "    \"reward\": " << stats.reward_frames << "\n";
  out << "  },\n";
  out << "  \"valid_training_frames\": " << valid_training_frames << ",\n";
  out << "  \"overall_reward_rate\": ";
  jnum_or_null(overall_reward_rate);
  out << ",\n";

  out << "  \"reward_value_mean\": ";
  jnum_or_null(reward_value_mean);
  out << ",\n";
  out << "  \"reward_value_max\": ";
  jnum_or_null(reward_value_max);
  out << ",\n";

  out << "  \"metric_training\": {\n";
  out << "    \"n\": " << stats.metric_stats.n() << ",\n";
  out << "    \"mean\": ";
  jnum_or_null(stats.metric_stats.mean());
  out << ",\n";
  out << "    \"stddev\": ";
  jnum_or_null(stats.metric_stats.stddev_population());
  out << ",\n";
  out << "    \"min\": ";
  jnum_or_null(std::isfinite(stats.metric_min) ? stats.metric_min : std::numeric_limits<double>::quiet_NaN());
  out << ",\n";
  out << "    \"max\": ";
  jnum_or_null(std::isfinite(stats.metric_max) ? stats.metric_max : std::numeric_limits<double>::quiet_NaN());
  out << "\n";
  out << "  }\n";
  out << "}\n";

  std::cout << "Wrote NF summary: " << outpath << "\n";
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

    if (!args.protocol.empty()) {
      OscMessage mp(prefix + "/protocol");
      mp.add_string(args.protocol);
      osc->send(mp);
    }

    OscMessage m2(prefix + "/fs");
    m2.add_float32(static_cast<float>(fs_hz));
    osc->send(m2);

    OscMessage m3(prefix + "/reward_direction");
    m3.add_string(reward_direction_name(args.reward_direction));
    osc->send(m3);

    OscMessage m_on(prefix + "/reward_on_frames");
    m_on.add_int32(args.reward_on_frames);
    osc->send(m_on);

    OscMessage m_off(prefix + "/reward_off_frames");
    m_off.add_int32(args.reward_off_frames);
    osc->send(m_off);

    if (args.threshold_hysteresis > 0.0) {
      OscMessage m_h(prefix + "/threshold_hysteresis");
      m_h.add_float32(static_cast<float>(args.threshold_hysteresis));
      osc->send(m_h);
    }

    OscMessage m_fb(prefix + "/feedback_mode");
    m_fb.add_string(to_lower(args.feedback_mode));
    osc->send(m_fb);

    if (std::isfinite(args.feedback_span)) {
      OscMessage m_fbs(prefix + "/feedback_span");
      m_fbs.add_float32(static_cast<float>(args.feedback_span));
      osc->send(m_fbs);
    }

    if (std::isfinite(args.initial_threshold)) {
      OscMessage m4(prefix + "/threshold_init");
      m4.add_float32(static_cast<float>(args.initial_threshold));
      osc->send(m4);
    }
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

    if (mode == "bundle") {
      OscBundle b;

      OscMessage mt(prefix + "/time");
      mt.add_float32(static_cast<float>(t_end_sec));
      b.add_message(mt);

      OscMessage mm(prefix + "/metric");
      mm.add_float32(static_cast<float>(metric));
      b.add_message(mm);

      OscMessage mth(prefix + "/threshold");
      mth.add_float32(static_cast<float>(threshold));
      b.add_message(mth);

      OscMessage mr(prefix + "/reward");
      mr.add_int32(reward);
      b.add_message(mr);

      OscMessage mrr(prefix + "/reward_rate");
      mrr.add_float32(static_cast<float>(reward_rate));
      b.add_message(mrr);

      OscMessage mht(prefix + "/have_threshold");
      mht.add_int32(have_threshold);
      b.add_message(mht);

      osc->send(b);
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

static void osc_send_feedback_span_used(OscUdpClient* osc,
                                        const std::string& prefix,
                                        double span) {
  if (!osc) return;
  if (!std::isfinite(span)) return;
  try {
    OscMessage m(prefix + "/feedback_span_used");
    m.add_float32(static_cast<float>(span));
    osc->send(m);
  } catch (...) {
    // best-effort
  }
}

static void osc_send_feedback_raw(OscUdpClient* osc,
                                 const std::string& prefix,
                                 double t_end_sec,
                                 double feedback_raw) {
  if (!osc) return;
  try {
    OscMessage m(prefix + "/feedback_raw");
    m.add_float32(static_cast<float>(t_end_sec));
    m.add_float32(static_cast<float>(feedback_raw));
    osc->send(m);
  } catch (...) {
    // best-effort
  }
}

static void osc_send_reward_value(OscUdpClient* osc,
                                  const std::string& prefix,
                                  double t_end_sec,
                                  double reward_value) {
  if (!osc) return;
  try {
    OscMessage m(prefix + "/reward_value");
    m.add_float32(static_cast<float>(t_end_sec));
    m.add_float32(static_cast<float>(reward_value));
    osc->send(m);
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


static int find_channel_index(const std::vector<std::string>& channels, const std::string& name) {
  const std::string target = normalize_channel_name(name);
  for (size_t i = 0; i < channels.size(); ++i) {
    if (normalize_channel_name(channels[i]) == target) return static_cast<int>(i);
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

static double compute_metric_band_ratio_or_asym(const OnlineBandpowerFrame& fr,
                                               const NfMetricSpec& spec,
                                               int ch_idx,
                                               int ch_a_idx,
                                               int ch_b_idx,
                                               int b_idx,
                                               int b_num,
                                               int b_den) {
  if (spec.type == NfMetricSpec::Type::Band) {
    const size_t c = static_cast<size_t>(ch_idx);
    return nf_eval_metric_band_or_ratio(fr, spec, c, static_cast<size_t>(b_idx), 0, 0);
  }
  if (spec.type == NfMetricSpec::Type::Ratio) {
    const size_t c = static_cast<size_t>(ch_idx);
    return nf_eval_metric_band_or_ratio(fr, spec, c, 0, static_cast<size_t>(b_num), static_cast<size_t>(b_den));
  }
  if (spec.type != NfMetricSpec::Type::Asymmetry) {
    throw std::runtime_error("compute_metric_band_ratio_or_asym: unsupported spec type");
  }
  const size_t ca = static_cast<size_t>(ch_a_idx);
  const size_t cb = static_cast<size_t>(ch_b_idx);
  return nf_eval_metric_asymmetry(fr, spec, ca, cb, static_cast<size_t>(b_idx));
}

static double clamp01(double x) {
  if (!std::isfinite(x)) return 0.5;
  if (x < 0.0) return 0.0;
  if (x > 1.0) return 1.0;
  return x;
}

// Pick the baseline quantile used to derive the initial threshold from baseline values.
//
// If the user explicitly passes --baseline-quantile Q, we use it.
// Otherwise, we choose an "auto" quantile that approximately matches the desired
// reward rate at initialization:
//   - reward above => P(x > thr) ~ R => thr ~ F^{-1}(1-R)
//   - reward below => P(x < thr) ~ R => thr ~ F^{-1}(R)
static double baseline_quantile_used(const Args& args) {
  if (std::isfinite(args.baseline_quantile)) return clamp01(args.baseline_quantile);
  const double q = (args.reward_direction == RewardDirection::Above)
                     ? (1.0 - args.target_reward_rate)
                     : args.target_reward_rate;
  return clamp01(q);
}

static double initial_threshold_from_baseline(const Args& args,
                                              const std::vector<double>& baseline_values,
                                              double fallback_value,
                                              double* q_used_out = nullptr) {
  const double q = baseline_quantile_used(args);
  if (q_used_out) *q_used_out = q;

  if (baseline_values.empty()) {
    return fallback_value;
  }
  std::vector<double> tmp = baseline_values;
  const double thr = quantile_inplace(&tmp, q);
  return std::isfinite(thr) ? thr : fallback_value;
}

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);

    // Apply protocol preset defaults (if requested).
    apply_protocol_preset(&args);

    if (!args.demo && args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required (or use --demo)");
    }
    if (args.target_reward_rate <= 0.0 || args.target_reward_rate >= 1.0) {
      throw std::runtime_error("--target-rate must be in (0,1)");
    }
    if (args.baseline_seconds < 0.0) {
      throw std::runtime_error("--baseline must be >= 0");
    }
    if (std::isfinite(args.baseline_quantile)) {
      if (args.baseline_quantile < 0.0 || args.baseline_quantile > 1.0) {
        throw std::runtime_error("--baseline-quantile must be in [0,1]");
      }
    }
    if (args.adapt_eta < 0.0) {
      throw std::runtime_error("--eta must be >= 0");
    }

    // Validate adaptation settings early so we fail fast on typos.
    const AdaptMode adapt_mode = parse_adapt_mode(args.adapt_mode);
    if (!std::isfinite(args.adapt_interval_seconds) || args.adapt_interval_seconds < 0.0) {
      throw std::runtime_error("--adapt-interval must be a finite value >= 0");
    }
    if (!std::isfinite(args.adapt_window_seconds) || args.adapt_window_seconds < 0.0) {
      throw std::runtime_error("--adapt-window must be a finite value >= 0");
    }
    if (args.adapt_min_samples < 1) {
      throw std::runtime_error("--adapt-min-samples must be >= 1");
    }
    if (args.reward_on_frames < 1) {
      throw std::runtime_error("--reward-on-frames must be >= 1");
    }
    if (args.reward_off_frames < 1) {
      throw std::runtime_error("--reward-off-frames must be >= 1");
    }

    if (!std::isfinite(args.threshold_hysteresis) || args.threshold_hysteresis < 0.0) {
      throw std::runtime_error("--threshold-hysteresis must be a finite value >= 0");
    }

    if (!std::isfinite(args.dwell_seconds) || args.dwell_seconds < 0.0) {
      throw std::runtime_error("--dwell must be a finite value >= 0");
    }
    if (!std::isfinite(args.refractory_seconds) || args.refractory_seconds < 0.0) {
      throw std::runtime_error("--refractory must be a finite value >= 0");
    }

    const std::string fb_mode = to_lower(args.feedback_mode);
    if (fb_mode != "binary" && fb_mode != "continuous") {
      throw std::runtime_error("--feedback-mode must be 'binary' or 'continuous'");
    }
    if (std::isfinite(args.feedback_span) && args.feedback_span <= 0.0) {
      throw std::runtime_error("--feedback-span must be a finite value > 0 (or omit it to auto-estimate)");
    }

    const bool want_blocks = (args.train_block_seconds > 0.0 || args.rest_block_seconds > 0.0);
    if (want_blocks) {
      if (!std::isfinite(args.train_block_seconds) || args.train_block_seconds <= 0.0) {
        throw std::runtime_error("--train-block must be a finite value > 0 when block scheduling is enabled");
      }
      if (!std::isfinite(args.rest_block_seconds) || args.rest_block_seconds <= 0.0) {
        throw std::runtime_error("--rest-block must be a finite value > 0 when block scheduling is enabled");
      }
    }
    if (args.relative_power && (args.relative_fmin_hz != 0.0 || args.relative_fmax_hz != 0.0)) {
      if (args.relative_fmin_hz < 0.0) {
        throw std::runtime_error("--relative-range LO must be >= 0");
      }
      if (!(args.relative_fmax_hz > args.relative_fmin_hz)) {
        throw std::runtime_error("--relative-range must satisfy LO < HI");
      }
    }
    if (args.artifact_min_bad_channels < 1) {
      throw std::runtime_error("--artifact-min-bad-ch must be >= 1");
    }

    if (!std::isfinite(args.metric_smooth_seconds) || args.metric_smooth_seconds < 0.0) {
      throw std::runtime_error("--metric-smooth must be a finite value >= 0");
    }

    // Playback pacing is opt-in: 0 => disabled. Any non-zero value must be > 0.
    if (args.playback_speed != 0.0) {
      if (!std::isfinite(args.playback_speed) || args.playback_speed <= 0.0) {
        throw std::runtime_error("--speed must be a finite value > 0");
      }
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

    // Optional: load channel-level QC labels and use them to improve robustness.
    // Cross-tool workflow:
    //   qeeg_channel_qc_cli -> qeeg_nf_cli
    bool have_qc = false;
    std::string qc_resolved_path;
    std::vector<char> qc_bad(rec.n_channels(), 0);
    std::vector<std::string> qc_reasons(rec.n_channels());
    std::vector<std::string> qc_bad_names;
    qc_bad_names.reserve(rec.n_channels());
    size_t qc_bad_count = 0;

    if (!args.channel_qc.empty()) {
      std::cout << "Loading channel QC: " << args.channel_qc << "\n";
      ChannelQcMap qc = load_channel_qc_any(args.channel_qc, &qc_resolved_path);
      have_qc = true;

      for (size_t c = 0; c < rec.n_channels(); ++c) {
        const std::string key = normalize_channel_name(rec.channel_names[c]);
        const auto it = qc.find(key);
        if (it != qc.end() && it->second.bad) {
          qc_bad[c] = 1;
          qc_reasons[c] = it->second.reasons;
          qc_bad_names.push_back(rec.channel_names[c]);
          ++qc_bad_count;
        }
      }

      std::cout << "Channel QC loaded from: " << qc_resolved_path
                << " (" << qc_bad_count << "/" << rec.n_channels() << " channels marked bad)\n";

      // Persist the applied mask for provenance.
      const std::string bad_out = args.outdir + "/bad_channels_used.txt";
      std::ofstream bout(bad_out);
      if (!bout) {
        std::cerr << "Warning: failed to write bad_channels_used.txt to: " << bad_out << "\n";
      } else {
        for (size_t c = 0; c < rec.n_channels(); ++c) {
          if (!qc_bad[c]) continue;
          bout << rec.channel_names[c];
          if (!qc_reasons[c].empty()) bout << "\t" << qc_reasons[c];
          bout << "\n";
        }
      }
    }

    // Merge artifact-ignore list with QC bad channels (deduplicated by normalized name).
    std::vector<std::string> artifact_ignore = args.artifact_ignore_channels;
    if (have_qc && !qc_bad_names.empty()) {
      artifact_ignore.insert(artifact_ignore.end(), qc_bad_names.begin(), qc_bad_names.end());
      std::unordered_set<std::string> seen;
      seen.reserve(artifact_ignore.size());
      std::vector<std::string> uniq;
      uniq.reserve(artifact_ignore.size());
      for (const auto& nm : artifact_ignore) {
        const std::string key = normalize_channel_name(nm);
        if (key.empty()) continue;
        if (seen.insert(key).second) uniq.push_back(nm);
      }
      artifact_ignore.swap(uniq);
    }

    // Convenience: write run parameters to JSON for easy downstream parsing.
    {
      const std::string meta_path = args.outdir + "/nf_run_meta.json";
      std::ofstream meta(meta_path);
      if (!meta) {
        std::cerr << "Warning: failed to write " << meta_path << "\n";
      } else {
        meta << "{\n";
        const bool derived_events_written = (args.biotrace_ui || args.export_derived_events);
        meta << "  \"Tool\": \"qeeg_nf_cli\",\n";
        meta << "  \"TimestampLocal\": \"" << json_escape(now_string_local()) << "\",\n";
        meta << "  \"OutputDir\": \"" << json_escape(args.outdir) << "\",\n";
        meta << "  \"Outputs\": [\n";
        bool first_out = true;
        auto emit_out = [&](const std::string& rel) {
          if (!first_out) meta << ",\n";
          first_out = false;
          meta << "    \"" << json_escape(rel) << "\"";
        };
        emit_out("nf_run_meta.json");
        emit_out("nf_feedback.csv");
        emit_out("nf_summary.json");
        if (have_qc) emit_out("bad_channels_used.txt");
        if (args.export_artifacts) emit_out("artifact_gate_timeseries.csv");
        if (args.export_bandpowers) emit_out("bandpower_timeseries.csv");
        if (args.export_coherence) {
          emit_out("coherence_timeseries.csv");
          emit_out("imcoh_timeseries.csv");
        }
        if (derived_events_written) {
          emit_out("nf_derived_events.csv");
          emit_out("nf_derived_events.tsv");
          emit_out("nf_derived_events.json");
        }
        if (args.biotrace_ui) emit_out("biotrace_ui.html");
        meta << "\n  ],\n";
        meta << "  \"demo\": " << (args.demo ? "true" : "false") << ",\n";
        meta << "  \"input_path\": \"" << json_escape(args.input_path) << "\",\n";
        meta << "  \"channel_qc\": ";
        if (!args.channel_qc.empty()) meta << "\"" << json_escape(args.channel_qc) << "\"";
        else meta << "null";
        meta << ",\n";
        meta << "  \"channel_qc_resolved\": ";
        if (have_qc) meta << "\"" << json_escape(qc_resolved_path) << "\"";
        else meta << "null";
        meta << ",\n";
        meta << "  \"qc_bad_channel_count\": " << qc_bad_count << ",\n";
        meta << "  \"qc_bad_channels\": [";
        for (size_t i = 0; i < qc_bad_names.size(); ++i) {
          if (i) meta << ", ";
          meta << "\"" << json_escape(qc_bad_names[i]) << "\"";
        }
        meta << "],\n";
        meta << "  \"allow_bad_metric_channels\": " << (args.allow_bad_metric_channels ? "true" : "false") << ",\n";
        meta << "  \"fs_hz\": " << rec.fs_hz << ",\n";
        meta << "  \"protocol\": ";
        if (!args.protocol.empty()) meta << "\"" << json_escape(args.protocol) << "\"";
        else meta << "null";
        meta << ",\n";
        meta << "  \"metric_spec\": \"" << json_escape(args.metric_spec) << "\",\n";
        meta << "  \"band_spec\": \"" << json_escape(args.band_spec) << "\",\n";
        meta << "  \"reward_direction\": \"" << reward_direction_name(args.reward_direction) << "\",\n";
        meta << "  \"threshold_init\": ";
        if (std::isfinite(args.initial_threshold)) meta << args.initial_threshold;
        else meta << "null";
        meta << ",\n";
        meta << "  \"baseline_seconds\": " << args.baseline_seconds << ",\n";
        meta << "  \"baseline_quantile\": ";
        if (std::isfinite(args.baseline_quantile)) meta << args.baseline_quantile;
        else meta << "null";
        meta << ",\n";
        meta << "  \"baseline_quantile_used\": " << baseline_quantile_used(args) << ",\n";
        meta << "  \"target_reward_rate\": " << args.target_reward_rate << ",\n";
        meta << "  \"adapt_eta\": " << args.adapt_eta << ",\n";
        meta << "  \"adapt_mode\": \"" << json_escape(args.adapt_mode) << "\",\n";
        meta << "  \"adapt_interval_seconds\": " << args.adapt_interval_seconds << ",\n";
        meta << "  \"adapt_window_seconds\": " << args.adapt_window_seconds << ",\n";
        meta << "  \"adapt_min_samples\": " << args.adapt_min_samples << ",\n";
        meta << "  \"reward_rate_window_seconds\": " << args.reward_rate_window_seconds << ",\n";
        meta << "  \"reward_on_frames\": " << args.reward_on_frames << ",\n";
        meta << "  \"reward_off_frames\": " << args.reward_off_frames << ",\n";
        meta << "  \"threshold_hysteresis\": " << args.threshold_hysteresis << ",\n";
        meta << "  \"dwell_seconds\": " << args.dwell_seconds << ",\n";
        meta << "  \"refractory_seconds\": " << args.refractory_seconds << ",\n";
        meta << "  \"feedback_mode\": \"" << json_escape(args.feedback_mode) << "\",\n";
        meta << "  \"feedback_span\": ";
        if (std::isfinite(args.feedback_span)) meta << args.feedback_span;
        else meta << "null";
        meta << ",\n";
        meta << "  \"train_block_seconds\": " << args.train_block_seconds << ",\n";
        meta << "  \"rest_block_seconds\": " << args.rest_block_seconds << ",\n";
        meta << "  \"start_with_rest\": " << (args.start_with_rest ? "true" : "false") << ",\n";
        meta << "  \"window_seconds\": " << args.window_seconds << ",\n";
        meta << "  \"update_seconds\": " << args.update_seconds << ",\n";
        meta << "  \"metric_smooth_seconds\": " << args.metric_smooth_seconds << ",\n";
        meta << "  \"playback_speed\": " << args.playback_speed << ",\n";
        meta << "  \"nperseg\": " << args.nperseg << ",\n";
        meta << "  \"log10_power\": " << (args.log10_power ? "true" : "false") << ",\n";
        meta << "  \"relative_power\": " << (args.relative_power ? "true" : "false") << ",\n";
        meta << "  \"relative_fmin_hz\": " << args.relative_fmin_hz << ",\n";
        meta << "  \"relative_fmax_hz\": " << args.relative_fmax_hz << ",\n";
        meta << "  \"overlap\": " << args.overlap << ",\n";
        meta << "  \"artifact_gate\": " << (args.artifact_gate ? "true" : "false") << ",\n";
        meta << "  \"artifact_ptp_z\": " << args.artifact_ptp_z << ",\n";
        meta << "  \"artifact_rms_z\": " << args.artifact_rms_z << ",\n";
        meta << "  \"artifact_kurtosis_z\": " << args.artifact_kurtosis_z << ",\n";
        meta << "  \"artifact_min_bad_channels\": " << args.artifact_min_bad_channels << ",\n";
        meta << "  \"artifact_ignore_channels\": [";
        for (size_t i = 0; i < artifact_ignore.size(); ++i) {
          if (i) meta << ", ";
          meta << "\"" << json_escape(artifact_ignore[i]) << "\"";
        }
        meta << "],\n";
        meta << "  \"biotrace_ui\": " << (args.biotrace_ui ? "true" : "false") << ",\n";
        meta << "  \"export_derived_events\": " << (args.export_derived_events ? "true" : "false") << ",\n";
        meta << "  \"derived_events_written\": " << (derived_events_written ? "true" : "false") << ",\n";
        meta << "  \"derived_events_csv\": "
             << (derived_events_written ? "\"" + json_escape("nf_derived_events.csv") + "\"" : "null")
             << ",\n";
        meta << "  \"derived_events_tsv\": "
             << (derived_events_written ? "\"" + json_escape("nf_derived_events.tsv") + "\"" : "null")
             << ",\n";
        meta << "  \"derived_events_json\": "
             << (derived_events_written ? "\"" + json_escape("nf_derived_events.json") + "\"" : "null")
             << "\n";
        meta << "}\n";
      }
    }



    // Optional OSC output for integration with external tools (UDP is best-effort / unreliable).
    std::unique_ptr<OscUdpClient> osc_client;
    OscUdpClient* osc = nullptr;
    std::string osc_prefix;
    std::string osc_mode = to_lower(args.osc_mode);

    if (args.osc_port != 0) {
      if (args.osc_port < 0 || args.osc_port > 65535) {
        throw std::runtime_error("--osc-port must be 0 (disable) or in [1, 65535]");
      }
      if (osc_mode != "state" && osc_mode != "split" && osc_mode != "bundle") {
        throw std::runtime_error("--osc-mode must be 'state', 'split' or 'bundle'");
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
    const NfMetricSpec metric = parse_nf_metric_spec(args.metric_spec);

    // If channel QC is provided, optionally fail fast when the selected metric uses bad channels.
    auto qc_is_bad = [&](int idx) -> bool {
      return have_qc && idx >= 0 && static_cast<size_t>(idx) < qc_bad.size() && qc_bad[static_cast<size_t>(idx)];
    };

    if (have_qc) {
      std::vector<std::string> bad_metric_channels;
      if (metric.type == NfMetricSpec::Type::Coherence || metric.type == NfMetricSpec::Type::Asymmetry) {
        const int ia = find_channel_index(rec.channel_names, metric.channel_a);
        const int ib = find_channel_index(rec.channel_names, metric.channel_b);
        if (qc_is_bad(ia)) bad_metric_channels.push_back(rec.channel_names[static_cast<size_t>(ia)]);
        if (qc_is_bad(ib)) bad_metric_channels.push_back(rec.channel_names[static_cast<size_t>(ib)]);
      } else {
        const int ich = find_channel_index(rec.channel_names, metric.channel);
        if (qc_is_bad(ich)) bad_metric_channels.push_back(rec.channel_names[static_cast<size_t>(ich)]);
      }

      if (!bad_metric_channels.empty()) {
        std::string msg = "NF metric uses channel(s) marked bad by channel QC:";
        for (const auto& ch : bad_metric_channels) msg += " " + ch;
        msg += " (use --allow-bad-metric-channels to override)";
        if (args.allow_bad_metric_channels) {
          std::cerr << "Warning: " << msg << "\n";
        } else {
          throw std::runtime_error(msg);
        }
      }
    }

    if ((args.log10_power || args.relative_power) &&
        (metric.type != NfMetricSpec::Type::Band && metric.type != NfMetricSpec::Type::Ratio && metric.type != NfMetricSpec::Type::Asymmetry)) {
      throw std::runtime_error("--log10 / --relative are only supported for bandpower, ratio, and asymmetry metrics");
    }

    // Output
    std::ofstream out(args.outdir + "/nf_feedback.csv");
    if (!out) throw std::runtime_error("Failed to write nf_feedback.csv");

    const bool do_artifacts = args.artifact_gate || args.export_artifacts;
    const std::string feedback_mode = to_lower(args.feedback_mode);
    const bool continuous_feedback = (feedback_mode == "continuous");

    // Cross-tool integration: optionally export derived events (reward/artifact/baseline)
    // as duration annotations that can be consumed by trace_plot_cli / export_bids_cli.
    const bool want_derived_events = (args.biotrace_ui || args.export_derived_events);
    std::vector<AnnotationEvent> derived_events;
    if (want_derived_events) derived_events.reserve(512);
    BoolSegmentBuilder reward_seg("NF:Reward");
    BoolSegmentBuilder artifact_seg("NF:Artifact");
    BoolSegmentBuilder train_seg("NF:Train");
    BoolSegmentBuilder rest_seg("NF:Rest");
    double prev_frame_end_sec = std::numeric_limits<double>::quiet_NaN();
    double last_frame_end_sec = std::numeric_limits<double>::quiet_NaN();

    enum class NfPhase { Baseline, Train, Rest };
    auto phase_name = [&](NfPhase p) -> const char* {
      switch (p) {
        case NfPhase::Baseline: return "baseline";
        case NfPhase::Train: return "train";
        case NfPhase::Rest: return "rest";
      }
      return "unknown";
    };

    const bool blocks_enabled = (args.train_block_seconds > 0.0 && args.rest_block_seconds > 0.0);
    const double schedule_start_sec = std::isfinite(args.initial_threshold) ? 0.0 : args.baseline_seconds;
    auto phase_of = [&](double t_end_sec) -> NfPhase {
      if (!std::isfinite(t_end_sec) || t_end_sec < 0.0) t_end_sec = 0.0;
      if (t_end_sec < schedule_start_sec) return NfPhase::Baseline;
      if (!blocks_enabled) return NfPhase::Train;
      const double cycle = args.train_block_seconds + args.rest_block_seconds;
      if (!(cycle > 0.0)) return NfPhase::Train;
      const double trel = t_end_sec - schedule_start_sec;
      const double m = std::fmod(trel, cycle);
      const double mm = (m < 0.0) ? (m + cycle) : m;
      if (args.start_with_rest) {
        return (mm < args.rest_block_seconds) ? NfPhase::Rest : NfPhase::Train;
      }
      return (mm < args.train_block_seconds) ? NfPhase::Train : NfPhase::Rest;
    };

    auto derived_update = [&](double t_end_sec, bool reward_on, bool artifact_on, NfPhase phase) {
      if (!want_derived_events) return;
      double frame_end = t_end_sec;
      double frame_start = std::isfinite(prev_frame_end_sec) ? prev_frame_end_sec : (frame_end - args.update_seconds);
      if (!std::isfinite(frame_start)) frame_start = 0.0;
      if (frame_start < 0.0) frame_start = 0.0;
      if (frame_end < frame_start) frame_end = frame_start;
      reward_seg.update(reward_on, frame_start, frame_end, &derived_events);
      if (do_artifacts) {
        artifact_seg.update(artifact_on, frame_start, frame_end, &derived_events);
      }
      // Training/rest schedule segments are useful for offline training and for downstream annotation.
      if (blocks_enabled) {
        train_seg.update(phase == NfPhase::Train, frame_start, frame_end, &derived_events);
        rest_seg.update(phase == NfPhase::Rest, frame_start, frame_end, &derived_events);
      }
      prev_frame_end_sec = frame_end;
      last_frame_end_sec = frame_end;
    };

    auto finalize_derived_events = [&]() {
      if (!want_derived_events) return;
      const double file_dur = static_cast<double>(rec.n_samples()) / rec.fs_hz;
      const double end_sec = std::isfinite(last_frame_end_sec) ? last_frame_end_sec : file_dur;
      reward_seg.finish(end_sec, &derived_events);
      if (do_artifacts) artifact_seg.finish(end_sec, &derived_events);
      if (blocks_enabled) {
        train_seg.finish(end_sec, &derived_events);
        rest_seg.finish(end_sec, &derived_events);
      }

      // Mark the initial threshold estimation baseline period (only when threshold is not forced).
      if (!std::isfinite(args.initial_threshold) && args.baseline_seconds > 0.0) {
        const double bl_end = std::min(file_dur, args.baseline_seconds);
        if (bl_end > 0.0 && std::isfinite(bl_end)) {
          derived_events.push_back({0.0, bl_end, "NF:Baseline"});
        }
      }

      std::sort(derived_events.begin(), derived_events.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
        if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
        return a.duration_sec < b.duration_sec;
      });

      if (args.export_derived_events || args.biotrace_ui) {
        const std::string p_csv = args.outdir + "/nf_derived_events.csv";
        const std::string p_tsv = args.outdir + "/nf_derived_events.tsv";
        const std::string p_json = args.outdir + "/nf_derived_events.json";
        write_events_csv(p_csv, derived_events);
        write_events_tsv(p_tsv, derived_events);
        // BIDS-style sidecar describing columns (and trial_type Levels).
        BidsEventsTsvOptions ev_opt;
        ev_opt.include_trial_type = true;
        ev_opt.include_trial_type_levels = true;
        write_bids_events_json(p_json, ev_opt, derived_events);
        std::cout << "Wrote derived events: " << p_csv << " (" << derived_events.size() << ")" << std::endl;
        std::cout << "Wrote derived events: " << p_tsv << " (" << derived_events.size() << ")" << std::endl;
        std::cout << "Wrote derived events: " << p_json << " (" << derived_events.size() << ")" << std::endl;
      }
    };

    // Optional BioTrace+ style UI timeline (HTML) that can be rendered after the run.
    std::vector<NfUiFrame> ui_frames;
    ui_frames.reserve(4096);

    auto ui_push = [&](double t_end_sec, double metric_val, double thr, bool thr_ready,
                       double feedback_raw, double reward_value, int reward, double rr,
                       const OnlineArtifactFrame& af) {
      if (!args.biotrace_ui) return;
      NfUiFrame uf;
      uf.t_end_sec = t_end_sec;
      uf.metric = metric_val;
      uf.threshold = thr_ready ? thr : std::numeric_limits<double>::quiet_NaN();
      uf.reward = reward;
      uf.feedback_raw = feedback_raw;
      uf.reward_value = reward_value;
      uf.reward_rate = rr;
      if (do_artifacts) {
        uf.artifact_ready = af.baseline_ready ? 1 : 0;
        uf.artifact = (af.baseline_ready && af.bad) ? 1 : 0;
        uf.bad_channels = static_cast<int>(af.bad_channel_count);
      }
      ui_frames.push_back(std::move(uf));
    };

    out << "t_end_sec,metric,threshold,reward,reward_rate";
    if (do_artifacts) {
      out << ",artifact_ready,artifact,bad_channels";
    }
    if (blocks_enabled) {
      out << ",phase";
    }
    const bool want_raw_reward_col = (args.dwell_seconds > 0.0 || args.refractory_seconds > 0.0);
    if (want_raw_reward_col) {
      out << ",raw_reward";
    }
    if (metric.type == NfMetricSpec::Type::Band) {
      out << ",band,channel";
    } else if (metric.type == NfMetricSpec::Type::Ratio) {
      out << ",band_num,band_den,channel";
    } else if (metric.type == NfMetricSpec::Type::Asymmetry) {
      out << ",band,channel_a,channel_b";
    } else if (metric.type == NfMetricSpec::Type::Coherence) {
      out << ",band,channel_a,channel_b,measure";
    } else {
      out << ",phase_band,amp_band,channel,method";
    }

    if (adapt_mode == AdaptMode::Quantile) {
      out << ",threshold_desired";
    }
    if (args.metric_smooth_seconds > 0.0) {
      out << ",metric_raw";
    }
    if (continuous_feedback) {
      out << ",feedback_raw,reward_value";
    }
    out << "\n";

    std::ofstream out_bp;
    std::ofstream out_coh;

    // Thresholding state
    std::vector<double> baseline_values;
    baseline_values.reserve(256);
    bool have_threshold = std::isfinite(args.initial_threshold);
    double threshold = have_threshold ? args.initial_threshold
                                     : std::numeric_limits<double>::quiet_NaN();

    const size_t rate_window_frames = std::max<size_t>(
      1,
      sec_to_samples(args.reward_rate_window_seconds, 1.0 / args.update_seconds));
    std::deque<int> reward_hist;
    reward_hist.clear();

    // Optional reward debouncing / hysteresis.
    BoolDebouncer reward_gate(static_cast<size_t>(args.reward_on_frames),
                              static_cast<size_t>(args.reward_off_frames),
                              /*initial_state=*/false);

    // Optional numeric hysteresis band around the threshold to reduce chatter.
    HysteresisGate thr_hyst(args.threshold_hysteresis, args.reward_direction, /*initial_state=*/false);

    auto reward_rate = [&]() {
      if (reward_hist.empty()) return 0.0;
      int sum = 0;
      for (int v : reward_hist) sum += v;
      return static_cast<double>(sum) / static_cast<double>(reward_hist.size());
    };

    RewardShaper reward_shaper(args.dwell_seconds, args.refractory_seconds);
    double prev_shaper_time = std::numeric_limits<double>::quiet_NaN();
    auto shaper_dt = [&](double t_end_sec) -> double {
      double dt = args.update_seconds;
      if (std::isfinite(prev_shaper_time) && std::isfinite(t_end_sec)) {
        const double d = t_end_sec - prev_shaper_time;
        if (std::isfinite(d) && d > 0.0) dt = d;
      }
      prev_shaper_time = t_end_sec;
      return dt;
    };
    auto shape_reward = [&](bool raw_reward, double t_end_sec, bool freeze) -> bool {
      return reward_shaper.update(raw_reward, shaper_dt(t_end_sec), t_end_sec, freeze);
    };
    auto append_phase_and_raw = [&](NfPhase phase, bool raw_reward) {
      if (blocks_enabled) {
        out << "," << phase_name(phase);
      }
      if (want_raw_reward_col) {
        out << "," << (raw_reward ? 1 : 0);
      }
    };

    AdaptiveThresholdConfig adapt_cfg;
    adapt_cfg.mode = adapt_mode;
    adapt_cfg.reward_direction = args.reward_direction;
    adapt_cfg.target_reward_rate = args.target_reward_rate;
    adapt_cfg.eta = args.adapt_eta;
    adapt_cfg.update_interval_seconds = args.adapt_interval_seconds;
    adapt_cfg.quantile_window_seconds = args.adapt_window_seconds;
    adapt_cfg.quantile_min_samples = static_cast<size_t>(std::max(1, args.adapt_min_samples));
    AdaptiveThresholdController adapt_ctrl(adapt_cfg);
    double feedback_span_used = std::numeric_limits<double>::quiet_NaN();
    bool feedback_span_ready = false;
    if (continuous_feedback && std::isfinite(args.feedback_span) && args.feedback_span > 0.0) {
      feedback_span_used = args.feedback_span;
      feedback_span_ready = true;
    }

    if (!args.no_adaptation && args.adapt_eta > 0.0) {
      std::cout << "Adaptive threshold: mode=" << adapt_mode_name(adapt_cfg.mode)
                << ", eta=" << args.adapt_eta;
      if (adapt_cfg.mode == AdaptMode::Quantile) {
        std::cout << ", window=" << adapt_cfg.quantile_window_seconds
                  << "s, min_samples=" << adapt_cfg.quantile_min_samples;
      }
      if (args.adapt_interval_seconds > 0.0) {
        std::cout << ", interval=" << args.adapt_interval_seconds << "s";
      }
      std::cout << "\n";
    }

    // Optional audio export: one feedback value per emitted NF update (including baseline frames).
    //
    // - binary feedback => values are 0/1
    // - continuous feedback => values are in [0,1] (gated by reward)
    std::vector<float> audio_reward_values;
    audio_reward_values.reserve(1024);

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
      aopt.ignore_channels = artifact_ignore;
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

    // Optional offline real-time pacing / metric smoothing.
    RealtimePacer pacer(args.playback_speed);
    const auto wall_start = std::chrono::steady_clock::now();

    const bool do_smooth = (args.metric_smooth_seconds > 0.0);
    ExponentialSmoother smoother(args.metric_smooth_seconds);
    double prev_metric_time = std::numeric_limits<double>::quiet_NaN();

    NfSummaryStats summary;
    if (have_threshold) {
      summary.threshold_init = threshold;
      summary.threshold_init_set = true;
    }
    if (feedback_span_ready) {
      summary.feedback_span_used = feedback_span_used;
      summary.feedback_span_used_set = true;
    }

    auto smooth_metric = [&](double raw, double t_end_sec, bool freeze) -> double {
      // Track dt between updates for proper time-constant behavior.
      double dt = args.update_seconds;
      if (std::isfinite(prev_metric_time) && std::isfinite(t_end_sec)) {
        const double d = t_end_sec - prev_metric_time;
        if (std::isfinite(d) && d > 0.0) dt = d;
      }
      prev_metric_time = t_end_sec;

      // Never convert invalid raw values into a "valid" smoothed value.
      if (!std::isfinite(raw)) return raw;

      if (!do_smooth) {
        return raw;
      }
      if (freeze) {
        // Hold the previous smoothed value during artifacts.
        return smoother.has_value() ? smoother.value() : raw;
      }
      return smoother.update(raw, dt);
    };

    auto append_feedback_optional_cols = [&](double metric_raw) {
      if (adapt_mode == AdaptMode::Quantile) {
        out << "," << adapt_ctrl.last_desired_threshold();
      }
      if (do_smooth) {
        out << "," << metric_raw;
      }
    };

    auto append_reward_value_cols = [&](double feedback_raw, double reward_value) {
      if (continuous_feedback) {
        out << "," << feedback_raw << "," << reward_value;
      }
    };

    if (metric.type == NfMetricSpec::Type::Coherence) {
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
      opt.measure = metric.coherence_measure;

      OnlineWelchCoherence eng(rec.channel_names, rec.fs_hz, bands, {{ia, ib}}, opt);

      // Resolve band index once we see a frame.
      int b_idx = -1;

      if (args.export_coherence) {
        const std::string coh_stem = (metric.coherence_measure == CoherenceMeasure::MagnitudeSquared) ? "coherence" : "imcoh";
        out_coh.open(args.outdir + "/" + coh_stem + "_timeseries.csv");
        if (!out_coh) throw std::runtime_error("Failed to write " + coh_stem + "_timeseries.csv");
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
          const bool artifact_state = (do_artifacts && af.baseline_ready && af.bad);

          const double val_raw = fr.coherences[static_cast<size_t>(b_idx)][0];
          const double val = smooth_metric(val_raw, fr.t_end_sec, artifact_hit);

          const NfPhase phase = phase_of(fr.t_end_sec);

          // Optional pacing for interactive offline runs (OSC / UI watchers).
          pacer.wait_until(fr.t_end_sec);

          if (!std::isfinite(val)) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            continue;
          }

          if (!have_threshold) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            if (fr.t_end_sec <= args.baseline_seconds) {
              ++summary.baseline_frames;
              // If artifact gating is enabled, avoid contaminating the baseline estimate.
              if (!artifact_hit) {
                baseline_values.push_back(val);
              }
            } else {
              double q_used = 0.5;
              threshold = initial_threshold_from_baseline(args, baseline_values, val, &q_used);
              have_threshold = true;
              if (!summary.threshold_init_set) {
                summary.threshold_init = threshold;
                summary.threshold_init_set = true;
              }

              if (continuous_feedback && !feedback_span_ready) {
                if (!baseline_values.empty()) {
                  std::vector<double> tmp = baseline_values;
                  const double med = median_inplace(&tmp);
                  const double sc = robust_scale(baseline_values, med);
                  if (std::isfinite(sc) && sc > 0.0) {
                    feedback_span_used = sc;
                  } else {
                    feedback_span_used = 1.0;
                    std::cerr << "Warning: baseline scale was non-finite or <= 0; using feedback_span_used=1.0\n";
                  }
                } else {
                  feedback_span_used = 1.0;
                  std::cerr << "Warning: no baseline samples available; using feedback_span_used=1.0\n";
                }
                feedback_span_ready = true;
                summary.feedback_span_used = feedback_span_used;
                summary.feedback_span_used_set = true;
                osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
                std::cout << "Feedback span used: " << feedback_span_used << " (robust baseline scale)\n";
              }
              std::cout << "Initial threshold set to: " << threshold
                        << " (baseline=" << args.baseline_seconds << "s, q=" << q_used
                        << ", n=" << baseline_values.size() << ")\n";
            }
            const double thr_send = have_threshold ? threshold : 0.0;
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                          have_threshold ? 1 : 0);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            ui_push(fr.t_end_sec, val, threshold, have_threshold, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, /*rr=*/0.0, af);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
            continue;
          }

          if (artifact_hit) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            ++summary.training_frames;
            ++summary.artifact_frames;
            if (phase == NfPhase::Rest) ++summary.rest_frames;
            summary.add_reward_value(0.0);
            reward_hist.push_back(0);
            while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
            const double rr = reward_rate();
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

            adapt_ctrl.prune(fr.t_end_sec);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            append_phase_and_raw(phase, /*raw_reward=*/false);
            out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b
                << "," << coherence_measure_name(metric.coherence_measure);
            append_feedback_optional_cols(val_raw);
            append_reward_value_cols(0.0, 0.0);
            out << "\n";
            ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
            continue;
          }

          if (phase == NfPhase::Rest) {
            // During rest blocks, keep displaying metrics but pause reinforcement and adaptation.
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            ++summary.training_frames;
            ++summary.rest_frames;
            summary.add_reward_value(0.0);
            reward_hist.push_back(0);
            while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
            const double rr = reward_rate();

            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

            adapt_ctrl.prune(fr.t_end_sec);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            append_phase_and_raw(phase, /*raw_reward=*/false);
            out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b
                << "," << coherence_measure_name(metric.coherence_measure);
            append_feedback_optional_cols(val_raw);
            append_reward_value_cols(0.0, 0.0);
            out << "\n";
            ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
            continue;
          }

          ++summary.training_frames;
          summary.add_training_metric(val);

          const double thr_used = threshold;
          const bool raw_reward = thr_hyst.update(val, thr_used);
          const bool shaped_raw = shape_reward(raw_reward, fr.t_end_sec, /*freeze=*/false);
          const bool reward = reward_gate.update(shaped_raw);
          derived_update(fr.t_end_sec, /*reward_on=*/reward, artifact_state, phase);
          if (reward) ++summary.reward_frames;
          if (continuous_feedback && (!feedback_span_ready || !std::isfinite(feedback_span_used) || feedback_span_used <= 0.0)) {
            feedback_span_used = 1.0;
            feedback_span_ready = true;
            summary.feedback_span_used = feedback_span_used;
            summary.feedback_span_used_set = true;
            osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
            std::cerr << "Warning: feedback_span was not initialized; using 1.0\n";
          }

          const double feedback_raw = continuous_feedback
                                        ? feedback_value(val, thr_used, args.reward_direction, feedback_span_used)
                                        : (raw_reward ? 1.0 : 0.0);
          const double reward_value = continuous_feedback ? (reward ? feedback_raw : 0.0) : (reward ? 1.0 : 0.0);
          summary.add_reward_value(reward_value);
          audio_reward_values.push_back(static_cast<float>(reward_value));
          reward_hist.push_back(reward ? 1 : 0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();

          // Collect training samples for quantile adaptation (noop for other modes).
          adapt_ctrl.observe(fr.t_end_sec, val);

          if (!args.no_adaptation && args.adapt_eta > 0.0) {
            threshold = adapt_ctrl.update(thr_used, rr, fr.t_end_sec);
          }

          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_used,
                        (reward ? 1 : 0), rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          if (continuous_feedback) {
            osc_send_feedback_raw(osc, osc_prefix, fr.t_end_sec, feedback_raw);
            osc_send_reward_value(osc, osc_prefix, fr.t_end_sec, reward_value);
          }

          out << fr.t_end_sec << "," << val << "," << thr_used << "," << (reward ? 1 : 0) << "," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          append_phase_and_raw(phase, raw_reward);
          out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b
              << "," << coherence_measure_name(metric.coherence_measure);
          append_feedback_optional_cols(val_raw);
          append_reward_value_cols(feedback_raw, reward_value);
          out << "\n";
          ui_push(fr.t_end_sec, val, thr_used, /*thr_ready=*/true, feedback_raw, reward_value, /*reward=*/(reward ? 1 : 0), rr, af);
        }
      }

      const double wall_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
      write_nf_summary_json(args, rec, metric, summary, threshold, adapt_ctrl, pacer, wall_elapsed);

      finalize_derived_events();
      write_reward_tone_wav_if_requested(args, audio_reward_values);
      write_biotrace_ui_html_if_requested(args, rec, metric, ui_frames, do_artifacts,
                                          want_derived_events ? &derived_events : nullptr);
      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    if (metric.type == NfMetricSpec::Type::Pac) {
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
          const bool artifact_state = (do_artifacts && af.baseline_ready && af.bad);

          const double val_raw = fr.value;
          const double val = smooth_metric(val_raw, fr.t_end_sec, artifact_hit);

          const NfPhase phase = phase_of(fr.t_end_sec);

          pacer.wait_until(fr.t_end_sec);

          if (!std::isfinite(val)) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            continue;
          }

          if (!have_threshold) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            if (fr.t_end_sec <= args.baseline_seconds) {
              ++summary.baseline_frames;
              if (!artifact_hit) {
                baseline_values.push_back(val);
              }
            } else {
              double q_used = 0.5;
              threshold = initial_threshold_from_baseline(args, baseline_values, val, &q_used);
              have_threshold = true;
              if (!summary.threshold_init_set) {
                summary.threshold_init = threshold;
                summary.threshold_init_set = true;
              }
              if (continuous_feedback && !feedback_span_ready) {
                if (!baseline_values.empty()) {
                  std::vector<double> tmp = baseline_values;
                  const double med = median_inplace(&tmp);
                  const double sc = robust_scale(baseline_values, med);
                  if (std::isfinite(sc) && sc > 0.0) {
                    feedback_span_used = sc;
                  } else {
                    feedback_span_used = 1.0;
                    std::cerr << "Warning: baseline scale was non-finite or <= 0; using feedback_span_used=1.0\n";
                  }
                } else {
                  feedback_span_used = 1.0;
                  std::cerr << "Warning: no baseline samples available; using feedback_span_used=1.0\n";
                }
                feedback_span_ready = true;
                summary.feedback_span_used = feedback_span_used;
                summary.feedback_span_used_set = true;
                osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
                std::cout << "Feedback span used: " << feedback_span_used << " (robust baseline scale)\n";
              }
              std::cout << "Initial threshold set to: " << threshold
                        << " (baseline=" << args.baseline_seconds << "s, q=" << q_used
                        << ", n=" << baseline_values.size() << ")\n";
            }
            const double thr_send = have_threshold ? threshold : 0.0;
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                          have_threshold ? 1 : 0);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            ui_push(fr.t_end_sec, val, threshold, have_threshold, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, /*rr=*/0.0, af);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
            continue;
          }

          if (artifact_hit) {
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            ++summary.training_frames;
            ++summary.artifact_frames;
            if (phase == NfPhase::Rest) ++summary.rest_frames;
            summary.add_reward_value(0.0);
            reward_hist.push_back(0);
            while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
            const double rr = reward_rate();
            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

            adapt_ctrl.prune(fr.t_end_sec);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            append_phase_and_raw(phase, /*raw_reward=*/false);
            out << "," << metric.phase_band << "," << metric.amp_band << "," << metric.channel;
            out << "," << (metric.pac_method == PacMethod::ModulationIndex ? "mi" : "mvl");
            append_feedback_optional_cols(val_raw);
            append_reward_value_cols(0.0, 0.0);
            out << "\n";
            ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
            continue;
          }

          if (phase == NfPhase::Rest) {
            // During rest blocks, keep displaying metrics but pause reinforcement and adaptation.
            (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
            reward_gate.reset(false);
            thr_hyst.reset(false);
            ++summary.training_frames;
            ++summary.rest_frames;
            summary.add_reward_value(0.0);
            reward_hist.push_back(0);
            while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
            const double rr = reward_rate();

            osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                          0, rr, 1);
            if (art) osc_send_artifact(osc, osc_prefix, af);
            audio_reward_values.push_back(0);
            derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

            adapt_ctrl.prune(fr.t_end_sec);

            out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
            if (do_artifacts) {
              out << "," << (af.baseline_ready ? 1 : 0)
                  << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                  << "," << af.bad_channel_count;
            }
            append_phase_and_raw(phase, /*raw_reward=*/false);
            out << "," << metric.phase_band << "," << metric.amp_band << "," << metric.channel;
            out << "," << (metric.pac_method == PacMethod::ModulationIndex ? "mi" : "mvl");
            append_feedback_optional_cols(val_raw);
            append_reward_value_cols(0.0, 0.0);
            out << "\n";
            ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
            continue;
          }

          ++summary.training_frames;
          summary.add_training_metric(val);

          const double thr_used = threshold;
          const bool raw_reward = thr_hyst.update(val, thr_used);
          const bool shaped_raw = shape_reward(raw_reward, fr.t_end_sec, /*freeze=*/false);
          const bool reward = reward_gate.update(shaped_raw);
          derived_update(fr.t_end_sec, /*reward_on=*/reward, artifact_state, phase);
          if (reward) ++summary.reward_frames;
          if (continuous_feedback && (!feedback_span_ready || !std::isfinite(feedback_span_used) || feedback_span_used <= 0.0)) {
            feedback_span_used = 1.0;
            feedback_span_ready = true;
            summary.feedback_span_used = feedback_span_used;
            summary.feedback_span_used_set = true;
            osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
            std::cerr << "Warning: feedback_span was not initialized; using 1.0\n";
          }

          const double feedback_raw = continuous_feedback
                                        ? feedback_value(val, thr_used, args.reward_direction, feedback_span_used)
                                        : (raw_reward ? 1.0 : 0.0);
          const double reward_value = continuous_feedback ? (reward ? feedback_raw : 0.0) : (reward ? 1.0 : 0.0);
          summary.add_reward_value(reward_value);
          audio_reward_values.push_back(static_cast<float>(reward_value));
          reward_hist.push_back(reward ? 1 : 0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();

          // Collect training samples for quantile adaptation (noop for other modes).
          adapt_ctrl.observe(fr.t_end_sec, val);

          if (!args.no_adaptation && args.adapt_eta > 0.0) {
            threshold = adapt_ctrl.update(thr_used, rr, fr.t_end_sec);
          }

          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_used,
                        (reward ? 1 : 0), rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          if (continuous_feedback) {
            osc_send_feedback_raw(osc, osc_prefix, fr.t_end_sec, feedback_raw);
            osc_send_reward_value(osc, osc_prefix, fr.t_end_sec, reward_value);
          }

          out << fr.t_end_sec << "," << val << "," << thr_used << "," << (reward ? 1 : 0) << "," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          append_phase_and_raw(phase, raw_reward);
          out << "," << metric.phase_band << "," << metric.amp_band << "," << metric.channel;
          out << "," << (metric.pac_method == PacMethod::ModulationIndex ? "mi" : "mvl");
          append_feedback_optional_cols(val_raw);
          append_reward_value_cols(feedback_raw, reward_value);
          out << "\n";
          ui_push(fr.t_end_sec, val, thr_used, /*thr_ready=*/true, feedback_raw, reward_value, /*reward=*/(reward ? 1 : 0), rr, af);
        }
      }

      const double wall_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
      write_nf_summary_json(args, rec, metric, summary, threshold, adapt_ctrl, pacer, wall_elapsed);

      finalize_derived_events();
      write_reward_tone_wav_if_requested(args, audio_reward_values);
      write_biotrace_ui_html_if_requested(args, rec, metric, ui_frames, do_artifacts,
                                          want_derived_events ? &derived_events : nullptr);
      std::cout << "Done. Outputs written to: " << args.outdir << "\n";
      return 0;
    }

    // Bandpower / ratio modes.
    OnlineBandpowerOptions opt;
    opt.window_seconds = args.window_seconds;
    opt.update_seconds = args.update_seconds;
    opt.welch.nperseg = args.nperseg;
    opt.welch.overlap_fraction = args.overlap;
    opt.relative_power = args.relative_power;
    opt.relative_fmin_hz = args.relative_fmin_hz;
    opt.relative_fmax_hz = args.relative_fmax_hz;
    opt.log10_power = args.log10_power;

    OnlineWelchBandpower eng(rec.channel_names, rec.fs_hz, bands, opt);

    // We'll resolve band/channel indices once the first frame is emitted.
    bool metric_resolved = false;
    int ch_idx = -1;
    int ch_a_idx = -1;
    int ch_b_idx = -1;
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
        if (!metric_resolved) {
          if (metric.type == NfMetricSpec::Type::Band) {
            ch_idx = find_channel_index(fr.channel_names, metric.channel);
            if (ch_idx < 0) throw std::runtime_error("Metric channel not found in recording: " + metric.channel);
            b_idx = find_band_index(fr.bands, metric.band);
            if (b_idx < 0) throw std::runtime_error("Metric band not found: " + metric.band);
          } else if (metric.type == NfMetricSpec::Type::Ratio) {
            ch_idx = find_channel_index(fr.channel_names, metric.channel);
            if (ch_idx < 0) throw std::runtime_error("Metric channel not found in recording: " + metric.channel);
            b_num = find_band_index(fr.bands, metric.band_num);
            b_den = find_band_index(fr.bands, metric.band_den);
            if (b_num < 0) throw std::runtime_error("Metric numerator band not found: " + metric.band_num);
            if (b_den < 0) throw std::runtime_error("Metric denominator band not found: " + metric.band_den);
          } else if (metric.type == NfMetricSpec::Type::Asymmetry) {
            ch_a_idx = find_channel_index(fr.channel_names, metric.channel_a);
            ch_b_idx = find_channel_index(fr.channel_names, metric.channel_b);
            if (ch_a_idx < 0) throw std::runtime_error("Metric channel_a not found in recording: " + metric.channel_a);
            if (ch_b_idx < 0) throw std::runtime_error("Metric channel_b not found in recording: " + metric.channel_b);
            if (ch_a_idx == ch_b_idx) throw std::runtime_error("asymmetry metric requires two different channels");
            b_idx = find_band_index(fr.bands, metric.band);
            if (b_idx < 0) throw std::runtime_error("Metric band not found: " + metric.band);
          } else {
            throw std::runtime_error("Unsupported NF metric type in bandpower engine");
          }
          metric_resolved = true;
        }

        const OnlineArtifactFrame af = take_artifact_frame(art ? &art_queue : nullptr,
                                                          fr.t_end_sec,
                                                          0.5 / rec.fs_hz);
        const bool artifact_hit = (args.artifact_gate && af.baseline_ready && af.bad);
        const bool artifact_state = (do_artifacts && af.baseline_ready && af.bad);

        const double val_raw = compute_metric_band_ratio_or_asym(fr, metric, ch_idx, ch_a_idx, ch_b_idx, b_idx, b_num, b_den);
        const double val = smooth_metric(val_raw, fr.t_end_sec, artifact_hit);

        const NfPhase phase = phase_of(fr.t_end_sec);

        pacer.wait_until(fr.t_end_sec);

        if (!std::isfinite(val)) {
          (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
          reward_gate.reset(false);
          thr_hyst.reset(false);
          derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_values.push_back(0);
          continue;
        }

        if (!have_threshold) {
          (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
          reward_gate.reset(false);
          thr_hyst.reset(false);
          if (fr.t_end_sec <= args.baseline_seconds) {
            ++summary.baseline_frames;
            if (!artifact_hit) {
              baseline_values.push_back(val);
            }
          } else {
            double q_used = 0.5;
            threshold = initial_threshold_from_baseline(args, baseline_values, val, &q_used);
            have_threshold = true;
            if (!summary.threshold_init_set) {
              summary.threshold_init = threshold;
              summary.threshold_init_set = true;
            }
            if (continuous_feedback && !feedback_span_ready) {
              if (!baseline_values.empty()) {
                std::vector<double> tmp = baseline_values;
                const double med = median_inplace(&tmp);
                const double scale = robust_scale(baseline_values, med);
                if (std::isfinite(scale) && scale > 0.0) {
                  feedback_span_used = scale;
                } else {
                  feedback_span_used = 1.0;
                  std::cerr << "Warning: could not estimate baseline scale for continuous feedback; using feedback_span=1.0\n";
                }
              } else {
                feedback_span_used = 1.0;
                std::cerr << "Warning: no baseline samples for continuous feedback; using feedback_span=1.0\n";
              }
              feedback_span_ready = true;
              summary.feedback_span_used = feedback_span_used;
              summary.feedback_span_used_set = true;
              osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
            }
            std::cout << "Initial threshold set to: " << threshold
                      << " (baseline=" << args.baseline_seconds << "s, q=" << q_used
                      << ", n=" << baseline_values.size() << ")\n";
          }
          const double thr_send = have_threshold ? threshold : 0.0;
          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_send, 0, 0.0,
                        have_threshold ? 1 : 0);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_values.push_back(0);
          ui_push(fr.t_end_sec, val, threshold, have_threshold, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, /*rr=*/0.0, af);
          derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);
          continue;
        }

        if (artifact_hit) {
          (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
          reward_gate.reset(false);
          thr_hyst.reset(false);
          ++summary.training_frames;
          ++summary.artifact_frames;
          if (phase == NfPhase::Rest) ++summary.rest_frames;
          summary.add_reward_value(0.0);
          reward_hist.push_back(0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();
          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                        0, rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_values.push_back(0);
          derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

          adapt_ctrl.prune(fr.t_end_sec);

          out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          append_phase_and_raw(phase, /*raw_reward=*/false);
          if (metric.type == NfMetricSpec::Type::Band) {
            out << "," << metric.band << "," << metric.channel;
          } else if (metric.type == NfMetricSpec::Type::Ratio) {
            out << "," << metric.band_num << "," << metric.band_den << "," << metric.channel;
          } else {
            // Asymmetry
            out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b;
          }
          append_feedback_optional_cols(val_raw);
          append_reward_value_cols(0.0, 0.0);
          out << "\n";
          ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
          continue;
        }

        if (phase == NfPhase::Rest) {
          // During rest blocks, keep displaying metrics but pause reinforcement and adaptation.
          (void)shape_reward(false, fr.t_end_sec, /*freeze=*/true);
          reward_gate.reset(false);
          thr_hyst.reset(false);
          ++summary.training_frames;
          ++summary.rest_frames;
          summary.add_reward_value(0.0);
          reward_hist.push_back(0);
          while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
          const double rr = reward_rate();

          osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, threshold,
                        0, rr, 1);
          if (art) osc_send_artifact(osc, osc_prefix, af);
          audio_reward_values.push_back(0);
          derived_update(fr.t_end_sec, /*reward_on=*/false, artifact_state, phase);

          adapt_ctrl.prune(fr.t_end_sec);

          out << fr.t_end_sec << "," << val << "," << threshold << ",0," << rr;
          if (do_artifacts) {
            out << "," << (af.baseline_ready ? 1 : 0)
                << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
                << "," << af.bad_channel_count;
          }
          append_phase_and_raw(phase, /*raw_reward=*/false);
          if (metric.type == NfMetricSpec::Type::Band) {
            out << "," << metric.band << "," << metric.channel;
          } else if (metric.type == NfMetricSpec::Type::Ratio) {
            out << "," << metric.band_num << "," << metric.band_den << "," << metric.channel;
          } else {
            // Asymmetry
            out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b;
          }
          append_feedback_optional_cols(val_raw);
          append_reward_value_cols(0.0, 0.0);
          out << "\n";
          ui_push(fr.t_end_sec, val, threshold, /*thr_ready=*/true, /*feedback_raw=*/0.0, /*reward_value=*/0.0, /*reward=*/0, rr, af);
          continue;
        }

        ++summary.training_frames;
        summary.add_training_metric(val);

        const double thr_used = threshold;
        const bool raw_reward = thr_hyst.update(val, thr_used);
        const bool shaped_raw = shape_reward(raw_reward, fr.t_end_sec, /*freeze=*/false);
        const bool reward = reward_gate.update(shaped_raw);
        derived_update(fr.t_end_sec, /*reward_on=*/reward, artifact_state, phase);
        if (reward) ++summary.reward_frames;
        if (continuous_feedback && (!feedback_span_ready || !std::isfinite(feedback_span_used) ||
                                   feedback_span_used <= 0.0)) {
          feedback_span_used = 1.0;
          feedback_span_ready = true;
          summary.feedback_span_used = feedback_span_used;
          summary.feedback_span_used_set = true;
          osc_send_feedback_span_used(osc, osc_prefix, feedback_span_used);
        }

        const double feedback_raw = continuous_feedback
                                ? feedback_value(val, thr_used, args.reward_direction, feedback_span_used)
                                : (raw_reward ? 1.0 : 0.0);
        const double reward_value = continuous_feedback ? (reward ? feedback_raw : 0.0) : (reward ? 1.0 : 0.0);
        summary.add_reward_value(reward_value);
        audio_reward_values.push_back(static_cast<float>(reward_value));
        reward_hist.push_back(reward ? 1 : 0);
        while (reward_hist.size() > rate_window_frames) reward_hist.pop_front();
        const double rr = reward_rate();

        // Collect training samples for quantile adaptation (noop for other modes).
        adapt_ctrl.observe(fr.t_end_sec, val);

        if (!args.no_adaptation && args.adapt_eta > 0.0) {
          threshold = adapt_ctrl.update(thr_used, rr, fr.t_end_sec);
        }

        osc_send_state(osc, osc_prefix, osc_mode, fr.t_end_sec, val, thr_used,
                      (reward ? 1 : 0), rr, 1);
        if (art) osc_send_artifact(osc, osc_prefix, af);
        if (continuous_feedback) {
          osc_send_feedback_raw(osc, osc_prefix, fr.t_end_sec, feedback_raw);
          osc_send_reward_value(osc, osc_prefix, fr.t_end_sec, reward_value);
        }

        out << fr.t_end_sec << "," << val << "," << thr_used << "," << (reward ? 1 : 0) << "," << rr;
        if (do_artifacts) {
          out << "," << (af.baseline_ready ? 1 : 0)
              << "," << ((af.baseline_ready && af.bad) ? 1 : 0)
              << "," << af.bad_channel_count;
        }
        append_phase_and_raw(phase, raw_reward);
        if (metric.type == NfMetricSpec::Type::Band) {
          out << "," << metric.band << "," << metric.channel;
        } else if (metric.type == NfMetricSpec::Type::Ratio) {
          out << "," << metric.band_num << "," << metric.band_den << "," << metric.channel;
        } else {
          // Asymmetry
          out << "," << metric.band << "," << metric.channel_a << "," << metric.channel_b;
        }
        append_feedback_optional_cols(val_raw);
        append_reward_value_cols(feedback_raw, reward_value);
        out << "\n";

        ui_push(fr.t_end_sec, val, thr_used, /*thr_ready=*/true, feedback_raw, reward_value, /*reward=*/(reward ? 1 : 0), rr, af);

        if (args.export_bandpowers) {
          out_bp << fr.t_end_sec;
          for (size_t b = 0; b < fr.bands.size(); ++b) {
            for (size_t c = 0; c < fr.channel_names.size(); ++c) {
              if (have_qc && c < qc_bad.size() && qc_bad[c]) out_bp << ",nan";
              else out_bp << "," << fr.powers[b][c];
            }
          }
          out_bp << "\n";
        }
      }
    }

    const double wall_elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - wall_start).count();
    write_nf_summary_json(args, rec, metric, summary, threshold, adapt_ctrl, pacer, wall_elapsed);

    finalize_derived_events();
    write_reward_tone_wav_if_requested(args, audio_reward_values);
    write_biotrace_ui_html_if_requested(args, rec, metric, ui_frames, do_artifacts,
                                        want_derived_events ? &derived_events : nullptr);
    std::cout << "Done. Outputs written to: " << args.outdir << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
