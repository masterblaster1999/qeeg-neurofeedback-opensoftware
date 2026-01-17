#pragma once

#include <string>

namespace qeeg {

// Project version string as defined by CMake's project(VERSION ...).
//
// This header is intentionally dependency-free and safe to include from both the
// library and CLI tools.
//
// CMake defines QEEG_VERSION_STRING for all targets that link against the core
// qeeg library.
#ifndef QEEG_VERSION_STRING
  #define QEEG_VERSION_STRING "0.0.0"
#endif

inline const char* version_cstr() {
  return QEEG_VERSION_STRING;
}

inline std::string version_string() {
  return std::string(version_cstr());
}

inline std::string build_type_string() {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

inline std::string compiler_string() {
#if defined(__clang__)
  return std::string("Clang ") + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  return std::string("GCC ") + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  return std::string("MSVC ") + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

inline std::string cpp_standard_string() {
  const long long v = static_cast<long long>(__cplusplus);
  if (v >= 202302L) return "c++23";
  if (v >= 202002L) return "c++20";
  if (v >= 201703L) return "c++17";
  if (v >= 201402L) return "c++14";
  if (v >= 201103L) return "c++11";
  return "c++?";
}

} // namespace qeeg
