#include "qeeg/microstates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

namespace qeeg {
namespace {

static bool demean_and_normalize(std::vector<double>* v, bool demean) {
  if (!v || v->empty()) return false;

  if (demean) {
    double m = 0.0;
    for (double x : *v) m += x;
    m /= static_cast<double>(v->size());
    for (double& x : *v) x -= m;
  }

  double n2 = 0.0;
  for (double x : *v) n2 += x * x;
  if (!(n2 > 0.0)) return false;

  double n = std::sqrt(n2);
  if (!std::isfinite(n) || n < 1e-12) return false;
  for (double& x : *v) x /= n;
  return true;
}

static double dot_unit(const std::vector<double>& a, const std::vector<double>& b) {
  // Assumes equal size.
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    s += a[i] * b[i];
  }
  return s;
}

static double dist2_unit(const std::vector<double>& x,
                         const std::vector<double>& c,
                         bool polarity_invariant) {
  double d = dot_unit(x, c);
  if (polarity_invariant) d = std::fabs(d);
  // For unit vectors: ||x - c||^2 = 2 - 2*dot(x,c)
  // With polarity invariance: use |dot|.
  return 2.0 - 2.0 * d;
}

static std::vector<size_t> find_gfp_peaks_raw(const std::vector<double>& gfp) {
  std::vector<size_t> peaks;
  if (gfp.size() < 3) return peaks;
  for (size_t i = 1; i + 1 < gfp.size(); ++i) {
    if (gfp[i] > gfp[i - 1] && gfp[i] >= gfp[i + 1]) {
      peaks.push_back(i);
    }
  }
  return peaks;
}

static std::vector<size_t> enforce_min_distance(const std::vector<size_t>& peaks,
                                                const std::vector<double>& gfp,
                                                size_t min_dist) {
  if (min_dist == 0 || peaks.empty()) return peaks;

  // Greedy by highest GFP: common in peak thinning.
  std::vector<size_t> order = peaks;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return gfp[a] > gfp[b];
  });

  std::vector<size_t> kept;
  kept.reserve(order.size());

  for (size_t idx : order) {
    bool ok = true;
    for (size_t j : kept) {
      const size_t d = (idx > j) ? (idx - j) : (j - idx);
      if (d < min_dist) {
        ok = false;
        break;
      }
    }
    if (ok) kept.push_back(idx);
  }

  std::sort(kept.begin(), kept.end());
  return kept;
}

static std::vector<size_t> pick_top_fraction(const std::vector<size_t>& peaks,
                                             const std::vector<double>& gfp,
                                             double frac,
                                             size_t max_peaks,
                                             size_t min_keep) {
  if (peaks.empty()) return peaks;
  if (frac <= 0.0) frac = 1.0;
  if (frac > 1.0) frac = 1.0;

  size_t want = static_cast<size_t>(std::ceil(frac * static_cast<double>(peaks.size())));
  if (want < min_keep) want = min_keep;
  if (want > peaks.size()) want = peaks.size();
  if (max_peaks > 0 && want > max_peaks) want = max_peaks;

  std::vector<size_t> order = peaks;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return gfp[a] > gfp[b];
  });
  order.resize(want);
  std::sort(order.begin(), order.end());
  return order;
}

static std::vector<std::vector<double>> extract_peak_topographies(const EEGRecording& rec,
                                                                  const std::vector<size_t>& peak_idx,
                                                                  bool demean) {
  const size_t C = rec.n_channels();
  std::vector<std::vector<double>> X;
  X.reserve(peak_idx.size());

  for (size_t t : peak_idx) {
    if (t >= rec.n_samples()) continue;
    std::vector<double> v(C, 0.0);
    for (size_t c = 0; c < C; ++c) {
      v[c] = static_cast<double>(rec.data[c][t]);
    }
    if (!demean_and_normalize(&v, demean)) continue;
    X.push_back(std::move(v));
  }
  return X;
}

