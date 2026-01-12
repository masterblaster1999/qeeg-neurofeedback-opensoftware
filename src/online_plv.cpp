#include "qeeg/online_plv.hpp"

#include "qeeg/bandpower.hpp"
#include "qeeg/signal.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace qeeg {
namespace {

static size_t sec_to_samples(double sec, double fs_hz) {
  if (fs_hz <= 0.0) return 0;
  if (sec <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sec * fs_hz));
}

static void validate_pair(const std::pair<int, int>& p, size_t n_channels) {
  if (p.first < 0 || p.second < 0) {
    throw std::runtime_error("OnlinePlvConnectivity: pair indices must be >= 0");
  }
  if (static_cast<size_t>(p.first) >= n_channels || static_cast<size_t>(p.second) >= n_channels) {
    throw std::runtime_error("OnlinePlvConnectivity: pair index out of range");
  }
  if (p.first == p.second) {
    throw std::runtime_error("OnlinePlvConnectivity: pair channels must be different");
  }
}

static void validate_band(const BandDefinition& b, double fs_hz) {
  if (fs_hz <= 0.0) throw std::runtime_error("OnlinePlvConnectivity: fs_hz must be > 0");
  if (!(b.fmin_hz > 0.0) || !(b.fmax_hz > 0.0) || !(b.fmin_hz < b.fmax_hz)) {
    throw std::runtime_error("OnlinePlvConnectivity: invalid band (requires 0 < fmin < fmax)");
  }
  const double nyq = 0.5 * fs_hz;
  if (b.fmax_hz >= nyq) {
    throw std::runtime_error("OnlinePlvConnectivity: band fmax must be < Nyquist");
  }
}

static bool finite_complex(const std::complex<double>& z) {
  return std::isfinite(z.real()) && std::isfinite(z.imag());
}

} // namespace

OnlinePlvConnectivity::Ring::Ring(size_t cap) : buf(cap, 0.0f) {
  if (cap == 0) throw std::runtime_error("OnlinePlvConnectivity: ring capacity must be > 0");
}

void OnlinePlvConnectivity::Ring::push(float x) {
  buf[head] = x;
  head = (head + 1) % buf.size();
  if (count < buf.size()) ++count;
}

bool OnlinePlvConnectivity::Ring::full() const {
  return count == buf.size();
}

void OnlinePlvConnectivity::Ring::extract(std::vector<float>* out) const {
  if (!out) return;
  out->resize(count);
  if (count == 0) return;

  const size_t cap = buf.size();
  const size_t start = (count == cap) ? head : 0;
  for (size_t i = 0; i < count; ++i) {
    (*out)[i] = buf[(start + i) % cap];
  }
}

OnlinePlvConnectivity::OnlinePlvConnectivity(std::vector<std::string> channel_names,
                                             double fs_hz,
                                             std::vector<BandDefinition> bands,
                                             std::vector<std::pair<int, int>> pairs,
                                             OnlinePlvOptions opt)
    : channel_names_(std::move(channel_names)),
      fs_hz_(fs_hz),
      bands_(std::move(bands)),
      pairs_(std::move(pairs)),
      opt_(opt) {
  if (channel_names_.empty()) throw std::runtime_error("OnlinePlvConnectivity: need at least 1 channel");
  if (fs_hz_ <= 0.0) throw std::runtime_error("OnlinePlvConnectivity: fs_hz must be > 0");
  if (!(opt_.window_seconds > 0.0)) throw std::runtime_error("OnlinePlvConnectivity: window_seconds must be > 0");
  if (!(opt_.update_seconds > 0.0)) throw std::runtime_error("OnlinePlvConnectivity: update_seconds must be > 0");
  if (opt_.plv.edge_trim_fraction < 0.0 || opt_.plv.edge_trim_fraction >= 0.5 || !std::isfinite(opt_.plv.edge_trim_fraction)) {
    throw std::runtime_error("OnlinePlvConnectivity: edge_trim_fraction must be in [0, 0.49]");
  }

  if (bands_.empty()) bands_ = default_eeg_bands();
  for (const auto& b : bands_) validate_band(b, fs_hz_);

  for (const auto& p : pairs_) validate_pair(p, channel_names_.size());

  pair_names_.reserve(pairs_.size());
  for (const auto& p : pairs_) {
    pair_names_.push_back(channel_names_[static_cast<size_t>(p.first)] + "-" +
                          channel_names_[static_cast<size_t>(p.second)]);
  }

  window_samples_ = sec_to_samples(opt_.window_seconds, fs_hz_);
  if (window_samples_ < 8) window_samples_ = 8;

  update_samples_ = sec_to_samples(opt_.update_seconds, fs_hz_);
  if (update_samples_ < 1) update_samples_ = 1;
  if (update_samples_ > window_samples_) update_samples_ = window_samples_;

  rings_.reserve(channel_names_.size());
  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_.emplace_back(window_samples_);
  }
}

