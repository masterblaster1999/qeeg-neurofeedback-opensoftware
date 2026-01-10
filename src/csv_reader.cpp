#include "qeeg/csv_reader.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qeeg {

namespace {

size_t count_delim_outside_quotes(const std::string& s, char delim) {
  bool in_quotes = false;
  size_t n = 0;

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      if (in_quotes && (i + 1) < s.size() && s[i + 1] == '"') {
        // Escaped quote
        ++i;
        continue;
      }
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && c == delim) ++n;
  }
  return n;
}

char detect_delim(const std::string& header_line) {
  // Heuristic delimiter detection. Comma is the default, but many datasets are
  // exported with ';' or tab delimiters depending on locale/software.
  // Count delimiters outside quoted fields to avoid being confused by channel
  // names that contain commas (e.g. "Ch,1,2").
  const size_t n_comma = count_delim_outside_quotes(header_line, ',');
  const size_t n_semi  = count_delim_outside_quotes(header_line, ';');
  const size_t n_tab   = count_delim_outside_quotes(header_line, '\t');

  char best = ',';
  size_t best_n = n_comma;
  if (n_semi > best_n) {
    best = ';';
    best_n = n_semi;
  }
  if (n_tab > best_n) {
    best = '\t';
    best_n = n_tab;
  }
  return best;
}

bool is_comment_or_empty(const std::string& t) {
  if (t.empty()) return true;
  if (starts_with(t, "#")) return true;
  if (starts_with(t, "//")) return true;
  return false;
}

static bool looks_like_header_row(const std::vector<std::string>& cols) {
  // Best-effort: require at least one alphabetic character across column labels
  // so we don't accidentally treat a first numeric sample row as a header.
  for (const auto& c : cols) {
    for (unsigned char ch : c) {
      if (std::isalpha(ch) != 0) return true;
    }
  }
  return false;
}

double median_inplace(std::vector<double>* v) {
  if (!v || v->empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v->begin(), v->end());
  const size_t n = v->size();
  const size_t mid = n / 2;
  if (n % 2 == 1) return (*v)[mid];
  return 0.5 * ((*v)[mid - 1] + (*v)[mid]);
}

double parse_double_strict(const std::string& s) {
  // std::stod is permissive: it stops at the first non-numeric character.
  // For EEG CSV numeric data, we want a strict parse so that values like
  // "1,23" don't silently become 1.
  const std::string t = trim(s);
  if (t.empty()) throw std::runtime_error("CSV: empty numeric cell");

  size_t idx = 0;
  double v = 0.0;
  try {
    v = std::stod(t, &idx);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("CSV: failed to parse double '") + t + "': " + e.what());
  }

  if (idx != t.size()) {
    // Reject trailing junk, but show a helpful error.
    std::ostringstream oss;
    oss << "CSV: failed to strictly parse double '" << t << "' (trailing '" << t.substr(idx)
        << "')";
    throw std::runtime_error(oss.str());
  }
  return v;
}

bool contains_char(const std::string& s, char c) {
  return s.find(c) != std::string::npos;
}