static std::vector<std::vector<double>> kmeans_templates(const std::vector<std::vector<double>>& X,
                                                         int k,
                                                         bool polarity_invariant,
                                                         bool demean_templates,
                                                         int max_iter,
                                                         double tol,
                                                         unsigned seed) {
  if (k <= 0) throw std::runtime_error("kmeans_templates: k must be > 0");
  if (X.empty()) throw std::runtime_error("kmeans_templates: no samples");

  const size_t N = X.size();
  const size_t D = X[0].size();
  for (const auto& row : X) {
    if (row.size() != D) throw std::runtime_error("kmeans_templates: inconsistent dimensions");
  }

  if (static_cast<size_t>(k) > N) k = static_cast<int>(N);

  std::mt19937 rng(seed);
  std::uniform_int_distribution<size_t> uni_idx(0, N - 1);

  // --- k-means++ initialization (polarity-aware distance) ---
  std::vector<std::vector<double>> centroids;
  centroids.reserve(static_cast<size_t>(k));
  centroids.push_back(X[uni_idx(rng)]);

  std::vector<double> dist2(N, 0.0);
  for (int c = 1; c < k; ++c) {
    double sum = 0.0;
    for (size_t i = 0; i < N; ++i) {
      double best = std::numeric_limits<double>::infinity();
      for (const auto& cen : centroids) {
        best = std::min(best, dist2_unit(X[i], cen, polarity_invariant));
      }
      dist2[i] = best;
      sum += best;
    }

    if (!(sum > 0.0) || !std::isfinite(sum)) {
      centroids.push_back(X[uni_idx(rng)]);
      continue;
    }

    std::uniform_real_distribution<double> uni(0.0, sum);
    double r = uni(rng);
    double acc = 0.0;
    size_t pick = 0;
    for (size_t i = 0; i < N; ++i) {
      acc += dist2[i];
      if (acc >= r) { pick = i; break; }
    }
    centroids.push_back(X[pick]);
  }

  // Ensure centroids are properly normalized (should already be, but safe).
  for (auto& c : centroids) {
    demean_and_normalize(&c, demean_templates);
  }

  std::vector<int> labels(N, -1);
  std::vector<int> signs(N, 1);

  for (int iter = 0; iter < max_iter; ++iter) {
    bool any_change = false;

    // Assignment step.
    for (size_t i = 0; i < N; ++i) {
      double best_d2 = std::numeric_limits<double>::infinity();
      int best_k = 0;
      int best_s = 1;
      for (int j = 0; j < k; ++j) {
        double d = dot_unit(X[i], centroids[static_cast<size_t>(j)]);
        int s = 1;
        if (polarity_invariant && d < 0.0) {
          s = -1;
          d = -d;
        }
        double d2 = 2.0 - 2.0 * d;
        if (d2 < best_d2) {
          best_d2 = d2;
          best_k = j;
          best_s = s;
        }
      }

      if (labels[i] != best_k || signs[i] != best_s) {
        labels[i] = best_k;
        signs[i] = best_s;
        any_change = true;
      }
    }

    // Update step.
    std::vector<std::vector<double>> new_centroids(static_cast<size_t>(k), std::vector<double>(D, 0.0));
    std::vector<int> counts(static_cast<size_t>(k), 0);
    for (size_t i = 0; i < N; ++i) {
      int lab = labels[i];
      if (lab < 0 || lab >= k) continue;
      const int s = signs[i];
      auto& acc = new_centroids[static_cast<size_t>(lab)];
      if (polarity_invariant && s < 0) {
        for (size_t d = 0; d < D; ++d) acc[d] -= X[i][d];
      } else {
        for (size_t d = 0; d < D; ++d) acc[d] += X[i][d];
      }
      counts[static_cast<size_t>(lab)] += 1;
    }

    for (int j = 0; j < k; ++j) {
      auto& c = new_centroids[static_cast<size_t>(j)];
      int cnt = counts[static_cast<size_t>(j)];
      if (cnt <= 0) {
        c = X[uni_idx(rng)];
      } else {
        const double inv = 1.0 / static_cast<double>(cnt);
        for (double& x : c) x *= inv;
      }
      demean_and_normalize(&c, demean_templates);
    }

    // Convergence check.
    double max_shift = 0.0;
    for (int j = 0; j < k; ++j) {
      const auto& old_c = centroids[static_cast<size_t>(j)];
      auto new_c = new_centroids[static_cast<size_t>(j)];

      // Align sign for shift computation under polarity invariance.
      if (polarity_invariant) {
        double d = dot_unit(old_c, new_c);
        if (d < 0.0) {
          for (double& x : new_c) x = -x;
        }
      }

      double s2 = 0.0;
      for (size_t d = 0; d < D; ++d) {
        double diff = new_c[d] - old_c[d];
        s2 += diff * diff;
      }
      max_shift = std::max(max_shift, std::sqrt(s2));
    }

    centroids.swap(new_centroids);

    if (!any_change) break;
    if (max_shift < tol) break;
  }

  return centroids;
}

