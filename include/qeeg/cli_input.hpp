#pragma once

#include <string>
#include <vector>

namespace qeeg {

// Options controlling how an "input" argument is resolved when the user passes a
// directory or a *_run_meta.json file.
//
// Many qeeg CLIs operate on a tabular per-channel CSV/TSV output (e.g.
// bandpowers.csv). To make CLIs chainable, we allow --input to point at:
//  - a direct .csv/.tsv file
//  - a *_run_meta.json file (we pick a tabular output listed in "Outputs")
//  - an output directory containing tabular files and/or run meta
//
// Nested run meta:
//   Many qeeg tools write *_run_meta.json that list other *_run_meta.json files
//   (for example, a pipeline workspace manifest). Resolvers follow nested run-meta
//   outputs recursively (bounded depth) to discover the concrete files.
struct ResolveInputTableOptions {
  // Preferred base filenames (case-insensitive) when multiple candidates exist.
  // Example: {"bandpowers.csv", "bandratios.csv"}.
  std::vector<std::string> preferred_filenames;

  // Preferred substrings (case-insensitive) used as a secondary ranking signal.
  // Example: {"coherence", "pairs"}.
  std::vector<std::string> preferred_contains;

  // If true, fall back to selecting any .csv/.tsv candidate when preferences do
  // not match.
  bool allow_any{true};
};

// Generic file resolver used for CLI chaining.
//
// This is similar to ResolveInputTableOptions, but supports an arbitrary set of
// allowed file extensions.
struct ResolveInputFileOptions {
  // Allowed file extensions (case-insensitive), including the leading dot.
  // Example: {".edf", ".bdf", ".vhdr"}.
  std::vector<std::string> allowed_extensions;

  // Preferred base filenames (case-insensitive) when multiple candidates exist.
  std::vector<std::string> preferred_filenames;

  // Preferred substrings (case-insensitive) used as a secondary ranking signal.
  std::vector<std::string> preferred_contains;

  // Substrings (case-insensitive) that should be avoided. Candidates whose
  // filename contains one of these strings are strongly penalized.
  std::vector<std::string> avoid_contains;

  // If true, fall back to selecting any allowed candidate even if preferences do
  // not match. If false, candidates with non-positive score are rejected.
  bool allow_any{true};
};

struct ResolvedInputPath {
  std::string path; // resolved file path
  std::string note; // optional human-readable note (e.g. "Resolved from run meta")
};

// Resolve a user-provided input spec into a concrete tabular path.
//
// Throws std::runtime_error on failure.
//
// Selector syntax:
//   To disambiguate when a directory or *_run_meta.json contains multiple
//   candidate files, append a selector after a '#':
//
//     <path>#<selector>
//
//   The selector can be an exact filename (case-insensitive), a substring,
//   or a simple glob pattern using '*' and '?'. Examples:
//
//     out_bandpower#bandpowers.csv
//     out_bandpower#*powers*
//     map_run_meta.json#bandpowers.csv
ResolvedInputPath resolve_input_table_path(const std::string& input_spec,
                                          const ResolveInputTableOptions& opt);

// Resolve a user-provided input spec into a concrete file path with one of the
// allowed extensions.
//
// Supports:
//  - a direct file
//  - a *_run_meta.json file (we pick a matching output listed in "Outputs")
//  - a directory containing matching files and/or run meta
//
// Nested run meta:
//   If a *_run_meta.json lists other *_run_meta.json files in its Outputs, the
//   resolver follows them recursively to locate compatible files.
//
// Throws std::runtime_error on failure.

// Selector syntax:
//   Same as resolve_input_table_path: <path>#<selector> can be used to
//   disambiguate when the input is a directory or *_run_meta.json that
//   contains multiple matching outputs.
ResolvedInputPath resolve_input_file_path(const std::string& input_spec,
                                         const ResolveInputFileOptions& opt);

// Convenience resolver for recording-like inputs accepted by qeeg::read_recording_auto.
//
// Supports EDF/BDF/BrainVision and common ASCII exports (CSV/TXT/TSV/ASC/ASCII),
// plus directories / *_run_meta.json that point to those files.
ResolvedInputPath resolve_input_recording_path(const std::string& input_spec);

} // namespace qeeg
