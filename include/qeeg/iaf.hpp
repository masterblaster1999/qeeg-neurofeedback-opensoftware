#pragma once

#include "qeeg/types.hpp"
#include "qeeg/welch_psd.hpp"

#include <limits>
#include <string>
#include <vector>

namespace qeeg {

// Individual Alpha Frequency (IAF) / alpha peak detection utilities.
//
// This is a *first-pass* estimator intended for research / educational use.
// It operates on a Welch PSD and searches for a dominant peak within an
// alpha range (default 7–13 Hz).
//
// Implementation notes:
// - Works on a dB PSD (10*log10(power)).
// - Optionally removes a 1/f trend by fitting a line in log(freq) space
//   outside the alpha search band and subtracting it.
// - Smooths the spectrum in frequency with a moving-average kernel.
// - Picks the max bin in the search band and refines it with a quadratic
//   interpolation (parabolic peak) when possible.

struct IafOptions {
  // Search range for alpha peak.
  double alpha_min_hz{7.0};
  double alpha_max_hz{13.0};

  // Remove a 1/f trend from the dB spectrum before peak search.
  bool detrend_1_f{true};
  double detrend_min_hz{2.0};
  double detrend_max_hz{40.0};

  // Frequency-domain smoothing width (Hz). 0 disables smoothing.
  double smooth_hz{1.0};

  // Minimum peak prominence (dB) relative to the median within the alpha
  // search range (after optional detrend + smoothing). <=0 disables.
  double min_prominence_db{0.5};

  // If true, require that the selected bin is a local maximum vs neighbors.
  bool require_local_max{true};
};

struct IafEstimate {
  bool found{false};
  double iaf_hz{std::numeric_limits<double>::quiet_NaN()};

  // Optional: Alpha-band center of gravity (CoG) estimate within the alpha
  // search range.
  //
  // This is a complementary estimator to peak alpha frequency (PAF). CoG is
  // commonly defined as a power-weighted mean frequency within the alpha band.
  //
  // In this first-pass implementation, CoG is computed from the same spectrum
  // used for peak detection (after optional detrend + smoothing) by converting
  // the dB spectrum back to linear units and weighting only the *above-median*
  // portion within the alpha band. If no above-median mass is present, this
  // value will remain NaN.
  double cog_hz{std::numeric_limits<double>::quiet_NaN()};

  // Value at the detected peak (units: dB if detrend_1_f=false; otherwise the
  // detrended dB residual).
  double peak_value_db{std::numeric_limits<double>::quiet_NaN()};

  // Peak - median(alpha band) (same units as peak_value_db).
  double prominence_db{std::numeric_limits<double>::quiet_NaN()};

  int peak_bin{-1};
};

// Estimate IAF from a pre-computed PSD.
IafEstimate estimate_iaf(const PsdResult& psd, const IafOptions& opt = IafOptions{});

// Convenience: compute PSD via Welch and estimate IAF.
IafEstimate estimate_iaf_from_signal(const std::vector<float>& x,
                                    double fs_hz,
                                    const WelchOptions& wopt,
                                    const IafOptions& opt = IafOptions{});

// A common individualized band scheme based on IAF.
//
// The defaults follow a simple relative layout:
//   delta: [delta_min, iaf-6]
//   theta: [iaf-6, iaf-2]
//   alpha: [iaf-2, iaf+2]
//   beta : [iaf+2, beta_max]
//   gamma: [beta_max, gamma_max]
//
// This is meant as a helper to generate a band spec string you can pass to
// CLIs that accept `--bands`.
struct IndividualizedBandsOptions {
  double delta_min_hz{0.5};
  double beta_max_hz{30.0};
  double gamma_max_hz{80.0};
  double delta_theta_split_below_iaf{6.0}; // iaf - 6
  double theta_alpha_split_below_iaf{2.0}; // iaf - 2
  double alpha_beta_split_above_iaf{2.0};  // iaf + 2
};

std::vector<BandDefinition> individualized_bands_from_iaf(
    double iaf_hz, const IndividualizedBandsOptions& opt = IndividualizedBandsOptions{});

// Convert a band list to a parseable spec string: "name:min-max,...".
std::string bands_to_spec_string(const std::vector<BandDefinition>& bands);

} // namespace qeeg
