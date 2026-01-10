#include "qeeg/line_noise.hpp"

#include "qeeg/bandpower.hpp"
#include "qeeg/robust_stats.hpp"

#include <algorithm>
#include <cmath>

namespace qeeg {

namespace {

static double mean_psd_density(const PsdResult& psd, double fmin_hz, double fmax_hz) {
  if (!(fmax_hz > fmin_hz)) return 0.0;
  if (psd.freqs_hz.empty() || psd.psd.empty()) return 0.0;
  const double nyq = psd.freqs_hz.back();
  if (!(nyq > 0.0)) return 0.0;
  if (fmax_hz <= 0.0 || fmin_hz >= nyq) return 0.0;

  // Clamp to available range.
  const double lo = std::max(0.0, fmin_hz);
  const double hi = std::min(nyq, fmax_hz);
  if (!(hi > lo)) return 0.0;

  const double area = integrate_bandpower(psd, lo, hi);
  const double width = hi - lo;
  if (!(width > 0.0)) return 0.0;
  const double mean = area / width;
  return std::isfinite(mean) ? mean : 0.0;
}

} // namespace

LineNoiseCandidate estimate_line_noise_candidate(const PsdResult& psd,
                                                 double center_hz,
                                                 double peak_half_width_hz,
                                                 double guard_hz,
                                                 double baseline_half_width_hz) {
  LineNoiseCandidate out;
  out.freq_hz = center_hz;

  if (!(center_hz > 0.0)) return out;
  if (!(peak_half_width_hz > 0.0)) return out;
  if (!(guard_hz > peak_half_width_hz)) return out;
  if (!(baseline_half_width_hz > guard_hz)) return out;

  const double peak_lo = center_hz - peak_half_width_hz;
  const double peak_hi = center_hz + peak_half_width_hz;
  const double base_l_lo = center_hz - baseline_half_width_hz;
  const double base_l_hi = center_hz - guard_hz;
  const double base_r_lo = center_hz + guard_hz;
  const double base_r_hi = center_hz + baseline_half_width_hz;

  const double peak_mean = mean_psd_density(psd, peak_lo, peak_hi);

  const double left_mean = mean_psd_density(psd, base_l_lo, base_l_hi);
  const double right_mean = mean_psd_density(psd, base_r_lo, base_r_hi);

  // Weighted mean by band widths (after clamping in mean_psd_density, the
  // effective widths may differ; we approximate with the nominal widths).
  const double left_w = std::max(0.0, base_l_hi - base_l_lo);
  const double right_w = std::max(0.0, base_r_hi - base_r_lo);
  const double wsum = left_w + right_w;

  double baseline_mean = 0.0;
  if (wsum > 0.0) {
    baseline_mean = (left_mean * left_w + right_mean * right_w) / wsum;
  }

  out.peak_mean = peak_mean;
  out.baseline_mean = baseline_mean;

  if (baseline_mean > 0.0 && std::isfinite(peak_mean) && std::isfinite(baseline_mean)) {
    out.ratio = peak_mean / baseline_mean;
  } else {
    out.ratio = 0.0;
  }

  if (!std::isfinite(out.ratio) || out.ratio < 0.0) out.ratio = 0.0;
  return out;
}

LineNoiseEstimate detect_line_noise_50_60(const EEGRecording& rec,
                                          const WelchOptions& opt,
                                          size_t max_channels,
                                          double min_ratio) {
  LineNoiseEstimate out;

  if (!(rec.fs_hz > 0.0)) return out;
  const double nyq = rec.fs_hz * 0.5;
  if (!(nyq > 1.0)) return out;

  const size_t n_ch = rec.n_channels();
  if (n_ch == 0) return out;

  const size_t use_ch = std::min(n_ch, (max_channels == 0 ? n_ch : max_channels));
  if (use_ch == 0) return out;

  std::vector<double> ratios50;
  std::vector<double> ratios60;
  ratios50.reserve(use_ch);
  ratios60.reserve(use_ch);

  for (size_t ch = 0; ch < use_ch; ++ch) {
    if (rec.data[ch].empty()) continue;

    const PsdResult psd = welch_psd(rec.data[ch], rec.fs_hz, opt);

    if (50.0 + 0.5 < nyq) {
      const auto c50 = estimate_line_noise_candidate(psd, 50.0);
      if (c50.ratio > 0.0) ratios50.push_back(c50.ratio);
    }

    if (60.0 + 0.5 < nyq) {
      const auto c60 = estimate_line_noise_candidate(psd, 60.0);
      if (c60.ratio > 0.0) ratios60.push_back(c60.ratio);
    }
  }

  out.n_channels_used = use_ch;

  const double med50 = ratios50.empty() ? 0.0 : median_inplace(&ratios50);
  const double med60 = ratios60.empty() ? 0.0 : median_inplace(&ratios60);

  out.cand50.freq_hz = 50.0;
  out.cand50.ratio = std::isfinite(med50) ? std::max(0.0, med50) : 0.0;
  out.cand60.freq_hz = 60.0;
  out.cand60.ratio = std::isfinite(med60) ? std::max(0.0, med60) : 0.0;

  const double best = std::max(out.cand50.ratio, out.cand60.ratio);
  if (best >= min_ratio && best > 0.0) {
    if (out.cand60.ratio > out.cand50.ratio) {
      out.recommended_hz = 60.0;
      out.strength_ratio = out.cand60.ratio;
    } else {
      out.recommended_hz = 50.0;
      out.strength_ratio = out.cand50.ratio;
    }
  }

  return out;
}

} // namespace qeeg
