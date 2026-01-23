#pragma once

#include "qeeg/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include <string>
#include <vector>

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

// Spectral bandwidth (standard deviation of frequency) within [fmin_hz, fmax_hz].
//
// This is computed as sqrt(E[f^2] - (E[f])^2), where expectations are power-weighted.
// If the range contains ~0 power, returns 0.
double spectral_bandwidth(const PsdResult& psd,
                          double fmin_hz,
                          double fmax_hz,
                          double eps = 1e-20);

// Spectral skewness (3rd standardized central moment of frequency) within [fmin_hz, fmax_hz].
//
// This treats frequency as a random variable with probability density proportional to the PSD
// (i.e. power-weighted). The returned value is dimensionless.
// If the range contains ~0 power, or the bandwidth is ~0, returns 0.
double spectral_skewness(const PsdResult& psd,
                         double fmin_hz,
                         double fmax_hz,
                         double eps = 1e-20);

// Spectral excess kurtosis (4th standardized central moment minus 3) of frequency within
// [fmin_hz, fmax_hz].
//
// Like skewness, this is computed on the power-weighted frequency distribution.
// If the range contains ~0 power, or the bandwidth is ~0, returns 0.
double spectral_kurtosis_excess(const PsdResult& psd,
                               double fmin_hz,
                               double fmax_hz,
                               double eps = 1e-20);

