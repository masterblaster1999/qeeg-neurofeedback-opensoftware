#include "qeeg/recording_ops.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

static bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  EEGRecording rec;
  rec.fs_hz = 10.0;
  rec.channel_names = {"C3", "C4"};
  rec.data.resize(2);
  for (int i = 0; i < 100; ++i) {
    rec.data[0].push_back(static_cast<float>(i));
    rec.data[1].push_back(static_cast<float>(1000 + i));
  }

  // Events relative to file start.
  // - point event at 1.0s
  // - duration event that overlaps slice
  // - duration event that starts before slice
  // - event entirely after slice
  rec.events.push_back(AnnotationEvent{1.0, 0.0, "P"});
  rec.events.push_back(AnnotationEvent{2.0, 2.0, "D"});
  rec.events.push_back(AnnotationEvent{0.5, 1.0, "PRE"});
  rec.events.push_back(AnnotationEvent{5.0, 1.0, "POST"});

  // Slice: [1.0s, 3.0s) => samples [10, 30)
  EEGRecording s = slice_recording_samples(rec, 10, 30, true);
  assert(s.n_channels() == 2);
  assert(s.n_samples() == 20);
  assert(s.data[0][0] == 10.0f);
  assert(s.data[0][19] == 29.0f);
  assert(s.data[1][0] == 1010.0f);
  assert(s.data[1][19] == 1029.0f);

  // Event checks.
  // Expected:
  //  - PRE clipped to [1.0, 1.5] => onset 0.0, dur 0.5
  //  - P at 1.0 => onset 0.0, dur 0.0
  //  - D clipped to [2.0, 3.0] => onset 1.0, dur 1.0
  // POST removed.
  assert(s.events.size() == 3);

  // Events are sorted by onset, then duration, then text.
  assert(approx(s.events[0].onset_sec, 0.0));
  assert(approx(s.events[1].onset_sec, 0.0));
  assert(approx(s.events[2].onset_sec, 1.0));

  // Identify by text.
  double pre_on = 0.0, pre_dur = 0.0;
  double p_on = 0.0, p_dur = 0.0;
  double d_on = 0.0, d_dur = 0.0;
  for (const auto& ev : s.events) {
    if (ev.text == "PRE") {
      pre_on = ev.onset_sec;
      pre_dur = ev.duration_sec;
    } else if (ev.text == "P") {
      p_on = ev.onset_sec;
      p_dur = ev.duration_sec;
    } else if (ev.text == "D") {
      d_on = ev.onset_sec;
      d_dur = ev.duration_sec;
    }
  }
  assert(approx(pre_on, 0.0));
  assert(approx(pre_dur, 0.5));
  assert(approx(p_on, 0.0));
  assert(approx(p_dur, 0.0));
  assert(approx(d_on, 1.0));
  assert(approx(d_dur, 1.0));

  // Time-based slicing should match.
  EEGRecording s2 = slice_recording_time(rec, 1.0, 2.0, true);
  assert(s2.n_samples() == 20);
  assert(s2.events.size() == 3);

  std::cout << "Recording ops test passed.\n";
  return 0;
}
