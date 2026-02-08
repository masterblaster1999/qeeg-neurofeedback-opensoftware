#include "qeeg/utils.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  using qeeg::parse_iso8601_to_utc_millis;

  int64_t ms = -1;

  // Unix epoch.
  assert(parse_iso8601_to_utc_millis("1970-01-01T00:00:00Z", &ms));
  assert(ms == 0);

  // Seconds and milliseconds.
  {
    int64_t ms1 = -1;
    int64_t ms2 = -1;
    int64_t ms3 = -1;
    int64_t ms4 = -1;

    assert(parse_iso8601_to_utc_millis("1970-01-01T00:00:00.001Z", &ms1));
    assert(ms1 == 1);

    assert(parse_iso8601_to_utc_millis("1970-01-01T00:00:01Z", &ms2));
    assert(ms2 == 1000);

    assert(parse_iso8601_to_utc_millis("1970-01-01T00:00:00.1234Z", &ms3));
    assert(ms3 == 123);

    assert(parse_iso8601_to_utc_millis("1970-01-01T00:00:00,001Z", &ms4));
    assert(ms4 == 1);
  }

  // Day boundary.
  {
    int64_t ms_day = -1;
    assert(parse_iso8601_to_utc_millis("1970-01-02T00:00:00Z", &ms_day));
    assert(ms_day == 86400000LL);
  }

  // Offsets (+HH:MM) should round-trip to the same instant.
  {
    int64_t ms_off = -1;
    assert(parse_iso8601_to_utc_millis("1970-01-01T01:00:00+01:00", &ms_off));
    assert(ms_off == 0);
  }

  // Offsets without colon (+HHMM) are accepted.
  {
    int64_t ms_off = -1;
    assert(parse_iso8601_to_utc_millis("1970-01-01T01:00:00+0100", &ms_off));
    assert(ms_off == 0);
  }

  // Offsets with hours only (+HH) are accepted.
  {
    int64_t ms_off = -1;
    assert(parse_iso8601_to_utc_millis("1970-01-01T01:00:00+01", &ms_off));
    assert(ms_off == 0);
  }
  // Negative offsets.
  {
    int64_t ms_off = -1;
    assert(parse_iso8601_to_utc_millis("1969-12-31T23:00:00-01:00", &ms_off));
    assert(ms_off == 0);

    int64_t ms_off2 = -1;
    assert(parse_iso8601_to_utc_millis("1969-12-31T23:00:00-0100", &ms_off2));
    assert(ms_off2 == 0);

    int64_t ms_off3 = -1;
    assert(parse_iso8601_to_utc_millis("1969-12-31T23:00:00-01", &ms_off3));
    assert(ms_off3 == 0);
  }

  // Space separator and surrounding whitespace are accepted.
  {
    int64_t ms_space = -1;
    assert(parse_iso8601_to_utc_millis("1970-01-01 00:00:00Z", &ms_space));
    assert(ms_space == 0);

    int64_t ms_ws = -1;
    assert(parse_iso8601_to_utc_millis("  1970-01-01T00:00:00Z\n", &ms_ws));
    assert(ms_ws == 0);
  }

  // Invalid forms should be rejected.
  {
    int64_t out = 0;
    assert(!parse_iso8601_to_utc_millis("", &out));
    assert(!parse_iso8601_to_utc_millis("1970-01-01", &out));
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:00:00", &out)); // missing TZ
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:00Z", &out));    // missing seconds
    assert(!parse_iso8601_to_utc_millis("1970-13-01T00:00:00Z", &out)); // invalid month
    assert(!parse_iso8601_to_utc_millis("1970-01-32T00:00:00Z", &out)); // invalid day
    assert(!parse_iso8601_to_utc_millis("1970-01-01T24:00:00Z", &out)); // invalid hour
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:60:00Z", &out)); // invalid minute
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:00:60Z", &out)); // invalid second

    // Unsupported numeric TZ format.
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:00:00+1:00", &out));
    assert(!parse_iso8601_to_utc_millis("1970-01-01T00:00:00+1", &out));
  }

  std::cout << "ok\n";
  return 0;
}
