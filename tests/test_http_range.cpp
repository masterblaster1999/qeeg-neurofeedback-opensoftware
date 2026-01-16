#include "qeeg/utils.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <iostream>

int main() {
  using qeeg::HttpRangeResult;
  using qeeg::parse_http_byte_range;

  uintmax_t start = 0;
  uintmax_t end = 0;

  {
    const auto r = parse_http_byte_range("", 100, &start, &end);
    assert(r == HttpRangeResult::kNone);
  }

  {
    const auto r = parse_http_byte_range("bytes=0-99", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 0);
    assert(end == 99);
  }

  {
    const auto r = parse_http_byte_range("bytes=0-0", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 0);
    assert(end == 0);
  }

  {
    const auto r = parse_http_byte_range("bytes=10-", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 10);
    assert(end == 99);
  }

  {
    // End is clamped to size-1.
    const auto r = parse_http_byte_range("bytes=90-200", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 90);
    assert(end == 99);
  }

  {
    // Suffix range: last N bytes.
    const auto r = parse_http_byte_range("bytes=-10", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 90);
    assert(end == 99);
  }

  {
    // Suffix larger than file returns entire file.
    const auto r = parse_http_byte_range("bytes=-200", 100, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 0);
    assert(end == 99);
  }

  {
    // Unsatisfiable start.
    const auto r = parse_http_byte_range("bytes=200-300", 100, &start, &end);
    assert(r == HttpRangeResult::kUnsatisfiable);
  }

  {
    // Invalid reversed range.
    const auto r = parse_http_byte_range("bytes=50-40", 100, &start, &end);
    assert(r == HttpRangeResult::kInvalid);
  }

  {
    // Multiple ranges not supported.
    const auto r = parse_http_byte_range("bytes=0-0,10-20", 100, &start, &end);
    assert(r == HttpRangeResult::kInvalid);
  }

  {
    // Case-insensitive unit parsing.
    const auto r = parse_http_byte_range("Bytes=1-2", 10, &start, &end);
    assert(r == HttpRangeResult::kSatisfiable);
    assert(start == 1);
    assert(end == 2);
  }



  {
    // Overflow should be treated as invalid (do not wrap).
    const std::string maxs = std::to_string(std::numeric_limits<uintmax_t>::max());
    const std::string too_big = maxs + "0"; // definitely larger than uintmax_t max

    const auto r1 = parse_http_byte_range("bytes=" + too_big + "-", 100, &start, &end);
    assert(r1 == HttpRangeResult::kInvalid);

    const auto r2 = parse_http_byte_range("bytes=0-" + too_big, 100, &start, &end);
    assert(r2 == HttpRangeResult::kInvalid);

    const auto r3 = parse_http_byte_range("bytes=-" + too_big, 100, &start, &end);
    assert(r3 == HttpRangeResult::kInvalid);
  }

  std::cout << "ok\n";
  return 0;
}