double parse_double_csv_locale(const std::string& s, char delim) {
  // Parse numeric values from CSV cells.
  //
  // Besides regular '.' decimal notation, support common "European" exports:
  //   - delimiter ';' (or tab)
  //   - decimal comma: 1,23
  //   - thousands dot + decimal comma: 1.234,56
  //
  // We only attempt comma-based parsing when the field delimiter is NOT a
  // comma, to avoid ambiguity.
  try {
    return parse_double_strict(s);
  } catch (const std::exception&) {
    // try fallbacks below
  }

  std::string t = trim(s);
  if (t.empty()) throw std::runtime_error("CSV: empty numeric cell");

  if (delim == ',') {
    // With comma-delimited files, a comma inside a numeric token is too
    // ambiguous to reinterpret.
    return parse_double_strict(t);
  }

  const bool has_comma = contains_char(t, ',');
  if (!has_comma) return parse_double_strict(t);

  const bool has_dot = contains_char(t, '.');
  const size_t last_comma = t.rfind(',');
  const size_t last_dot = has_dot ? t.rfind('.') : std::string::npos;

  std::string u = t;

  if (!has_dot) {
    // Simple decimal comma: 1,23 -> 1.23
    // Only do this when there's exactly one comma; otherwise we risk turning
    // something like 1,234,567 into nonsense.
    if (u.find(',') != u.rfind(',')) {
      return parse_double_strict(t);
    }
    std::replace(u.begin(), u.end(), ',', '.');
    return parse_double_strict(u);
  }

  // If both '.' and ',' are present and the last comma comes after the last
  // dot, this is likely German-style thousands dot + decimal comma:
  //   1.234,56 -> 1234.56
  if (last_dot != std::string::npos && last_comma != std::string::npos && last_comma > last_dot) {
    // Remove dots (thousands separators)
    u.erase(std::remove(u.begin(), u.end(), '.'), u.end());
    // Replace decimal comma
    std::replace(u.begin(), u.end(), ',', '.');
    return parse_double_strict(u);
  }

  // Otherwise, don't guess.
  return parse_double_strict(t);
}

std::string normalize_col_key(std::string s) {
  // Build a conservative alphanumeric-only lowercase key for header matching.
  // This is similar to normalize_channel_name(), but without EEG-specific
  // prefix/suffix stripping.
  s = strip_utf8_bom(std::move(s));
  s = trim(s);
  s = to_lower(std::move(s));

  std::string t;
  t.reserve(s.size());
  for (unsigned char ch : s) {
    if (std::isalnum(ch) != 0) t.push_back(static_cast<char>(std::tolower(ch)));
  }
  return t;
}

bool is_event_column_key(const std::string& key) {
  // Common marker/event column names in ASCII exports.
  if (key.empty()) return false;

  // Exact matches.
  static const std::vector<std::string> exact = {
      "event",
      "events",
      "marker",
      "markers",
      "trigger",
      "triggers",
      "trig",
      "stim",
      "stimulus",
      "stimulation",
      "annotation",
      "annotations",
      "mark",
      "mrk",
      "eventcode",
      "markercode",
      "triggercode",
  };
  for (const auto& e : exact) {
    if (key == e) return true;
  }

  // Prefix matches (conservative): markerX, eventX, triggerX, ...
  // We only accept suffixes that look like identifiers, e.g. digits or "code".
  const std::vector<std::string> prefixes = {"marker", "event", "trigger", "trig", "stim", "annotation"};
  for (const auto& p : prefixes) {
    if (starts_with(key, p) && key.size() > p.size()) {
      const std::string suf = key.substr(p.size());
      if (suf == "code") return true;
      bool all_digits = true;
      for (char c : suf) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) return true;
    }
  }

  return false;
}

bool is_nan_like(const std::string& t) {
  const std::string low = to_lower(trim(t));
  return low == "nan" || low == "na" || low == "none" || low == "null";
}

std::string marker_cell_to_label(const std::string& cell, char delim) {
  std::string t = trim(cell);
  if (t.empty()) return "";
  if (is_nan_like(t)) return "";

  // Try numeric parse. If numeric ~0 -> inactive.
  try {
    const double v = parse_double_csv_locale(t, delim);
    if (!std::isfinite(v) || std::fabs(v) <= 1e-12) return "";

    // Prefer integer-like labels if close.
    const double r = std::llround(v);
    if (std::fabs(v - r) <= 1e-6) {
      return std::to_string(static_cast<long long>(r));
    }
    return t;
  } catch (const std::exception&) {
    // Non-numeric marker label.
    return t;
  }
}

struct PendingEvent {
  size_t start_sample{0};
  size_t end_sample{0}; // exclusive
  std::string text;
};

} // namespace

