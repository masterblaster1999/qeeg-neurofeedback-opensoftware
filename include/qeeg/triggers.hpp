#pragma once

#include "qeeg/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qeeg {

// Best-effort extraction of discrete trigger/stim channels into AnnotationEvent entries.
//
// Motivation:
// - EDF+/BDF+ can store events in an "Annotations" signal (which EDFReader/BDFReader parse).
// - Many systems instead store triggers in a dedicated numeric channel (often named
//   TRIG / TRIGGER / STI / STATUS / MARKER / EVENT, etc.).
//
// This module provides a conservative heuristic to:
//   1) identify a likely trigger channel, and
//   2) convert transitions in that channel into AnnotationEvent entries.
//
// It is intended to improve interoperability for exports that do not include EDF+/BDF+
// annotations (e.g., some BDF recordings store triggers in a "Status" channel).

struct TriggerExtractionOptions {
  // If non-empty, force a specific channel name (matched via normalize_channel_name).
  // If empty, a trigger-like channel will be chosen automatically.
  std::string channel_name;

  // Optional bitmask applied to the rounded integer value before edge detection.
  // For example, BioSemi "Status" words often carry trigger codes in the lower 16 bits.
  // 0 means "no mask".
  std::uint32_t mask{0};

  // If true and mask==0, a channel whose name looks like "status" will default to mask=0xFFFF.
  bool auto_status_mask_16bit{true};

  // Treat values with absolute magnitude <= zero_epsilon as 0 (helps with tiny float noise).
  double zero_epsilon{1e-6};

  // If true, only transitions to non-zero codes produce events.
  bool ignore_zero{true};

  // Optional debounce: suppress repeated events with the same code occurring within this
  // time window. 0 disables.
  double min_event_interval_sec{0.0};
};

struct TriggerExtractionResult {
  std::string used_channel;            // empty if none found
  std::vector<AnnotationEvent> events; // may be empty
};

// Extract events from a single trigger channel.
// Throws if opt.channel_name is set but not found.
TriggerExtractionResult extract_events_from_trigger_channel(const EEGRecording& rec,
                                                           const TriggerExtractionOptions& opt);

// Auto-detect a trigger channel and extract its events.
// Returns empty result if no suitable channel is found.
TriggerExtractionResult extract_events_from_triggers_auto(const EEGRecording& rec,
                                                         const TriggerExtractionOptions& opt = {});

} // namespace qeeg
