#pragma once

#include "qeeg/types.hpp"

#include <string>
#include <unordered_map>

namespace qeeg {

struct ChannelMap {
  // Mapping from normalized channel key -> desired channel name.
  // Keys are produced by normalize_channel_name().
  std::unordered_map<std::string, std::string> normalized_to_name;
};

// Load a mapping file that remaps channel labels.
//
// Supported formats (one mapping per line):
//   old,new        (CSV; delimiter can be ',' ';' or tab; optional header allowed)
//   old=new
//
// Lines starting with '#' are treated as comments.
//
// The 'new' value may be empty or one of: "DROP", "NONE" (case-insensitive) to drop
// that channel.
ChannelMap load_channel_map_file(const std::string& path);

// Apply a ChannelMap to an EEGRecording in-place.
// - Channels present in the map are renamed (or dropped).
// - Channels not mentioned are kept as-is.
// - Throws if the resulting channel set has duplicate normalized names.
void apply_channel_map(EEGRecording* rec, const ChannelMap& map);

// Write a channel-map template CSV with two columns ("old,new") where "new" is blank.
void write_channel_map_template(const std::string& path, const EEGRecording& rec);

} // namespace qeeg
