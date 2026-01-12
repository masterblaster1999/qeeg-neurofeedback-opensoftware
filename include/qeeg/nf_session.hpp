#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace qeeg {

// Small helpers for interoperating with qeeg_nf_cli output folders.
//
// nf_cli can export derived segments/events into stable filenames inside --outdir.
// Other tools can accept --nf-outdir and auto-attach these events without users
// having to remember the exact filename.
//
// These helpers are intentionally tiny and dependency-light (C++17 filesystem).

inline std::filesystem::path normalize_nf_outdir_path(const std::string& nf_outdir) {
  if (nf_outdir.empty()) return {};
  std::error_code ec;
  const auto p = std::filesystem::u8path(nf_outdir);

  // Allow users to pass a file within the outdir (e.g. nf_run_meta.json or
  // biotrace_ui.html) and still resolve the output directory.
  if (std::filesystem::is_regular_file(p, ec)) {
    return p.parent_path();
  }

  return p;
}

inline std::filesystem::path nf_derived_events_csv_path(const std::string& nf_outdir) {
  const auto dir = normalize_nf_outdir_path(nf_outdir);
  if (dir.empty()) return {};
  return dir / "nf_derived_events.csv";
}

inline std::filesystem::path nf_derived_events_tsv_path(const std::string& nf_outdir) {
  const auto dir = normalize_nf_outdir_path(nf_outdir);
  if (dir.empty()) return {};
  return dir / "nf_derived_events.tsv";
}

// Preferred lookup for derived events: return TSV if present, otherwise CSV.
inline std::optional<std::string> find_nf_derived_events_table(const std::string& nf_outdir) {
  if (nf_outdir.empty()) return std::nullopt;
  std::error_code ec;

  const auto p_tsv = nf_derived_events_tsv_path(nf_outdir);
  if (!p_tsv.empty() && std::filesystem::is_regular_file(p_tsv, ec)) {
    return p_tsv.u8string();
  }

  const auto p_csv = nf_derived_events_csv_path(nf_outdir);
  if (!p_csv.empty() && std::filesystem::is_regular_file(p_csv, ec)) {
    return p_csv.u8string();
  }

  return std::nullopt;
}

// Backwards-compatible helper.
inline std::optional<std::string> find_nf_derived_events_csv(const std::string& nf_outdir) {
  if (nf_outdir.empty()) return std::nullopt;
  std::error_code ec;
  const auto p = nf_derived_events_csv_path(nf_outdir);
  if (!p.empty() && std::filesystem::is_regular_file(p, ec)) {
    return p.u8string();
  }
  return std::nullopt;
}

} // namespace qeeg
