#include "qeeg/channel_qc.hpp"

#include "qeeg/robust_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qeeg {
namespace {

struct BasicStats {
  double mn{0.0};
  double mx{0.0};
  double mean{0.0};
  double stddev{0.0};
};

static BasicStats compute_basic_stats(const std::vector<float>& x) {
  BasicStats s;
  double mn = std::numeric_limits<double>::infinity();
  double mx = -std::numeric_limits<double>::infinity();
  double sum = 0.0;
  double sum2 = 0.0;
  size_t n = 0;

  for (float vf : x) {
    const double v = static_cast<double>(vf);
    if (!std::isfinite(v)) {
      continue;
    }
    mn = std::min(mn, v);
    mx = std::max(mx, v);
    sum += v;
    sum2 += v * v;
    ++n;
  }

  if (n == 0) {
    s.mn = 0.0;
    s.mx = 0.0;
    s.mean = 0.0;
    s.stddev = 0.0;
    return s;
  }

  const double mean = sum / static_cast<double>(n);
  const double ex2 = sum2 / static_cast<double>(n);
  const double var = std::max(0.0, ex2 - mean * mean);

  s.mn = std::isfinite(mn) ? mn : 0.0;
  s.mx = std::isfinite(mx) ? mx : 0.0;
  s.mean = mean;
  s.stddev = std::sqrt(var);
  return s;
}

static std::vector<size_t> make_downsample_indices(size_t n, size_t max_samples) {
  if (max_samples < 1) max_samples = 1;
  if (n <= max_samples) {
    std::vector<size_t> idx;
    idx.reserve(n);
    for (size_t i = 0; i < n; ++i) idx.push_back(i);
    return idx;
  }
  const size_t stride = static_cast<size_t>(std::ceil(static_cast<double>(n) / static_cast<double>(max_samples)));
  std::vector<size_t> idx;
  idx.reserve(max_samples + 1);
  for (size_t i = 0; i < n; i += stride) {
    idx.push_back(i);
    if (idx.size() >= max_samples) break;
  }
  if (idx.empty()) idx.push_back(0);
  return idx;
}

static double robust_scale_downsample(const std::vector<float>& x, const std::vector<size_t>& idx) {
  std::vector<double> v;
  v.reserve(idx.size());
  for (size_t i : idx) {
    if (i >= x.size()) break;
    const double d = static_cast<double>(x[i]);
    if (!std::isfinite(d)) continue;
    v.push_back(d);
  }
  if (v.empty()) return 0.0;
  std::vector<double> tmp = v;
  const double med = median_inplace(&tmp);
  return robust_scale(v, med);
}

static double abs_corr_downsample(const std::vector<float>& x,
                                 const std::vector<double>& mean_sig,
                                 const std::vector<size_t>& idx) {
  // Pearson correlation on downsampled points.
  // Returns |corr|, or 0 if degenerate.
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double syy = 0.0;
  double sxy = 0.0;
  size_t n = 0;

  for (size_t k = 0; k < idx.size(); ++k) {
    const size_t i = idx[k];
    if (i >= x.size() || k >= mean_sig.size()) break;
    const double a = static_cast<double>(x[i]);
    const double b = mean_sig[k];
    if (!std::isfinite(a) || !std::isfinite(b)) continue;
    sx += a;
    sy += b;
    sxx += a * a;
    syy += b * b;
    sxy += a * b;
    ++n;
  }

  if (n < 2) return 0.0;
  const double invn = 1.0 / static_cast<double>(n);
  const double mx = sx * invn;
  const double my = sy * invn;
  const double vx = std::max(0.0, sxx * invn - mx * mx);
  const double vy = std::max(0.0, syy * invn - my * my);
  if (!(vx > 1e-18) || !(vy > 1e-18)) return 0.0;
  const double cov = sxy * invn - mx * my;
  const double corr = cov / (std::sqrt(vx) * std::sqrt(vy));
  return std::min(1.0, std::fabs(corr));
}

static void add_reason(std::string* reasons, const std::string& r) {
  if (!reasons) return;
  if (reasons->empty()) {
    *reasons = r;
  } else {
    *reasons += ";" + r;
  }
}

} // namespace

