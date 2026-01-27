#include "qeeg/interpolate.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

static bool approx(double a, double b, double eps) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  Montage montage = Montage::builtin_standard_1020_19();

  EEGRecording rec;
  rec.fs_hz = 100.0;
  const size_t n_samp = 200;

  // Choose channels that exist in the builtin montage.
  rec.channel_names = {"Fp1", "Fp2", "C3", "C4", "Pz", "Fz"};
  rec.data.resize(rec.channel_names.size());
  for (auto& ch : rec.data) ch.assign(n_samp, 0.0f);

  std::mt19937 rng(123);
  std::uniform_real_distribution<double> U(-50.0, 50.0);

  // Fill good channels with random data.
  for (size_t ch = 0; ch < rec.n_channels(); ++ch) {
    for (size_t i = 0; i < n_samp; ++i) {
      rec.data[ch][i] = static_cast<float>(U(rng));
    }
  }

  // Mark Fz as bad and zero it out.
  const size_t bad_idx = 5;
  for (size_t i = 0; i < n_samp; ++i) rec.data[bad_idx][i] = 0.0f;

  const std::vector<size_t> bad = {bad_idx};

  InterpolateOptions opt;
  opt.spline.n_terms = 60;
  opt.spline.m = 4;
  opt.spline.lambda = 1e-8;

  const InterpolateReport rep = interpolate_bad_channels_spherical_spline(&rec, montage, bad, opt);
  assert(rep.interpolated.size() == 1);
  assert(rep.interpolated[0] == bad_idx);

  // Reconstruct the expected interpolation using the same good channel list/order.
  std::vector<size_t> good_idx;
  std::vector<Vec3> good_pos;
  for (size_t ch = 0; ch < rec.n_channels(); ++ch) {
    if (ch == bad_idx) continue;
    Vec2 p2;
    const bool ok = montage.get(rec.channel_names[ch], &p2);
    assert(ok);
    good_idx.push_back(ch);
    good_pos.push_back(project_to_unit_sphere(p2));
  }
  Vec2 q2;
  assert(montage.get(rec.channel_names[bad_idx], &q2));
  const Vec3 q = project_to_unit_sphere(q2);
  const std::vector<double> w = spherical_spline_weights(good_pos, q, opt.spline);
  assert(w.size() == good_idx.size());

  for (size_t i = 0; i < n_samp; ++i) {
    double expected = 0.0;
    for (size_t k = 0; k < good_idx.size(); ++k) {
      expected += w[k] * static_cast<double>(rec.data[good_idx[k]][i]);
    }
    const double got = static_cast<double>(rec.data[bad_idx][i]);
    assert(approx(got, expected, 1e-4));
  }

  std::cout << "test_interpolate OK\n";
  return 0;
}
