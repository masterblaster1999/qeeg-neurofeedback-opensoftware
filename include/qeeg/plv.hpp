#pragma once

#include "qeeg/types.hpp"

#include <vector>

namespace qeeg {

// Phase-based connectivity measures
//
// This module currently provides:
//   - PLV: Phase Locking Value
//   - PLI: Phase Lag Index
//   - wPLI: Weighted Phase Lag Index
//   - wPLI2 (debiased): Debiased estimator of squared wPLI
//
// All measures are computed from a narrow-band analytic signal per channel:
//   bandpass -> Hilbert (FFT-based) -> complex analytic signal z(t)
//
// Notes:
// - PLV is sensitive to zero-lag coupling (which can be inflated by field spread / volume conduction).
// - PLI/wPLI are based on the sign / magnitude of the *imaginary* component of the cross-spectrum,
//   which suppresses purely zero-lag interactions.

struct PlvOptions {
  // If true, use forward-backward (zero-phase) filtering for the internal
  // bandpass filter used to extract phases.
  bool zero_phase{true};

  // Fraction of samples to discard at each edge of the analysis window after
  // filtering / Hilbert transform.
  //
  // Must be in [0, 0.49]. 0.10 => keep the middle 80%.
  double edge_trim_fraction{0.10};
};

// Compute PLV between two single-channel signals.
double compute_plv(const std::vector<float>& x,
                   const std::vector<float>& y,
                   double fs_hz,
                   const BandDefinition& band,
                   const PlvOptions& opt = {});

// Compute a symmetric PLV matrix for a multi-channel recording.
//
// channels: vector of channel time series; channels[c][t]
// Returns an NxN matrix in row-major nested vectors.
std::vector<std::vector<double>> compute_plv_matrix(const std::vector<std::vector<float>>& channels,
                                                    double fs_hz,
                                                    const BandDefinition& band,
                                                    const PlvOptions& opt = {});

// Phase Lag Index (PLI)
//
// PLI measures the consistency of the *sign* of the imaginary component of the
// analytic cross-product:
//   PLI = | mean_t sign( Im( z_x(t) * conj(z_y(t)) ) ) |
//
// Returns values in [0, 1]. 0 means symmetric lead/lag (or purely zero-lag);
// 1 means a perfectly consistent non-zero phase lead/lag.
double compute_pli(const std::vector<float>& x,
                   const std::vector<float>& y,
                   double fs_hz,
                   const BandDefinition& band,
                   const PlvOptions& opt = {});

// Weighted Phase Lag Index (wPLI)
//
// wPLI weights each sample by the magnitude of the imaginary component, which
// can improve robustness to noise relative to PLI:
//   wPLI = | sum_t Im( z_x(t) * conj(z_y(t)) ) | / sum_t | Im( z_x(t) * conj(z_y(t)) ) |
//
// Returns values in [0, 1]. If the denominator is ~0 (e.g., purely zero-lag),
// the function returns 0.
double compute_wpli(const std::vector<float>& x,
                    const std::vector<float>& y,
                    double fs_hz,
                    const BandDefinition& band,
                    const PlvOptions& opt = {});

// Debiased estimator of **squared** wPLI.
//
// This implements the common debiasing described by Vinck et al. (2011) and
// used in toolboxes like FieldTrip ("wpli_debiased") and MNE
// ("wpli2_debiased").
//
// It estimates wPLI^2 and can be more stable across small sample sizes.
//
// Notes:
// - The raw estimator can yield small negative values due to the bias
//   correction; this implementation clamps to [0, 1] for convenience.
// - If the denominator is ~0 (e.g., purely zero-lag), the function returns 0.
double compute_wpli2_debiased(const std::vector<float>& x,
                              const std::vector<float>& y,
                              double fs_hz,
                              const BandDefinition& band,
                              const PlvOptions& opt = {});

// Compute symmetric PLI / wPLI matrices for a multi-channel recording.
//
// The diagonal is set to 0 (self-coupling is not meaningful for these metrics).
std::vector<std::vector<double>> compute_pli_matrix(const std::vector<std::vector<float>>& channels,
                                                    double fs_hz,
                                                    const BandDefinition& band,
                                                    const PlvOptions& opt = {});

std::vector<std::vector<double>> compute_wpli_matrix(const std::vector<std::vector<float>>& channels,
                                                     double fs_hz,
                                                     const BandDefinition& band,
                                                     const PlvOptions& opt = {});

// Compute a symmetric matrix of debiased squared wPLI.
//
// The diagonal is set to 0 (self-coupling is not meaningful for this metric).
std::vector<std::vector<double>> compute_wpli2_debiased_matrix(const std::vector<std::vector<float>>& channels,
                                                               double fs_hz,
                                                               const BandDefinition& band,
                                                               const PlvOptions& opt = {});

} // namespace qeeg