static void smooth_min_duration(std::vector<int>* labels, int min_len) {
  if (!labels) return;
  const int n = static_cast<int>(labels->size());
  if (n <= 0) return;
  if (min_len <= 1) return;

  bool changed = true;
  int guard = 0;
  while (changed && guard++ < 10) {
    changed = false;
    int i = 0;
    while (i < n) {
      int lab = (*labels)[i];
      int j = i + 1;
      while (j < n && (*labels)[j] == lab) ++j;
      int len = j - i;

      if (lab >= 0 && len < min_len) {
        int new_lab = lab;
        if (i == 0) {
          new_lab = (j < n) ? (*labels)[j] : lab;
        } else if (j >= n) {
          new_lab = (*labels)[i - 1];
        } else {
          // Choose the neighbor with the longer run (tie => previous).
          int prev_lab = (*labels)[i - 1];
          int next_lab = (*labels)[j];

          int prev_len = 0;
          for (int p = i - 1; p >= 0 && (*labels)[p] == prev_lab; --p) prev_len++;

          int next_len = 0;
          for (int q = j; q < n && (*labels)[q] == next_lab; ++q) next_len++;

          new_lab = (next_len > prev_len) ? next_lab : prev_lab;
        }

        if (new_lab != lab && new_lab >= 0) {
          for (int t = i; t < j; ++t) (*labels)[t] = new_lab;
          changed = true;
        }
      }

      i = j;
    }
  }
}

} // namespace

std::vector<double> compute_gfp(const EEGRecording& rec) {
  const size_t C = rec.n_channels();
  const size_t N = rec.n_samples();

  std::vector<double> gfp(N, 0.0);
  if (C == 0 || N == 0) return gfp;

  for (size_t t = 0; t < N; ++t) {
    double mean = 0.0;
    for (size_t c = 0; c < C; ++c) {
      mean += static_cast<double>(rec.data[c][t]);
    }
    mean /= static_cast<double>(C);

    double var = 0.0;
    for (size_t c = 0; c < C; ++c) {
      double d = static_cast<double>(rec.data[c][t]) - mean;
      var += d * d;
    }
    var /= static_cast<double>(C);
    gfp[t] = std::sqrt(std::max(0.0, var));
  }

  return gfp;
}

