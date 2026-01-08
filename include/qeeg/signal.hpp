#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <complex>
#include <vector>

namespace qeeg {

// Shared signal-processing helpers.
//
// These utilities are intentionally small and dependency-light. They are used
// by multiple higher-level metrics (e.g., PAC, PLV) to avoid duplicating common
// filtering / analytic-signal logic.

// Apply a simple bandpass built from a high-pass + low-pass biquad.
//
// - lo_hz or hi_hz may be 0 to disable that edge.
// - If both are 0, returns x unchanged.
// - If zero_phase is true, uses forward-backward filtering (filtfilt).
std::vector<float> bandpass_filter(const std::vector<float>& x,
                                  double fs_hz,
                                  double lo_hz,
                                  double hi_hz,
                                  bool zero_phase = true,
                                  double q = 0.7071067811865476);

// Convenience overload for a BandDefinition.
inline std::vector<float> bandpass_filter(const std::vector<float>& x,
                                         double fs_hz,
                                         const BandDefinition& band,
                                         bool zero_phase = true,
                                         double q = 0.7071067811865476) {
  return bandpass_filter(x, fs_hz, band.fmin_hz, band.fmax_hz, zero_phase, q);
}

// Compute the analytic signal using an FFT-based Hilbert transform
// construction.
//
// Returns a complex vector with the same length as x.
std::vector<std::complex<double>> analytic_signal_fft(const std::vector<float>& x);

// For windowed metrics, return how many samples to discard at each edge.
// frac is clamped to [0, 0.49].
size_t edge_trim_samples(size_t n, double frac);

} // namespace qeeg
