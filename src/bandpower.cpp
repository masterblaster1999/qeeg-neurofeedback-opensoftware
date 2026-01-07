#include "qeeg/bandpower.hpp"

#include "qeeg/utils.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>

namespace qeeg {

std::vector<BandDefinition> default_eeg_bands() {
  // Common band edges used in many EEG contexts.
  // (See README for caveats; these can vary by protocol.)
  return {
      {"delta", 0.5, 4.0},
      {"theta", 4.0, 7.0},
      {"alpha", 8.0, 12.0},
      {"beta", 13.0, 30.0},
      {"gamma", 30.0, 80.0},
  };
}

static BandDefinition parse_one_band(const std::string& token) {
  // token: name:fmin-fmax
  auto parts = split(token, ':');
  if (parts.size() != 2) {
    throw std::runtime_error("Invalid band token (expected name:fmin-fmax): " + token);
  }
  std::string name = trim(parts[0]);
  auto edges = split(parts[1], '-');
  if (edges.size() != 2) {
    throw std::runtime_error("Invalid band edges (expected fmin-fmax): " + token);
  }
  double fmin = to_double(edges[0]);
  double fmax = to_double(edges[1]);
  if (!(fmin >= 0.0 && fmax > fmin)) {
    throw std::runtime_error("Invalid band range in: " + token);
  }
  return {name, fmin, fmax};
}

std::vector<BandDefinition> parse_band_spec(const std::string& spec) {
  std::string s = trim(spec);
  if (s.empty()) return default_eeg_bands();

  std::vector<BandDefinition> out;
  for (const auto& tok : split(s, ',')) {
    std::string t = trim(tok);
    if (t.empty()) continue;
    out.push_back(parse_one_band(t));
  }
  if (out.empty()) return default_eeg_bands();
  return out;
}

double integrate_bandpower(const PsdResult& psd, double fmin_hz, double fmax_hz) {
  if (psd.freqs_hz.size() != psd.psd.size() || psd.freqs_hz.size() < 2) {
    throw std::runtime_error("integrate_bandpower: invalid psd input");
  }
  if (!(fmax_hz > fmin_hz)) {
    throw std::runtime_error("integrate_bandpower: fmax must be > fmin");
  }

  double area = 0.0;
  for (size_t i = 0; i + 1 < psd.freqs_hz.size(); ++i) {
    double f0 = psd.freqs_hz[i];
    double f1 = psd.freqs_hz[i + 1];
    double p0 = psd.psd[i];
    double p1 = psd.psd[i + 1];

    // Overlap with [fmin, fmax]
    double a = std::max(f0, fmin_hz);
    double b = std::min(f1, fmax_hz);
    if (b <= a) continue;

    // Linear interpolation for p at boundaries:
    auto lerp = [](double x0, double y0, double x1, double y1, double x) -> double {
      if (x1 == x0) return y0;
      double t = (x - x0) / (x1 - x0);
      return y0 + t * (y1 - y0);
    };

    double pa = lerp(f0, p0, f1, p1, a);
    double pb = lerp(f0, p0, f1, p1, b);

    // trapezoid area over [a,b]
    area += 0.5 * (pa + pb) * (b - a);
  }
  return area;
}

ReferenceStats load_reference_csv(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open reference CSV: " + path);

  ReferenceStats ref;
  std::string line;
  size_t lineno = 0;

  while (std::getline(f, line)) {
    ++lineno;
    std::string t = trim(line);
    if (t.empty()) continue;
    if (starts_with(t, "#")) continue;

    auto cols = split(t, ',');
    if (cols.size() < 4) {
      throw std::runtime_error("Reference CSV parse error at line " + std::to_string(lineno) +
                               " (expected channel,band,mean,std)");
    }

    std::string ch = to_lower(trim(cols[0]));
    std::string band = to_lower(trim(cols[1]));
    double mean = to_double(cols[2]);
    double stdv = to_double(cols[3]);
    if (stdv <= 0.0) continue;

    std::string key = band + "|" + ch;
    ref.mean[key] = mean;
    ref.stdev[key] = stdv;
  }

  return ref;
}

bool compute_zscore(const ReferenceStats& ref,
                    const std::string& channel,
                    const std::string& band,
                    double value,
                    double* out_z) {
  std::string key = to_lower(band) + "|" + to_lower(channel);
  auto it_m = ref.mean.find(key);
  auto it_s = ref.stdev.find(key);
  if (it_m == ref.mean.end() || it_s == ref.stdev.end()) return false;
  double stdv = it_s->second;
  if (stdv <= 0.0) return false;
  *out_z = (value - it_m->second) / stdv;
  return true;
}

} // namespace qeeg
