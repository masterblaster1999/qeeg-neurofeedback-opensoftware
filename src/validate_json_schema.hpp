// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace qeeg {

// Canonical JSON Schema locations for qeeg-* CLI machine-readable outputs.
//
// These URLs match the $id fields in the corresponding schema documents under /schemas.
inline std::string schema_url(const char* schema_file) {
  static constexpr const char* k_schema_base_url =
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/";
  return std::string(k_schema_base_url) + schema_file;
}

} // namespace qeeg
