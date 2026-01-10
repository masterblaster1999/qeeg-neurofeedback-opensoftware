#pragma once

#include "qeeg/types.hpp"

#include <cstddef>

namespace qeeg {

// Slice a recording by sample index.
//
// - start_sample: inclusive
// - end_sample:   exclusive
//
// If adjust_events=true, EEGRecording::events are filtered to those overlapping the slice,
// and their onset times are shifted so the slice starts at t=0.
EEGRecording slice_recording_samples(const EEGRecording& rec,
                                     std::size_t start_sample,
                                     std::size_t end_sample,
                                     bool adjust_events = true);

// Slice a recording by time.
//
// If start_sec <= 0, slicing starts at t=0.
// If duration_sec <= 0, the slice runs to the end of the recording.
EEGRecording slice_recording_time(const EEGRecording& rec,
                                  double start_sec,
                                  double duration_sec,
                                  bool adjust_events = true);

} // namespace qeeg
