#include "qeeg/annotations.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

static std::vector<uint8_t> bytes_from_string(const std::string& s) {
  std::vector<uint8_t> out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<uint8_t>(c));
  return out;
}

int main() {
  using qeeg::parse_edfplus_annotations_record;

  // 1) Mandatory per-record timestamp marker: "+0<0x14><0x14>" should yield no events.
  {
    std::string s;
    s += "+0";
    s.push_back('\x14');
    s.push_back('\x14');
    // add padding
    s.push_back('\x00');
    s.push_back('\x00');
    auto ev = parse_edfplus_annotations_record(bytes_from_string(s));
    if (!ev.empty()) {
      std::cerr << "Expected no events for record timestamp marker\n";
      return 1;
    }
  }

  // 2) One TAL event: +12.5<0x15>1.0<0x14>Stim<0x14>
  {
    std::string s;
    s += "+12.5";
    s.push_back('\x15');
    s += "1.0";
    s.push_back('\x14');
    s += "Stim";
    s.push_back('\x14');
    auto ev = parse_edfplus_annotations_record(bytes_from_string(s));
    if (ev.size() != 1) {
      std::cerr << "Expected 1 event, got " << ev.size() << "\n";
      return 1;
    }
    if (!approx(ev[0].onset_sec, 12.5) || !approx(ev[0].duration_sec, 1.0) || ev[0].text != "Stim") {
      std::cerr << "Event parse mismatch\n";
      return 1;
    }
  }

  // 3) Multiple annotation texts in one TAL: +5<0x15>2<0x14>A<0x14>B<0x14>
  {
    std::string s;
    s += "+5";
    s.push_back('\x15');
    s += "2";
    s.push_back('\x14');
    s += "A";
    s.push_back('\x14');
    s += "B";
    s.push_back('\x14');
    auto ev = parse_edfplus_annotations_record(bytes_from_string(s));
    if (ev.size() != 2) {
      std::cerr << "Expected 2 events, got " << ev.size() << "\n";
      return 1;
    }
    if (!approx(ev[0].onset_sec, 5.0) || ev[0].text != "A") return 1;
    if (!approx(ev[1].onset_sec, 5.0) || ev[1].text != "B") return 1;
  }

  // 4) Two TALs in one record: timestamp + event.
  {
    std::string s;
    s += "+0";
    s.push_back('\x14');
    s.push_back('\x14');
    s += "+10";
    s.push_back('\x14');
    s += "Blink";
    s.push_back('\x14');
    auto ev = parse_edfplus_annotations_record(bytes_from_string(s));
    if (ev.size() != 1 || !approx(ev[0].onset_sec, 10.0) || ev[0].text != "Blink") {
      std::cerr << "Expected a single 'Blink' event at 10 seconds\n";
      return 1;
    }
  }

  std::cout << "OK\n";
  return 0;
}