// Spectral flatness within [fmin_hz, fmax_hz].
//
// Defined as geometric_mean(PSD) / arithmetic_mean(PSD), yielding values in (0,1] for
// non-negative PSD. A value near 1 indicates a flatter spectrum; values near 0 indicate
// a peaky spectrum.
// If the range contains ~0 power, returns 0.
double spectral_flatness(const PsdResult& psd,
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

// Peak frequency refined by quadratic (parabolic) interpolation around the argmax bin.
//
// This is a lightweight sub-bin refinement commonly used for peak frequency estimation.
// The refinement is only applied if the peak falls on a sampled PSD bin that has
// valid neighbors on both sides within the analysis range. Otherwise, this falls
// back to spectral_peak_frequency().
//
// If log_domain=true, the parabola is fit in log10(PSD) space (recommended for PSD).
double spectral_peak_frequency_parabolic(const PsdResult& psd,
                                         double fmin_hz,
                                         double fmax_hz,
                                         bool log_domain = true,
                                         double eps = 1e-20);

// PSD value at a frequency expressed as power in decibels (10*log10(PSD)).
//
// Returns NaN if PSD(freq_hz) is non-positive or inputs are invalid.
double spectral_value_db(const PsdResult& psd, double freq_hz, double eps = 1e-20);

// Full-width at half-maximum (FWHM) around a peak frequency, in Hz.
//
// This finds the nearest frequencies to the left and right of peak_freq_hz where
// the PSD drops to half the peak value (in linear power), using piecewise-linear
// interpolation between PSD sample points. If a half-maximum crossing cannot be
// found on either side within [fmin_hz,fmax_hz], returns NaN.
double spectral_peak_fwhm_hz(const PsdResult& psd,
                             double peak_freq_hz,
                             double fmin_hz,
                             double fmax_hz,
                             double eps = 1e-20);

// Log-log (log10) linear fit of PSD within [fmin_hz, fmax_hz].
//
// This is a lightweight "aperiodic" summary (a simple 1/f^k fit) that can be
// useful when tracking broad spectral changes (e.g. arousal) across channels.
//
// The model is:
//   y = intercept + slope * x
// where:
//   x = log10(frequency_hz)
//   y = log10(PSD)
//
// For a power law PSD ~= A / f^k, the fitted slope ~= -k and the intercept is
// log10(A) (approximately the log-power at 1 Hz).
//
// If robust=true, a small number of Huber IRLS iterations are used to reduce
// the influence of narrowband peaks.
// Simple frequency interval in Hz (inclusive) used by some fitting utilities.
struct FrequencyRange {
  double fmin_hz{0.0};
  double fmax_hz{0.0};
};

struct SpectralLogLogFit {
  double slope{std::numeric_limits<double>::quiet_NaN()};
  double intercept{std::numeric_limits<double>::quiet_NaN()};
  double r2{std::numeric_limits<double>::quiet_NaN()};
  // Root-mean-square error in the log10(PSD) domain.
  //
  // This is computed over the points used in the fit, weighted by the final
  // IRLS weights (Huber) when robust=true.
  double rmse{std::numeric_limits<double>::quiet_NaN()};
  // Unweighted RMSE in the log10(PSD) domain (weights=1).
  //
  // This is useful for comparing fit quality across different models in a
  // consistent way even when robust fitting uses IRLS weights internally.
  double rmse_unweighted{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_points{0};
};

// Piecewise (two-slope) log-log fit of PSD with a single breakpoint ("knee").
//
// This fits a continuous, piecewise-linear model in log10-log10 space:
//
//   y = a + slope_low  * (x - x0)   for x <= x0
//   y = a + slope_high * (x - x0)   for x >= x0
//
// where x = log10(frequency_hz), y = log10(PSD), and x0 is the knee location.
// This provides a simple approximation to an aperiodic "knee" model, allowing
// different 1/f slopes at low vs high frequencies.
//
// intercept_low and intercept_high are reported as the per-segment intercepts at
// x=0 (i.e., predicted log10(PSD) at 1 Hz if extrapolated from that segment).
struct SpectralLogLogTwoSlopeFit {
  bool found{false};
  double knee_hz{std::numeric_limits<double>::quiet_NaN()};
  double slope_low{std::numeric_limits<double>::quiet_NaN()};
  double slope_high{std::numeric_limits<double>::quiet_NaN()};
  double intercept_low{std::numeric_limits<double>::quiet_NaN()};
  double intercept_high{std::numeric_limits<double>::quiet_NaN()};
  double r2{std::numeric_limits<double>::quiet_NaN()};
  // Root-mean-square error in the log10(PSD) domain.
  double rmse{std::numeric_limits<double>::quiet_NaN()};
  // Unweighted RMSE in the log10(PSD) domain (weights=1).
  //
  // This is useful for comparing fit quality across different models in a
  // consistent way even when robust fitting uses IRLS weights internally.
  double rmse_unweighted{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_points{0};
};

// Aperiodic knee fit in the style of a curved 1/f model (semi-log PSD model).
//
// Model (in log10-power units):
//   log10(P(f)) = offset - log10(knee + f^exponent)
//
// When knee == 0, this reduces to a standard 1/f^exponent model:
//   log10(P(f)) = offset - exponent * log10(f)
//
// Note: The knee parameter has units of f^exponent. For interpretability, the
// knee can be converted to an approximate knee frequency:
//   knee_freq_hz = knee^(1/exponent)  (when exponent>0 and knee>0)
struct SpectralAperiodicKneeFit {
  bool found{false};
  double offset{std::numeric_limits<double>::quiet_NaN()};
  double knee{std::numeric_limits<double>::quiet_NaN()};
  double knee_freq_hz{std::numeric_limits<double>::quiet_NaN()};
  double exponent{std::numeric_limits<double>::quiet_NaN()};
  double r2{std::numeric_limits<double>::quiet_NaN()};
  double rmse{std::numeric_limits<double>::quiet_NaN()};
  // Unweighted RMSE in the log10(PSD) domain (weights=1).
  //
  // This is useful for comparing fit quality across different models in a
  // consistent way even when robust fitting uses IRLS weights internally.
  double rmse_unweighted{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_points{0};
};

// Fit the curved aperiodic model with an optional knee.
// If robust is true, a small Huber IRLS loop is applied to reduce the influence
// of narrowband peaks.
SpectralAperiodicKneeFit spectral_aperiodic_knee_fit(const PsdResult& psd,
                                                    double fmin_hz,
                                                    double fmax_hz,
                                                    const std::vector<FrequencyRange>& exclude_ranges_hz,
                                                    bool robust = true,
                                                    int max_iter = 8,
                                                    double eps = 1e-20);

SpectralAperiodicKneeFit spectral_aperiodic_knee_fit(const PsdResult& psd,
                                                    double fmin_hz,
                                                    double fmax_hz,
                                                    bool robust = true,
                                                    int max_iter = 8,
                                                    double eps = 1e-20);

SpectralLogLogFit spectral_loglog_fit(const PsdResult& psd,
                                     double fmin_hz,
                                     double fmax_hz,
                                     bool robust = true,
                                     int max_iter = 8,
                                     double eps = 1e-20);

// Same as spectral_loglog_fit() but excludes one or more frequency ranges (in Hz)
// from the fit. Exclusions are applied in linear frequency space and treated
// as inclusive [fmin_hz, fmax_hz].
//
// This is useful to ignore known narrowband components (e.g., alpha peaks
// or line noise) when estimating the 1/f background.
SpectralLogLogFit spectral_loglog_fit(const PsdResult& psd,
                                     double fmin_hz,
                                     double fmax_hz,
                                     const std::vector<FrequencyRange>& exclude_ranges_hz,
                                     bool robust = true,
                                     int max_iter = 8,
                                     double eps = 1e-20);

// Two-slope log-log fit with an estimated knee frequency.
//
// - min_points_per_side enforces that at least this many points fall on each side
//   of the knee, to avoid degenerate fits.
// - When robust=true, a Huber IRLS loop is run on the selected knee to reduce
//   the influence of narrowband peaks.
SpectralLogLogTwoSlopeFit spectral_loglog_two_slope_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       bool robust = true,
                                                       int max_iter = 8,
                                                       std::size_t min_points_per_side = 6,
                                                       double eps = 1e-20);

// Same as spectral_loglog_two_slope_fit() but excludes one or more frequency ranges
// (in Hz) from the fit (inclusive ranges in linear frequency space).
SpectralLogLogTwoSlopeFit spectral_loglog_two_slope_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       const std::vector<FrequencyRange>& exclude_ranges_hz,
                                                       bool robust = true,
                                                       int max_iter = 8,
                                                       std::size_t min_points_per_side = 6,
                                                       double eps = 1e-20);

// Bandpower helpers.
//
// These are convenience wrappers around spectral_total_power() that are commonly
// used for neurofeedback/QEEG summaries.

// Bandpower within [band_lo_hz, band_hi_hz] (integral of PSD).
//
// This is equivalent to spectral_total_power(psd, band_lo_hz, band_hi_hz).
inline double spectral_band_power(const PsdResult& psd, double band_lo_hz, double band_hi_hz) {
  return spectral_total_power(psd, band_lo_hz, band_hi_hz);
}

// Relative bandpower: bandpower divided by total power in [total_lo_hz, total_hi_hz].
//
// The band range is intersected with the total range before integration. If the
// total power is ~0, returns 0.
inline double spectral_relative_band_power(const PsdResult& psd,
                                          double band_lo_hz,
                                          double band_hi_hz,
                                          double total_lo_hz,
                                          double total_hi_hz,
                                          double eps = 1e-20) {
  if (!(eps > 0.0) || !std::isfinite(eps)) eps = 1e-20;
  const double lo = std::max(band_lo_hz, total_lo_hz);
  const double hi = std::min(band_hi_hz, total_hi_hz);
  if (!(hi > lo)) return 0.0;
  const double band = spectral_total_power(psd, lo, hi);
  const double total = spectral_total_power(psd, total_lo_hz, total_hi_hz);
  if (!(total > eps) || !std::isfinite(total) || !std::isfinite(band)) return 0.0;
  return band / total;
}


// Interpolate the PSD at an arbitrary frequency (linear interpolation).
//
// - If freq_hz is outside the PSD support, this returns the nearest endpoint value.
// - Non-finite or negative PSD values are clamped to 0.
double spectral_psd_at_frequency(const PsdResult& psd, double freq_hz);

// Prominence (in dB) of the PSD at `freq_hz` relative to a log-log aperiodic fit.
//
// This computes:
//   10 * ( log10(PSD(freq_hz)) - (fit.intercept + fit.slope*log10(freq_hz)) )
//
// Positive values indicate power above the aperiodic (1/f-like) background.
// Returns NaN if inputs are invalid.
double spectral_prominence_db_from_loglog_fit(const PsdResult& psd,
                                             double freq_hz,
                                             const SpectralLogLogFit& fit,
                                             double eps = 1e-20);

// Periodic (oscillatory) power above an aperiodic background.
//
// Given a log-log aperiodic fit, this integrates the residual power above the
// fitted background within [fmin_hz,fmax_hz]:
//   ∫ max(0, PSD(f) - PSD_aperiodic(f)) df
//
// where PSD_aperiodic(f) = 10^(fit.intercept + fit.slope * log10(f)).
//
// If positive_only=false, integrates the signed residual (PSD - background).
// Returns NaN if inputs are invalid.
double spectral_periodic_power_from_loglog_fit(const PsdResult& psd,
                                              double fmin_hz,
                                              double fmax_hz,
                                              const SpectralLogLogFit& fit,
                                              bool positive_only = true,
                                              double eps = 1e-20);

// Periodic power as a fraction of total power in [fmin_hz,fmax_hz].
//
// Returns 0 if total power is ~0. Returns NaN if inputs are invalid.
double spectral_periodic_power_fraction_from_loglog_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       const SpectralLogLogFit& fit,
                                                       bool positive_only = true,
                                                       double eps = 1e-20);

// Spectral edge frequency computed on the periodic (aperiodic-adjusted) component.
//
// This computes a spectral edge frequency (e.g., "periodic SEF95") on the
// periodic residual power above the provided aperiodic log-log fit:
//
//   periodic_psd(f) = max(0, PSD(f) - PSD_aperiodic(f))
//
// where PSD_aperiodic(f) = 10^(fit.intercept + fit.slope*log10(f)).
//
// The returned value is the frequency f_edge such that the cumulative periodic
// power in [fmin_hz,f_edge] equals edge * total_periodic_power.
//
// Returns NaN if the periodic power in the range is ~0, or if inputs are invalid.
double spectral_periodic_edge_frequency_from_loglog_fit(const PsdResult& psd,
                                                       double fmin_hz,
                                                       double fmax_hz,
                                                       const SpectralLogLogFit& fit,
                                                       double edge = 0.95,
                                                       double eps = 1e-20);

// Most-prominent (aperiodic-adjusted) peak within a range.
//
// This searches for the frequency bin in [fmin_hz,fmax_hz] whose PSD has the
// largest positive prominence (in dB) above the provided log-log aperiodic fit.
// By default, the peak is required to be a local maximum in prominence.
//
// This is often more informative than the raw PSD argmax when spectra have
// a strong 1/f background (raw argmax tends to occur at low frequencies).
//
// If no peak satisfies the criteria (e.g., no positive prominence), found=false
// and numeric fields are NaN.


// Evaluate the fitted aperiodic background model at a frequency.
//
// Returns log10(PSD_aperiodic(freq_hz)) for the given model.
// Returns NaN if inputs are invalid.
//
// These helpers are useful for computing prominence / periodic residual metrics
// relative to different aperiodic background models.
double spectral_aperiodic_log10_psd_from_loglog_fit(const SpectralLogLogFit& fit,
                                                    double freq_hz,
                                                    double eps = 1e-20);

double spectral_aperiodic_log10_psd_from_two_slope_fit(const SpectralLogLogTwoSlopeFit& fit,
                                                       double freq_hz,
                                                       double eps = 1e-20);

double spectral_aperiodic_log10_psd_from_knee_fit(const SpectralAperiodicKneeFit& fit,
                                                  double freq_hz,
                                                  double eps = 1e-20);

// Prominence (in dB) of the PSD at `freq_hz` relative to a two-slope aperiodic fit.
double spectral_prominence_db_from_two_slope_fit(const PsdResult& psd,
                                                 double freq_hz,
                                                 const SpectralLogLogTwoSlopeFit& fit,
                                                 double eps = 1e-20);

// Prominence (in dB) of the PSD at `freq_hz` relative to a curved aperiodic knee model fit.
double spectral_prominence_db_from_knee_fit(const PsdResult& psd,
                                            double freq_hz,
                                            const SpectralAperiodicKneeFit& fit,
                                            double eps = 1e-20);

// Periodic (oscillatory) power above an aperiodic two-slope background.
double spectral_periodic_power_from_two_slope_fit(const PsdResult& psd,
                                                  double fmin_hz,
                                                  double fmax_hz,
                                                  const SpectralLogLogTwoSlopeFit& fit,
                                                  bool positive_only = true,
                                                  double eps = 1e-20);

double spectral_periodic_power_fraction_from_two_slope_fit(const PsdResult& psd,
                                                           double fmin_hz,
                                                           double fmax_hz,
                                                           const SpectralLogLogTwoSlopeFit& fit,
                                                           bool positive_only = true,
                                                           double eps = 1e-20);

double spectral_periodic_edge_frequency_from_two_slope_fit(const PsdResult& psd,
                                                           double fmin_hz,
                                                           double fmax_hz,
                                                           const SpectralLogLogTwoSlopeFit& fit,
                                                           double edge = 0.95,
                                                           double eps = 1e-20);

// Periodic (oscillatory) power above a curved aperiodic knee model background.
double spectral_periodic_power_from_knee_fit(const PsdResult& psd,
                                             double fmin_hz,
                                             double fmax_hz,
                                             const SpectralAperiodicKneeFit& fit,
                                             bool positive_only = true,
                                             double eps = 1e-20);

double spectral_periodic_power_fraction_from_knee_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      const SpectralAperiodicKneeFit& fit,
                                                      bool positive_only = true,
                                                      double eps = 1e-20);

