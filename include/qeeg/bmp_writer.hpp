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

// Render a scalar grid to BMP.
// - NaNs are rendered as white background
void render_grid_to_bmp(const std::string& path,
                        int size,
                        const std::vector<float>& grid_values,
                        double vmin,
                        double vmax);

// ---- Optional annotations / richer visuals ----

// Options for composing an output BMP with a vertical colorbar (right side).
struct VerticalColorbarOptions {
  bool enabled{true};

  // Layout around the original image.
  int pad_left{4};
  int pad_right{4};
  int pad_top{20};     // also provides space for top label text
  int pad_bottom{20};  // also provides space for bottom label text

  // Colorbar geometry.
  int bar_width{16};
  int gap{6};          // gap between image <-> bar, and bar <-> labels

  // Text rendering (tiny built-in 5x7 font).
  bool draw_labels{true};
  int font_scale{2};   // integer scale factor

  // Styling.
  RGB text_color{0, 0, 0};
  RGB border_color{0, 0, 0};
};

// Write an image with a right-side vertical colorbar, using the same heat colormap.
// `pixels` must be width*height and be row-major top-to-bottom.
// `vmin/vmax` are the numeric limits shown in the labels (when enabled) and implied by the colormap.
void write_bmp24_with_vertical_colorbar(const std::string& path,
                                       int width,
                                       int height,
                                       const std::vector<RGB>& pixels,
                                       double vmin,
                                       double vmax,
                                       const VerticalColorbarOptions& opt = VerticalColorbarOptions{});

// Overlay options specific to EEG topomaps.
struct TopomapAnnotationOptions {
  bool draw_head_outline{true};
  bool draw_nose{true};
  bool draw_electrodes{true};

  int outline_thickness{1};
  int electrode_radius{2};

  RGB outline_color{0, 0, 0};
  RGB electrode_color{0, 0, 0};
};

// Combined options for annotated topomap rendering.
struct AnnotatedTopomapOptions {
  VerticalColorbarOptions colorbar{};
  TopomapAnnotationOptions overlay{};
};

// Render a topomap grid to BMP with optional head outline + electrode dots + colorbar.
// - `electrodes_unit` should contain electrode positions in unit-circle coordinates (same convention as Montage: x,y in [-1,1]).
// - NaNs are rendered as white background.
void render_grid_to_bmp_annotated(const std::string& path,
                                 int size,
                                 const std::vector<float>& grid_values,
                                 double vmin,
                                 double vmax,
                                 const std::vector<Vec2>& electrodes_unit,
                                 const AnnotatedTopomapOptions& opt = AnnotatedTopomapOptions{});

} // namespace qeeg
