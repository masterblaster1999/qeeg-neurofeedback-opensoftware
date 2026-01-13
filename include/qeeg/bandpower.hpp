#pragma once

#include "qeeg/types.hpp"

#include <string>
#include <vector>

namespace qeeg {

// Common default EEG bands (delta/theta/alpha/beta/gamma).
std::vector<BandDefinition> default_eeg_bands();

// Parse a band spec string like:
//   "delta:0.5-4,theta:4-7,alpha:8-12,beta:13-30,gamma:30-80"
//
// Convenience: if the string starts with '@', the remainder is treated as a
// path to a text file containing a band spec (one per line or comma-separated).
// Example:
//   parse_band_spec("@out_iaf/iaf_band_spec.txt")
//
// Convenience: IAF-relative band specs (based on qeeg_iaf_cli outputs):
//   parse_band_spec("iaf=10.2")        // explicit IAF Hz
//   parse_band_spec("iaf:out_iaf")     // reads out_iaf/iaf_band_spec.txt or out_iaf/iaf_summary.txt
std::vector<BandDefinition> parse_band_spec(const std::string& spec);

// Integrate PSD between [fmin_hz, fmax_hz] using trapezoidal rule.
double integrate_bandpower(const PsdResult& psd, double fmin_hz, double fmax_hz);

// Convenience: compute relative bandpower
//
//   relative = bandpower([band_fmin_hz, band_fmax_hz]) / total_power([total_fmin_hz, total_fmax_hz])
//
// This is commonly used as a simple normalization (dimensionless fraction).
// If total power is ~0, the function returns 0.
double compute_relative_bandpower(const PsdResult& psd,
                                 double band_fmin_hz,
                                 double band_fmax_hz,
                                 double total_fmin_hz,
                                 double total_fmax_hz,
                                 double eps = 1e-20);

// Optional reference stats loader (channel,band,mean,std).
//
// Notes:
// - Comment lines starting with "#" or "//" are ignored.
// - If the file contains comment metadata in the form "# key=value" (as written
//   by qeeg_reference_cli), select values may be parsed into ReferenceStats.
ReferenceStats load_reference_csv(const std::string& path);

// Compute z-score if reference exists; returns (found?).
bool compute_zscore(const ReferenceStats& ref,
                    const std::string& channel,
                    const std::string& band,
                    double value,
                    double* out_z);

} // namespace qeeg
