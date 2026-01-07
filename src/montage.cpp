#include "qeeg/montage.hpp"

#include "qeeg/utils.hpp"

#include <fstream>
#include <stdexcept>

namespace qeeg {

static std::string key(const std::string& ch) { return to_lower(trim(ch)); }

Montage Montage::builtin_standard_1020_19() {
  Montage m;

  // NOTE: These 2D coordinates are intentionally simple and approximate (unit circle head model).
  // For accurate neurophysiology/clinical work, use digitized electrode locations or a vetted template montage.
  //
  // Common 19-channel 10-20 labels: Fp1 Fp2 F7 F3 Fz F4 F8 T3 C3 Cz C4 T4 T5 P3 Pz P4 T6 O1 O2
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

  put("T3",  -1.02,  0.00); // older name for T7
  put("C3",  -0.52,  0.02);
  put("Cz",   0.00,  0.00);
  put("C4",   0.52,  0.02);
  put("T4",   1.02,  0.00); // older name for T8

  put("T5",  -0.92, -0.55); // older name for P7
  put("P3",  -0.42, -0.52);
  put("Pz",   0.00, -0.56);
  put("P4",   0.42, -0.52);
  put("T6",   0.92, -0.55); // older name for P8

  put("O1",  -0.50, -0.92);
  put("O2",   0.50, -0.92);

  return m;
}

Montage Montage::load_csv(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open montage CSV: " + path);

  Montage m;
  std::string line;
  size_t lineno = 0;
  while (std::getline(f, line)) {
    ++lineno;
    std::string t = trim(line);
    if (t.empty() || starts_with(t, "#")) continue;

    // allow header row
    if (lineno == 1) {
      auto cols = split(t, ',');
      if (cols.size() >= 3) {
        std::string c0 = to_lower(trim(cols[0]));
        std::string c1 = to_lower(trim(cols[1]));
        std::string c2 = to_lower(trim(cols[2]));
        if ((c0 == "name" || c0 == "channel") && (c1 == "x") && (c2 == "y")) {
          continue;
        }
      }
    }

    auto cols = split(t, ',');
    if (cols.size() < 3) {
      throw std::runtime_error("Montage CSV parse error at line " + std::to_string(lineno) +
                               " (expected name,x,y)");
    }
    std::string name = trim(cols[0]);
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
