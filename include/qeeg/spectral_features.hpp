#pragma once

#include "qeeg/types.hpp"

namespace qeeg {

// Spectral summary features computed from a one-sided PSD.
//
// Notes:
// - fmin_hz/fmax_hz define the analysis range. Values outside the PSD support
//   are ignored.
// - Functions are best-effort for small numerical issues (e.g. tiny negative PSD
//   bins due to floating point noise).

// Compute the total power within [fmin_hz, fmax_hz] (integral of PSD).
double spectral_total_power(const PsdResult& psd, double fmin_hz, double fmax_hz);

// Compute the (optionally normalized) spectral entropy within [fmin_hz, fmax_hz].
//
// If normalize=true, entropy is divided by log(N) where N is the number of
// frequency intervals with non-zero power, yielding a value in [0,1].
// If the range contains ~0 power, returns 0.
double spectral_entropy(const PsdResult& psd,
                        double fmin_hz,
                        double fmax_hz,
                        bool normalize = true,
                        double eps = 1e-20);

// Power-weighted mean frequency (a.k.a. "spectral centroid") within [fmin_hz, fmax_hz].
// If the range contains ~0 power, returns 0.
double spectral_mean_frequency(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double eps = 1e-20);

// Frequency at which the cumulative power reaches `edge` (e.g. 0.95 for SEF95).
//
// edge must be in (0,1]. If the range contains ~0 power, returns fmin_hz.
double spectral_edge_frequency(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double edge = 0.95,
                               double eps = 1e-20);

// Frequency of the maximum PSD bin within [fmin_hz, fmax_hz].
//
// This is a simple argmax on sampled PSD values, with linear interpolation
// to include the exact fmin/fmax boundaries.
double spectral_peak_frequency(const PsdResult& psd, double fmin_hz, double fmax_hz);

} // namespace qeeg
