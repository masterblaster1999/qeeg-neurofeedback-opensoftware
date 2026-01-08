#pragma once

#include "qeeg/types.hpp"

#include <vector>

namespace qeeg {

// Phase Locking Value (PLV)
//
// PLV is a phase-based connectivity metric defined as the magnitude of the
// mean unit phasor of the instantaneous phase difference between two signals:
//   PLV = | (1/N) * sum_t exp(i * (phi_x(t) - phi_y(t))) |
//
// Returns values in [0, 1] (higher => more consistent phase difference).

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

} // namespace qeeg
