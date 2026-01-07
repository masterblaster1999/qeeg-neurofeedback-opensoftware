#include "qeeg/bmp_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
    if (t >= stops[i].t && t <= stops[i + 1].t) {
      a = &stops[i];
      b = &stops[i + 1];
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

namespace {

static inline void set_pixel(std::vector<RGB>& px, int w, int h, int x, int y, const RGB& c) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
}

static void draw_line(std::vector<RGB>& px, int w, int h, int x0, int y0, int x1, int y1, const RGB& c) {
  // Integer Bresenham
  int dx = std::abs(x1 - x0);
  int sx = (x0 < x1) ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;

  while (true) {
    set_pixel(px, w, h, x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void draw_rect_border(std::vector<RGB>& px, int w, int h, int x0, int y0, int rw, int rh, const RGB& c) {
  if (rw <= 0 || rh <= 0) return;
  for (int x = x0; x < x0 + rw; ++x) {
    set_pixel(px, w, h, x, y0, c);
    set_pixel(px, w, h, x, y0 + rh - 1, c);
  }
  for (int y = y0; y < y0 + rh; ++y) {
    set_pixel(px, w, h, x0, y, c);
    set_pixel(px, w, h, x0 + rw - 1, y, c);
  }
}

static void fill_circle(std::vector<RGB>& px, int w, int h, int cx, int cy, int r, const RGB& c) {
  if (r <= 0) {
    set_pixel(px, w, h, cx, cy, c);
    return;
  }
  const int r2 = r * r;
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        set_pixel(px, w, h, cx + dx, cy + dy, c);
      }
    }
  }
}

static void draw_circle_outline(std::vector<RGB>& px,
                               int w,
                               int h,
                               double cx,
                               double cy,
                               double r,
                               int thickness,
                               const RGB& c) {
  if (!(r > 0.0)) return;
  thickness = std::max(1, thickness);
  const double pi = std::acos(-1.0);

  for (int t = 0; t < thickness; ++t) {
    const double rr = r - static_cast<double>(t);
    if (!(rr > 0.0)) continue;

    int steps = static_cast<int>(std::lround(2.0 * pi * rr * 2.0));
    if (steps < 180) steps = 180;

    for (int s = 0; s < steps; ++s) {
      const double ang = (2.0 * pi * static_cast<double>(s)) / static_cast<double>(steps);
      const int x = static_cast<int>(std::lround(cx + rr * std::cos(ang)));
      const int y = static_cast<int>(std::lround(cy + rr * std::sin(ang)));
      set_pixel(px, w, h, x, y, c);
    }
  }
}

// Tiny built-in 5x7 font.
// Each glyph is 7 rows of 5 bits stored in the low bits of each byte.
struct GlyphDef {
  char ch;
  uint8_t rows[7];
};

static const GlyphDef kGlyphs[] = {
  {'0', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
  {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
  {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
  {'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
  {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
  {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
  {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
  {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
  {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
  {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},

  {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04}},
  {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
  {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},

  // Scientific notation support
  {'e', {0x00, 0x0E, 0x11, 0x1F, 0x10, 0x11, 0x0E}},
  {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},

  // For "nan" / "inf" just in case
  {'n', {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}},
  {'a', {0x00, 0x0E, 0x01, 0x0F, 0x11, 0x11, 0x0F}},
  {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
  {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},

  {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

static const uint8_t* glyph_rows(char ch) {
  // Try exact match first.
  for (const auto& g : kGlyphs) {
    if (g.ch == ch) return g.rows;
  }
  // Try lowercase variant.
  const char low = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (low != ch) {
    for (const auto& g : kGlyphs) {
      if (g.ch == low) return g.rows;
    }
  }
  return nullptr;
}

static void draw_char(std::vector<RGB>& px,
                      int w,
                      int h,
                      int x0,
                      int y0,
                      char ch,
                      const RGB& c,
                      int scale) {
  scale = std::max(1, scale);
  const uint8_t* rows = glyph_rows(ch);
  if (!rows) {
    rows = glyph_rows(' ');
  }

  for (int r = 0; r < 7; ++r) {
    const uint8_t bits = rows[r];
    for (int col = 0; col < 5; ++col) {
      const bool on = (bits & (1u << (4 - col))) != 0;
      if (!on) continue;
      for (int sy = 0; sy < scale; ++sy) {
        for (int sx = 0; sx < scale; ++sx) {
          set_pixel(px, w, h, x0 + col * scale + sx, y0 + r * scale + sy, c);
        }
      }
    }
  }
}

static void draw_text(std::vector<RGB>& px,
                      int w,
                      int h,
                      int x0,
                      int y0,
                      const std::string& text,
                      const RGB& c,
                      int scale) {
  scale = std::max(1, scale);
  const int adv = (5 + 1) * scale;
  const int line_h = (7 + 1) * scale;

  int x = x0;
  int y = y0;
  for (char ch : text) {
    if (ch == '\n') {
      y += line_h;
      x = x0;
      continue;
    }
    draw_char(px, w, h, x, y, ch, c, scale);
    x += adv;
  }
}

static std::string format_compact(double v) {
  if (!std::isfinite(v)) {
    return std::isnan(v) ? std::string("nan") : std::string("inf");
  }
  char buf[64];
  // 5 significant digits, switches to scientific as needed.
  std::snprintf(buf, sizeof(buf), "%.5g", v);
  return std::string(buf);
}

struct CanvasWithColorbar {
  int width{0};
  int height{0};
  int x_img{0};
  int y_img{0};
  int x_bar{0};
  int y_bar{0};
  int x_label{0};
  int y_label_top{0};
  int y_label_bottom{0};

  std::string label_top;
  std::string label_bottom;
  std::vector<RGB> pixels;
};

static CanvasWithColorbar compose_with_vertical_colorbar(const int img_w,
                                                        const int img_h,
                                                        const std::vector<RGB>& img_px,
                                                        double vmin,
                                                        double vmax,
                                                        const VerticalColorbarOptions& opt) {
  if (img_w <= 0 || img_h <= 0) throw std::runtime_error("compose_with_vertical_colorbar: invalid dimensions");
  if (static_cast<int>(img_px.size()) != img_w * img_h) {
    throw std::runtime_error("compose_with_vertical_colorbar: pixels size mismatch");
  }

  CanvasWithColorbar out;
  out.label_top = format_compact(vmax);
  out.label_bottom = format_compact(vmin);

  if (!opt.enabled) {
    out.width = img_w;
    out.height = img_h;
    out.x_img = 0;
    out.y_img = 0;
    out.pixels = img_px;
    return out;
  }

  const int pad_left = std::max(0, opt.pad_left);
  const int pad_right = std::max(0, opt.pad_right);
  const int pad_top = std::max(0, opt.pad_top);
  const int pad_bottom = std::max(0, opt.pad_bottom);
  const int gap = std::max(0, opt.gap);
  const int bar_w = std::max(2, opt.bar_width);
  const int scale = std::max(1, opt.font_scale);

  const int char_adv = (5 + 1) * scale;
  const int font_h = 7 * scale;

  int label_w = 0;
  if (opt.draw_labels) {
    const size_t max_len = std::max(out.label_top.size(), out.label_bottom.size());
    label_w = static_cast<int>(max_len) * char_adv + 4;
  }

  const int extra_label_block = (opt.draw_labels ? (gap + label_w) : 0);

  out.width = pad_left + img_w + gap + bar_w + extra_label_block + pad_right;
  out.height = pad_top + img_h + pad_bottom;

  out.x_img = pad_left;
  out.y_img = pad_top;
  out.x_bar = pad_left + img_w + gap;
  out.y_bar = pad_top;
  out.x_label = out.x_bar + bar_w + gap;

  // Center the label text inside the top/bottom padding regions.
  out.y_label_top = std::max(0, (pad_top - font_h) / 2);
  out.y_label_bottom = pad_top + img_h + std::max(0, (pad_bottom - font_h) / 2);

  out.pixels.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), RGB{255, 255, 255});

  // Copy image
  for (int y = 0; y < img_h; ++y) {
    for (int x = 0; x < img_w; ++x) {
      out.pixels[static_cast<size_t>(out.y_img + y) * static_cast<size_t>(out.width) +
                 static_cast<size_t>(out.x_img + x)] =
          img_px[static_cast<size_t>(y) * static_cast<size_t>(img_w) + static_cast<size_t>(x)];
    }
  }

  // Draw vertical colorbar (vmax at top, vmin at bottom)
  const double denom = (img_h <= 1) ? 1.0 : static_cast<double>(img_h - 1);
  for (int y = 0; y < img_h; ++y) {
    const double t01 = 1.0 - static_cast<double>(y) / denom;
    const RGB c = colormap_heat(t01);
    for (int x = 0; x < bar_w; ++x) {
      set_pixel(out.pixels, out.width, out.height, out.x_bar + x, out.y_bar + y, c);
    }
  }

  // Colorbar border
  draw_rect_border(out.pixels, out.width, out.height, out.x_bar, out.y_bar, bar_w, img_h, opt.border_color);

  // Labels
  if (opt.draw_labels) {
    draw_text(out.pixels, out.width, out.height,
              out.x_label, out.y_label_top,
              out.label_top, opt.text_color, scale);

    draw_text(out.pixels, out.width, out.height,
              out.x_label, out.y_label_bottom,
              out.label_bottom, opt.text_color, scale);
  }

  return out;
}

static void overlay_topomap(CanvasWithColorbar& canvas,
                            int grid_size,
                            const std::vector<Vec2>& electrodes_unit,
                            const TopomapAnnotationOptions& opt) {
  if (grid_size <= 0) return;

  std::vector<RGB>& px = canvas.pixels;
  const int w = canvas.width;
  const int h = canvas.height;

  const double cx = static_cast<double>(canvas.x_img) + 0.5 * static_cast<double>(grid_size - 1);
  const double cy = static_cast<double>(canvas.y_img) + 0.5 * static_cast<double>(grid_size - 1);
  const double r = 0.5 * static_cast<double>(grid_size - 1);

  if (opt.draw_head_outline) {
    draw_circle_outline(px, w, h, cx, cy, r, opt.outline_thickness, opt.outline_color);
  }

  if (opt.draw_nose) {
    // Draw a small nose marker above the top of the circle.
    const int nose_len = std::max(6, static_cast<int>(std::lround(r * 0.12)));
    const int nose_w = std::max(6, static_cast<int>(std::lround(r * 0.10)));

    const int x_mid = static_cast<int>(std::lround(cx));
    const int y_top = static_cast<int>(std::lround(cy - r));

    const int x_left = x_mid - nose_w / 2;
    const int x_right = x_mid + nose_w / 2;
    const int y_tip = y_top - nose_len;

    // Only draw if it fits at least partially.
    if (y_tip < h) {
      draw_line(px, w, h, x_left, y_top, x_mid, y_tip, opt.outline_color);
      draw_line(px, w, h, x_mid, y_tip, x_right, y_top, opt.outline_color);
    }
  }

  if (opt.draw_electrodes) {
    const int er = std::max(0, opt.electrode_radius);
    for (const Vec2& p_in : electrodes_unit) {
      const double x = std::max(-1.0, std::min(1.0, p_in.x));
      const double y = std::max(-1.0, std::min(1.0, p_in.y));

      const int ix = canvas.x_img + static_cast<int>(std::lround((x + 1.0) * 0.5 * static_cast<double>(grid_size - 1)));
      const int iy = canvas.y_img + static_cast<int>(std::lround((1.0 - y) * 0.5 * static_cast<double>(grid_size - 1)));

      fill_circle(px, w, h, ix, iy, er, opt.electrode_color);
    }
  }
}

} // namespace

void write_bmp24_with_vertical_colorbar(const std::string& path,
                                       int width,
                                       int height,
                                       const std::vector<RGB>& pixels,
                                       double vmin,
                                       double vmax,
                                       const VerticalColorbarOptions& opt) {
  CanvasWithColorbar canvas = compose_with_vertical_colorbar(width, height, pixels, vmin, vmax, opt);
  write_bmp24(path, canvas.width, canvas.height, canvas.pixels);
}

void render_grid_to_bmp_annotated(const std::string& path,
                                 int size,
                                 const std::vector<float>& grid_values,
                                 double vmin,
                                 double vmax,
                                 const std::vector<Vec2>& electrodes_unit,
                                 const AnnotatedTopomapOptions& opt) {
  if (size <= 0) throw std::runtime_error("render_grid_to_bmp_annotated: invalid size");
  if (static_cast<int>(grid_values.size()) != size * size) {
    throw std::runtime_error("render_grid_to_bmp_annotated: grid size mismatch");
  }
  if (!(vmax > vmin)) {
    vmax = vmin + 1e-12;
  }

  // Base heatmap pixels
  std::vector<RGB> base(static_cast<size_t>(size) * static_cast<size_t>(size));
  for (int j = 0; j < size; ++j) {
    for (int i = 0; i < size; ++i) {
      float v = grid_values[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)];
      RGB px{255, 255, 255};
      if (!std::isnan(v)) {
        const double t01 = (static_cast<double>(v) - vmin) / (vmax - vmin);
        px = colormap_heat(t01);
      }
      base[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)] = px;
    }
  }

  CanvasWithColorbar canvas = compose_with_vertical_colorbar(size, size, base, vmin, vmax, opt.colorbar);
  overlay_topomap(canvas, size, electrodes_unit, opt.overlay);

  write_bmp24(path, canvas.width, canvas.height, canvas.pixels);
}

} // namespace qeeg
