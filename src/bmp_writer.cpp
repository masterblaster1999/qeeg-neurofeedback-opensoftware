#include "qeeg/bmp_writer.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>

namespace qeeg {

static void write_u16(std::ofstream& f, uint16_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
}

static void write_u32(std::ofstream& f, uint32_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
  f.put(static_cast<char>((v >> 16) & 0xFF));
  f.put(static_cast<char>((v >> 24) & 0xFF));
}

void write_bmp24(const std::string& path, int width, int height, const std::vector<RGB>& pixels) {
  if (width <= 0 || height <= 0) throw std::runtime_error("write_bmp24: invalid dimensions");
  if (static_cast<int>(pixels.size()) != width * height) {
    throw std::runtime_error("write_bmp24: pixels size mismatch");
  }

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to open output BMP: " + path);

  const int row_stride = width * 3;
  const int padding = (4 - (row_stride % 4)) % 4;
  const uint32_t pixel_data_size = static_cast<uint32_t>((row_stride + padding) * height);
  const uint32_t file_size = 14 + 40 + pixel_data_size;

  // BITMAPFILEHEADER
  f.put('B'); f.put('M');                 // bfType
  write_u32(f, file_size);                // bfSize
  write_u16(f, 0); write_u16(f, 0);       // bfReserved1/2
  write_u32(f, 14 + 40);                  // bfOffBits

  // BITMAPINFOHEADER
  write_u32(f, 40);                       // biSize
  write_u32(f, static_cast<uint32_t>(width));  // biWidth
  write_u32(f, static_cast<uint32_t>(height)); // biHeight (positive => bottom-up)
  write_u16(f, 1);                        // biPlanes
  write_u16(f, 24);                       // biBitCount
  write_u32(f, 0);                        // biCompression (BI_RGB)
  write_u32(f, pixel_data_size);          // biSizeImage
  write_u32(f, 2835);                     // biXPelsPerMeter (~72 DPI)
  write_u32(f, 2835);                     // biYPelsPerMeter
  write_u32(f, 0);                        // biClrUsed
  write_u32(f, 0);                        // biClrImportant

  // Pixel data: BMP is BGR and bottom-up
  for (int y = height - 1; y >= 0; --y) {
    const RGB* row = &pixels[static_cast<size_t>(y) * static_cast<size_t>(width)];
    for (int x = 0; x < width; ++x) {
      const RGB& p = row[x];
      f.put(static_cast<char>(p.b));
      f.put(static_cast<char>(p.g));
      f.put(static_cast<char>(p.r));
    }
    for (int i = 0; i < padding; ++i) f.put(0);
  }
}

static double clamp01(double t) {
  if (t < 0.0) return 0.0;
  if (t > 1.0) return 1.0;
  return t;
}

RGB colormap_heat(double t01) {
  // Piecewise linear gradient:
  // 0.0: blue
  // 0.25: cyan
  // 0.5: green
  // 0.75: yellow
  // 1.0: red
  double t = clamp01(t01);

  auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };

  struct Stop { double t; double r; double g; double b; };
  const Stop stops[] = {
    {0.00, 0.0, 0.0, 1.0},
    {0.25, 0.0, 1.0, 1.0},
    {0.50, 0.0, 1.0, 0.0},
    {0.75, 1.0, 1.0, 0.0},
    {1.00, 1.0, 0.0, 0.0},
  };

  const Stop* a = &stops[0];
  const Stop* b = &stops[4];
  for (int i = 0; i < 4; ++i) {
    if (t >= stops[i].t && t <= stops[i+1].t) {
      a = &stops[i];
      b = &stops[i+1];
      break;
    }
  }

  double local = (b->t == a->t) ? 0.0 : (t - a->t) / (b->t - a->t);
  double r = lerp(a->r, b->r, local);
  double g = lerp(a->g, b->g, local);
  double bl = lerp(a->b, b->b, local);

  RGB out;
  out.r = static_cast<uint8_t>(std::round(255.0 * clamp01(r)));
  out.g = static_cast<uint8_t>(std::round(255.0 * clamp01(g)));
  out.b = static_cast<uint8_t>(std::round(255.0 * clamp01(bl)));
  return out;
}

void render_grid_to_bmp(const std::string& path,
                        int size,
                        const std::vector<float>& grid_values,
                        double vmin,
                        double vmax) {
  if (size <= 0) throw std::runtime_error("render_grid_to_bmp: invalid size");
  if (static_cast<int>(grid_values.size()) != size * size) {
    throw std::runtime_error("render_grid_to_bmp: grid size mismatch");
  }
  if (!(vmax > vmin)) {
    // Avoid divide-by-zero; make a tiny range
    vmax = vmin + 1e-12;
  }

  std::vector<RGB> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
  for (int j = 0; j < size; ++j) {
    for (int i = 0; i < size; ++i) {
      float v = grid_values[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)];
      RGB px{255, 255, 255}; // white for NaN
      if (!std::isnan(v)) {
        double t = (static_cast<double>(v) - vmin) / (vmax - vmin);
        px = colormap_heat(t);
      }
      pixels[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)] = px;
    }
  }

  write_bmp24(path, size, size, pixels);
}

} // namespace qeeg
