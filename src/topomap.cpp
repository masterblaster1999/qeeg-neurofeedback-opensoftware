#include "qeeg/topomap.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace qeeg {

static inline bool inside_head(double x, double y) {
  return (x*x + y*y) <= 1.0;
}

Grid2D make_topomap_idw(const Montage& montage,
                        const std::vector<std::string>& channel_names,
                        const std::vector<double>& channel_values,
                        const TopomapOptions& opt) {
  if (channel_names.size() != channel_values.size()) {
    throw std::runtime_error("make_topomap_idw: channel_names and channel_values size mismatch");
  }
  if (opt.grid_size < 8) throw std::runtime_error("make_topomap_idw: grid_size too small");

  // Gather points
  std::vector<Vec2> pos;
  std::vector<double> val;
  pos.reserve(channel_names.size());
  val.reserve(channel_names.size());

  for (size_t i = 0; i < channel_names.size(); ++i) {
    const double v = channel_values[i];
    if (!std::isfinite(v)) continue; // allow callers to mask channels with NaN/Inf
    Vec2 p;
    if (!montage.get(channel_names[i], &p)) continue;
    pos.push_back(p);
    val.push_back(v);
  }

  if (pos.size() < 3) {
    throw std::runtime_error("make_topomap_idw: need at least 3 channels with montage positions");
  }

  Grid2D grid;
  grid.size = opt.grid_size;
  grid.values.assign(static_cast<size_t>(grid.size) * static_cast<size_t>(grid.size),
                     std::numeric_limits<float>::quiet_NaN());

  const int N = grid.size;

  for (int j = 0; j < N; ++j) {
    // y: +1 at top, -1 at bottom
    double y = 1.0 - 2.0 * static_cast<double>(j) / static_cast<double>(N - 1);
    for (int i = 0; i < N; ++i) {
      double x = -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(N - 1);

      if (!inside_head(x, y)) {
        continue; // leave NaN
      }

      // IDW interpolation
      double num = 0.0;
      double den = 0.0;
      bool snapped = false;
      double snapped_val = 0.0;

      for (size_t k = 0; k < pos.size(); ++k) {
        double dx = x - pos[k].x;
        double dy = y - pos[k].y;
        double d2 = dx*dx + dy*dy;

        if (d2 <= opt.idw_eps * opt.idw_eps) {
          snapped = true;
          snapped_val = val[k];
          break;
        }

        double d = std::sqrt(d2);
        double w = 1.0 / std::pow(d, opt.idw_power);
        num += w * val[k];
        den += w;
      }

      float out = std::numeric_limits<float>::quiet_NaN();
      if (snapped) {
        out = static_cast<float>(snapped_val);
      } else if (den > 0.0) {
        out = static_cast<float>(num / den);
      }
      grid.values[static_cast<size_t>(j) * static_cast<size_t>(N) + static_cast<size_t>(i)] = out;
    }
  }

  return grid;
}


Grid2D make_topomap_spherical_spline(const Montage& montage,
                                    const std::vector<std::string>& channel_names,
                                    const std::vector<double>& channel_values,
                                    const TopomapOptions& opt) {
  if (channel_names.size() != channel_values.size()) {
    throw std::runtime_error("make_topomap_spherical_spline: channel_names and channel_values size mismatch");
  }
  if (opt.grid_size < 8) throw std::runtime_error("make_topomap_spherical_spline: grid_size too small");

  // Gather points on unit sphere (z >= 0 hemisphere)
  std::vector<Vec3> pos;
  std::vector<double> val;
  pos.reserve(channel_names.size());
  val.reserve(channel_names.size());

  for (size_t i = 0; i < channel_names.size(); ++i) {
    const double v = channel_values[i];
    if (!std::isfinite(v)) continue; // allow callers to mask channels with NaN/Inf
    Vec2 p2;
    if (!montage.get(channel_names[i], &p2)) continue;
    pos.push_back(project_to_unit_sphere(p2));
    val.push_back(v);
  }

  if (pos.size() < 3) {
    throw std::runtime_error("make_topomap_spherical_spline: need at least 3 channels with montage positions");
  }

  const SphericalSplineInterpolator interp = SphericalSplineInterpolator::fit(pos, val, opt.spline);

  Grid2D grid;
  grid.size = opt.grid_size;
  grid.values.assign(static_cast<size_t>(grid.size) * static_cast<size_t>(grid.size),
                     std::numeric_limits<float>::quiet_NaN());

  const int N = grid.size;

  for (int j = 0; j < N; ++j) {
    // y: +1 at top, -1 at bottom
    const double y = 1.0 - 2.0 * static_cast<double>(j) / static_cast<double>(N - 1);
    for (int i = 0; i < N; ++i) {
      const double x = -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(N - 1);

      if (!inside_head(x, y)) {
        continue; // leave NaN
      }

      const Vec3 q = project_to_unit_sphere(Vec2{x, y});
      const double v = interp.evaluate(q);
      grid.values[static_cast<size_t>(j) * static_cast<size_t>(N) + static_cast<size_t>(i)] =
          static_cast<float>(v);
    }
  }

  return grid;
}

Grid2D make_topomap(const Montage& montage,
                    const std::vector<std::string>& channel_names,
                    const std::vector<double>& channel_values,
                    const TopomapOptions& opt) {
  switch (opt.method) {
    case TopomapInterpolation::IDW:
      return make_topomap_idw(montage, channel_names, channel_values, opt);
    case TopomapInterpolation::SPHERICAL_SPLINE:
      return make_topomap_spherical_spline(montage, channel_names, channel_values, opt);
    default:
      return make_topomap_idw(montage, channel_names, channel_values, opt);
  }
}

} // namespace qeeg
