#include "qeeg/microstates.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

using namespace qeeg;

static void expect_true(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "TEST FAIL: " << msg << "\n";
    std::exit(1);
  }
}

static std::vector<double> normalize(std::vector<double> v) {
  double n2 = 0.0;
  for (double x : v) n2 += x * x;
  double n = std::sqrt(std::max(0.0, n2));
  if (n <= 0.0) return v;
  for (double& x : v) x /= n;
  return v;
}

static double abs_dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return std::fabs(s);
}

int main() {
  // Synthetic microstate-like data: 4 repeating spatial templates multiplied by a common oscillation.
  const double fs = 100.0;
  const double f = 10.0;
  const size_t C = 5;
  const size_t seg_len = 200; // samples
  const size_t nseg = 8;
  const size_t N = seg_len * nseg;

  std::vector<std::vector<double>> true_tpl;
  true_tpl.push_back(normalize({ 1,  0,  0, -1,  0}));
  true_tpl.push_back(normalize({ 0,  1,  0,  0, -1}));
  true_tpl.push_back(normalize({ 0.5, 0.5, -1, 0, 0}));
  true_tpl.push_back(normalize({ 1, -1,  0,  0,  0}));

  EEGRecording rec;
  rec.fs_hz = fs;
  rec.channel_names = {"C1","C2","C3","C4","C5"};
  rec.data.assign(C, std::vector<float>(N, 0.0f));

  std::mt19937 rng(123);
  std::normal_distribution<double> noise(0.0, 0.10);
  const double pi = std::acos(-1.0);
  const double A = 5.0;

  for (size_t t = 0; t < N; ++t) {
    int state = static_cast<int>((t / seg_len) % 4);
    double s = std::sin(2.0 * pi * f * (static_cast<double>(t) / fs));
    for (size_t c = 0; c < C; ++c) {
      double x = A * s * true_tpl[static_cast<size_t>(state)][c] + noise(rng);
      rec.data[c][t] = static_cast<float>(x);
    }
  }

  MicrostatesOptions opt;
  opt.k = 4;
  opt.peak_pick_fraction = 0.10;
  opt.max_peaks = 400;
  opt.min_peak_distance_samples = 5;
  opt.demean_topography = true;
  opt.polarity_invariant = true;
  opt.max_iterations = 100;
  opt.convergence_tol = 1e-6;
  opt.seed = 42;

  MicrostatesResult r = estimate_microstates(rec, opt);
  expect_true(r.templates.size() == 4, "expected 4 templates");

  // Match estimated templates to ground truth up to permutation and polarity.
  std::vector<int> used(4, 0);
  for (int i = 0; i < 4; ++i) {
    double best = -1.0;
    int best_j = -1;
    for (int j = 0; j < 4; ++j) {
      if (used[j]) continue;
      double c = abs_dot(true_tpl[static_cast<size_t>(i)], r.templates[static_cast<size_t>(j)]);
      if (c > best) { best = c; best_j = j; }
    }
    expect_true(best_j >= 0, "template matching failed");
    used[best_j] = 1;
    expect_true(best > 0.80, "template correlation too low (expected >0.80)");
  }

  expect_true(r.gev > 0.50, "GEV unexpectedly low");

  std::cout << "test_microstates OK (GEV=" << r.gev << ")\n";
  return 0;
}
