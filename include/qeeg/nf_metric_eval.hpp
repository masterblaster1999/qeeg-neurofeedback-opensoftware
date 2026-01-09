#pragma once

#include "qeeg/nf_metric.hpp"
#include "qeeg/online_bandpower.hpp"

#include <cstddef>
#include <cmath>
#include <stdexcept>

namespace qeeg {

// Evaluate a bandpower or ratio NfMetricSpec for a single OnlineBandpowerFrame.
//
// This helper exists so both qeeg_nf_cli and unit tests share the same semantics.
//
// Semantics:
// - Band metric: returns the selected band value.
// - Ratio metric:
//   - If the frame is *not* log10-transformed, returns (num + eps) / (den + eps).
//   - If the frame *is* log10-transformed (log10_power=true), returns (log10(num) - log10(den)),
//     i.e. log10(num/den). This avoids the nonsensical ratio-of-logs behavior.
inline double nf_eval_metric_band_or_ratio(const OnlineBandpowerFrame& fr,
                                           const NfMetricSpec& spec,
                                           size_t channel_index,
                                           size_t band_index,
                                           size_t band_num_index,
                                           size_t band_den_index) {
  if (channel_index >= fr.channel_names.size()) {
    throw std::runtime_error("nf_eval_metric_band_or_ratio: channel_index out of range");
  }
  if (spec.type == NfMetricSpec::Type::Band) {
    if (band_index >= fr.powers.size() || band_index >= fr.bands.size()) {
      throw std::runtime_error("nf_eval_metric_band_or_ratio: band_index out of range");
    }
    if (channel_index >= fr.powers[band_index].size()) {
      throw std::runtime_error("nf_eval_metric_band_or_ratio: channel_index out of range for band row");
    }
    return fr.powers[band_index][channel_index];
  }
  if (spec.type != NfMetricSpec::Type::Ratio) {
    throw std::runtime_error("nf_eval_metric_band_or_ratio: spec type must be Band or Ratio");
  }

  if (band_num_index >= fr.powers.size() || band_num_index >= fr.bands.size() ||
      band_den_index >= fr.powers.size() || band_den_index >= fr.bands.size()) {
    throw std::runtime_error("nf_eval_metric_band_or_ratio: ratio band index out of range");
  }
  if (channel_index >= fr.powers[band_num_index].size() ||
      channel_index >= fr.powers[band_den_index].size()) {
    throw std::runtime_error("nf_eval_metric_band_or_ratio: channel_index out of range for ratio rows");
  }

  const double num = fr.powers[band_num_index][channel_index];
  const double den = fr.powers[band_den_index][channel_index];

  if (fr.log10_power) {
    // powers[][] are already log10-transformed.
    return num - den; // log10(num/den)
  }

  const double eps = 1e-12;
  return (num + eps) / (den + eps);
}

} // namespace qeeg
