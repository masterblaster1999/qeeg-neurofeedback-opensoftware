#include "qeeg/channel_qc.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

int main() {
  using namespace qeeg;

  EEGRecording rec;
  rec.fs_hz = 100.0;
  const size_t n_samp = 5000; // 50s

  rec.channel_names = {"Good", "Flat", "Noisy", "Artifact"};
  rec.data.resize(rec.channel_names.size());
  for (auto& ch : rec.data) ch.assign(n_samp, 0.0f);

  // Good: 10 uV sine
  for (size_t i = 0; i < n_samp; ++i) {
    const double t = static_cast<double>(i) / rec.fs_hz;
    rec.data[0][i] = static_cast<float>(10.0 * std::sin(2.0 * 3.141592653589793 * 10.0 * t));
  }

  // Flat: 0
  // (already zeros)

  // Noisy: high-amplitude Gaussian noise
  std::mt19937 rng(123);
  std::normal_distribution<double> N(0.0, 200.0);
  for (size_t i = 0; i < n_samp; ++i) {
    rec.data[2][i] = static_cast<float>(N(rng));
  }

  // Artifact: mostly sine, but with big single-sample spikes after 10 seconds.
  for (size_t i = 0; i < n_samp; ++i) {
    const double t = static_cast<double>(i) / rec.fs_hz;
    rec.data[3][i] = static_cast<float>(10.0 * std::sin(2.0 * 3.141592653589793 * 10.0 * t));
  }
  const size_t baseline_end = static_cast<size_t>(10.0 * rec.fs_hz);
  const size_t spike_stride = static_cast<size_t>(0.5 * rec.fs_hz); // every 0.5s
  for (size_t i = baseline_end; i < n_samp; i += spike_stride) {
    rec.data[3][i] += 1000.0f;
  }

  ChannelQCOptions opt;
  opt.flatline_ptp = 1.0;
  opt.noisy_scale_factor = 10.0;
  opt.artifact_bad_window_fraction = 0.30;
  opt.max_samples_for_robust = 2000; // keep test fast

  // Artifact window scoring tuned for this synthetic data.
  opt.artifact_opt.window_seconds = 1.0;
  opt.artifact_opt.step_seconds = 0.5;
  opt.artifact_opt.baseline_seconds = 10.0;
  opt.artifact_opt.ptp_z = 6.0;
  opt.artifact_opt.rms_z = 6.0;
  opt.artifact_opt.kurtosis_z = 6.0;
  opt.artifact_opt.min_bad_channels = 1;

  const ChannelQCResult qc = evaluate_channel_qc(rec, opt);

  // Expect 3 bad channels.
  assert(qc.bad_indices.size() == 3);

  // Good channel should not be bad.
  assert(!qc.channels[0].bad);

  // Flatline flagged.
  assert(qc.channels[1].bad);
  assert(qc.channels[1].flatline);

  // Noisy flagged.
  assert(qc.channels[2].bad);
  assert(qc.channels[2].noisy);

  // Artifact-heavy flagged.
  assert(qc.channels[3].bad);
  assert(qc.channels[3].artifact_often_bad);

  std::cout << "test_channel_qc OK\n";
  return 0;
}
