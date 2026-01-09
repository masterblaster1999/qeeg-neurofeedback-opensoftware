#include "qeeg/online_artifacts.hpp"

#include "qeeg/robust_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace qeeg {
namespace {

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

} // namespace

OnlineArtifactGate::Ring::Ring(size_t cap) : buf(cap, 0.0f) {
  if (cap == 0) throw std::runtime_error("OnlineArtifactGate: ring capacity must be > 0");
}

void OnlineArtifactGate::Ring::push(float x) {
  buf[head] = x;
  head = (head + 1) % buf.size();
  if (count < buf.size()) ++count;
}

bool OnlineArtifactGate::Ring::full() const {
  return count == buf.size();
}

OnlineArtifactGate::OnlineArtifactGate(std::vector<std::string> channel_names,
                                       double fs_hz,
                                       OnlineArtifactOptions opt)
    : channel_names_(std::move(channel_names)), fs_hz_(fs_hz), opt_(opt) {
  if (channel_names_.empty()) throw std::runtime_error("OnlineArtifactGate: need at least 1 channel");
  if (fs_hz_ <= 0.0) throw std::runtime_error("OnlineArtifactGate: fs_hz must be > 0");
  if (!(opt_.window_seconds > 0.0)) throw std::runtime_error("OnlineArtifactGate: window_seconds must be > 0");
  if (!(opt_.update_seconds > 0.0)) throw std::runtime_error("OnlineArtifactGate: update_seconds must be > 0");
  if (opt_.update_seconds > opt_.window_seconds) {
    throw std::runtime_error("OnlineArtifactGate: update_seconds must be <= window_seconds");
  }
  if (opt_.min_bad_channels < 1) {
    throw std::runtime_error("OnlineArtifactGate: min_bad_channels must be >= 1");
  }

  window_samples_ = std::max<size_t>(8, sec_to_samples(opt_.window_seconds, fs_hz_));
  update_samples_ = std::max<size_t>(1, sec_to_samples(opt_.update_seconds, fs_hz_));
  baseline_end_samples_ = sec_to_samples(opt_.baseline_seconds, fs_hz_);

  rings_.reserve(channel_names_.size());
  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_.emplace_back(window_samples_);
  }

  base_ptp_.assign(channel_names_.size(), {});
  base_rms_.assign(channel_names_.size(), {});
  base_kurt_.assign(channel_names_.size(), {});
  baseline_stats_.assign(channel_names_.size(), ArtifactChannelStats{});
}

OnlineArtifactGate::RawFeatures OnlineArtifactGate::compute_raw_features() const {
  const size_t n_ch = channel_names_.size();
  RawFeatures f;
  f.ptp.assign(n_ch, 0.0);
  f.rms.assign(n_ch, 0.0);
  f.kurtosis.assign(n_ch, 0.0);

  for (size_t ch = 0; ch < n_ch; ++ch) {
    const auto& r = rings_[ch];
    const size_t cap = r.buf.size();
    const size_t count = r.count;
    if (count == 0) continue;

    const size_t start = (count == cap) ? r.head : 0;
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    double s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double v = static_cast<double>(r.buf[(start + i) % cap]);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      s1 += v;
      const double v2 = v * v;
      s2 += v2;
      s3 += v2 * v;
      s4 += v2 * v2;
    }

    const double n = static_cast<double>(count);
    const double mean = s1 / n;
    const double ex2 = s2 / n;
    const double ex3 = s3 / n;
    const double ex4 = s4 / n;
    const double var = std::max(0.0, ex2 - mean * mean);

    // Fourth central moment using raw moments.
    const double mu4 = ex4 - 4.0 * mean * ex3 + 6.0 * mean * mean * ex2 - 3.0 * std::pow(mean, 4);
    double kurt_excess = 0.0;
    if (var > 1e-24) {
      kurt_excess = (mu4 / (var * var)) - 3.0;
    }

    f.ptp[ch] = mx - mn;
    f.rms[ch] = std::sqrt(std::max(0.0, ex2));
    f.kurtosis[ch] = kurt_excess;
  }

  return f;
}