double spectral_periodic_edge_frequency_from_knee_fit(const PsdResult& psd,
                                                      double fmin_hz,
                                                      double fmax_hz,
                                                      const SpectralAperiodicKneeFit& fit,
                                                      double edge = 0.95,
                                                      double eps = 1e-20);

struct SpectralProminentPeak {
  bool found{false};
  std::size_t peak_bin{0};
  double peak_hz{std::numeric_limits<double>::quiet_NaN()};
  double peak_hz_refined{std::numeric_limits<double>::quiet_NaN()};
  double prominence_db{std::numeric_limits<double>::quiet_NaN()};
};

// Returns the most prominent peak (largest prominence in dB) relative to a
// provided log-log aperiodic fit.
//
// - require_local_max: if true, only considers peaks that are local maxima in
//   the prominence curve.
// - min_prominence_db: minimum required prominence in dB (strictly greater).
//   If the best peak does not exceed this threshold, found=false.
// - eps: numerical floor for PSD when computing log10.
SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralLogLogFit& fit,
                                                   bool require_local_max = true,
                                                   double min_prominence_db = 0.0,
                                                   double eps = 1e-20);

// Overloads for other aperiodic background models.
SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralLogLogTwoSlopeFit& fit,
                                                   bool require_local_max = true,
                                                   double min_prominence_db = 0.0,
                                                   double eps = 1e-20);

SpectralProminentPeak spectral_max_prominence_peak(const PsdResult& psd,
                                                   double fmin_hz,
                                                   double fmax_hz,
                                                   const SpectralAperiodicKneeFit& fit,
                                                   bool require_local_max = true,
                                                   double min_prominence_db = 0.0,
                                                   double eps = 1e-20);

// Convenience helpers.
inline double spectral_loglog_slope(const PsdResult& psd,
                                   double fmin_hz,
                                   double fmax_hz,
                                   bool robust = true,
                                   int max_iter = 8,
                                   double eps = 1e-20) {
  return spectral_loglog_fit(psd, fmin_hz, fmax_hz, robust, max_iter, eps).slope;
}

// Aperiodic exponent k in PSD ~= A / f^k.
inline double spectral_aperiodic_exponent(const PsdResult& psd,
                                         double fmin_hz,
                                         double fmax_hz,
                                         bool robust = true,
                                         int max_iter = 8,
                                         double eps = 1e-20) {
  const double s = spectral_loglog_slope(psd, fmin_hz, fmax_hz, robust, max_iter, eps);
  if (!std::isfinite(s)) return std::numeric_limits<double>::quiet_NaN();
  return -s;
}

} // namespace qeeg
