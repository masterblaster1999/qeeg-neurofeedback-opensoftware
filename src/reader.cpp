#include "qeeg/reader.hpp"

#include "qeeg/csv_reader.hpp"
#include "qeeg/bdf_reader.hpp"
#include "qeeg/edf_reader.hpp"
#include "qeeg/brainvision_reader.hpp"
#include "qeeg/triggers.hpp"
#include "qeeg/utils.hpp"

#include <stdexcept>

namespace qeeg {

EEGRecording read_recording_auto(const std::string& path, double fs_hz_for_csv) {
  std::string low = to_lower(path);

  // BioTrace+/NeXus session containers/backups are proprietary. They can be exported
  // to open formats (EDF/ASCII) from within BioTrace+.
  if (ends_with(low, ".bcd") || ends_with(low, ".mbd")) {
    throw std::runtime_error(
        "Unsupported input file extension (.bcd/.mbd). These are BioTrace+/NeXus session files "
        "and are not supported here. Export to EDF or ASCII (CSV/TXT) from BioTrace+ and try again: " +
        path);
  }

  if (ends_with(low, ".edf") || ends_with(low, ".edf+") || ends_with(low, ".rec")) {
    EDFReader r;
    EEGRecording rec = r.read(path);
    // If the file contains no EDF+ annotations, try to recover event markers from a trigger-like channel.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }
  if (ends_with(low, ".bdf") || ends_with(low, ".bdf+")) {
    BDFReader r;
    EEGRecording rec = r.read(path);
    // Many BDF recordings store triggers in a "Status" channel rather than an Annotations signal.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }

  if (ends_with(low, ".vhdr")) {
    BrainVisionReader r;
    EEGRecording rec = r.read(path);
    // BrainVision recordings typically have markers; only fall back to trigger-channel extraction
    // if no marker file entries were parsed.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }

  // Treat common ASCII export extensions as CSV-like inputs.
  if (ends_with(low, ".csv") || ends_with(low, ".txt") || ends_with(low, ".tsv") || ends_with(low, ".asc") ||
      ends_with(low, ".ascii")) {
    CSVReader r(fs_hz_for_csv);
    EEGRecording rec = r.read(path);
    // CSVReader already detects marker/event columns, but a file may also contain a separate
    // trigger channel. Only attempt extraction if no events were found.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }

  throw std::runtime_error(
      "Unsupported input file extension (expected .edf/.bdf or .csv/.txt): " + path);
}

} // namespace qeeg