MicrostatesResult estimate_microstates(const EEGRecording& rec, const MicrostatesOptions& opt) {
  if (rec.n_channels() < 2) {
    throw std::runtime_error("estimate_microstates: need >= 2 channels");
  }
  if (rec.n_samples() < 3) {
    throw std::runtime_error("estimate_microstates: need >= 3 samples");
  }
  if (opt.k <= 0) {
    throw std::runtime_error("estimate_microstates: k must be > 0");
  }
  if (rec.fs_hz <= 0.0) {
    throw std::runtime_error("estimate_microstates: fs_hz must be > 0");
  }

  MicrostatesResult out;
  out.gfp = compute_gfp(rec);

  // --- GFP peak selection ---
  std::vector<size_t> peaks = find_gfp_peaks_raw(out.gfp);
  peaks = enforce_min_distance(peaks, out.gfp, opt.min_peak_distance_samples);

  // If we have no local maxima (flat GFP), fall back to a uniform subsample.
  if (peaks.empty()) {
    const size_t stride = std::max<size_t>(1, rec.n_samples() / std::max<size_t>(1, opt.max_peaks));
    for (size_t t = 0; t < rec.n_samples(); t += stride) peaks.push_back(t);
  }

  const size_t min_keep = static_cast<size_t>(std::max(1, opt.k));
  peaks = pick_top_fraction(peaks, out.gfp, opt.peak_pick_fraction, opt.max_peaks, min_keep);

  // Extract and normalize peak topographies.
  std::vector<std::vector<double>> X = extract_peak_topographies(rec, peaks, opt.demean_topography);
  if (X.empty()) {
    throw std::runtime_error("estimate_microstates: no usable peak topographies (all zero-norm?)");
  }

  int k = opt.k;
  if (static_cast<size_t>(k) > X.size()) k = static_cast<int>(X.size());

  // --- K-means templates ---
  out.templates = kmeans_templates(X,
                                  k,
                                  opt.polarity_invariant,
                                  opt.demean_topography,
                                  opt.max_iterations,
                                  opt.convergence_tol,
                                  opt.seed);

  // --- Assign every sample ---
  const size_t C = rec.n_channels();
  const size_t N = rec.n_samples();
  out.labels.assign(N, -1);
  out.corr.assign(N, 0.0);

  std::vector<double> topo(C, 0.0);
  for (size_t t = 0; t < N; ++t) {
    for (size_t c = 0; c < C; ++c) topo[c] = static_cast<double>(rec.data[c][t]);
    if (!demean_and_normalize(&topo, opt.demean_topography)) {
      out.labels[t] = -1;
      out.corr[t] = 0.0;
      continue;
    }

    double best_absdot = -1.0;
    int best_k = 0;
    for (int j = 0; j < k; ++j) {
      double d = dot_unit(topo, out.templates[static_cast<size_t>(j)]);
      if (opt.polarity_invariant) d = std::fabs(d);
      if (d > best_absdot) {
        best_absdot = d;
        best_k = j;
      }
    }
    out.labels[t] = best_k;
    out.corr[t] = std::max(0.0, std::min(1.0, best_absdot));
  }

  // Optional temporal smoothing.
  if (opt.min_segment_samples > 1) {
    smooth_min_duration(&out.labels, opt.min_segment_samples);
  }

  // --- Stats ---
  out.coverage.assign(static_cast<size_t>(k), 0.0);
  out.mean_duration_sec.assign(static_cast<size_t>(k), 0.0);
  out.occurrence_per_sec.assign(static_cast<size_t>(k), 0.0);
  out.transition_counts.assign(static_cast<size_t>(k), std::vector<int>(static_cast<size_t>(k), 0));

  std::vector<size_t> sample_counts(static_cast<size_t>(k), 0);
  for (int lab : out.labels) {
    if (lab >= 0 && lab < k) sample_counts[static_cast<size_t>(lab)] += 1;
  }
  const double total_samples = static_cast<double>(out.labels.size());
  for (int j = 0; j < k; ++j) {
    out.coverage[static_cast<size_t>(j)] = sample_counts[static_cast<size_t>(j)] / std::max(1.0, total_samples);
  }

  // Segment-wise stats.
  std::vector<size_t> seg_count(static_cast<size_t>(k), 0);
  std::vector<double> seg_len_sum(static_cast<size_t>(k), 0.0);

  int prev_seg_lab = -1;
  size_t i = 0;
  while (i < N) {
    int lab = out.labels[i];
    size_t j = i + 1;
    while (j < N && out.labels[j] == lab) ++j;
    size_t len = j - i;

    if (lab >= 0 && lab < k) {
      seg_count[static_cast<size_t>(lab)] += 1;
      seg_len_sum[static_cast<size_t>(lab)] += static_cast<double>(len);

      if (prev_seg_lab >= 0 && prev_seg_lab < k) {
        out.transition_counts[static_cast<size_t>(prev_seg_lab)][static_cast<size_t>(lab)] += 1;
      }
      prev_seg_lab = lab;
    }

    i = j;
  }

  const double duration_sec = static_cast<double>(N) / rec.fs_hz;
  for (int j = 0; j < k; ++j) {
    if (seg_count[static_cast<size_t>(j)] > 0) {
      out.mean_duration_sec[static_cast<size_t>(j)] =
          (seg_len_sum[static_cast<size_t>(j)] / static_cast<double>(seg_count[static_cast<size_t>(j)])) / rec.fs_hz;
      out.occurrence_per_sec[static_cast<size_t>(j)] =
          static_cast<double>(seg_count[static_cast<size_t>(j)]) / std::max(1e-9, duration_sec);
    }
  }

  // Global explained variance (GEV) and per-state contributions.
  //
  // Global:    sum_t GFP(t)^2 * corr(t)^2 / sum_t GFP(t)^2
  // Per-state: sum_{t in state k} GFP(t)^2 * corr(t)^2 / sum_t GFP(t)^2
  std::vector<double> num_state(static_cast<size_t>(k), 0.0);
  double num = 0.0;
  double den = 0.0;
  for (size_t t = 0; t < N; ++t) {
    const double w = out.gfp[t] * out.gfp[t];
    den += w;
    const double c = out.corr[t];
    const double contrib = w * c * c;
    num += contrib;

    const int lab = out.labels[t];
    if (lab >= 0 && lab < k) {
      num_state[static_cast<size_t>(lab)] += contrib;
    }
  }
  out.gev = (den > 0.0) ? (num / den) : 0.0;

  out.gev_state.assign(static_cast<size_t>(k), 0.0);
  if (den > 0.0) {
    for (int j = 0; j < k; ++j) {
      out.gev_state[static_cast<size_t>(j)] = num_state[static_cast<size_t>(j)] / den;
    }
  }

  return out;
}


