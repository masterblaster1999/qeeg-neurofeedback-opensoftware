#include "qeeg/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qeeg {

static constexpr double kPi = 3.141592653589793238462643383279502884;

Biquad::Biquad(const BiquadCoeffs& c) {
  set_coeffs(c);
}

void Biquad::set_coeffs(const BiquadCoeffs& c) {
  c_ = c;
  reset();
}

void Biquad::reset() {
  z1_ = 0.0;
  z2_ = 0.0;
}

float Biquad::process(float x) {
  const double in = static_cast<double>(x);
  const double y = c_.b0 * in + z1_;
  z1_ = c_.b1 * in - c_.a1 * y + z2_;
  z2_ = c_.b2 * in - c_.a2 * y;
  return static_cast<float>(y);
}

BiquadChain::BiquadChain(const std::vector<BiquadCoeffs>& stages) {
  for (const auto& c : stages) add_stage(c);
}

void BiquadChain::add_stage(const BiquadCoeffs& c) {
  stages_.emplace_back(c);
}

void BiquadChain::reset() {
  for (auto& s : stages_) s.reset();
}

float BiquadChain::process(float x) {
  float y = x;
  for (auto& s : stages_) {
    y = s.process(y);
  }
  return y;
}

void BiquadChain::process_inplace(std::vector<float>* x) {
  if (!x) return;
  if (stages_.empty()) return;
  for (float& v : *x) {
    v = process(v);
  }
}

std::vector<BiquadCoeffs> BiquadChain::stage_coeffs() const {
  std::vector<BiquadCoeffs> out;
  out.reserve(stages_.size());
  for (const auto& s : stages_) out.push_back(s.coeffs());
  return out;
}

static void validate_design_inputs(double fs_hz, double f0_hz, double Q, const char* what) {
  if (fs_hz <= 0.0) throw std::runtime_error(std::string(what) + ": fs_hz must be > 0");
  if (!(f0_hz > 0.0)) throw std::runtime_error(std::string(what) + ": f0_hz must be > 0");
  if (!(f0_hz < 0.5 * fs_hz)) {
    throw std::runtime_error(std::string(what) + ": f0_hz must be < fs/2");
  }
  if (!(Q > 0.0)) throw std::runtime_error(std::string(what) + ": Q must be > 0");
}

static BiquadCoeffs normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
  if (a0 == 0.0) throw std::runtime_error("biquad normalize: a0 is zero");
  BiquadCoeffs c;
  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

BiquadCoeffs design_lowpass(double fs_hz, double f0_hz, double Q) {
  validate_design_inputs(fs_hz, f0_hz, Q, "design_lowpass");

  const double w0 = 2.0 * kPi * (f0_hz / fs_hz);
  const double cosw0 = std::cos(w0);
  const double sinw0 = std::sin(w0);
  const double alpha = sinw0 / (2.0 * Q);

  const double b0 = (1.0 - cosw0) / 2.0;
  const double b1 = (1.0 - cosw0);
  const double b2 = (1.0 - cosw0) / 2.0;
  const double a0 = 1.0 + alpha;
  const double a1 = -2.0 * cosw0;
  const double a2 = 1.0 - alpha;

  return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs design_highpass(double fs_hz, double f0_hz, double Q) {
  validate_design_inputs(fs_hz, f0_hz, Q, "design_highpass");

  const double w0 = 2.0 * kPi * (f0_hz / fs_hz);
  const double cosw0 = std::cos(w0);
  const double sinw0 = std::sin(w0);
  const double alpha = sinw0 / (2.0 * Q);

  const double b0 = (1.0 + cosw0) / 2.0;
  const double b1 = -(1.0 + cosw0);
  const double b2 = (1.0 + cosw0) / 2.0;
  const double a0 = 1.0 + alpha;
  const double a1 = -2.0 * cosw0;
  const double a2 = 1.0 - alpha;

  return normalize(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs design_notch(double fs_hz, double f0_hz, double Q) {
  validate_design_inputs(fs_hz, f0_hz, Q, "design_notch");

  const double w0 = 2.0 * kPi * (f0_hz / fs_hz);
  const double cosw0 = std::cos(w0);
  const double sinw0 = std::sin(w0);
  const double alpha = sinw0 / (2.0 * Q);

  const double b0 = 1.0;
  const double b1 = -2.0 * cosw0;
  const double b2 = 1.0;
  const double a0 = 1.0 + alpha;
  const double a1 = -2.0 * cosw0;
  const double a2 = 1.0 - alpha;

  return normalize(b0, b1, b2, a0, a1, a2);
}

static void reflect_pad(const std::vector<float>& x, size_t padlen, std::vector<float>* out) {
  if (!out) return;
  out->clear();
  const size_t n = x.size();
  if (n == 0) return;

  padlen = std::min(padlen, (n > 0 ? n - 1 : 0));
  out->reserve(n + 2 * padlen);

  // Left pad: x[padlen], x[padlen-1], ..., x[1]
  for (size_t i = 0; i < padlen; ++i) {
    out->push_back(x[padlen - i]);
  }

  // Original
  out->insert(out->end(), x.begin(), x.end());

  // Right pad: x[n-2], x[n-3], ..., x[n-1-padlen]
  for (size_t i = 0; i < padlen; ++i) {
    out->push_back(x[n - 2 - i]);
  }
}

void filtfilt_inplace(std::vector<float>* x,
                      const std::vector<BiquadCoeffs>& stages,
                      size_t padlen) {
  if (!x) return;
  const size_t n = x->size();
  if (n < 2) return;
  if (stages.empty()) return;

  const size_t max_pad = n - 1;
  const size_t default_pad = 6 * stages.size();
  if (padlen == 0) {
    padlen = std::min(default_pad, max_pad);
  } else {
    padlen = std::min(padlen, max_pad);
  }

  std::vector<float> xp;
  reflect_pad(*x, padlen, &xp);

  // Forward
  BiquadChain chain(stages);
  chain.reset();
  chain.process_inplace(&xp);

  // Backward
  std::reverse(xp.begin(), xp.end());
  chain.reset();
  chain.process_inplace(&xp);
  std::reverse(xp.begin(), xp.end());

  // Unpad
  for (size_t i = 0; i < n; ++i) {
    (*x)[i] = xp[i + padlen];
  }
}

} // namespace qeeg
