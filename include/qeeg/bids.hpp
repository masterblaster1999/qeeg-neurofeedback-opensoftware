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

// Options for events.tsv export.
//
// BIDS requires at least `onset` and `duration` columns. Any additional columns
// are allowed, and SHOULD be described in an accompanying events.json.
//
// Note: Historically, BIDS treated `sample` and `value` as optional columns.
// In newer BIDS versions they are treated as arbitrary additional columns, but
// they are still commonly used by downstream tools.
struct BidsEventsTsvOptions {
  // Write `trial_type` derived from AnnotationEvent::text.
  bool include_trial_type{true};

  // If include_trial_type==true, optionally add a `Levels` map to events.json.
  //
  // This is most useful when the number of unique trial_type values is small
  // (e.g., NF-derived events like "NF:Reward" / "NF:Artifact"), enabling a
  // more self-describing exported dataset.
  bool include_trial_type_levels{false};

  // Maximum unique trial_type values to include in `Levels`.
  // If the unique count exceeds this threshold, the Levels section is omitted.
  size_t trial_type_levels_max{64};

  // Add a `sample` column derived from onset_sec * fs_hz.
  bool include_sample{false};

  // Add a `value` column derived from parsing AnnotationEvent::text as an integer.
  bool include_value{false};

  // Base for `sample` indices (0 or 1).
  // If include_sample==true and fs_hz>0, output sample = round(onset_sec * fs_hz) + sample_index_base.
  int sample_index_base{0};
};

// Write events.tsv with required columns: onset, duration.
// By default also writes `trial_type` derived from AnnotationEvent::text.
//
// If opts.include_sample==true, you should pass a valid sampling frequency (fs_hz).
void write_bids_events_tsv(const std::string& path,
                           const std::vector<AnnotationEvent>& events,
                           const BidsEventsTsvOptions& opts,
                           double fs_hz = 0.0);

// Backwards-compatible convenience wrapper.
inline void write_bids_events_tsv(const std::string& path, const std::vector<AnnotationEvent>& events) {
  BidsEventsTsvOptions opts;
  write_bids_events_tsv(path, events, opts, /*fs_hz=*/0.0);
}

// Write a minimal events.json describing the columns in events.tsv.
// By default describes `trial_type` only.
void write_bids_events_json(const std::string& path, const BidsEventsTsvOptions& opts);

// Write events.json and optionally include a `Levels` mapping for trial_type.
//
// If opts.include_trial_type_levels==true, this overload derives the set of
// unique trial_type values from `events`, up to opts.trial_type_levels_max.
void write_bids_events_json(const std::string& path,
                            const BidsEventsTsvOptions& opts,
                            const std::vector<AnnotationEvent>& events);

// Backwards-compatible convenience wrapper.
inline void write_bids_events_json(const std::string& path) {
  BidsEventsTsvOptions opts;
  write_bids_events_json(path, opts);
}

// ---- *_electrodes.tsv / *_coordsystem.json ----

// Minimal representation of a BIDS EEG electrodes.tsv row.
//
// Notes:
// - BIDS requires x/y/z columns in electrodes.tsv, but allows "n/a" for
//   unknown positions.
// - Units are specified in *_coordsystem.json.
struct BidsElectrode {
  std::string name;
  std::optional<double> x;
  std::optional<double> y;
  std::optional<double> z;

  // Optional / recommended columns.
  std::string type;     // e.g., "cup", "ring", "clip-on"
  std::string material; // e.g., "Ag/AgCl"
  std::optional<double> impedance_kohm;
};

// Load a simple electrode coordinate table (CSV or TSV).
//
// The file must contain a header row with at least: name, x, y, z.
// Optional columns: type, material, impedance.
//
// Values of "n/a" (case-insensitive) or empty fields are treated as missing.
std::vector<BidsElectrode> load_bids_electrodes_table(const std::string& path);

// Write electrodes.tsv.
//
// This writes required columns in the mandated order: name, x, y, z.
// It also writes type/material/impedance columns for convenience.
void write_bids_electrodes_tsv(const std::string& path,
                               const std::vector<BidsElectrode>& electrodes);

// Validate a coordinate unit token for BIDS.
// Accepted (case-sensitive): "m", "mm", "cm", "n/a".
bool is_valid_bids_coordinate_unit(const std::string& unit);

// Minimal EEG coordinate system metadata for *_coordsystem.json.
struct BidsCoordsystemJsonEegMetadata {
  // Required when providing EEG electrode positions.
  std::string eeg_coordinate_system; // e.g., "CapTrak", "EEGLAB", "Other"
  std::string eeg_coordinate_units;  // "m", "mm", "cm", or "n/a"

  // RECOMMENDED, but REQUIRED if eeg_coordinate_system == "Other".
  std::string eeg_coordinate_system_description;
};

// Write a minimal *_coordsystem.json containing EEGCoordinateSystem/Units.
//
// If meta.eeg_coordinate_system == "Other", the description must be provided.
void write_bids_coordsystem_json(const std::string& path,
                                 const BidsCoordsystemJsonEegMetadata& meta);

} // namespace qeeg
