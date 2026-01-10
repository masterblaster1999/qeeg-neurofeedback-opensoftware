#include "qeeg/segments.hpp"

#include <algorithm>

namespace qeeg {

std::vector<IndexSegment> merge_segments(std::vector<IndexSegment> segs, std::size_t merge_gap) {
  // Drop empty segments.
  segs.erase(std::remove_if(segs.begin(), segs.end(),
                            [](const IndexSegment& s) { return s.end <= s.start; }),
             segs.end());
  if (segs.empty()) return {};

  std::sort(segs.begin(), segs.end(), [](const IndexSegment& a, const IndexSegment& b) {
    if (a.start != b.start) return a.start < b.start;
    return a.end < b.end;
  });

  std::vector<IndexSegment> out;
  out.reserve(segs.size());
  IndexSegment cur = segs[0];

  for (std::size_t i = 1; i < segs.size(); ++i) {
    const auto& s = segs[i];
    if (s.start <= cur.end + merge_gap) {
      cur.end = std::max(cur.end, s.end);
    } else {
      out.push_back(cur);
      cur = s;
    }
  }
  out.push_back(cur);
  return out;
}

std::vector<IndexSegment> complement_segments(const std::vector<IndexSegment>& bad,
                                              std::size_t total_length) {
  std::vector<IndexSegment> out;
  if (total_length == 0) return out;

  std::size_t cur = 0;
  for (const auto& b : bad) {
    const std::size_t bs = std::min(b.start, total_length);
    const std::size_t be = std::min(b.end, total_length);
    if (be <= bs) continue;

    if (bs > cur) {
      out.push_back(IndexSegment{cur, bs});
    }
    cur = std::max(cur, be);
    if (cur >= total_length) break;
  }
  if (cur < total_length) {
    out.push_back(IndexSegment{cur, total_length});
  }
  return out;
}

std::vector<IndexSegment> filter_min_length(std::vector<IndexSegment> segs, std::size_t min_len) {
  if (min_len == 0) return segs;
  segs.erase(std::remove_if(segs.begin(), segs.end(),
                            [&](const IndexSegment& s) { return s.length() < min_len; }),
             segs.end());
  return segs;
}

} // namespace qeeg
