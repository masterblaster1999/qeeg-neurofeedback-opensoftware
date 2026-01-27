#include "qeeg/event_ops.hpp"

#include "test_support.hpp"
#include <iostream>
#include <vector>

using qeeg::AnnotationEvent;
using qeeg::deduplicate_events;
using qeeg::merge_events;

int main() {
  // 1) Dedup: microsecond-quantized equality
  std::vector<AnnotationEvent> v = {
      {1.0, 0.0, "A"},
      {1.0000001, 0.0, "A"}, // within 0.1 microsecond
      {1.0, 0.0, " A "},      // text trims to "A"
      {2.0, 0.5, "B"},
      {2.0, 0.5, "B"},
      {2.0, -1.0, "C"}, // duration clamps to 0
  };

  deduplicate_events(&v);
  // Expected unique events:
  // - (1.0,0,A)
  // - (2.0,0.5,B)
  // - (2.0,0,C)
  assert(v.size() == 3);
  assert(v[0].text == "A");
  assert(v[1].text == "B");
  assert(v[2].text == "C");
  assert(v[2].duration_sec == 0.0);

  // 2) Merge + dedup
  std::vector<AnnotationEvent> dst = {{0.5, 0.0, "X"}};
  std::vector<AnnotationEvent> extra = {{0.5, 0.0, "X"}, {0.75, 1.0, "Y"}};
  merge_events(&dst, extra);
  assert(dst.size() == 2);
  assert(dst[0].text == "X");
  assert(dst[1].text == "Y");

  std::cout << "test_event_ops: OK\n";
  return 0;
}
