#pragma once

#include "qeeg/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace qeeg {

// Small, dependency-light helpers for working with AnnotationEvent vectors.
//
// Rationale:
// - Multiple tools can generate/consume events (EDF+/BDF+ annotations, CSV/TSV tables,
//   NF-derived segments, etc.).
// - When merging events from multiple sources, duplicates are common (e.g., round-trip exports).
// - We want deterministic ordering + lightweight de-duplication.

namespace detail {

inline std::string trim_ws(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  size_t j = s.size();
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  return s.substr(i, j - i);
}

inline int64_t sec_to_us(double sec) {
  if (!std::isfinite(sec)) return 0;
  // Microsecond quantization is a reasonable compromise:
  // - avoids float equality issues
  // - remains much finer than typical EEG sample periods
  return static_cast<int64_t>(std::llround(sec * 1e6));
}

} // namespace detail

// Sort events deterministically.
//
// Ordering:
//   1) onset (ascending)
//   2) duration (descending)
//        - puts segments (duration > 0) before point/impulse events (duration == 0)
//        - longer segments first when they share the same onset
//   3) text (ascending)
inline void sort_events(std::vector<AnnotationEvent>* events) {
  if (!events) return;
  std::sort(events->begin(), events->end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    const int64_t aon_us = detail::sec_to_us(a.onset_sec);
    const int64_t bon_us = detail::sec_to_us(b.onset_sec);
    if (aon_us != bon_us) return aon_us < bon_us;

    const int64_t adur_us = detail::sec_to_us(a.duration_sec);
    const int64_t bdur_us = detail::sec_to_us(b.duration_sec);
    if (adur_us != bdur_us) return adur_us > bdur_us; // duration DESC

    const std::string at = detail::trim_ws(a.text);
    const std::string bt = detail::trim_ws(b.text);
    return at < bt;
  });
}

// Normalize events in-place:
// - trims text
// - clamps negative/NaN durations to 0
inline void normalize_events(std::vector<AnnotationEvent>* events) {
  if (!events) return;
  for (auto& ev : *events) {
    if (!std::isfinite(ev.onset_sec)) ev.onset_sec = 0.0;
    if (!std::isfinite(ev.duration_sec) || ev.duration_sec < 0.0) ev.duration_sec = 0.0;
    ev.text = detail::trim_ws(ev.text);
  }
}

// De-duplicate events in-place.
//
// Events are treated as identical if:
// - onset and duration match after microsecond quantization, and
// - text matches exactly (after normalize_events trims it).
inline void deduplicate_events(std::vector<AnnotationEvent>* events) {
  if (!events) return;
  if (events->empty()) return;
  normalize_events(events);
  sort_events(events);

  std::vector<AnnotationEvent> out;
  out.reserve(events->size());

  int64_t prev_on_us = 0;
  int64_t prev_dur_us = 0;
  std::string prev_txt;
  bool have_prev = false;

  for (const auto& ev : *events) {
    const int64_t on_us = detail::sec_to_us(ev.onset_sec);
    const int64_t dur_us = detail::sec_to_us(ev.duration_sec);
    const std::string txt = detail::trim_ws(ev.text);

    if (!have_prev || on_us != prev_on_us || dur_us != prev_dur_us || txt != prev_txt) {
      out.push_back({ev.onset_sec, ev.duration_sec, txt});
      prev_on_us = on_us;
      prev_dur_us = dur_us;
      prev_txt = txt;
      have_prev = true;
    }
  }

  events->swap(out);
}

// Merge extra events into dst and de-duplicate.
inline void merge_events(std::vector<AnnotationEvent>* dst, const std::vector<AnnotationEvent>& extra) {
  if (!dst) return;
  if (extra.empty()) {
    // Still normalize/dedup for deterministic output.
    deduplicate_events(dst);
    return;
  }
  dst->insert(dst->end(), extra.begin(), extra.end());
  deduplicate_events(dst);
}

} // namespace qeeg
