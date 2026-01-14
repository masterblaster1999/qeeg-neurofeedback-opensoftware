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

// Read the input path from a qeeg run meta JSON file.
//
// Different tools have historically emitted slightly different schemas, so
// this function is best-effort and checks multiple keys:
//
//   - Top-level "InputPath" (written by qeeg::write_run_meta_json)
//   - Top-level "input_path" (legacy nf_run_meta.json)
//   - Nested "Input": { "Path": ... } (e.g., map_run_meta.json, iaf_run_meta.json)
//
// Returns an empty string if missing/unreadable.
std::string read_run_meta_input_path(const std::string& json_path);

// Write a qeeg *_run_meta.json file (lightweight JSON emitter).
//
// This is intended for tools that want to expose their outputs to qeeg_ui_cli
// and qeeg_ui_server_cli without adding a full JSON dependency.
//
// - json_path: full path to the JSON file to write (usually <outdir>/*_run_meta.json).
// - tool: executable name (e.g. "qeeg_coherence_cli").
// - outdir: tool output directory (for convenience; stored in OutputDir).
// - input_path: input file path (may be empty; stored as null).
// - outputs: relative output paths (relative to outdir).
//
// Returns true on success, false on write failure.
bool write_run_meta_json(const std::string& json_path,
                        const std::string& tool,
                        const std::string& outdir,
                        const std::string& input_path,
                        const std::vector<std::string>& outputs);

} // namespace qeeg
