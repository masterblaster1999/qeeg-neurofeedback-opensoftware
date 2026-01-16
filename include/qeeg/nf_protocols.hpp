#pragma once

#include "qeeg/nf_threshold.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qeeg {

// A tiny set of built-in neurofeedback protocol presets.
//
// These are intended as *starting points* and examples only.
// Protocols in practice vary widely (channels, bands, reward/inhibit choices,
// thresholds, artifact handling, etc.).
struct NfProtocolPreset {
  // Stable machine-readable identifier (recommended lowercase).
  std::string name;

  // Human-friendly title and one-line description.
  std::string title;
  std::string description;

  // `qeeg_nf_cli --metric ...` template.
  //
  // Supported placeholders:
  //  - {ch}      : primary channel (band/ratio/PAC)
  //  - {a}, {b}  : channel pair (coherence)
  //
  // Example: "smr:{ch}" or "coh:alpha:{a}:{b}".
  std::string metric_template;

  // `qeeg_nf_cli --bands ...` spec. Empty means "use qeeg defaults".
  // (This can be used to introduce bands like SMR or hi-beta.)
  std::string band_spec;

  // Default values if placeholders are used and no override is provided.
  std::string default_channel;
  std::string default_channel_a;
  std::string default_channel_b;

  // Recommended NF loop defaults.
  RewardDirection reward_direction{RewardDirection::Above};
  double target_reward_rate{0.6};
  double baseline_seconds{10.0};
  double window_seconds{2.0};
  double update_seconds{0.25};
  double metric_smooth_seconds{0.0};
};

// Return the built-in protocol list.
std::vector<NfProtocolPreset> built_in_nf_protocols();

// Case-insensitive lookup by preset name.
std::optional<NfProtocolPreset> find_nf_protocol_preset(const std::string& name);

// Render the preset metric string by applying placeholder substitutions.
//
// If the template contains placeholders and the corresponding channel value is
// missing, this function throws std::runtime_error.
std::string nf_render_protocol_metric(const NfProtocolPreset& p,
                                      const std::string& channel_override = {},
                                      const std::string& channel_a_override = {},
                                      const std::string& channel_b_override = {});

// Render the preset band spec string (placeholder-capable, though most built-ins
// do not use placeholders here).
std::string nf_render_protocol_bands(const NfProtocolPreset& p,
                                     const std::string& channel_override = {},
                                     const std::string& channel_a_override = {},
                                     const std::string& channel_b_override = {});

} // namespace qeeg
