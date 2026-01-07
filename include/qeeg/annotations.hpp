#pragma once

#include "qeeg/types.hpp"

#include <cstdint>
#include <vector>

namespace qeeg {

// Parse a single EDF+/BDF+ annotation channel datarecord (TAL data) into a list of
// human-friendly events.
//
// `record_bytes` should be the 8-bit bytes from one datarecord of an
// "EDF Annotations" / "BDF Annotations" signal. The function is tolerant to
// trailing 0x00 padding.
//
// Returned events:
// - onset_sec is the annotation onset in seconds relative to the file start.
// - duration_sec is 0 if absent.
// - text is the annotation label.
//
// Record-start markers (which typically have empty text) are ignored.
std::vector<AnnotationEvent> parse_edfplus_annotations_record(const std::vector<uint8_t>& record_bytes);

} // namespace qeeg
