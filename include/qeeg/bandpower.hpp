#pragma once

#include "qeeg/types.hpp"

#include <string>
#include <vector>

namespace qeeg {

// Common default EEG bands (delta/theta/alpha/beta/gamma).
std::vector<BandDefinition> default_eeg_bands();

// Parse a band spec string like:
// "delta:0.5-4,theta:4-7,alpha:8-12,beta:13-30,gamma:30-80"
std::vector<BandDefinition> parse_band_spec(const std::string& spec);

// Integrate PSD between [fmin_hz, fmax_hz] using trapezoidal rule.
double integrate_bandpower(const PsdResult& psd, double fmin_hz, double fmax_hz);

// Optional reference stats loader (channel,band,mean,std).
ReferenceStats load_reference_csv(const std::string& path);

// Compute z-score if reference exists; returns (found?).
bool compute_zscore(const ReferenceStats& ref,
                    const std::string& channel,
                    const std::string& band,
                    double value,
                    double* out_z);

} // namespace qeeg
