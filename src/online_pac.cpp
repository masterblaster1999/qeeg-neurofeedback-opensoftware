#include "qeeg/online_pac.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qeeg {

OnlinePAC::Ring::Ring(size_t cap) : buf(cap, 0.0f) {
  if (cap == 0) throw std::runtime_error("OnlinePAC::Ring: cap must be > 0");
}

void OnlinePAC::Ring::push(float v) {
  buf[head] = v;
  head = (head + 1) % buf.size();
  if (count < buf.size()) ++count;
}

bool OnlinePAC::Ring::full() const { return count == buf.size(); }

void OnlinePAC::Ring::extract(std::vector<float>* out) const {
  if (!out) return;
  out->assign(buf.size(), 0.0f);
  // Oldest element is at head when full; otherwise at 0.
  const size_t n = buf.size();
  if (count == 0) return;

  const size_t start = full() ? head : 0;
  for (size_t i = 0; i < n; ++i) {
    (*out)[i] = buf[(start + i) % n];
  }
}

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

OnlinePAC::OnlinePAC(double fs_hz,
                     BandDefinition phase_band,
                     BandDefinition amp_band,
                     OnlinePacOptions opt)
    : fs_hz_(fs_hz), phase_band_(std::move(phase_band)), amp_band_(std::move(amp_band)), opt_(std::move(opt)),
      window_samples_(0), update_samples_(0), ring_(1) {
  if (fs_hz_ <= 0.0) throw std::runtime_error("OnlinePAC: fs_hz must be > 0");
  if (opt_.window_seconds <= 0.0) throw std::runtime_error("OnlinePAC: window_seconds must be > 0");
  if (opt_.update_seconds <= 0.0) throw std::runtime_error("OnlinePAC: update_seconds must be > 0");
  if (opt_.update_seconds > opt_.window_seconds) {
    throw std::runtime_error("OnlinePAC: update_seconds must be <= window_seconds");
  }

  window_samples_ = std::max<size_t>(1, sec_to_samples(opt_.window_seconds, fs_hz_));
  update_samples_ = std::max<size_t>(1, sec_to_samples(opt_.update_seconds, fs_hz_));

  ring_ = Ring(window_samples_);
}

OnlinePacFrame OnlinePAC::compute_frame() const {
  std::vector<float> w;
  ring_.extract(&w);
  OnlinePacFrame fr;
  fr.t_end_sec = static_cast<double>(total_samples_) / fs_hz_;
  fr.value = compute_pac(w, fs_hz_, phase_band_, amp_band_, opt_.pac).value;
  return fr;
}

std::vector<OnlinePacFrame> OnlinePAC::push_block(const std::vector<float>& x) {
  std::vector<OnlinePacFrame> out;
  if (x.empty()) return out;

  for (float v : x) {
    ring_.push(v);
    ++total_samples_;
    ++since_last_update_;

    if (since_last_update_ >= update_samples_) {
      // Keep remainder so update timing stays stable when chunk sizes don't
      // divide update_samples_.
      since_last_update_ -= update_samples_;
      if (ring_.full()) out.push_back(compute_frame());
    }
  }

  return out;
}

} // namespace qeeg
