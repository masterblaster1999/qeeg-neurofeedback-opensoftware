#pragma once

#include "qeeg/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qeeg {

// Minimal helpers for exporting recordings into a BIDS folder layout.
//
// This is intentionally dependency-light and only covers what qeeg_export_bids_cli
// needs today:
// - formatting the entity chain for filenames (sub/ses/task/acq/run)
// - writing dataset_description.json
// - writing *_eeg.json, *_channels.tsv, *_events.tsv, *_events.json

struct BidsEntities {
  // REQUIRED
  std::string sub;
  std::string task;

  // OPTIONAL
  std::string ses;
  std::string acq;
  std::string run; // index label ("1", "01", ...)
};

// BIDS labels are typically restricted to letters and digits.
// This helper is intentionally strict (no underscores or dashes).
bool is_valid_bids_label(const std::string& label);

// Format the shared entity chain for an EEG recording, WITHOUT the suffix.
// Example: "sub-01_ses-01_task-rest_acq-high_run-01"
std::string format_bids_entity_chain(const BidsEntities& ent);

// Convenience: add a suffix to the entity chain.
// Example suffixes: "eeg", "channels", "events".
std::string format_bids_filename_stem(const BidsEntities& ent, const std::string& suffix);

// ---- dataset_description.json ----

struct BidsDatasetDescription {
  std::string name{"qeeg-export"};
  std::string bids_version{"1.10.1"};
  std::string dataset_type{"raw"};
};

// Create dataset_description.json in dataset_root if it does not exist.
// If overwrite=true, replaces an existing file.
void write_bids_dataset_description(const std::string& dataset_root,
                                    const BidsDatasetDescription& desc,
                                    bool overwrite = false);

// ---- *_eeg.json ----

struct BidsEegJsonMetadata {
  // REQUIRED by BIDS EEG:
  std::string eeg_reference{"n/a"};
  std::optional<double> power_line_frequency_hz; // nullopt => "n/a"
  // BIDS expects an object OR "n/a".
  // This implementation only supports "n/a" (default) or a raw JSON object string.
  // If software_filters_raw_json is empty, "n/a" is written.
  std::string software_filters_raw_json{};

  // Optional helpers (not required but commonly useful):
  std::string task_name{};
  std::string eeg_ground{};
  std::string cap_manufacturer{};
  std::string cap_model{};
};

// Write an EEG JSON sidecar with REQUIRED keys and a few RECOMMENDED ones.
void write_bids_eeg_json(const std::string& path,
                         const EEGRecording& rec,
                         const BidsEegJsonMetadata& meta);

// ---- *_channels.tsv ----

// Heuristic: guess a BIDS channel type (EEG/EOG/ECG/EMG/TRIG/MISC/...).
// Returned value is always upper-case.
std::string guess_bids_channel_type(const std::string& channel_name);

// Write channels.tsv with required columns: name, type, units.
// Also writes status/status_description as optional columns.
void write_bids_channels_tsv(const std::string& path,
                             const EEGRecording& rec,
                             const std::vector<std::string>& channel_status = {},
                             const std::vector<std::string>& channel_status_desc = {});

// ---- *_events.tsv / *_events.json ----

// Write events.tsv with required columns: onset, duration.
// Adds trial_type derived from AnnotationEvent::text.
void write_bids_events_tsv(const std::string& path, const std::vector<AnnotationEvent>& events);

// Write a minimal events.json describing the trial_type column.
void write_bids_events_json(const std::string& path);

} // namespace qeeg
