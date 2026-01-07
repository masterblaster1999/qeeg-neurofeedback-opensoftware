#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace qeeg {

static inline bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && is_space(s[b])) ++b;
  size_t e = s.size();
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  // Handle trailing empty field
  if (!s.empty() && s.back() == delim) out.emplace_back("");
  return out;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int to_int(const std::string& s) {
  try {
    size_t idx = 0;
    int v = std::stoi(trim(s), &idx, 10);
    if (idx == 0) throw std::invalid_argument("no digits");
    return v;
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse int from '" + s + "': " + e.what());
  }
}

double to_double(const std::string& s) {
  try {
    size_t idx = 0;
    double v = std::stod(trim(s), &idx);
    if (idx == 0) throw std::invalid_argument("no digits");
    return v;
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse double from '" + s + "': " + e.what());
  }
}

bool file_exists(const std::string& path) {
  return std::filesystem::exists(std::filesystem::u8path(path));
}

void ensure_directory(const std::string& path) {
  std::filesystem::create_directories(std::filesystem::u8path(path));
}

} // namespace qeeg
