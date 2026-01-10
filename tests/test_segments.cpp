#include "qeeg/segments.hpp"

#include <cassert>
#include <iostream>
#include <vector>

int main() {
  using namespace qeeg;

  // Overlap merge.
  {
    std::vector<IndexSegment> segs = {
        {0, 10},
        {5, 12},
        {20, 25},
        {26, 30},
        {40, 40}, // empty => drop
    };

    auto m0 = merge_segments(segs, 0);
    assert(m0.size() == 3);
    assert(m0[0].start == 0 && m0[0].end == 12);
    assert(m0[1].start == 20 && m0[1].end == 25);
    assert(m0[2].start == 26 && m0[2].end == 30);

    auto m1 = merge_segments(segs, 1);
    assert(m1.size() == 2);
    assert(m1[0].start == 0 && m1[0].end == 12);
    assert(m1[1].start == 20 && m1[1].end == 30);
  }

  // Complement.
  {
    std::vector<IndexSegment> bad = {
        {0, 12},
        {20, 30},
    };
    const std::size_t total = 40;
    auto good = complement_segments(bad, total);
    assert(good.size() == 2);
    assert(good[0].start == 12 && good[0].end == 20);
    assert(good[1].start == 30 && good[1].end == 40);

    auto good2 = filter_min_length(good, 9);
    assert(good2.size() == 1);
    assert(good2[0].start == 30 && good2[0].end == 40);
  }

  std::cout << "Segments test passed.\n";
  return 0;
}
