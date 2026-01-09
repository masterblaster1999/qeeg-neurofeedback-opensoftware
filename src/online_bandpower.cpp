#include "qeeg/online_bandpower.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qeeg {

OnlineWelchBandpower::Ring::Ring(size_t cap) : buf(cap, 0.0f) {
  if (cap == 0) throw std::runtime_error("OnlineWelchBandpower: ring capacity must be > 0");
}

void OnlineWelchBandpower::Ring::push(float x) {
  buf[head] = x;
  head = (head + 1) % buf.size();
  if (count < buf.size()) {
    ++count;
  }
}

bool OnlineWelchBandpower::Ring::full() const {
  return count == buf.size();
}

void OnlineWelchBandpower::Ring::extract(std::vector<float>* out) const {
  if (!out) return;
  out->resize(count);
  if (count == 0) return;
  // Oldest element is head when full, otherwise at 0.
  const size_t cap = buf.size();
  const size_t start = (count == cap) ? head : 0;
  for (size_t i = 0; i < count; ++i) {
    (*out)[i] = buf[(start + i) % cap];
  }
}

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

static bool user_specified_range(double fmin_hz, double fmax_hz) {
  // Keep the sentinel simple and ABI-friendly: (0,0) means "unspecified".
  return (fmin_hz != 0.0 || fmax_hz != 0.0);
}

OnlineWelchBandpower::OnlineWelchBandpower(std::vector<std::string> channel_names,
                                           double fs_hz,
                                           std::vector<BandDefinition> bands,
                                           OnlineBandpowerOptions opt)
    : channel_names_(std::move(channel_names)),
      fs_hz_(fs_hz),
      bands_(std::move(bands)),
      opt_(opt) {
  if (channel_names_.empty()) throw std::runtime_error("OnlineWelchBandpower: need at least 1 channel");
  if (fs_hz_ <= 0.0) throw std::runtime_error("OnlineWelchBandpower: fs_hz must be > 0");
  if (!(opt_.window_seconds > 0.0)) throw std::runtime_error("OnlineWelchBandpower: window_seconds must be > 0");
  if (!(opt_.update_seconds > 0.0)) throw std::runtime_error("OnlineWelchBandpower: update_seconds must be > 0");
  if (bands_.empty()) bands_ = default_eeg_bands();

  if (opt_.relative_power && user_specified_range(opt_.relative_fmin_hz, opt_.relative_fmax_hz)) {
    if (opt_.relative_fmin_hz < 0.0) {
      throw std::runtime_error("OnlineWelchBandpower: relative_fmin_hz must be >= 0");
    }
    if (!(opt_.relative_fmax_hz > opt_.relative_fmin_hz)) {
      throw std::runtime_error("OnlineWelchBandpower: relative range must satisfy fmin < fmax");
    }
  }

  window_samples_ = sec_to_samples(opt_.window_seconds, fs_hz_);
  if (window_samples_ < 8) window_samples_ = 8;

  update_samples_ = sec_to_samples(opt_.update_seconds, fs_hz_);
  if (update_samples_ < 1) update_samples_ = 1;
  // Match OnlineWelchCoherence behavior: if update interval > window, clamp to window.
  if (update_samples_ > window_samples_) update_samples_ = window_samples_;

  rings_.reserve(channel_names_.size());
  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_.emplace_back(window_samples_);
  }
}

OnlineBandpowerFrame OnlineWelchBandpower::compute_frame() const {
  OnlineBandpowerFrame fr;
  fr.t_end_sec = static_cast<double>(total_samples_) / fs_hz_;
  fr.channel_names = channel_names_;
  fr.bands = bands_;

  fr.relative_power = opt_.relative_power;
  fr.log10_power = opt_.log10_power;

  double rel_lo = 0.0;
  double rel_hi = 0.0;
  if (opt_.relative_power) {
    if (user_specified_range(opt_.relative_fmin_hz, opt_.relative_fmax_hz)) {
      rel_lo = opt_.relative_fmin_hz;
      rel_hi = opt_.relative_fmax_hz;
    } else {
      // Default: cover the full band range.
      rel_lo = bands_.front().fmin_hz;
      rel_hi = bands_.front().fmax_hz;
      for (const auto& b : bands_) {
        rel_lo = std::min(rel_lo, b.fmin_hz);
        rel_hi = std::max(rel_hi, b.fmax_hz);
      }
    }
    fr.relative_fmin_hz = rel_lo;
    fr.relative_fmax_hz = rel_hi;
  }

  fr.powers.assign(bands_.size(), std::vector<double>(channel_names_.size(), 0.0));

  const double eps = 1e-20;
  std::vector<float> window;
  window.reserve(window_samples_);

  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_[c].extract(&window);
    if (window.empty()) throw std::runtime_error("OnlineWelchBandpower: internal window extraction failed");

    const PsdResult psd = welch_psd(window, fs_hz_, opt_.welch);

    double total_power = 1.0;
    if (opt_.relative_power) {
      total_power = integrate_bandpower(psd, rel_lo, rel_hi);
    }

    for (size_t b = 0; b < bands_.size(); ++b) {
      double v = integrate_bandpower(psd, bands_[b].fmin_hz, bands_[b].fmax_hz);
      if (opt_.relative_power) {
        v = v / std::max(eps, total_power);
      }
      if (opt_.log10_power) {
        v = std::log10(std::max(eps, v));
      }
      fr.powers[b][c] = v;
    }
  }

  return fr;
}

std::vector<OnlineBandpowerFrame> OnlineWelchBandpower::push_block(const std::vector<std::vector<float>>& block) {
  if (block.empty()) return {};
  if (block.size() != channel_names_.size()) {
    throw std::runtime_error("OnlineWelchBandpower::push_block: channel count mismatch");
  }
  const size_t n = block[0].size();
  for (size_t c = 1; c < block.size(); ++c) {
    if (block[c].size() != n) {
      throw std::runtime_error("OnlineWelchBandpower::push_block: all channels must have same #samples");
    }
  }

  std::vector<OnlineBandpowerFrame> frames;
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
      frames.push_back(compute_frame());
    }
  }

  return frames;
}

} // namespace qeeg
