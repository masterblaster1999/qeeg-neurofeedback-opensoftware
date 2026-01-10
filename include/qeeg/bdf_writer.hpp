#pragma once

#include "qeeg/types.hpp"

#include <string>

namespace qeeg {

// Minimal BDF (24-bit) writer with optional BDF+ annotations.
//
// Pragmatic goals:
// - Write a standards-friendly BDF header (EDF-like header with 24-bit samples).
// - Store samples as 24-bit little-endian signed integers (two's complement).
// - Optionally embed EEGRecording::events as a BDF+ "BDF Annotations" signal.
//
// Notes / limitations:
// - This is a minimal BDF/BDF+ implementation intended for interoperability with common tooling.
// - If record_duration_seconds > 0, data are written in fixed-duration datarecords and the last
//   record is padded (with zeros) if needed.
// - If record_duration_seconds <= 0, a single datarecord is written (no padding).
struct BDFWriterOptions {
  // Typical BDF uses 1 second datarecords, but any positive value is allowed as long as
  // fs_hz * record_duration_seconds is (close to) an integer.
  //
  // If <= 0, the writer uses a single datarecord covering the full recording duration.
  double record_duration_seconds{1.0};

  // Header identification fields (ASCII, space-padded).
  std::string patient_id{"X"};
  std::string recording_id{"qeeg-export"};

  // Start date/time fields. EDF/BDF expects "dd.mm.yy" and "hh.mm.ss".
  std::string start_date_dd_mm_yy{"01.01.85"};
  std::string start_time_hh_mm_ss{"00.00.00"};

  // Physical dimension string for EEG channels (8 chars). EEG is typically "uV".
  std::string physical_dimension{"uV"};

  // Per-channel physical min/max are derived from the data and padded by this fraction
  // (e.g. 0.05 = 5% margin).
  double physical_padding_fraction{0.05};

  // --- BDF+ annotations ---

  // If true and the input recording contains events, emit a BDF+ annotation channel
  // ("BDF Annotations") that encodes EEGRecording::events using TAL entries.
  //
  // If false, always emit a plain BDF even when rec.events is non-empty.
  bool write_bdfplus_annotations{true};

  // Override the number of annotation samples per datarecord for the BDF+ annotation signal.
  //
  // Each annotation "sample" stores one 8-bit TAL byte in the low 8 bits of a 24-bit word.
  //
  // 0 => auto (best-effort), with a conservative minimum.
  int annotation_samples_per_record{0};
};

class BDFWriter {
public:
  void write(const EEGRecording& rec, const std::string& path,
             const BDFWriterOptions& opts = BDFWriterOptions{});
};

} // namespace qeeg
