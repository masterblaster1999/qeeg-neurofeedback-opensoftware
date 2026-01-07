#pragma once

#include "qeeg/types.hpp"
#include "qeeg/welch_psd.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// Magnitude-squared coherence estimate using a Welch-style method.
//
// Coherence is a frequency-domain measure of linear relationship between two
// signals, typically defined as:
//   Cxy(f) = |Pxy(f)|^2 / (Pxx(f) * Pyy(f))
// where Pxx and Pyy are the auto power spectral densities and Pxy is the
// cross power spectral density.
//
// This implementation mirrors the Welch PSD implementation already in the
// codebase (Hann window, overlap, mean detrend), and returns a one-sided
// coherence spectrum aligned to [0, fs/2]. Values are clamped to [0, 1].
struct CoherenceResult {
  std::vector<double> freqs_hz;      // length = n_freq_bins
  std::vector<double> coherence;     // same length, unitless in [0,1]
};

// Compute magnitude-squared coherence between x and y.
CoherenceResult welch_coherence(const std::vector<float>& x,
                               const std::vector<float>& y,
                               double fs_hz,
                               const WelchOptions& opt);

// Average coherence over a frequency band using trapezoidal integration and
// dividing by the band width.
//
// Returns NaN if the band does not overlap the spectrum.
double average_band_coherence(const CoherenceResult& coh,
                             double fmin_hz,
                             double fmax_hz);

// Convenience overload.
inline double average_band_coherence(const CoherenceResult& coh, const BandDefinition& band) {
  return average_band_coherence(coh, band.fmin_hz, band.fmax_hz);
}

} // namespace qeeg
