#pragma once

#include "qeeg/montage.hpp"
#include "qeeg/types.hpp"

#include <string>
#include <limits>
#include <vector>

namespace qeeg {

struct Grid2D {
  int size{256};                     // grid is size x size
  std::vector<float> values;         // row-major, length size*size
  float nan_value{std::numeric_limits<float>::quiet_NaN()};
};

struct TopomapOptions {
  int grid_size{256};
  double idw_power{2.0};
  double idw_eps{1e-6};
};

// Build a grid by interpolating per-channel values at montage positions.
// Any channels without montage positions are ignored.
Grid2D make_topomap_idw(const Montage& montage,
                        const std::vector<std::string>& channel_names,
                        const std::vector<double>& channel_values,
                        const TopomapOptions& opt);

} // namespace qeeg