ChannelQCResult evaluate_channel_qc(const EEGRecording& rec, const ChannelQCOptions& opt) {
  if (rec.fs_hz <= 0.0) throw std::runtime_error("evaluate_channel_qc: invalid sampling rate");
  if (rec.n_channels() == 0 || rec.n_samples() == 0) {
    throw std::runtime_error("evaluate_channel_qc: empty recording");
  }

  ChannelQCResult out;
  out.opt = opt;
  out.channels.resize(rec.n_channels());

  const size_t n_ch = rec.n_channels();
  const size_t n_samp = rec.n_samples();

  const std::vector<size_t> idx = make_downsample_indices(n_samp, opt.max_samples_for_robust);

  // 1) Per-channel basic stats and robust scale.
  std::vector<double> scales;
  scales.reserve(n_ch);

  for (size_t ch = 0; ch < n_ch; ++ch) {
    ChannelQCChannelResult r;
    r.channel = (ch < rec.channel_names.size()) ? rec.channel_names[ch] : ("ch" + std::to_string(ch));

    const BasicStats st = compute_basic_stats(rec.data[ch]);
    r.min_value = st.mn;
    r.max_value = st.mx;
    r.ptp = st.mx - st.mn;
    r.mean = st.mean;
    r.stddev = st.stddev;
    r.robust_scale = robust_scale_downsample(rec.data[ch], idx);

    out.channels[ch] = r;
    scales.push_back(r.robust_scale);
  }

  // Median scale (typical amplitude) used for relative checks.
  double median_scale = 1.0;
  {
    std::vector<double> tmp = scales;
    median_scale = median_inplace(&tmp);
    if (!(median_scale > 1e-12)) median_scale = 1.0;
  }

  // 2) Optional artifact-based bad-window fraction per channel.
  std::vector<double> bad_window_frac(n_ch, 0.0);
  bool artifacts_ok = false;
  if (opt.artifact_bad_window_fraction > 0.0) {
    try {
      const ArtifactDetectionResult ares = detect_artifacts(rec, opt.artifact_opt);
      const std::vector<size_t> counts = artifact_bad_counts_per_channel(ares);
      const double denom = static_cast<double>(std::max<size_t>(1, ares.windows.size()));
      for (size_t ch = 0; ch < n_ch && ch < counts.size(); ++ch) {
        bad_window_frac[ch] = static_cast<double>(counts[ch]) / denom;
      }
      artifacts_ok = true;
    } catch (...) {
      // Best-effort: keep fractions at 0.0
      artifacts_ok = false;
    }
  }

  // 3) Optional correlation against global mean.
  std::vector<double> mean_sig;
  if (opt.min_abs_corr > 0.0) {
    mean_sig.assign(idx.size(), 0.0);
    // Compute mean over channels at each sampled index.
    for (size_t k = 0; k < idx.size(); ++k) {
      const size_t i = idx[k];
      double sum = 0.0;
      size_t n = 0;
      for (size_t ch = 0; ch < n_ch; ++ch) {
        if (i >= rec.data[ch].size()) continue;
        const double v = static_cast<double>(rec.data[ch][i]);
        if (!std::isfinite(v)) continue;
        sum += v;
        ++n;
      }
      mean_sig[k] = (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
    }
  }

  // 4) Decide flags + reasons.
  for (size_t ch = 0; ch < n_ch; ++ch) {
    auto& r = out.channels[ch];
    r.artifact_bad_window_fraction = bad_window_frac[ch];

    if (opt.min_abs_corr > 0.0) {
      r.abs_corr_with_mean = abs_corr_downsample(rec.data[ch], mean_sig, idx);
    }

    // Flatline
    const bool flat_abs_ptp = (opt.flatline_ptp > 0.0) && (r.ptp < opt.flatline_ptp);
    const bool flat_abs_scale = (opt.flatline_scale > 0.0) && (r.robust_scale < opt.flatline_scale);
    const bool flat_rel = (opt.flatline_scale_factor > 0.0) && (r.robust_scale < opt.flatline_scale_factor * median_scale);
    r.flatline = flat_abs_ptp || flat_abs_scale || flat_rel;
    if (r.flatline) add_reason(&r.reasons, "flatline");

    // Noisy
    r.noisy = (opt.noisy_scale_factor > 0.0) && (r.robust_scale > opt.noisy_scale_factor * median_scale);
    if (r.noisy) add_reason(&r.reasons, "noisy");

    // Often flagged by artifact scoring
    if (opt.artifact_bad_window_fraction > 0.0 && artifacts_ok) {
      r.artifact_often_bad = (r.artifact_bad_window_fraction >= opt.artifact_bad_window_fraction);
      if (r.artifact_often_bad) add_reason(&r.reasons, "artifact_often_bad");
    }

    // Low correlation against global mean
    if (opt.min_abs_corr > 0.0) {
      r.corr_low = (r.abs_corr_with_mean < opt.min_abs_corr);
      if (r.corr_low) add_reason(&r.reasons, "low_corr");
    }

    r.bad = r.flatline || r.noisy || r.artifact_often_bad || r.corr_low;
    if (r.bad) out.bad_indices.push_back(ch);
  }

  return out;
}

} // namespace qeeg
