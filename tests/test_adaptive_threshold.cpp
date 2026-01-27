#include "qeeg/adaptive_threshold.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>

using namespace qeeg;

static bool near(double a, double b, double tol = 1e-9) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::fabs(a - b) <= tol;
}

int main() {
  // Parse modes.
  assert(parse_adapt_mode("exp") == AdaptMode::Exponential);
  assert(parse_adapt_mode("exponential") == AdaptMode::Exponential);
  assert(parse_adapt_mode("quantile") == AdaptMode::Quantile);
  assert(parse_adapt_mode("q") == AdaptMode::Quantile);

  // Quantile mode: with values 0..9, target_rate=0.5 and Above => q=0.5 => median 4.5
  {
    AdaptiveThresholdConfig cfg;
    cfg.mode = AdaptMode::Quantile;
    cfg.reward_direction = RewardDirection::Above;
    cfg.target_reward_rate = 0.5;
    cfg.eta = 1.0;  // immediate
    cfg.quantile_window_seconds = 100.0;
    cfg.quantile_min_samples = 1;
    AdaptiveThresholdController ctl(cfg);

    for (int i = 0; i < 10; ++i) {
      ctl.observe(static_cast<double>(i), static_cast<double>(i));
    }
    const double thr = ctl.update(/*current_threshold=*/0.0, /*reward_rate=*/0.0, /*t_end_sec=*/9.0);
    assert(near(thr, 4.5));
  }

  // Quantile mode with pruning: keep last 5 seconds (times 5..9 => values 5..9 => median 7)
  {
    AdaptiveThresholdConfig cfg;
    cfg.mode = AdaptMode::Quantile;
    cfg.reward_direction = RewardDirection::Above;
    cfg.target_reward_rate = 0.5;
    cfg.eta = 1.0;
    cfg.quantile_window_seconds = 5.0;
    cfg.quantile_min_samples = 1;
    AdaptiveThresholdController ctl(cfg);

    for (int i = 0; i < 10; ++i) {
      ctl.observe(static_cast<double>(i), static_cast<double>(i));
    }
    const double thr = ctl.update(0.0, 0.0, 9.0);
    assert(near(thr, 7.0));
  }

  // Update interval: skip updates until enough time has passed.
  {
    AdaptiveThresholdConfig cfg;
    cfg.mode = AdaptMode::Quantile;
    cfg.reward_direction = RewardDirection::Above;
    cfg.target_reward_rate = 0.5;
    cfg.eta = 1.0;
    cfg.update_interval_seconds = 10.0;
    cfg.quantile_window_seconds = 100.0;
    cfg.quantile_min_samples = 1;
    AdaptiveThresholdController ctl(cfg);

    for (int i = 0; i < 10; ++i) ctl.observe(static_cast<double>(i), static_cast<double>(i));
    const double thr1 = ctl.update(0.0, 0.0, 9.0);
    assert(near(thr1, 4.5));

    // Push an extreme new value that would change the median, but interval prevents update.
    ctl.observe(10.0, 1000.0);
    const double thr2 = ctl.update(thr1, 0.0, 12.0);  // dt=3 < 10
    assert(near(thr2, thr1));

    // Now allow update.
    const double thr3 = ctl.update(thr2, 0.0, 20.0);
    assert(!near(thr3, thr1));
  }

  std::cout << "test_adaptive_threshold: OK\n";
  return 0;
}
