#include "qeeg/artifacts.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
  using namespace qeeg;

  // Synthetic 2-channel recording:
  // - baseline: low-amplitude sine + small deterministic noise
  // - inject a large spike artifact into channel 0 around t=5s
  // - inject a flatline dropout into channel 1 around t=7..8s

  EEGRecording rec;
  rec.fs_hz = 250.0;
  rec.channel_names = {"Cz", "Pz"};

  const double seconds = 10.0;
  const size_t n = static_cast<size_t>(std::llround(seconds * rec.fs_hz));
  rec.data.assign(2, std::vector<float>(n, 0.0f));

  const double pi = std::acos(-1.0);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rec.fs_hz;
    const float base = static_cast<float>(0.5 * std::sin(2.0 * pi * 10.0 * t));
    rec.data[0][i] = base;
    rec.data[1][i] = base;
    // Deterministic small "noise" so MAD != 0.
    rec.data[0][i] += static_cast<float>(0.01 * std::sin(2.0 * pi * 3.0 * t));
    rec.data[1][i] += static_cast<float>(0.01 * std::cos(2.0 * pi * 7.0 * t));
  }

  // Inject spike artifact into channel 0.
  const size_t spike_center = static_cast<size_t>(std::llround(5.0 * rec.fs_hz));
  for (size_t k = 0; k < 10; ++k) {
    const size_t idx = spike_center + k;
    if (idx < n) rec.data[0][idx] += 100.0f;
  }

  // Inject flatline dropout into channel 1 from 7s to 8s.
  const size_t flat_start = static_cast<size_t>(std::llround(7.0 * rec.fs_hz));
  const size_t flat_end = static_cast<size_t>(std::llround(8.0 * rec.fs_hz));
  for (size_t i = flat_start; i < flat_end && i < n; ++i) {
    rec.data[1][i] = 0.0f;
  }

  ArtifactDetectionOptions opt;
  opt.window_seconds = 1.0;
  opt.step_seconds = 0.5;
  opt.baseline_seconds = 2.0;

  // High-outlier thresholds (classic spike/noise detection).
  opt.ptp_z = 6.0;
  opt.rms_z = 6.0;
  opt.kurtosis_z = 6.0;

  // Low-outlier thresholds (flatline/dropouts).
  opt.ptp_z_low = 6.0;
  opt.rms_z_low = 6.0;

  opt.min_bad_channels = 1;

  const auto res = detect_artifacts(rec, opt);
  assert(!res.windows.empty());

  // We expect at least one window to be flagged as bad.
  assert(res.total_bad_windows > 0);

  // Ensure the spike artifact is driven by channel 0 around t=5s.
  bool saw_bad_ch0_spike = false;
  for (const auto& w : res.windows) {
    if (!w.bad) continue;
    if (!(w.t_start_sec <= 5.0 && w.t_end_sec >= 5.0)) continue;
    if (w.channels.size() >= 1 && w.channels[0].bad) {
      saw_bad_ch0_spike = true;
      break;
    }
  }
  assert(saw_bad_ch0_spike);

  // Ensure the flatline dropout is detected in channel 1 around t=7.25s.
  bool saw_bad_ch1_flat = false;
  for (const auto& w : res.windows) {
    if (!w.bad) continue;
    if (!(w.t_start_sec <= 7.25 && w.t_end_sec >= 7.25)) continue;
    if (w.channels.size() >= 2 && w.channels[1].bad) {
      saw_bad_ch1_flat = true;
      break;
    }
  }
  assert(saw_bad_ch1_flat);

  // Helpers: per-channel counts and merged bad segments.
  const auto ch_counts = artifact_bad_counts_per_channel(res);
  assert(ch_counts.size() == rec.n_channels());
  assert(ch_counts[0] > 0);
  assert(ch_counts[1] > 0);

  const auto segs = artifact_bad_segments(res);
  assert(!segs.empty());

  bool covers_spike_time = false;
  for (const auto& s : segs) {
    if (s.t_start_sec <= 5.0 && s.t_end_sec >= 5.0) {
      covers_spike_time = true;
      // The injected spike artifact was only in channel 0.
      if (s.bad_windows_per_channel.size() >= 2) {
        assert(s.bad_windows_per_channel[0] > 0);
        // Channel 1 could still be flagged in overlap windows depending on thresholds,
        // but channel 0 should be at least as often flagged for this segment.
        assert(s.bad_windows_per_channel[0] >= s.bad_windows_per_channel[1]);
      }
      break;
    }
  }
  assert(covers_spike_time);

  bool covers_flat_time = false;
  for (const auto& s : segs) {
    if (s.t_start_sec <= 7.25 && s.t_end_sec >= 7.25) {
      covers_flat_time = true;
      if (s.bad_windows_per_channel.size() >= 2) {
        assert(s.bad_windows_per_channel[1] > 0);
      }
      break;
    }
  }
  assert(covers_flat_time);

  std::cout << "Artifact detection test passed. Bad windows: " << res.total_bad_windows
            << ", segments: " << segs.size() << "\n";
  return 0;
}
