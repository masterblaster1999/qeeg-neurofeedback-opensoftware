#pragma once

#include <cstddef>
#include <vector>

namespace qeeg {

// A simple half-open index segment: [start, end)
struct IndexSegment {
  std::size_t start{0};
  std::size_t end{0};

  std::size_t length() const { return (end > start) ? (end - start) : 0; }
};

// Merge overlapping or "close" segments.
//
// - Segments are treated as half-open: [start, end)
// - If merge_gap > 0, segments with a gap <= merge_gap are merged.
// - Empty/invalid segments (end <= start) are dropped.
std::vector<IndexSegment> merge_segments(std::vector<IndexSegment> segs, std::size_t merge_gap = 0);

// Return the complement of "bad" segments within [0, total_length).
//
// The input should be merged/sorted (use merge_segments first).
std::vector<IndexSegment> complement_segments(const std::vector<IndexSegment>& bad,
                                              std::size_t total_length);

// Drop segments shorter than min_len.
std::vector<IndexSegment> filter_min_length(std::vector<IndexSegment> segs, std::size_t min_len);

} // namespace qeeg