EEGRecording CSVReader::read(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open CSV: " + path);

  // Read header row. Some exporters (e.g. BioTrace+) may emit a few metadata
  // lines before the actual delimited header row, so we scan until we find a
  // plausible header.
  std::string header;
  size_t lineno = 0;
  char delim = ',';
  std::vector<std::string> cols;

  while (std::getline(f, header)) {
    ++lineno;
    std::string t = trim(header);
    if (is_comment_or_empty(t)) continue;
    t = strip_utf8_bom(t);

    const char d = detect_delim(t);
    auto c = split_csv_row(t, d);
    for (auto& x : c) x = trim(x);

    if (c.size() < 2) continue;
    if (!looks_like_header_row(c)) continue;

    delim = d;
    cols = std::move(c);
    break;
  }

  if (cols.empty()) {
    throw std::runtime_error("CSV: missing header row");
  }

  bool has_time = false;
  bool time_in_ms = false;
  {
    std::string c0 = to_lower(cols[0]);
    // Common conventions for a leading time axis column.
    if (c0 == "t" || c0 == "time" || c0 == "timestamp" || starts_with(c0, "time")) {
      has_time = true;

      // Optional hint for units.
      if (c0.find("ms") != std::string::npos || c0.find("millis") != std::string::npos) {
        time_in_ms = true;
      }
    }
  }

  const size_t time_col = has_time ? 0 : static_cast<size_t>(-1);
  const size_t first_data_col = has_time ? 1 : 0;
  if (cols.size() - first_data_col < 1) throw std::runtime_error("CSV: no data columns");

  // Detect marker/event columns (best-effort). These columns are excluded from
  // EEG data channels and converted into rec.events.
  std::vector<bool> is_event_col(cols.size(), false);
  std::vector<size_t> event_cols;
  std::vector<std::string> event_col_names;
  for (size_t i = first_data_col; i < cols.size(); ++i) {
    const std::string key = normalize_col_key(cols[i]);
    if (is_event_column_key(key)) {
      is_event_col[i] = true;
      event_cols.push_back(i);
      event_col_names.push_back(trim(cols[i]));
    }
  }

  EEGRecording rec;

  // Map original column index -> output channel index.
  std::vector<int> out_idx(cols.size(), -1);
  for (size_t i = 0; i < cols.size(); ++i) {
    if (has_time && i == time_col) continue;
    if (is_event_col[i]) continue;

    std::string name = trim(cols[i]);
    if (name.empty()) {
      throw std::runtime_error("CSV: empty column name at index " + std::to_string(i));
    }

    out_idx[i] = static_cast<int>(rec.channel_names.size());
    rec.channel_names.push_back(std::move(name));
  }

  if (rec.channel_names.empty()) {
    throw std::runtime_error("CSV: no numeric data channels (only time/event columns?)");
  }

  rec.data.resize(rec.channel_names.size());

  // If fs is not provided and we have a time column, collect time samples to infer fs.
  std::vector<double> time_values;
  if (has_time && fs_hz_ <= 0.0) time_values.reserve(2048);

  // Track marker/event columns during row parsing.
  std::vector<std::string> cur_marker_label(event_cols.size());
  std::vector<size_t> cur_marker_start(event_cols.size(), 0);
  std::vector<PendingEvent> pending_events;
  pending_events.reserve(256);

  std::string line;
  size_t sample_idx = 0;
  while (std::getline(f, line)) {
    ++lineno;
    std::string raw = line;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::string t = trim(raw);
    if (is_comment_or_empty(t)) continue;

    auto vals = split_csv_row(raw, delim);
    if (vals.size() != cols.size()) {
      throw std::runtime_error("CSV: column count mismatch at line " + std::to_string(lineno) +
                               " (expected " + std::to_string(cols.size()) + ", got " +
                               std::to_string(vals.size()) + ")");
    }

    if (has_time && fs_hz_ <= 0.0) {
      time_values.push_back(parse_double_csv_locale(vals[0], delim));
    }

    // Numeric data columns.
    for (size_t i = first_data_col; i < vals.size(); ++i) {
      const int oi = out_idx[i];
      if (oi < 0) continue; // time or event column
      double v = parse_double_csv_locale(vals[i], delim);
      rec.data[static_cast<size_t>(oi)].push_back(static_cast<float>(v));
    }

    // Event columns: treat as a state signal and emit events on transitions.
    for (size_t k = 0; k < event_cols.size(); ++k) {
      const size_t ci = event_cols[k];
      std::string label = marker_cell_to_label(vals[ci], delim);
      if (!label.empty() && event_cols.size() > 1) {
        // Disambiguate if multiple marker columns exist.
        label = event_col_names[k] + ":" + label;
      }

      if (label != cur_marker_label[k]) {
        // Close previous.
        if (!cur_marker_label[k].empty()) {
          PendingEvent pe;
          pe.start_sample = cur_marker_start[k];
          pe.end_sample = sample_idx;
          pe.text = cur_marker_label[k];
          pending_events.push_back(std::move(pe));
        }
        // Start new.
        if (!label.empty()) {
          cur_marker_start[k] = sample_idx;
        }
        cur_marker_label[k] = std::move(label);
      }
    }

    ++sample_idx;
  }

  if (rec.n_samples() == 0) throw std::runtime_error("CSV: no samples found");

  // Ensure equal lengths
  const size_t n = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != n) throw std::runtime_error("CSV: channel length mismatch");
  }

  // Close any trailing open marker events.
  for (size_t k = 0; k < cur_marker_label.size(); ++k) {
    if (!cur_marker_label[k].empty()) {
      PendingEvent pe;
      pe.start_sample = cur_marker_start[k];
      pe.end_sample = n;
      pe.text = cur_marker_label[k];
      pending_events.push_back(std::move(pe));
    }
  }

  // Sampling rate: either provided explicitly or inferred from a time column.
  if (fs_hz_ > 0.0) {
    rec.fs_hz = fs_hz_;
  } else {
    if (!has_time) {
      throw std::runtime_error(
          "CSVReader: fs_hz must be provided (>0) unless the first column is a time axis (e.g., 'time,Ch1,Ch2')");
    }
    if (time_values.size() < 2) {
      throw std::runtime_error("CSV: need at least 2 time samples to infer fs");
    }

    std::vector<double> dts;
    dts.reserve(time_values.size() - 1);
    for (size_t i = 1; i < time_values.size(); ++i) {
      const double dt = time_values[i] - time_values[i - 1];
      if (dt > 0.0 && std::isfinite(dt)) dts.push_back(dt);
    }
    if (dts.empty()) {
      throw std::runtime_error("CSV: time column must be strictly increasing to infer fs");
    }

    double dt_med = median_inplace(&dts);
    if (!(dt_med > 0.0) || !std::isfinite(dt_med)) {
      throw std::runtime_error("CSV: failed to infer dt from time column");
    }

    // Units:
    // - If the header name hints at ms, use that.
    // - Otherwise, use a pragmatic EEG-specific heuristic: if dt is > 1.0, the
    //   file is likely using milliseconds (dt=4 for 250Hz, etc).
    double dt_sec = dt_med;
    if (time_in_ms || dt_med > 1.0) dt_sec = dt_med / 1000.0;

    if (!(dt_sec > 0.0) || !std::isfinite(dt_sec)) {
      throw std::runtime_error("CSV: invalid inferred dt_sec from time column");
    }

    rec.fs_hz = 1.0 / dt_sec;
    if (!(rec.fs_hz > 0.0) || !std::isfinite(rec.fs_hz)) {
      throw std::runtime_error("CSV: failed to infer fs from time column");
    }
  }

  // Convert pending marker events to AnnotationEvent (seconds).
  if (!pending_events.empty()) {
    rec.events.reserve(pending_events.size());
    const double inv_fs = 1.0 / rec.fs_hz;
    for (const auto& pe : pending_events) {
      if (pe.end_sample <= pe.start_sample) continue;
      AnnotationEvent ev;
      ev.onset_sec = static_cast<double>(pe.start_sample) * inv_fs;
      ev.duration_sec = static_cast<double>(pe.end_sample - pe.start_sample) * inv_fs;
      ev.text = pe.text;
      rec.events.push_back(std::move(ev));
    }

    std::sort(rec.events.begin(), rec.events.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
      if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
      if (a.duration_sec != b.duration_sec) return a.duration_sec < b.duration_sec;
      return a.text < b.text;
    });
  }

  return rec;
}

} // namespace qeeg
