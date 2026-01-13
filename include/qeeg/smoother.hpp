#pragma once

#include <cmath>
#include <limits>

namespace qeeg {

// A tiny exponential moving average (EMA) smoother with a time constant.
//
// This is useful for stabilizing real-time feedback signals (e.g., NF metrics)
// without adding heavy dependencies.
//
// Semantics:
// - If tau_sec <= 0, the smoother is disabled and update(x, dt) returns x.
// - If x is non-finite, update() returns the current value without updating.
// - When enabled, this uses the exact discrete-time update:
//     y <- y + (1 - exp(-dt/tau)) * (x - y)
//   where tau is the time constant in seconds.
class ExponentialSmoother {
public:
  ExponentialSmoother() = default;
  explicit ExponentialSmoother(double tau_sec) { set_time_constant(tau_sec); }

  void reset() {
    has_ = false;
    y_ = std::numeric_limits<double>::quiet_NaN();
  }

  void set_time_constant(double tau_sec) {
    tau_sec_ = tau_sec;
    if (!std::isfinite(tau_sec_) || tau_sec_ <= 0.0) {
      // Disabled mode: keep current value cleared to avoid confusion.
      reset();
      tau_sec_ = 0.0;
    }
  }

  double time_constant() const { return tau_sec_; }
  bool enabled() const { return tau_sec_ > 0.0; }
  bool has_value() const { return has_; }
  double value() const { return y_; }

  // Update the smoother.
  // - dt_sec is the elapsed time since the previous update (seconds).
  // - returns the updated value (or the last value if x is not finite).
  double update(double x, double dt_sec) {
    if (!std::isfinite(x)) return y_;

    // Disabled: pass-through.
    if (!enabled()) {
      has_ = true;
      y_ = x;
      return y_;
    }

    // First value: initialize.
    if (!has_) {
      has_ = true;
      y_ = x;
      return y_;
    }

    const double dt = (std::isfinite(dt_sec) && dt_sec > 0.0) ? dt_sec : 0.0;

    // If dt is 0, treat as an instantaneous update.
    double alpha = 1.0;
    if (dt > 0.0) {
      alpha = 1.0 - std::exp(-dt / tau_sec_);
      if (!std::isfinite(alpha)) alpha = 1.0;
      if (alpha < 0.0) alpha = 0.0;
      if (alpha > 1.0) alpha = 1.0;
    }

    y_ += alpha * (x - y_);
    return y_;
  }

private:
  double tau_sec_{0.0};
  bool has_{false};
  double y_{std::numeric_limits<double>::quiet_NaN()};
};

} // namespace qeeg