std::vector<MicrostateSegment> microstate_segments(const std::vector<int>& labels,
                                                   const std::vector<double>& corr,
                                                   const std::vector<double>& gfp,
                                                   double fs_hz,
                                                   bool include_undefined) {
  if (fs_hz <= 0.0) throw std::runtime_error("microstate_segments: fs_hz must be > 0");
  if (labels.size() != corr.size() || labels.size() != gfp.size()) {
    throw std::runtime_error("microstate_segments: labels, corr, gfp must have the same length");
  }

  std::vector<MicrostateSegment> segs;
  const size_t N = labels.size();
  if (N == 0) return segs;

  size_t i = 0;
  while (i < N) {
    const int lab = labels[i];
    size_t j = i + 1;
    double sum_corr = corr[i];
    double sum_gfp = gfp[i];

    while (j < N && labels[j] == lab) {
      sum_corr += corr[j];
      sum_gfp += gfp[j];
      ++j;
    }

    const size_t len = j - i;
    if (include_undefined || lab >= 0) {
      MicrostateSegment s;
      s.label = lab;
      s.start_sample = i;
      s.end_sample = j;
      s.start_sec = static_cast<double>(i) / fs_hz;
      s.end_sec = static_cast<double>(j) / fs_hz;
      s.duration_sec = static_cast<double>(len) / fs_hz;
      if (len > 0) {
        s.mean_corr = sum_corr / static_cast<double>(len);
        s.mean_gfp = sum_gfp / static_cast<double>(len);
      }
      segs.push_back(s);
    }

    i = j;
  }

  return segs;
}

} // namespace qeeg