OnlinePlvFrame OnlinePlvConnectivity::compute_frame() const {
  OnlinePlvFrame fr;
  fr.t_end_sec = static_cast<double>(total_samples_) / fs_hz_;
  fr.measure = opt_.measure;
  fr.channel_names = channel_names_;
  fr.bands = bands_;
  fr.pairs = pairs_;
  fr.pair_names = pair_names_;
  fr.values.assign(bands_.size(), std::vector<double>(pairs_.size(), std::numeric_limits<double>::quiet_NaN()));

  // Extract the analysis window for all channels.
  std::vector<std::vector<float>> win(channel_names_.size());
  for (size_t c = 0; c < channel_names_.size(); ++c) {
    rings_[c].extract(&win[c]);
    if (win[c].empty()) {
      throw std::runtime_error("OnlinePlvConnectivity: internal window extraction failed");
    }
  }

  // For each band, build analytic signals per channel once, then compute requested pairs.
  std::vector<std::vector<std::complex<double>>> z(channel_names_.size());

  for (size_t bi = 0; bi < bands_.size(); ++bi) {
    const BandDefinition& band = bands_[bi];

    size_t m = std::numeric_limits<size_t>::max();
    for (size_t c = 0; c < channel_names_.size(); ++c) {
      const std::vector<float> xf = bandpass_filter(win[c], fs_hz_, band, opt_.plv.zero_phase);
      z[c] = analytic_signal_fft(xf);
      m = std::min(m, z[c].size());
    }

    if (m < 4) {
      // Not enough samples to compute a stable estimate.
      continue;
    }

    const size_t trim = edge_trim_samples(m, opt_.plv.edge_trim_fraction);
    const size_t i0 = trim;
    const size_t i1 = (m > trim ? (m - trim) : 0);
    if (i1 <= i0 + 1) continue;
    const size_t L = i1 - i0;

    const double eps = 1e-20;

    for (size_t pi = 0; pi < pairs_.size(); ++pi) {
      const int ia = pairs_[pi].first;
      const int ib = pairs_[pi].second;

      const auto& za = z[static_cast<size_t>(ia)];
      const auto& zb = z[static_cast<size_t>(ib)];

      if (opt_.measure == PhaseConnectivityMeasure::PLV) {
        std::complex<double> acc(0.0, 0.0);
        size_t cnt = 0;
        for (size_t k = 0; k < L; ++k) {
          const auto& a = za[i0 + k];
          const auto& b = zb[i0 + k];
          if (!finite_complex(a) || !finite_complex(b)) continue;
          const double ma = std::abs(a);
          const double mb = std::abs(b);
          if (!(ma > 0.0) || !(mb > 0.0)) continue;
          const std::complex<double> ua = a / ma;
          const std::complex<double> ub = b / mb;
          acc += ua * std::conj(ub);
          ++cnt;
        }
        double v = std::numeric_limits<double>::quiet_NaN();
        if (cnt > 0) {
          v = std::abs(acc) / static_cast<double>(cnt);
          if (!std::isfinite(v)) v = std::numeric_limits<double>::quiet_NaN();
          if (std::isfinite(v)) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
          }
        }
        fr.values[bi][pi] = v;
        continue;
      }

      if (opt_.measure == PhaseConnectivityMeasure::PLI) {
        double acc = 0.0;
        size_t cnt = 0;
        for (size_t k = 0; k < L; ++k) {
          const auto& a = za[i0 + k];
          const auto& b = zb[i0 + k];
          if (!finite_complex(a) || !finite_complex(b)) continue;
          const double im = (a * std::conj(b)).imag();
          if (!std::isfinite(im)) continue;
          double s = 0.0;
          if (im > 0.0) s = 1.0;
          else if (im < 0.0) s = -1.0;
          acc += s;
          ++cnt;
        }
        double v = std::numeric_limits<double>::quiet_NaN();
        if (cnt > 0) {
          v = std::fabs(acc) / static_cast<double>(cnt);
          if (!std::isfinite(v)) v = std::numeric_limits<double>::quiet_NaN();
          if (std::isfinite(v)) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
          }
        }
        fr.values[bi][pi] = v;
        continue;
      }

      if (opt_.measure == PhaseConnectivityMeasure::WeightedPLI) {
        double sum_im = 0.0;
        double sum_abs = 0.0;
        for (size_t k = 0; k < L; ++k) {
          const auto& a = za[i0 + k];
          const auto& b = zb[i0 + k];
          if (!finite_complex(a) || !finite_complex(b)) continue;
          const double im = (a * std::conj(b)).imag();
          if (!std::isfinite(im)) continue;
          sum_im += im;
          sum_abs += std::fabs(im);
        }

        double v = 0.0;
        if (sum_abs > eps) {
          v = std::fabs(sum_im) / sum_abs;
          if (!std::isfinite(v)) v = std::numeric_limits<double>::quiet_NaN();
          if (std::isfinite(v)) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
          }
        }
        fr.values[bi][pi] = v;
        continue;
      }

      if (opt_.measure == PhaseConnectivityMeasure::WeightedPLI2Debiased) {
        double sum_im = 0.0;
        double sum_abs = 0.0;
        double sum_sq = 0.0;
        size_t cnt = 0;

        for (size_t k = 0; k < L; ++k) {
          const auto& a = za[i0 + k];
          const auto& b = zb[i0 + k];
          if (!finite_complex(a) || !finite_complex(b)) continue;
          const double im = (a * std::conj(b)).imag();
          if (!std::isfinite(im)) continue;
          sum_im += im;
          sum_abs += std::fabs(im);
          sum_sq += im * im;
          ++cnt;
        }

        double v = std::numeric_limits<double>::quiet_NaN();
        if (cnt >= 2) {
          const double denom = (sum_abs * sum_abs) - sum_sq;
          if (denom > eps) {
            const double numer = (sum_im * sum_im) - sum_sq;
            v = numer / denom;
            if (!std::isfinite(v)) v = std::numeric_limits<double>::quiet_NaN();
            if (std::isfinite(v)) {
              if (v < 0.0) v = 0.0;
              if (v > 1.0) v = 1.0;
            }
          } else {
            v = 0.0;
          }
        }

        fr.values[bi][pi] = v;
        continue;
      }

      // Unknown measure enum value.
      fr.values[bi][pi] = std::numeric_limits<double>::quiet_NaN();
    }
  }

  return fr;
}

std::vector<OnlinePlvFrame> OnlinePlvConnectivity::push_block(const std::vector<std::vector<float>>& block) {
  if (block.empty()) return {};
  if (block.size() != channel_names_.size()) {
    throw std::runtime_error("OnlinePlvConnectivity::push_block: channel count mismatch");
  }
  const size_t n = block[0].size();
  for (size_t c = 1; c < block.size(); ++c) {
    if (block[c].size() != n) {
      throw std::runtime_error("OnlinePlvConnectivity::push_block: all channels must have same #samples");
    }
  }

  std::vector<OnlinePlvFrame> frames;
  frames.reserve(1 + (n / std::max<size_t>(1, update_samples_)));

  for (size_t i = 0; i < n; ++i) {
    for (size_t c = 0; c < block.size(); ++c) {
      rings_[c].push(block[c][i]);
    }
    ++total_samples_;
    ++since_last_update_;

    if (!rings_[0].full()) continue;

    if (since_last_update_ >= update_samples_) {
      since_last_update_ -= update_samples_;
      frames.push_back(compute_frame());
    }
  }

  return frames;
}

} // namespace qeeg
