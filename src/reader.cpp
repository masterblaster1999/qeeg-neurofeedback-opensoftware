#include "qeeg/reader.hpp"

#include "qeeg/csv_reader.hpp"
#include "qeeg/bdf_reader.hpp"
#include "qeeg/edf_reader.hpp"
#include "qeeg/brainvision_reader.hpp"
#include "qeeg/cli_input.hpp"
#include "qeeg/triggers.hpp"
#include "qeeg/utils.hpp"

#include <filesystem>
#include <stdexcept>

namespace qeeg {

EEGRecording read_recording_auto(const std::string& path, double fs_hz_for_csv) {
  // CLI chaining convenience:
  // Allow users (and CLI tools) to pass a directory or a *_run_meta.json as the
  // recording path. This enables "tool output directory" -> "next tool input"
  // workflows without each caller needing to resolve paths manually.
  //
  // We only invoke the resolver when needed:
  //  - directory paths
  //  - .json paths (treated as run meta)
  //  - selector syntax (<path>#<selector>)
  //
  // For regular files, keep existing extension-based dispatch (including the
  // helpful BioTrace+/NeXus .bcd/.mbd message).
  std::string resolved_path = path;

  // Selector syntax uses '#'. Use the base path for filesystem checks but
  // preserve the full spec when calling the resolver.
  const size_t hash = path.find('#');
  const std::string base = (hash == std::string::npos) ? path : path.substr(0, hash);

  bool want_resolve = (hash != std::string::npos);
  try {
    const std::filesystem::path p = std::filesystem::u8path(base);
    if (std::filesystem::exists(p) &&
        (std::filesystem::is_directory(p) ||
         to_lower(p.extension().u8string()) == ".json")) {
      want_resolve = true;
    }
  } catch (...) {
    // If we can't stat the path for any reason, fall back to the original
    // behavior unless the user explicitly asked for selector-based resolution.
  }

  if (want_resolve) {
    const ResolvedInputPath r = resolve_input_recording_path(path);
    resolved_path = r.path;
  }

  std::string low = to_lower(resolved_path);

  // BioTrace+/NeXus session containers/backups are proprietary. They can be exported
  // to open formats (EDF/ASCII) from within BioTrace+.
  if (ends_with(low, ".bcd") || ends_with(low, ".mbd")) {
    throw std::runtime_error(
        "Unsupported input file extension (.bcd/.mbd). These are BioTrace+/NeXus session containers and "
        "are not directly supported here.\n\n"
        "Recommended: export to EDF/BDF or ASCII (CSV/TXT) from BioTrace+ and use that file instead.\n"
        "Best-effort: some .bcd/.mbd files are ZIP containers that include an embedded EDF/BDF/ASCII export. "
        "If so, try: python3 scripts/biotrace_extract_container.py --input session.bcd --outdir extracted --print\n\n"
        "Input: " + resolved_path);
  }

  if (ends_with(low, ".edf") || ends_with(low, ".edf+") || ends_with(low, ".rec")) {
    EDFReader r;
    EEGRecording rec = r.read(resolved_path);
    // If the file contains no EDF+ annotations, try to recover event markers from a trigger-like channel.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }
  if (ends_with(low, ".bdf") || ends_with(low, ".bdf+")) {
    BDFReader r;
    EEGRecording rec = r.read(resolved_path);
    // Many BDF recordings store triggers in a "Status" channel rather than an Annotations signal.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }

  if (ends_with(low, ".vhdr")) {
    BrainVisionReader r;
    EEGRecording rec = r.read(resolved_path);
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
      ends_with(low, ".ascii") || ends_with(low, ".m2k")) {
    CSVReader r(fs_hz_for_csv);
    EEGRecording rec = r.read(resolved_path);
    // CSVReader already detects marker/event columns, but a file may also contain a separate
    // trigger channel. Only attempt extraction if no events were found.
    if (rec.events.empty()) {
      const TriggerExtractionResult tr = extract_events_from_triggers_auto(rec);
      if (!tr.events.empty()) rec.events = tr.events;
    }
    return rec;
  }

  throw std::runtime_error(
      "Unsupported input file extension (expected .edf/.bdf or .csv/.txt): " + resolved_path);
}

} // namespace qeeg
