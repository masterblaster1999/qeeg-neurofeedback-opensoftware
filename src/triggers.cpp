#include "qeeg/triggers.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qeeg {

namespace {

static bool is_trigger_like_name(const std::string& name_key) {
  // name_key is expected to be normalize_channel_name() output (lowercase, alnum-only).
  if (name_key.empty()) return false;

  // Common trigger/stim labels.
  if (starts_with(name_key, "trig") || starts_with(name_key, "trigger")) return true;
  if (starts_with(name_key, "stim") || starts_with(name_key, "sti")) return true;
  if (starts_with(name_key, "marker") || starts_with(name_key, "event")) return true;
  if (starts_with(name_key, "status")) return true;
  if (starts_with(name_key, "din") || starts_with(name_key, "digital")) return true;
  return false;
}

static bool is_status_like_name(const std::string& name_key) {
  if (name_key.empty()) return false;
  if (starts_with(name_key, "status")) return true;
  return false;
}

struct DiscreteStats {
  double near_integer_frac{0.0};
  double nonzero_frac{0.0};
  std::size_t unique_count{0};
  int min_code{0};
  int max_code{0};
};

static DiscreteStats compute_discrete_stats(const std::vector<float>& x,
                                            std::uint32_t mask,
                                            double zero_epsilon,
                                            std::size_t max_samples) {
  DiscreteStats s;
  if (x.empty()) return s;

  const std::size_t n = x.size();
  const std::size_t step = std::max<std::size_t>(1, n / std::max<std::size_t>(1, max_samples));

  std::size_t n_finite = 0;
  std::size_t n_near_int = 0;
  std::size_t n_nonzero = 0;

  std::unordered_set<int> uniq;
  uniq.reserve(128);

  int min_code = std::numeric_limits<int>::max();
  int max_code = std::numeric_limits<int>::min();

  for (std::size_t i = 0; i < n; i += step) {
    const double v = static_cast<double>(x[i]);
    if (!std::isfinite(v)) continue;

    ++n_finite;

    const double r = std::round(v);
    // Integer-valued channels should be exactly representable for common 16-bit and 24-bit ranges.
    if (std::fabs(v - r) <= 1e-3) {
      ++n_near_int;
    }

    long long code_ll = static_cast<long long>(r);
    if (mask != 0) {
      code_ll &= static_cast<long long>(mask);
    }
    int code = static_cast<int>(code_ll);
    if (std::fabs(static_cast<double>(code)) > zero_epsilon) {
      ++n_nonzero;
    }

    min_code = std::min(min_code, code);
    max_code = std::max(max_code, code);

    if (uniq.size() <= 2048) {
      uniq.insert(code);
    }
  }

  if (n_finite == 0) return s;

  s.near_integer_frac = static_cast<double>(n_near_int) / static_cast<double>(n_finite);
  s.nonzero_frac = static_cast<double>(n_nonzero) / static_cast<double>(n_finite);
  s.unique_count = uniq.size();

  if (min_code == std::numeric_limits<int>::max()) {
    min_code = 0;
    max_code = 0;
  }
  s.min_code = min_code;
  s.max_code = max_code;
  return s;
}

static double score_trigger_candidate(const DiscreteStats& s) {
  // Higher is better. We want:
  // - integer-like values
  // - sparse non-zero codes
  // - limited number of unique codes
  // - non-trivial range
  const double range = static_cast<double>(s.max_code - s.min_code);
  if (range <= 0.0) return 0.0;
  if (s.near_integer_frac < 0.98) return 0.0;
  if (s.unique_count > 1024) return 0.0;
  if (s.unique_count < 2) return 0.0;

  const double sparsity = 1.0 - std::min(1.0, s.nonzero_frac);
  const double uniq_penalty = std::log(static_cast<double>(s.unique_count) + 2.0);
  return (s.near_integer_frac * sparsity * std::log(range + 1.0)) / uniq_penalty;
}

static std::uint32_t default_mask_for_channel(const std::string& name_key,
                                              const TriggerExtractionOptions& opt) {
  if (opt.mask != 0) return opt.mask;
  if (opt.auto_status_mask_16bit && is_status_like_name(name_key)) {
    // Many BDF recordings (e.g., BioSemi) store trigger codes in the lower 16 bits
    // of a 24-bit Status word.
    return 0xFFFFu;
  }
  return 0;
}

static std::vector<AnnotationEvent> extract_segments(const std::vector<float>& x,
                                                     double fs_hz,
                                                     std::uint32_t mask,
                                                     double zero_epsilon,
                                                     bool ignore_zero,
                                                     double min_interval_sec) {
  std::vector<AnnotationEvent> out;
  if (x.empty() || fs_hz <= 0.0) return out;

  auto decode = [&](float fv) -> int {
    const double v = static_cast<double>(fv);
    if (!std::isfinite(v)) return 0;
    const double r = std::round(v);
    long long code_ll = static_cast<long long>(r);
    if (mask != 0) {
      code_ll &= static_cast<long long>(mask);
    }
    int code = static_cast<int>(code_ll);
    if (std::fabs(static_cast<double>(code)) <= zero_epsilon) return 0;
    return code;
  };

  // Debounce by code.
  // We keep only a small fixed number of recent codes to avoid unbounded memory.
  struct Recent {
    int code;
    double t;
  };
  std::vector<Recent> recent;
  recent.reserve(32);

  auto seen_recent = [&](int code, double t) -> bool {
    if (min_interval_sec <= 0.0) return false;
    for (const auto& r : recent) {
      if (r.code == code && (t - r.t) < min_interval_sec) return true;
    }
    return false;
  };

  auto push_recent = [&](int code, double t) {
    if (min_interval_sec <= 0.0) return;
    if (recent.size() < 32) {
      recent.push_back({code, t});
      return;
    }
    // Replace the oldest.
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < recent.size(); ++i) {
      if (recent[i].t < recent[oldest].t) oldest = i;
    }
    recent[oldest] = {code, t};
  };

