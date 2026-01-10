#pragma once

#include "qeeg/montage.hpp"
#include "qeeg/spherical_spline.hpp"
#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

// Interpolate EEG channels using Perrin-style spherical spline interpolation.
//
// This is intended for simple "bad channel" replacement when you have a montage
// with approximate electrode positions.
//
// ⚠️ Research/educational use only.

struct InterpolateOptions {
  SphericalSplineOptions spline{};
};

struct InterpolateReport {
  // Channels that were actually interpolated (indices into EEGRecording).
  std::vector<size_t> interpolated;

  // Bad channels that were requested but skipped because the montage lacked a position.
  std::vector<size_t> skipped_no_position;

  // Bad channels skipped because there were not enough "good" channels with positions
  // to fit the spline system.
  std::vector<size_t> skipped_not_enough_good;

  // Good channel indices that were used as sources (must have montage positions).
  std::vector<size_t> good_used;
};

// Interpolate the channels listed in bad_indices, in-place.
//
// Notes:
// - Only channels that exist in the montage will be interpolated.
// - At least 3 good channels with positions are required.
// - Events are preserved.
InterpolateReport interpolate_bad_channels_spherical_spline(EEGRecording* rec,
                                                           const Montage& montage,
                                                           const std::vector<size_t>& bad_indices,
                                                           const InterpolateOptions& opt = {});

} // namespace qeeg
