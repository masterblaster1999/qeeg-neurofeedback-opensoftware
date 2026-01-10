#include "qeeg/triggers.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // 1) Simple TRIG channel edge extraction.
  {
    EEGRecording rec;
    rec.fs_hz = 100.0;
    rec.channel_names = {"Cz", "TRIG"};
    rec.data.resize(2);
    rec.data[0] = std::vector<float>(100, 0.0f);
    rec.data[1] = std::vector<float>(100, 0.0f);
    // Pulse code 5 from sample 10..12
    rec.data[1][10] = 5.0f;
    rec.data[1][11] = 5.0f;
    rec.data[1][12] = 5.0f;
    // Pulse code 2 at sample 50
    rec.data[1][50] = 2.0f;

    const auto r = extract_events_from_triggers_auto(rec);
    assert(r.used_channel == "TRIG");
    assert(r.events.size() == 2);
    assert(near(r.events[0].onset_sec, 10.0 / 100.0));
    assert(r.events[0].duration_sec == 0.0);
    assert(r.events[0].text == "5");
    assert(near(r.events[1].onset_sec, 50.0 / 100.0));
    assert(r.events[1].text == "2");
  }

  // 2) BioSemi-style Status word: trigger code in lower 16 bits.
  {
    EEGRecording rec;
    rec.fs_hz = 256.0;
    rec.channel_names = {"Status"};
    rec.data.resize(1);

    const int base = (1 << 20); // sets a high bit; masked low-16 is 0.
    rec.data[0] = std::vector<float>(512, static_cast<float>(base));
    // Code 7 at sample 64
    rec.data[0][64] = static_cast<float>(base + 7);
    rec.data[0][65] = static_cast<float>(base + 7);
    // Code 300 at sample 128
    rec.data[0][128] = static_cast<float>(base + 300);

    const auto r = extract_events_from_triggers_auto(rec);
    assert(r.used_channel == "Status");
    assert(r.events.size() == 2);
    assert(near(r.events[0].onset_sec, 64.0 / 256.0));
    assert(r.events[0].text == "7");
    assert(near(r.events[1].onset_sec, 128.0 / 256.0));
    assert(r.events[1].text == "300");
  }

  // 3) A continuous-valued channel should not be misclassified as a trigger.
  {
    EEGRecording rec;
    rec.fs_hz = 100.0;
    rec.channel_names = {"Trigger"};
    rec.data.resize(1);
    rec.data[0].reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      // Some smooth-ish signal
      rec.data[0].push_back(static_cast<float>(std::sin(0.01 * i)));
    }
    const auto r = extract_events_from_triggers_auto(rec);
    assert(r.used_channel.empty());
    assert(r.events.empty());
  }

  std::cout << "All tests passed.\n";
  return 0;
}
