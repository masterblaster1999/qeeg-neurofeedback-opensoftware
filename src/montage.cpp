#include "qeeg/montage.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace qeeg {

static std::string key(const std::string& ch) { return normalize_channel_name(ch); }

namespace {

size_t count_delim_outside_quotes(const std::string& s, char delim) {
  bool in_quotes = false;
  size_t n = 0;

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      if (in_quotes && (i + 1) < s.size() && s[i + 1] == '"') {
        // Escaped quote
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
  if (starts_with(t, "#")) return true;
  if (starts_with(t, "//")) return true;
  return false;
}

std::vector<std::string> parse_row(const std::string& raw, char delim) {
  auto cols = split_csv_row(raw, delim);
  for (auto& c : cols) c = trim(c);
  return cols;
}

bool looks_like_header(const std::vector<std::string>& cols) {
  if (cols.size() < 3) return false;
  const std::string c0 = to_lower(cols[0]);
  const std::string c1 = to_lower(cols[1]);
  const std::string c2 = to_lower(cols[2]);
  const bool ok0 = (c0 == "name" || c0 == "channel" || c0 == "ch");
  return ok0 && c1 == "x" && c2 == "y";
}

} // namespace

Montage Montage::builtin_standard_1020_19() {
  Montage m;

  // NOTE: These 2D coordinates are intentionally simple and approximate (unit circle head model).
  // For accurate neurophysiology/clinical work, use digitized electrode locations or a vetted template montage.
  //
  // Common 19-channel 10-20 labels (modern):
  // Fp1 Fp2 F7 F3 Fz F4 F8 T7 C3 Cz C4 T8 P7 P3 Pz P4 P8 O1 O2
  //
  // NOTE: Older naming conventions use T3/T4/T5/T6 in place of T7/T8/P7/P8.
  // We normalize those aliases so either spelling matches.
  auto put = [&](const std::string& name, double x, double y) {
    m.pos_by_name_[key(name)] = Vec2{x, y};
  };

  put("Fp1", -0.50,  0.92);
  put("Fp2",  0.50,  0.92);

  put("F7",  -0.92,  0.62);
  put("F3",  -0.42,  0.55);
  put("Fz",   0.00,  0.58);
  put("F4",   0.42,  0.55);
  put("F8",   0.92,  0.62);

  put("T7",  -1.02,  0.00);
  put("C3",  -0.52,  0.02);
  put("Cz",   0.00,  0.00);
  put("C4",   0.52,  0.02);
  put("T8",   1.02,  0.00);

  put("P7",  -0.92, -0.55);
  put("P3",  -0.42, -0.52);
  put("Pz",   0.00, -0.56);
  put("P4",   0.42, -0.52);
  put("P8",   0.92, -0.55);

  put("O1",  -0.50, -0.92);
  put("O2",   0.50, -0.92);

  return m;
}



Montage Montage::builtin_standard_1010_61() {
  Montage m;

  // NOTE: These 2D coordinates are intentionally simple and approximate (unit circle head model).
  // They provide a reasonable out-of-the-box montage for common 10-10/10-20 channel labels.
  //
  // Coordinate convention (matches qeeg topomap renderer):
  //   x: left (-) to right (+)
  //   y: frontal/nasion (+) to occipital (-)
  //
  // For accurate neurophysiology/clinical work, use digitized electrode locations or a vetted template montage.
  auto put = [&](const std::string& name, double x, double y) {
    m.pos_by_name_[key(name)] = Vec2{x, y};
  };

  // Midline (F...O) and common 10-10 rings (approx).
  // This set mirrors the lightweight 10-10 coordinates used in the HTML dashboard preview.
  put("Fp1", -0.35,  0.92);
  put("Fpz",  0.00,  0.98);
  put("Fp2",  0.35,  0.92);

  put("AF7", -0.55,  0.80);
  put("AF3", -0.25,  0.78);
  put("AFz",  0.00,  0.80);
  put("AF4",  0.25,  0.78);
  put("AF8",  0.55,  0.80);

  put("F7",  -0.82,  0.62);
  put("F5",  -0.62,  0.62);
  put("F3",  -0.42,  0.62);
  put("F1",  -0.18,  0.62);
  put("Fz",   0.00,  0.62);
  put("F2",   0.18,  0.62);
  put("F4",   0.42,  0.62);
  put("F6",   0.62,  0.62);
  put("F8",   0.82,  0.62);

  put("FT7", -0.92,  0.34);
  put("FC5", -0.66,  0.34);
  put("FC3", -0.42,  0.34);
  put("FC1", -0.18,  0.34);
  put("FCz",  0.00,  0.34);
  put("FC2",  0.18,  0.34);
  put("FC4",  0.42,  0.34);
  put("FC6",  0.66,  0.34);
  put("FT8",  0.92,  0.34);

  put("T7",  -1.00,  0.00);
  put("C5",  -0.66,  0.00);
  put("C3",  -0.42,  0.00);
  put("C1",  -0.18,  0.00);
  put("Cz",   0.00,  0.00);
  put("C2",   0.18,  0.00);
  put("C4",   0.42,  0.00);
  put("C6",   0.66,  0.00);
  put("T8",   1.00,  0.00);

  put("TP7", -0.92, -0.34);
  put("CP5", -0.66, -0.34);
  put("CP3", -0.42, -0.34);
  put("CP1", -0.18, -0.34);
  put("CPz",  0.00, -0.34);
  put("CP2",  0.18, -0.34);
  put("CP4",  0.42, -0.34);
  put("CP6",  0.66, -0.34);
  put("TP8",  0.92, -0.34);

  put("P7",  -0.82, -0.62);
  put("P5",  -0.62, -0.62);
  put("P3",  -0.42, -0.62);
  put("P1",  -0.18, -0.62);
  put("Pz",   0.00, -0.62);
  put("P2",   0.18, -0.62);
  put("P4",   0.42, -0.62);
  put("P6",   0.62, -0.62);
  put("P8",   0.82, -0.62);

  put("PO7", -0.55, -0.80);
  put("PO3", -0.25, -0.80);
  put("POz",  0.00, -0.84);
  put("PO4",  0.25, -0.80);
  put("PO8",  0.55, -0.80);

  put("O1",  -0.35, -0.94);
  put("Oz",   0.00, -0.98);
  put("O2",   0.35, -0.94);

  return m;
}

Montage Montage::load_csv(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open montage CSV: " + path);

  Montage m;
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
      // Determine delimiter from the first non-empty line.
      t = strip_utf8_bom(t);
      delim = detect_delim(t);
    }

