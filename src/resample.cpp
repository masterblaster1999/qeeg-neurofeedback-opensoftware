#include "qeeg/resample.hpp"

#include <algorithm>
#include <cmath>

namespace qeeg {

std::vector<float> resample_linear(const std::vector<float>& in, std::size_t out_len) {
  if (out_len == 0) return {};
  if (in.empty()) return {};
  if (in.size() == 1) return std::vector<float>(out_len, in[0]);

  const std::size_t in_len = in.size();

  // Use index-space mapping. For evenly-spaced samples spanning the same
  // duration, the ratio in_len/out_len matches fs_in/fs_out.
  const double ratio = static_cast<double>(in_len) / static_cast<double>(out_len);

  std::vector<float> out;
  out.resize(out_len);

  for (std::size_t j = 0; j < out_len; ++j) {
    const double pos = static_cast<double>(j) * ratio;
    std::size_t i0 = static_cast<std::size_t>(std::floor(pos));

    if (i0 >= in_len - 1) {
      out[j] = in.back();
      continue;
    }

    const std::size_t i1 = i0 + 1;
    const double frac = pos - static_cast<double>(i0);

    const double v0 = static_cast<double>(in[i0]);
    const double v1 = static_cast<double>(in[i1]);
    const double v = v0 * (1.0 - frac) + v1 * frac;

    out[j] = static_cast<float>(v);
  }

  return out;
}

} // namespace qeeg
