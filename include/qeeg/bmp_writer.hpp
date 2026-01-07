#pragma once

#include "qeeg/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qeeg {

// Write a 24-bit BMP.
// Pixels are row-major, top-to-bottom, left-to-right.
// Size must be width*height.
void write_bmp24(const std::string& path, int width, int height, const std::vector<RGB>& pixels);

// Simple "blue->cyan->green->yellow->red" gradient colormap.
RGB colormap_heat(double t01);

// Render a scalar grid to BMP with circle mask.
// - NaNs are rendered as white background
void render_grid_to_bmp(const std::string& path,
                        int size,
                        const std::vector<float>& grid_values,
                        double vmin,
                        double vmax);

} // namespace qeeg