    auto cols = parse_row(t, delim);
    if (cols.empty()) continue;

    if (!saw_header_or_data) {
      saw_header_or_data = true;
      if (looks_like_header(cols)) {
        continue; // skip header
      }
    }

    if (cols.size() < 3) {
      throw std::runtime_error("Montage CSV parse error at line " + std::to_string(lineno) +
                               " (expected name,x,y)");
    }
    std::string name = trim(cols[0]);
    if (name.empty()) {
      throw std::runtime_error("Montage CSV parse error at line " + std::to_string(lineno) +
                               " (empty channel name)");
    }
    double x = to_double(cols[1]);
    double y = to_double(cols[2]);
    m.pos_by_name_[key(name)] = Vec2{x, y};
  }

  if (m.pos_by_name_.empty()) {
    throw std::runtime_error("Montage CSV contained no channels: " + path);
  }
  return m;
}

bool Montage::has(const std::string& channel) const {
  return pos_by_name_.find(key(channel)) != pos_by_name_.end();
}

bool Montage::get(const std::string& channel, Vec2* out) const {
  auto it = pos_by_name_.find(key(channel));
  if (it == pos_by_name_.end()) return false;
  *out = it->second;
  return true;
}

std::vector<std::string> Montage::channel_names() const {
  std::vector<std::string> names;
  names.reserve(pos_by_name_.size());
  for (const auto& kv : pos_by_name_) names.push_back(kv.first);
  return names;
}

} // namespace qeeg
