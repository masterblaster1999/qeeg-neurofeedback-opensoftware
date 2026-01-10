#include "qeeg/interpolate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace qeeg {
namespace {

static std::unordered_set<size_t> to_set(const std::vector<size_t>& v) {
  std::unordered_set<size_t> s;
  s.reserve(v.size());
  for (size_t x : v) s.insert(x);
  return s;
}

} // namespace

InterpolateReport interpolate_bad_channels_spherical_spline(EEGRecording* rec,
                                                           const Montage& montage,
                                                           const std::vector<size_t>& bad_indices,
                                                           const InterpolateOptions& opt) {
  if (!rec) throw std::runtime_error("interpolate_bad_channels_spherical_spline: rec is null");
  if (rec->fs_hz <= 0.0) throw std::runtime_error("interpolate_bad_channels_spherical_spline: invalid sampling rate");
  if (rec->n_channels() == 0 || rec->n_samples() == 0) {
    throw std::runtime_error("interpolate_bad_channels_spherical_spline: empty recording");
  }

  InterpolateReport rep;

  const size_t n_ch = rec->n_channels();
  const size_t n_samp = rec->n_samples();
  const std::unordered_set<size_t> bad = to_set(bad_indices);

  // Build list of good channels that have montage positions.
  std::vector<Vec3> good_pos;
  std::vector<size_t> good_idx;
  good_pos.reserve(n_ch);
  good_idx.reserve(n_ch);

  for (size_t ch = 0; ch < n_ch; ++ch) {
    if (bad.find(ch) != bad.end()) continue;
    Vec2 p2;
    if (!montage.get(rec->channel_names[ch], &p2)) continue;
    good_idx.push_back(ch);
    good_pos.push_back(project_to_unit_sphere(p2));
  }

  rep.good_used = good_idx;

  if (good_idx.size() < 3) {
    // Not enough points to fit a spline.
    for (size_t ch : bad_indices) {
      if (ch < n_ch) rep.skipped_not_enough_good.push_back(ch);
    }
    return rep;
  }

  // Interpolate each bad channel, if it has a position.
  for (size_t bch : bad_indices) {
    if (bch >= n_ch) continue;

    Vec2 q2;
    if (!montage.get(rec->channel_names[bch], &q2)) {
      rep.skipped_no_position.push_back(bch);
      continue;
    }
    const Vec3 q = project_to_unit_sphere(q2);

    const std::vector<double> w = spherical_spline_weights(good_pos, q, opt.spline);
    if (w.size() != good_idx.size()) {
      throw std::runtime_error("interpolate_bad_channels_spherical_spline: weight size mismatch");
    }

    // Apply weights to each time sample.
    auto& y = rec->data[bch];
    if (y.size() != n_samp) {
      throw std::runtime_error("interpolate_bad_channels_spherical_spline: channel length mismatch");
    }

    for (size_t t = 0; t < n_samp; ++t) {
      double acc = 0.0;
      for (size_t i = 0; i < good_idx.size(); ++i) {
        acc += w[i] * static_cast<double>(rec->data[good_idx[i]][t]);
      }
      y[t] = static_cast<float>(acc);
    }

    rep.interpolated.push_back(bch);
  }

  // Keep report lists deterministic.
  std::sort(rep.interpolated.begin(), rep.interpolated.end());
  std::sort(rep.skipped_no_position.begin(), rep.skipped_no_position.end());
  std::sort(rep.skipped_not_enough_good.begin(), rep.skipped_not_enough_good.end());
  return rep;
}

} // namespace qeeg
