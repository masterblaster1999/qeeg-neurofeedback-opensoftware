#pragma once

#include <cstddef>
#include <vector>

namespace qeeg {

// Linearly resample a uniformly-sampled signal to a target length.
//
// This is intentionally small and dependency-free. It is primarily used to
// align mixed-rate EDF/BDF channel groups (common in BioTrace+ / NeXus exports)
// to a single sampling rate so the rest of the pipeline can treat recordings
// as a dense matrix.
//
// The mapping is based on the index ratio (in_len / out_len):
//   pos = j * (in_len / out_len)
// then linear interpolation between floor(pos) and floor(pos)+1.
//
// Edge cases:
// - out_len == 0 => returns empty
// - in is empty  => returns empty
// - in.size() == 1 => returns out_len copies of the single value
std::vector<float> resample_linear(const std::vector<float>& in, std::size_t out_len);

// Zero-order-hold resample to a target length.
//
// This is useful for discrete-valued channels such as triggers / status words / event markers.
// Linear interpolation can create intermediate values and therefore false edges (spurious events)
// when the channel is later decoded as integer codes.
//
// Mapping is the same index-ratio approach as resample_linear, but values are held from the
// nearest earlier input sample:
//   i = floor(j * (in_len / out_len))
//
// Edge cases match resample_linear:
// - out_len == 0 => empty
// - in empty => empty
// - in.size() == 1 => out_len copies
std::vector<float> resample_hold(const std::vector<float>& in, std::size_t out_len);

} // namespace qeeg
