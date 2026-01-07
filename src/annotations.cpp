#include "qeeg/annotations.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

static bool is_tal_start_byte(uint8_t b) {
  return b == static_cast<uint8_t>('+') || b == static_cast<uint8_t>('-');
}

// Best-effort TAL delimiter logic.
// In practice, TAL entries begin with '+' (or '-') and are often preceded by
// either the start of the record, a 0x14 separator, or a 0x00 pad.
static bool is_probable_tal_start(const std::vector<uint8_t>& bytes, size_t idx) {
  if (idx >= bytes.size()) return false;
  if (!is_tal_start_byte(bytes[idx])) return false;
  if (idx == 0) return true;
  const uint8_t prev = bytes[idx - 1];
  return (prev == 0x14 || prev == 0x00);
}

static bool parse_double_best_effort(const std::string& s, double* out) {
  try {
    *out = std::stod(s);
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<AnnotationEvent> parse_edfplus_annotations_record(const std::vector<uint8_t>& record_bytes) {
  std::vector<AnnotationEvent> out;
  if (record_bytes.empty()) return out;

  // Trim to the last 0x14 delimiter (EDF+ TAL record padding is typically 0x00).
  size_t last_14 = std::string::npos;
  for (size_t i = record_bytes.size(); i-- > 0;) {
    if (record_bytes[i] == 0x14) {
      last_14 = i;
      break;
    }
  }
  if (last_14 == std::string::npos) return out;
  const size_t valid_len = last_14 + 1;

  // Find TAL starts.
  std::vector<size_t> starts;
  for (size_t i = 0; i < valid_len; ++i) {
    if (is_probable_tal_start(record_bytes, i)) {
      starts.push_back(i);
    }
  }
  if (starts.empty()) return out;

  // Parse each TAL chunk.
  for (size_t si = 0; si < starts.size(); ++si) {
    const size_t start = starts[si];
    const size_t end = (si + 1 < starts.size()) ? starts[si + 1] : valid_len;
    if (end <= start) continue;

    std::string s;
    s.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
      s.push_back(static_cast<char>(record_bytes[i]));
    }

    // Split TAL fields: onset[/duration] and one or more annotation texts.
    const std::vector<std::string> fields = split(s, '\x14');
    if (fields.empty()) continue;

    // onset and (optional) duration are separated by 0x15.
    const std::vector<std::string> td = split(fields[0], '\x15');
    std::string onset_s = td.empty() ? std::string() : trim(td[0]);
    std::string dur_s = (td.size() >= 2) ? trim(td[1]) : std::string();
    if (!onset_s.empty() && onset_s.front() == '+') onset_s.erase(onset_s.begin());

    double onset_sec = 0.0;
    if (!parse_double_best_effort(onset_s, &onset_sec)) continue;

    double duration_sec = 0.0;
    if (!dur_s.empty()) {
      (void)parse_double_best_effort(dur_s, &duration_sec);
    }

    // Remaining fields are annotation texts. Empty strings are allowed and mean
    // "no annotation" (e.g., the mandatory per-record timestamp marker).
    for (size_t fi = 1; fi < fields.size(); ++fi) {
      std::string text = trim(fields[fi]);
      if (text.empty()) continue;

      AnnotationEvent ev;
      ev.onset_sec = onset_sec;
      ev.duration_sec = duration_sec;
      ev.text = std::move(text);
      out.push_back(std::move(ev));
    }
  }

  // Sort by time for convenience.
  std::sort(out.begin(), out.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
    if (a.duration_sec != b.duration_sec) return a.duration_sec < b.duration_sec;
    return a.text < b.text;
  });

  return out;
}

} // namespace qeeg
