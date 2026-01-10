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

} // namespace qeeg
