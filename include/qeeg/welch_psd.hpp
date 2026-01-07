#pragma once

#include "qeeg/types.hpp"

#include <cstddef>
#include <vector>

namespace qeeg {

struct WelchOptions {
  size_t nperseg{1024};
  double overlap_fraction{0.5}; // 0..<1
};

PsdResult welch_psd(const std::vector<float>& x, double fs_hz, const WelchOptions& opt);

} // namespace qeeg
