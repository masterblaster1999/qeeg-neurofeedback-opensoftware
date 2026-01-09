#include "qeeg/online_coherence.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qeeg {

OnlineWelchCoherence::Ring::Ring(size_t cap) : buf(cap, 0.0f) {
  if (cap == 0) throw std::runtime_error("OnlineWelchCoherence: ring capacity must be > 0");
}

void OnlineWelchCoherence::Ring::push(float x) {
  buf[head] = x;
  head = (head + 1) % buf.size();
  if (count < buf.size()) {
    ++count;
  }
}

bool OnlineWelchCoherence::Ring::full() const {
  return count == buf.size();
}

void OnlineWelchCoherence::Ring::extract(std::vector<float>* out) const {
  out->resize(count);
  if (count == 0) return;
  const size_t cap = buf.size();
  // Oldest sample is at (head - count)
  size_t start = (head + cap - (count % cap)) % cap;
  for (size_t i = 0; i < count; ++i) {
    (*out)[i] = buf[(start + i) % cap];
  }
}

OnlineWelchCoherence::OnlineWelchCoherence(std::vector<std::string> channel_names,
                                         double fs_hz,
                                         std::vector<BandDefinition> bands,
                                         std::vector<std::pair<int,int>> pairs,
                                         OnlineCoherenceOptions opt)
  : channel_names_(std::move(channel_names)),
    fs_hz_(fs_hz),
    bands_(std::move(bands)),
    pairs_(std::move(pairs)),
    opt_(opt) {

  if (fs_hz_ <= 0.0) throw std::runtime_error("OnlineWelchCoherence: fs_hz must be > 0");
  if (channel_names_.empty()) throw std::runtime_error("OnlineWelchCoherence: channel_names is empty");
  if (bands_.empty()) throw std::runtime_error("OnlineWelchCoherence: bands is empty");
  if (pairs_.empty()) throw std::runtime_error("OnlineWelchCoherence: pairs is empty");

  for (const auto& pr : pairs_) {
    if (pr.first < 0 || pr.second < 0) {
      throw std::runtime_error("OnlineWelchCoherence: pair indices must be >= 0");
    }
    if (static_cast<size_t>(pr.first) >= channel_names_.size() ||
        static_cast<size_t>(pr.second) >= channel_names_.size()) {
      throw std::runtime_error("OnlineWelchCoherence: pair index out of range");
    }
    if (pr.first == pr.second) {
      throw std::runtime_error("OnlineWelchCoherence: pair indices must be different");
    }
  }

  // Build pair names.
  pair_names_.reserve(pairs_.size());
  for (const auto& pr : pairs_) {
    pair_names_.push_back(channel_names_[static_cast<size_t>(pr.first)] + "-" +
                          channel_names_[static_cast<size_t>(pr.second)]);
  }

  window_samples_ = static_cast<size_t>(std::llround(opt_.window_seconds * fs_hz_));
  update_samples_ = static_cast<size_t>(std::llround(opt_.update_seconds * fs_hz_));
  if (window_samples_ < 8) window_samples_ = 8;
  if (update_samples_ < 1) update_samples_ = 1;
  if (update_samples_ > window_samples_) update_samples_ = window_samples_;

  rings_.clear();
  rings_.reserve(channel_names_.size());
  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_.emplace_back(window_samples_);
  }
}

std::vector<OnlineCoherenceFrame> OnlineWelchCoherence::push_block(const std::vector<std::vector<float>>& block) {
  if (block.size() != channel_names_.size()) {
    throw std::runtime_error("OnlineWelchCoherence::push_block: block channel count mismatch");
  }
  if (block.empty()) return {};

  const size_t n = block[0].size();
  for (size_t c = 1; c < block.size(); ++c) {
    if (block[c].size() != n) {
      throw std::runtime_error("OnlineWelchCoherence::push_block: all channels must have same length");
    }
  }

  std::vector<OnlineCoherenceFrame> frames;

  for (size_t i = 0; i < n; ++i) {
    for (size_t c = 0; c < rings_.size(); ++c) {
      rings_[c].push(block[c][i]);
    }
    ++total_samples_;
    ++since_last_update_;

    if (since_last_update_ >= update_samples_) {
      // Keep remainder so update timing stays stable when chunk sizes don't
      // divide update_samples_.
      since_last_update_ -= update_samples_;
      // Only emit once the window is full for all channels.
      bool ready = true;
      for (const auto& r : rings_) {
        if (!r.full()) { ready = false; break; }
      }
      if (ready) {
        frames.push_back(compute_frame());
      }
    }
  }

  return frames;
}

OnlineCoherenceFrame OnlineWelchCoherence::compute_frame() const {
  OnlineCoherenceFrame fr;
  fr.channel_names = channel_names_;
  fr.bands = bands_;
  fr.pairs = pairs_;
  fr.pair_names = pair_names_;
  fr.measure = opt_.measure;
  fr.t_end_sec = static_cast<double>(total_samples_) / fs_hz_;

  // Extract windowed signals once.
  std::vector<std::vector<float>> windowed(channel_names_.size());
  for (size_t c = 0; c < rings_.size(); ++c) {
    rings_[c].extract(&windowed[c]);
  }

  fr.coherences.assign(bands_.size(), std::vector<double>(pairs_.size(), 0.0));

  // Compute coherence per pair and reduce into band means.
  for (size_t p = 0; p < pairs_.size(); ++p) {
    const int ia = pairs_[p].first;
    const int ib = pairs_[p].second;
    const auto spec = welch_coherence_spectrum(windowed[static_cast<size_t>(ia)],
                                               windowed[static_cast<size_t>(ib)],
                                               fs_hz_,
                                               opt_.welch,
                                               opt_.measure);

    for (size_t b = 0; b < bands_.size(); ++b) {
      const double v = average_band_value(spec, bands_[b]);
      fr.coherences[b][p] = std::isfinite(v) ? v : 0.0;
    }
  }

  return fr;
}

} // namespace qeeg
