#pragma once

#include "qeeg/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace qeeg {

class Montage {
public:
  Montage() = default;

  // Built-in approximate 10-20 19-channel (2D unit-circle coordinates).
  static Montage builtin_standard_1020_19();

  // Load montage from CSV: name,x,y (comments allowed with #).
  static Montage load_csv(const std::string& path);

  bool has(const std::string& channel) const;
  bool get(const std::string& channel, Vec2* out) const;

  std::vector<std::string> channel_names() const;

private:
  std::unordered_map<std::string, Vec2> pos_by_name_; // stored lowercase keys
};

} // namespace qeeg
