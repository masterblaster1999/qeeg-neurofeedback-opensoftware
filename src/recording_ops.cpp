#include "qeeg/recording_ops.hpp"
#include "qeeg/event_ops.hpp"

#include <algorithm>
#include <cmath>

namespace qeeg {

static bool overlaps(double a0, double a1, double b0, double b1) {
  return (a0 < b1) && (b0 < a1);
}

EEGRecording slice_recording_samples(const EEGRecording& rec,
                                     std::size_t start_sample,
                                     std::size_t end_sample,
                                     bool adjust_events) {
  EEGRecording out;
  out.fs_hz = rec.fs_hz;
  out.channel_names = rec.channel_names;
  out.data.resize(rec.n_channels());

  const std::size_t N = rec.n_samples();
  if (start_sample > N) start_sample = N;
  if (end_sample > N) end_sample = N;
  if (end_sample < start_sample) end_sample = start_sample;

  for (std::size_t ch = 0; ch < rec.n_channels(); ++ch) {
    const auto& x = rec.data[ch];
    const std::size_t n = x.size();
    const std::size_t s = std::min(start_sample, n);
    const std::size_t e = std::min(end_sample, n);
    out.data[ch] = std::vector<float>(x.begin() + static_cast<std::ptrdiff_t>(s),
                                      x.begin() + static_cast<std::ptrdiff_t>(e));
  }

  if (!adjust_events) {
    out.events = rec.events;
    return out;
  }

  // Filter + shift events.
  if (!(rec.fs_hz > 0.0)) {
    // Without a sampling rate, we can't safely translate sample indices to seconds.
    // Fall back to copying the event list unchanged.
    out.events = rec.events;
    return out;
  }

  const double fs = rec.fs_hz;
  const double slice_start_sec = static_cast<double>(start_sample) / fs;
  const double slice_end_sec = static_cast<double>(end_sample) / fs;
  if (!(slice_end_sec > slice_start_sec)) {
    out.events.clear();
    return out;
  }

  out.events.reserve(rec.events.size());
  for (const auto& ev : rec.events) {
    const double ev_on = ev.onset_sec;
    const double dur = std::max(0.0, ev.duration_sec);
    const double ev_off = ev_on + dur;

    // Point event.
    if (!(dur > 0.0)) {
      if (ev_on >= slice_start_sec && ev_on < slice_end_sec) {
        AnnotationEvent e2 = ev;
        e2.onset_sec = ev_on - slice_start_sec;
        e2.duration_sec = 0.0;
        out.events.push_back(std::move(e2));
      }
      continue;
    }

    // Duration event: keep if it overlaps the slice.
    if (!overlaps(ev_on, ev_off, slice_start_sec, slice_end_sec)) continue;

    const double clip_start = std::max(ev_on, slice_start_sec);
    const double clip_end = std::min(ev_off, slice_end_sec);
    if (!(clip_end > clip_start)) continue;

    AnnotationEvent e2 = ev;
    e2.onset_sec = clip_start - slice_start_sec;
    e2.duration_sec = clip_end - clip_start;
    out.events.push_back(std::move(e2));
  }

  sort_events(&out.events);

  return out;
}

EEGRecording slice_recording_time(const EEGRecording& rec,
                                  double start_sec,
                                  double duration_sec,
                                  bool adjust_events) {
  if (rec.n_channels() == 0 || rec.n_samples() == 0) return rec;
  if (!(rec.fs_hz > 0.0)) return rec;

  const double fs = rec.fs_hz;
  const std::size_t N = rec.n_samples();

  std::size_t start = 0;
  if (start_sec > 0.0) {
    start = static_cast<std::size_t>(std::llround(start_sec * fs));
    if (start > N) start = N;
  }

  std::size_t end = N;
  if (duration_sec > 0.0) {
    const std::size_t len = static_cast<std::size_t>(std::llround(duration_sec * fs));
    end = std::min(N, start + len);
  }

  return slice_recording_samples(rec, start, end, adjust_events);
}

} // namespace qeeg
