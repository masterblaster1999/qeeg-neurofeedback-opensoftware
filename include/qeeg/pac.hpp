#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// Phase-Amplitude Coupling (PAC) between a low-frequency phase band and a
// high-frequency amplitude band.
//
// Two common estimators are provided:
// - Modulation Index (MI): normalized KL-divergence of the mean amplitude
//   distribution over phase bins.
// - Mean Vector Length (MVL): magnitude of the mean complex vector
//   amp(t) * exp(i * phase(t)), normalized by sum(amp).
//
// Notes:
// - This is a first-pass, dependency-light implementation intended for
//   research/educational use.
// - PAC estimates are sensitive to filtering choices, window length, and
//   biases (e.g., phase-clustering / non-sinusoidal waveforms).

enum class PacMethod {
  ModulationIndex,
  MeanVectorLength,
};

struct PacOptions {
  PacMethod method{PacMethod::ModulationIndex};

  // Number of phase bins for MI. Typical values: 18 or 20.
  size_t n_phase_bins{18};

  // If true, use forward-backward (zero-phase) filtering for the internal
  // bandpass filters used to extract phase/amplitude.
  //
  // In a true real-time setting, this is not causal. For offline analysis,
  // it reduces phase distortion.
  bool zero_phase{true};

  // Fraction of samples to discard at each edge of the analysis window after
  // filtering / Hilbert transform.
  // This reduces edge artifacts in windowed PAC estimation.
  //
  // Must be in [0, 0.49]. 0.10 => keep the middle 80%.
  double edge_trim_fraction{0.10};
};

struct PacResult {
  // Primary PAC value (MI or MVL depending on options).
  double value{0.0};

  // For MI, we also expose the per-bin mean amplitude distribution (useful for
  // debugging/visualization). Empty for MVL.
  std::vector<double> mean_amp_by_phase_bin;
};

// Compute PAC for a single signal x.
//
// x: input signal (single channel)
// fs_hz: sampling rate
// phase_band: low-frequency band to extract phase from
// amp_band: high-frequency band to extract amplitude from
// opt: estimator options
//
// Returns a PacResult with value (MI or MVL).
PacResult compute_pac(const std::vector<float>& x,
                      double fs_hz,
                      const BandDefinition& phase_band,
                      const BandDefinition& amp_band,
                      const PacOptions& opt = {});

} // namespace qeeg
