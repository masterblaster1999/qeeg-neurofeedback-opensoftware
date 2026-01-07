#pragma once

#include <string>
#include <vector>

namespace qeeg {

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string to_lower(std::string s);

bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

int to_int(const std::string& s);
double to_double(const std::string& s);

bool file_exists(const std::string& path);
void ensure_directory(const std::string& path);

} // namespace qeeg
