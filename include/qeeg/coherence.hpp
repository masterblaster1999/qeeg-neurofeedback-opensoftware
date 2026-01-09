#pragma once

#include "qeeg/types.hpp"
#include "qeeg/welch_psd.hpp"

#include <cstddef>
#include <string>
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

// Some coherence-like measures are derived from the complex-valued coherency:
//   Cohy(f) = Pxy(f) / sqrt(Pxx(f) * Pyy(f))
//
// Here we expose a minimal switch to compute either:
// - Magnitude-squared coherence (MSC): |Pxy|^2 / (Pxx * Pyy)
// - Absolute imaginary part of coherency: |Im(Cohy(f))|
//
// The imaginary part of coherency is sometimes used to suppress spurious
// zero-lag coupling driven by field spread / volume conduction.
enum class CoherenceMeasure {
  MagnitudeSquared,
  ImaginaryCoherencyAbs,
};

// Generic coherence-like spectrum.
//
// - For MagnitudeSquared: values are in [0,1].
// - For ImaginaryCoherencyAbs: values are in [0,1].
struct CoherenceSpectrum {
  std::vector<double> freqs_hz;
  std::vector<double> values;
  CoherenceMeasure measure{CoherenceMeasure::MagnitudeSquared};
};

// Compute a coherence-like spectrum.
CoherenceSpectrum welch_coherence_spectrum(const std::vector<float>& x,
                                          const std::vector<float>& y,
                                          double fs_hz,
                                          const WelchOptions& opt,
                                          CoherenceMeasure measure);

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

// Average a generic coherence-like spectrum over a band.
double average_band_value(const CoherenceSpectrum& spec,
                          double fmin_hz,
                          double fmax_hz);

inline double average_band_value(const CoherenceSpectrum& spec, const BandDefinition& band) {
  return average_band_value(spec, band.fmin_hz, band.fmax_hz);
}

// Parse a measure token used by some CLIs.
// Accepts: "msc" (default), "coh", "imcoh", "absimag".
CoherenceMeasure parse_coherence_measure_token(const std::string& token);

inline std::string coherence_measure_name(CoherenceMeasure m) {
  switch (m) {
    case CoherenceMeasure::MagnitudeSquared: return "msc";
    case CoherenceMeasure::ImaginaryCoherencyAbs: return "imcoh";
    default: return "msc";
  }
}

} // namespace qeeg
