#include "qeeg/preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qeeg {

static constexpr double kButterworthQ = 0.707106781186547524400844362104849039;

std::vector<BiquadCoeffs> make_iir_stage_coeffs(double fs_hz, const PreprocessOptions& opt) {
  if (fs_hz <= 0.0) throw std::runtime_error("make_iir_stage_coeffs: fs_hz must be > 0");

  std::vector<BiquadCoeffs> stages;
  stages.reserve(3);

  if (opt.notch_hz > 0.0) {
    if (opt.notch_q <= 0.0) throw std::runtime_error("--notch-q must be > 0");
    stages.push_back(design_notch(fs_hz, opt.notch_hz, opt.notch_q));
  }

  const double lo = opt.bandpass_low_hz;
  const double hi = opt.bandpass_high_hz;
  if (lo > 0.0 && hi > 0.0 && !(lo < hi)) {
    throw std::runtime_error("bandpass: requires bandpass_low_hz < bandpass_high_hz");
  }

  if (lo > 0.0) {
    stages.push_back(design_highpass(fs_hz, lo, kButterworthQ));
  }
  if (hi > 0.0) {
    stages.push_back(design_lowpass(fs_hz, hi, kButterworthQ));
  }

  return stages;
}

void apply_average_reference_inplace(EEGRecording& rec) {
  const size_t C = rec.n_channels();
  const size_t N = rec.n_samples();
  if (C == 0 || N == 0) return;

  for (size_t i = 0; i < N; ++i) {
    double m = 0.0;
    for (size_t c = 0; c < C; ++c) m += rec.data[c][i];
    m /= static_cast<double>(C);
    for (size_t c = 0; c < C; ++c) rec.data[c][i] = static_cast<float>(rec.data[c][i] - m);
  }
}

void preprocess_recording_inplace(EEGRecording& rec, const PreprocessOptions& opt) {
  if (rec.n_channels() == 0 || rec.n_samples() == 0) return;
  if (rec.fs_hz <= 0.0) throw std::runtime_error("preprocess_recording_inplace: rec.fs_hz must be > 0");

  if (opt.average_reference) {
    apply_average_reference_inplace(rec);
  }

  const auto stages = make_iir_stage_coeffs(rec.fs_hz, opt);
  if (stages.empty()) return;

  for (size_t c = 0; c < rec.n_channels(); ++c) {
    if (opt.zero_phase) {
      filtfilt_inplace(&rec.data[c], stages);
    } else {
      BiquadChain chain(stages);
      chain.reset();
      chain.process_inplace(&rec.data[c]);
    }
  }
}

StreamingPreprocessor::StreamingPreprocessor(size_t n_channels, double fs_hz, const PreprocessOptions& opt)
    : n_channels_(n_channels), fs_hz_(fs_hz), opt_(opt) {
  if (n_channels_ == 0) throw std::runtime_error("StreamingPreprocessor: n_channels must be > 0");
  if (fs_hz_ <= 0.0) throw std::runtime_error("StreamingPreprocessor: fs_hz must be > 0");
  if (opt_.zero_phase) {
    // Streaming must be causal.
    opt_.zero_phase = false;
  }

  const auto stages = make_iir_stage_coeffs(fs_hz_, opt_);
  chains_.reserve(n_channels_);
  for (size_t c = 0; c < n_channels_; ++c) {
    chains_.emplace_back(stages);
  }
}

void StreamingPreprocessor::reset() {
  for (auto& ch : chains_) ch.reset();
}

void StreamingPreprocessor::process_block(std::vector<std::vector<float>>* block) {
  if (!block) return;
  if (block->empty()) return;
  if (block->size() != n_channels_) {
    throw std::runtime_error("StreamingPreprocessor::process_block: channel count mismatch");
  }

  const size_t n = (*block)[0].size();
  for (size_t c = 1; c < block->size(); ++c) {
    if ((*block)[c].size() != n) {
      throw std::runtime_error("StreamingPreprocessor::process_block: all channels must have same #samples");
    }
  }

  if (n == 0) return;

  if (opt_.average_reference) {
    // Sample-major loop to compute CAR before filtering.
    for (size_t i = 0; i < n; ++i) {
      double m = 0.0;
      for (size_t c = 0; c < n_channels_; ++c) m += (*block)[c][i];
      m /= static_cast<double>(n_channels_);

      for (size_t c = 0; c < n_channels_; ++c) {
        float v = static_cast<float>((*block)[c][i] - m);
        v = chains_[c].process(v);
        (*block)[c][i] = v;
      }
    }
    return;
  }

  // No CAR: channel-major loop is fine.
  for (size_t c = 0; c < n_channels_; ++c) {
    auto& x = (*block)[c];
    auto& chain = chains_[c];
    for (size_t i = 0; i < n; ++i) {
      x[i] = chain.process(x[i]);
    }
  }
}

} // namespace qeeg
