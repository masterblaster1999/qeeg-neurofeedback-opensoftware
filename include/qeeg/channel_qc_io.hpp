#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace qeeg {

// Small helpers for interoperating with qeeg_channel_qc_cli output folders.
//
// channel_qc_cli writes:
//   - channel_qc.csv      (per-channel metrics + bad flag + reasons)
//   - bad_channels.txt    (one channel per line)
//
// Other tools (e.g., qeeg_export_bids_cli) can consume these files to populate
// BIDS channels.tsv status/status_description columns.

struct ChannelQcLabel {
  bool bad{false};
  std::string reasons; // may be empty

  // Best-effort original label as it appeared in the input file.
  //
  // This is useful when a downstream tool wants to re-emit a channel list
  // (for example, writing a BIDS channels.tsv derivative) without losing the
  // user's original casing/spaces.
  std::string name;
};

// Map key is normalize_channel_name(channel).
using ChannelQcMap = std::unordered_map<std::string, ChannelQcLabel>;

// Load channel_qc.csv produced by qeeg_channel_qc_cli.
//
// Required columns:
//   - channel (or name)
//   - bad
// Optional columns:
//   - reasons
ChannelQcMap load_channel_qc_csv(const std::string& path);

// Load the channel list (in file order) from channel_qc.csv produced by
// qeeg_channel_qc_cli.
//
// This is primarily useful when a tool wants to preserve channel order without
// having access to the original recording (for example, when exporting a BIDS
// channels.tsv derivative from QC output).
std::vector<std::string> load_channel_qc_csv_channel_names(const std::string& path);

// Load bad_channels.txt (one channel name per line).
ChannelQcMap load_bad_channels_list(const std::string& path);

// Convenience loader:
//   - If `path` is a directory, loads <path>/channel_qc.csv (preferred) or <path>/bad_channels.txt.
//   - If `path` is a file, loads it based on extension (.csv => channel_qc.csv format; otherwise list).
//   - If `path` is a file with an unrecognized extension, treats its parent directory as an outdir.
//
// If resolved_path is non-null, it is set to the file that was loaded.
ChannelQcMap load_channel_qc_any(const std::string& path, std::string* resolved_path = nullptr);

} // namespace qeeg