  // Convert constant-code runs into events.
  // To preserve backwards-compatible behavior with the prior "edge" extractor,
  // we do NOT emit an event for the initial segment starting at sample 0.
  int prev = decode(x[0]);
  std::size_t seg_start = 0;

  auto push_segment_event = [&](int code, std::size_t start, std::size_t end) {
    if (start == 0) return; // only emit on transitions (not initial state)
    if (ignore_zero && code == 0) return;
    if (end <= start) return;

    const double t = static_cast<double>(start) / fs_hz;
    if (seen_recent(code, t)) return;
    push_recent(code, t);

    AnnotationEvent ev;
    ev.onset_sec = t;
    ev.duration_sec = static_cast<double>(end - start) / fs_hz;
    ev.text = std::to_string(code);
    out.push_back(std::move(ev));
  };

  for (std::size_t i = 1; i < x.size(); ++i) {
    const int cur = decode(x[i]);
    if (cur == prev) continue;

    // Close previous segment [seg_start, i).
    push_segment_event(prev, seg_start, i);

    // Start new segment.
    prev = cur;
    seg_start = i;
  }

  // Close final segment.
  push_segment_event(prev, seg_start, x.size());

  return out;
}

} // namespace

TriggerExtractionResult extract_events_from_trigger_channel(const EEGRecording& rec,
                                                           const TriggerExtractionOptions& opt) {
  if (rec.channel_names.size() != rec.data.size()) {
    throw std::runtime_error("extract_events_from_trigger_channel: channel_names/data size mismatch");
  }

  std::size_t idx = static_cast<std::size_t>(-1);
  std::string used;
  std::string key;
  std::uint32_t mask = 0;

  if (!opt.channel_name.empty()) {
    const std::string want = normalize_channel_name(opt.channel_name);
    for (std::size_t i = 0; i < rec.channel_names.size(); ++i) {
      const std::string k = normalize_channel_name(rec.channel_names[i]);
      if (!k.empty() && k == want) {
        idx = i;
        used = rec.channel_names[i];
        key = k;
        break;
      }
    }
    if (idx == static_cast<std::size_t>(-1)) {
      throw std::runtime_error("Trigger channel not found: " + opt.channel_name);
    }
    mask = default_mask_for_channel(key, opt);
  } else {
    // Auto: choose best trigger-like candidate.
    double best_score = 0.0;
    std::size_t best_idx = static_cast<std::size_t>(-1);
    std::string best_used;
    std::string best_key;
    std::uint32_t best_mask = 0;

    for (std::size_t i = 0; i < rec.channel_names.size(); ++i) {
      const std::string k = normalize_channel_name(rec.channel_names[i]);
      if (!is_trigger_like_name(k)) continue;
      const std::uint32_t m = default_mask_for_channel(k, opt);
      const DiscreteStats st = compute_discrete_stats(rec.data[i], m, opt.zero_epsilon, 20000);
      const double sc = score_trigger_candidate(st);
      if (sc > best_score) {
        best_score = sc;
        best_idx = i;
        best_used = rec.channel_names[i];
        best_key = k;
        best_mask = m;
      }
    }

    // If no trigger-like names matched, do a very conservative fallback scan for discrete sparse channels.
    if (best_idx == static_cast<std::size_t>(-1)) {
      for (std::size_t i = 0; i < rec.channel_names.size(); ++i) {
        const std::string k = normalize_channel_name(rec.channel_names[i]);
        // Skip empty names.
        if (k.empty()) continue;
        // Avoid over-triggering on typical EEG names.
        // Only consider channels whose names strongly suggest non-EEG.
        if (starts_with(k, "aux") || starts_with(k, "misc")) {
          const DiscreteStats st = compute_discrete_stats(rec.data[i], /*mask=*/0, opt.zero_epsilon, 20000);
          const double sc = score_trigger_candidate(st);
          if (sc > best_score) {
            best_score = sc;
            best_idx = i;
            best_used = rec.channel_names[i];
            best_key = k;
            best_mask = default_mask_for_channel(k, opt);
          }
        }
      }
    }

    idx = best_idx;
    used = best_used;
    key = best_key;
    mask = best_mask;

    // For auto path, if best_score is very low, treat as none.
    if (idx == static_cast<std::size_t>(-1) || best_score <= 0.0) {
      return TriggerExtractionResult{};
    }
  }


  TriggerExtractionResult res;
  res.used_channel = used;
  res.events = extract_segments(rec.data[idx], rec.fs_hz, mask, opt.zero_epsilon, opt.ignore_zero,
                             opt.min_event_interval_sec);
  return res;
}

TriggerExtractionResult extract_events_from_triggers_auto(const EEGRecording& rec,
                                                         const TriggerExtractionOptions& opt) {
  TriggerExtractionOptions o = opt;
  o.channel_name.clear();
  return extract_events_from_trigger_channel(rec, o);
}

} // namespace qeeg
