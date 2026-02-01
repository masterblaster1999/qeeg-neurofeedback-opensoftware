#pragma once

#include <string>
#include <vector>

namespace qeeg {

// Summary fields extracted from a qeeg *_run_meta.json file.
//
// This is a minimal, dependency-free extractor intended for JSON files emitted
// by this project. It is not a general JSON parser.
struct RunMetaSummary {
  std::string tool;
  std::string input_path;
  std::string timestamp_local;
  std::string timestamp_utc;
  std::string version;        // prefers QeegVersion; falls back to Version
  std::string git_describe;
  std::string build_type;
  std::string compiler;
  std::string cpp_standard;
  std::vector<std::string> outputs;
};

// Read a RunMetaSummary from a qeeg run meta JSON file.
//
// Best-effort: missing keys remain empty.
RunMetaSummary read_run_meta_summary(const std::string& json_path);

// Read the "Outputs" array from a qeeg *_run_meta.json file.
//
// This is a minimal, dependency-free JSON reader intended for files emitted by
// this project. It is not a general JSON parser.
//
// Output path safety:
//   The returned strings are intended to be interpreted as paths *relative to*
//   the run meta file's directory.
//
//   To make CLI chaining safer and more robust across platforms, this reader
//   omits entries that attempt to escape the run directory (e.g., contain ".."
//   segments) or that look like absolute paths / drive-prefixed paths.
//
//   Path separators are normalized by converting '\\' to '/'. Any entry that
//   contains an embedded NUL byte is ignored.
//
// If the file cannot be read or the key does not exist, returns an empty vector.
std::vector<std::string> read_run_meta_outputs(const std::string& json_path);

// Read the top-level "Tool" string (e.g., "qeeg_map_cli") from a qeeg run
// meta JSON file. Returns an empty string if missing/unreadable.
std::string read_run_meta_tool(const std::string& json_path);

// Read timestamps written by qeeg::write_run_meta_json.
//
// These are best-effort and return an empty string if the key is missing.
std::string read_run_meta_timestamp_local(const std::string& json_path);
std::string read_run_meta_timestamp_utc(const std::string& json_path);

// Read version/build identifiers from a qeeg run meta JSON.
//
// Best-effort:
//   - Version prefers "QeegVersion" but falls back to "Version" (nf_run_meta.json).
//   - Git describe reads "GitDescribe".
std::string read_run_meta_version(const std::string& json_path);
std::string read_run_meta_git_describe(const std::string& json_path);

// Read basic build information from a qeeg run meta JSON.
//
// Best-effort and returns empty string if missing.
std::string read_run_meta_build_type(const std::string& json_path);
std::string read_run_meta_compiler(const std::string& json_path);
std::string read_run_meta_cpp_standard(const std::string& json_path);

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
// The output schema is intentionally small and stable so it can be produced without a
// JSON dependency and consumed by qeeg_ui_cli / qeeg_ui_server_cli.
//
// Keys written (top-level):
//   - Tool
//   - QeegVersion
//   - GitDescribe
//   - BuildType
//   - Compiler
//   - CppStandard
//   - TimestampLocal
//   - TimestampUTC
//   - OutputDir
//   - InputPath (string or null)
//   - Outputs (array of relative paths)
//
// Parameters:
//   - json_path: full path to the JSON file to write (usually <outdir>/*_run_meta.json).
//   - tool: executable name (e.g. "qeeg_coherence_cli").
//   - outdir: tool output directory (for convenience; stored in OutputDir).
//   - input_path: input file path (may be empty; stored as null).
//   - outputs: relative output paths (relative to outdir).
//
// Returns true on success, false on write failure.
bool write_run_meta_json(const std::string& json_path,
                         const std::string& tool,
                         const std::string& outdir,
                         const std::string& input_path,
                         const std::vector<std::string>& outputs);

} // namespace qeeg
