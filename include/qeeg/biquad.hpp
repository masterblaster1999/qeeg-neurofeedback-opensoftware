#pragma once

#include <cstddef>
#include <vector>

namespace qeeg {

// Normalized biquad coefficients for Direct Form II Transposed:
//
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
//
// with a0 assumed to be 1.0 (i.e., b* and a* already divided by a0).
struct BiquadCoeffs {
  double b0{1.0};
  double b1{0.0};
  double b2{0.0};
  double a1{0.0};
  double a2{0.0};
};

class Biquad {
public:
  Biquad() = default;
  explicit Biquad(const BiquadCoeffs& c);

  void set_coeffs(const BiquadCoeffs& c);
  const BiquadCoeffs& coeffs() const { return c_; }

  void reset();

  // Process one sample.
  float process(float x);

private:
  BiquadCoeffs c_{};
  double z1_{0.0};
  double z2_{0.0};
};

// A small cascade of biquad filters.
class BiquadChain {
public:
  BiquadChain() = default;
  explicit BiquadChain(const std::vector<BiquadCoeffs>& stages);

  void add_stage(const BiquadCoeffs& c);
  void reset();

  size_t n_stages() const { return stages_.size(); }
  bool empty() const { return stages_.empty(); }

  float process(float x);
  void process_inplace(std::vector<float>* x);

  std::vector<BiquadCoeffs> stage_coeffs() const;

private:
  std::vector<Biquad> stages_;
};

// Design helpers (RBJ-style biquad cookbook forms).
BiquadCoeffs design_lowpass(double fs_hz, double f0_hz, double Q);
BiquadCoeffs design_highpass(double fs_hz, double f0_hz, double Q);
BiquadCoeffs design_notch(double fs_hz, double f0_hz, double Q);

// Forward-backward filtering ("filtfilt"-style) using a cascade of biquads.
//
// - Applies the cascade forward, then reverses the signal and applies the same
//   cascade again.
// - Produces approximately zero phase distortion.
// - Uses simple reflection padding to reduce edge transients.
//
// padlen:
// - If 0, a conservative default based on #stages is used.
void filtfilt_inplace(std::vector<float>* x,
                      const std::vector<BiquadCoeffs>& stages,
                      size_t padlen = 0);

} // namespace qeeg