void OnlineArtifactGate::ensure_baseline_stats_built(double t_end_sec) {
  if (baseline_ready_) return;
  // Baseline duration <= 0 => treat baseline as immediately ready with neutral stats.
  if (!(opt_.baseline_seconds > 0.0)) {
    baseline_ready_ = true;
    return;
  }
  if (t_end_sec <= opt_.baseline_seconds) return;

  // If baseline collections are empty (short recordings), fall back to what we have.
  bool empty = true;
  for (const auto& v : base_ptp_) {
    if (!v.empty()) { empty = false; break; }
  }
  if (empty) {
    baseline_ready_ = true;
    return;
  }

  for (size_t ch = 0; ch < channel_names_.size(); ++ch) {
    std::vector<double> tmp;

    tmp = base_ptp_[ch];
    const double ptp_med = median_inplace(&tmp);
    baseline_stats_[ch].ptp_median = ptp_med;
    baseline_stats_[ch].ptp_scale = robust_scale(base_ptp_[ch], ptp_med);

    tmp = base_rms_[ch];
    const double rms_med = median_inplace(&tmp);
    baseline_stats_[ch].rms_median = rms_med;
    baseline_stats_[ch].rms_scale = robust_scale(base_rms_[ch], rms_med);

    tmp = base_kurt_[ch];
    const double k_med = median_inplace(&tmp);
    baseline_stats_[ch].kurtosis_median = k_med;
    baseline_stats_[ch].kurtosis_scale = robust_scale(base_kurt_[ch], k_med);
  }

  baseline_ready_ = true;
  // Free baseline storage (we keep stats).
  base_ptp_.clear();
  base_rms_.clear();
  base_kurt_.clear();
}

std::vector<OnlineArtifactFrame> OnlineArtifactGate::push_block(const std::vector<std::vector<float>>& block) {
  if (block.empty()) return {};
  if (block.size() != channel_names_.size()) {
    throw std::runtime_error("OnlineArtifactGate::push_block: channel count mismatch");
  }
  const size_t n = block[0].size();
  for (size_t c = 1; c < block.size(); ++c) {
    if (block[c].size() != n) {
      throw std::runtime_error("OnlineArtifactGate::push_block: all channels must have same #samples");
    }
  }

  std::vector<OnlineArtifactFrame> frames;
  frames.reserve(1 + (n / std::max<size_t>(1, update_samples_)));

  for (size_t i = 0; i < n; ++i) {
    for (size_t c = 0; c < block.size(); ++c) {
      rings_[c].push(block[c][i]);
    }
    ++total_samples_;
    ++since_last_update_;

    if (!rings_[0].full()) continue;

    if (since_last_update_ >= update_samples_) {
      // Emit one frame. Keep remainder so timing stays stable when update_samples_ doesn't divide block sizes.
      since_last_update_ -= update_samples_;

      OnlineArtifactFrame fr;
      fr.t_end_sec = static_cast<double>(total_samples_) / fs_hz_;

      const RawFeatures raw = compute_raw_features();

      // Accumulate baseline distributions until baseline ends.
      if (opt_.baseline_seconds > 0.0 && static_cast<size_t>(total_samples_) <= baseline_end_samples_) {
        for (size_t ch = 0; ch < channel_names_.size(); ++ch) {
          base_ptp_[ch].push_back(raw.ptp[ch]);
          base_rms_[ch].push_back(raw.rms[ch]);
          base_kurt_[ch].push_back(raw.kurtosis[ch]);
        }
      }

      ensure_baseline_stats_built(fr.t_end_sec);
      fr.baseline_ready = baseline_ready_;

      if (baseline_ready_) {
        size_t bad_ch = 0;
        double max_ptp = 0.0, max_rms = 0.0, max_kurt = 0.0;
        for (size_t ch = 0; ch < channel_names_.size(); ++ch) {
          const auto& st = baseline_stats_[ch];
          const double z_ptp = (raw.ptp[ch] - st.ptp_median) / st.ptp_scale;
          const double z_rms = (raw.rms[ch] - st.rms_median) / st.rms_scale;
          const double z_k = (raw.kurtosis[ch] - st.kurtosis_median) / st.kurtosis_scale;

          max_ptp = std::max(max_ptp, z_ptp);
          max_rms = std::max(max_rms, z_rms);
          max_kurt = std::max(max_kurt, z_k);

          bool bad = false;
          if (opt_.ptp_z > 0.0 && z_ptp > opt_.ptp_z) bad = true;
          if (opt_.rms_z > 0.0 && z_rms > opt_.rms_z) bad = true;
          if (opt_.kurtosis_z > 0.0 && z_k > opt_.kurtosis_z) bad = true;
          if (bad) ++bad_ch;
        }
        fr.bad_channel_count = bad_ch;
        fr.bad = (bad_ch >= opt_.min_bad_channels);
        fr.max_ptp_z = max_ptp;
        fr.max_rms_z = max_rms;
        fr.max_kurtosis_z = max_kurt;
      }

      frames.push_back(fr);
    }
  }

  return frames;
}

} // namespace qeeg
