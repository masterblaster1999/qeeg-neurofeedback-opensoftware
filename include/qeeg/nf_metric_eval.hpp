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

// Evaluate an asymmetry metric of the form:
//   asym:BAND:CH_A:CH_B
//
// Semantics:
// - If the frame is *not* log10-transformed, returns log10((Pa + eps) / (Pb + eps)).
// - If the frame *is* log10-transformed, returns Pa_log10 - Pb_log10, which equals log10(Pa/Pb).
inline double nf_eval_metric_asymmetry(const OnlineBandpowerFrame& fr,
                                       const NfMetricSpec& spec,
                                       size_t channel_a_index,
                                       size_t channel_b_index,
                                       size_t band_index) {
  if (spec.type != NfMetricSpec::Type::Asymmetry) {
    throw std::runtime_error("nf_eval_metric_asymmetry: spec type must be Asymmetry");
  }
  if (channel_a_index >= fr.channel_names.size() || channel_b_index >= fr.channel_names.size()) {
    throw std::runtime_error("nf_eval_metric_asymmetry: channel index out of range");
  }
  if (band_index >= fr.powers.size() || band_index >= fr.bands.size()) {
    throw std::runtime_error("nf_eval_metric_asymmetry: band_index out of range");
  }
  if (channel_a_index >= fr.powers[band_index].size() || channel_b_index >= fr.powers[band_index].size()) {
    throw std::runtime_error("nf_eval_metric_asymmetry: channel index out of range for band row");
  }

  const double pa = fr.powers[band_index][channel_a_index];
  const double pb = fr.powers[band_index][channel_b_index];

  if (fr.log10_power) {
    // powers[][] are already log10-transformed.
    return pa - pb; // log10(Pa/Pb)
  }

  const double eps = 1e-12;
  return std::log10((pa + eps) / (pb + eps));
}

} // namespace qeeg
