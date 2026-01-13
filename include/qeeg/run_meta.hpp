#pragma once

#include <string>
#include <vector>

namespace qeeg {

// Read the "Outputs" array from a qeeg *_run_meta.json file.
//
// This is a minimal, dependency-free JSON reader intended for files emitted by
// this project. It is not a general JSON parser.
//
// If the file cannot be read or the key does not exist, returns an empty vector.
std::vector<std::string> read_run_meta_outputs(const std::string& json_path);

// Read the top-level "Tool" string (e.g., "qeeg_map_cli") from a qeeg run
// meta JSON file. Returns an empty string if missing/unreadable.
std::string read_run_meta_tool(const std::string& json_path);

} // namespace qeeg
