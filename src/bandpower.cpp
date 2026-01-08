#include "qeeg/bandpower.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace {

size_t count_delim_outside_quotes(const std::string& s, char delim) {
  bool in_quotes = false;
  size_t n = 0;

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      if (in_quotes && (i + 1) < s.size() && s[i + 1] == '"') {
        ++i;
        continue;
      }
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && c == delim) ++n;
  }
  return n;
}

char detect_delim(const std::string& line) {
  const size_t n_comma = count_delim_outside_quotes(line, ',');
  const size_t n_semi  = count_delim_outside_quotes(line, ';');
  const size_t n_tab   = count_delim_outside_quotes(line, '\t');

  char best = ',';
  size_t best_n = n_comma;
  if (n_semi > best_n) {
    best = ';';
    best_n = n_semi;
  }
  if (n_tab > best_n) {
    best = '\t';
    best_n = n_tab;
  }
  return best;
}

bool is_comment_or_empty(const std::string& t) {
  if (t.empty()) return true;
  if (qeeg::starts_with(t, "#")) return true;
  if (qeeg::starts_with(t, "//")) return true;
  return false;
}

std::vector<std::string> parse_row(const std::string& raw, char delim) {
  auto cols = qeeg::split_csv_row(raw, delim);
  for (auto& c : cols) c = qeeg::trim(c);
  return cols;
}

bool looks_like_reference_header(const std::vector<std::string>& cols) {
  if (cols.size() < 4) return false;
  const std::string c0 = qeeg::to_lower(cols[0]);
  const std::string c1 = qeeg::to_lower(cols[1]);
  const std::string c2 = qeeg::to_lower(cols[2]);
  const std::string c3 = qeeg::to_lower(cols[3]);

  const bool ok0 = (c0 == "channel" || c0 == "ch" || c0 == "name");
  const bool ok1 = (c1 == "band");
  const bool ok2 = (c2 == "mean");
  const bool ok3 = (c3 == "std" || c3 == "stdev" || c3 == "stddev");
  return ok0 && ok1 && ok2 && ok3;
}

} // namespace

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
  bool saw_header_or_data = false;
  char delim = ',';

  while (std::getline(f, line)) {
    ++lineno;
    std::string raw = line;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::string t = trim(raw);
    if (is_comment_or_empty(t)) continue;

    if (!saw_header_or_data) {
      t = strip_utf8_bom(t);
      delim = detect_delim(t);
    }

    auto cols = parse_row(t, delim);
    if (cols.empty()) continue;

    if (!saw_header_or_data) {
      saw_header_or_data = true;
      if (looks_like_reference_header(cols)) {
        continue; // skip header
      }
    }

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
