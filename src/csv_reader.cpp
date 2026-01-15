#include "qeeg/csv_reader.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <locale>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
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
  // NOTE: Avoid std::stod here: it relies on the current C locale (LC_NUMERIC),
  // which can cause "0.004" to mis-parse in decimal-comma locales. Always parse
  // numeric cells using the classic "C" locale for consistent behavior.
  const std::string t = trim(s);
  if (t.empty()) throw std::runtime_error("CSV: empty numeric cell");

  // Be explicit about special tokens for portability across libstdc++/libc++.
  const std::string low = to_lower(t);
  if (low == "nan") return std::numeric_limits<double>::quiet_NaN();
  if (low == "inf" || low == "+inf" || low == "infinity" || low == "+infinity") {
    return std::numeric_limits<double>::infinity();
  }
  if (low == "-inf" || low == "-infinity") {
    return -std::numeric_limits<double>::infinity();
  }

  std::istringstream iss(t);
  iss.imbue(std::locale::classic());
  double v = 0.0;
  iss >> v;
  if (!iss) {
    throw std::runtime_error(std::string("CSV: failed to parse double '") + t + "'");
  }

  // Allow trailing whitespace, but reject any other trailing characters.
  iss >> std::ws;
  if (!iss.eof()) {
    std::string rest;
    std::getline(iss, rest);
    if (rest.size() > 64) rest = rest.substr(0, 64) + "...";
    std::ostringstream oss;
    oss << "CSV: failed to strictly parse double '" << t << "' (trailing '" << rest << "')";
    throw std::runtime_error(oss.str());
  }
  return v;
}

struct ParsedTimeCell {
  double value{0.0};
  bool is_hms{false};
};

// Forward declarations used by header parsing.
bool is_time_col_name(const std::string& s);
bool is_sample_col_name(const std::string& s);

double parse_double_csv_locale(const std::string& s, char delim);

double parse_double_relaxed_decimal(const std::string& s) {
  // Parse with '.' decimal, but also tolerate a single ',' as the decimal
  // separator (common in some exports) without applying any locale-specific
  // thousands rules.
  std::string t = trim(s);
  if (t.empty()) throw std::runtime_error("CSV: empty numeric cell");

  // Try strict parse first.
  try {
    return parse_double_strict(t);
  } catch (const std::exception&) {
    // fall through
  }

  // If there's exactly one comma and no dot, interpret comma as decimal.
  if (t.find('.') == std::string::npos) {
    const size_t c1 = t.find(',');
    if (c1 != std::string::npos && c1 == t.rfind(',')) {
      std::replace(t.begin(), t.end(), ',', '.');
      return parse_double_strict(t);
    }
  }
  return parse_double_strict(t);
}

ParsedTimeCell parse_time_cell(const std::string& s, char delim) {
  // BioTrace+ (and other tools) can export time axes in either numeric form
  // (seconds, milliseconds, sample index) or as hh:mm:ss(.sss) strings.
  //
  // We parse hh:mm:ss variants into seconds (double).
  const std::string t0 = trim(s);
  if (t0.empty()) throw std::runtime_error("CSV: empty time cell");

  if (t0.find(':') == std::string::npos) {
    return ParsedTimeCell{parse_double_csv_locale(t0, delim), false};
  }

  // Split by ':' and interpret from the right.
  std::vector<std::string> parts;
  {
    std::string cur;
    for (char c : t0) {
      if (c == ':') {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    parts.push_back(cur);
  }
  if (parts.size() < 2 || parts.size() > 3) {
    throw std::runtime_error("CSV: unsupported time format: '" + t0 + "'");
  }

  auto parse_int = [&](const std::string& x) -> int {
    const std::string u = trim(x);
    if (u.empty()) throw std::runtime_error("CSV: empty time component");
    for (unsigned char ch : u) {
      if (std::isdigit(ch) == 0) {
        throw std::runtime_error("CSV: invalid time component: '" + u + "'");
      }
    }
    return std::stoi(u);
  };

  int hours = 0;
  int minutes = 0;
  double seconds = 0.0;

  if (parts.size() == 3) {
    hours = parse_int(parts[0]);
    minutes = parse_int(parts[1]);
    seconds = parse_double_relaxed_decimal(parts[2]);
  } else {
    minutes = parse_int(parts[0]);
    seconds = parse_double_relaxed_decimal(parts[1]);
  }

  if (hours < 0 || minutes < 0) {
    throw std::runtime_error("CSV: negative time component: '" + t0 + "'");
  }

  const double total_sec = static_cast<double>(hours) * 3600.0 +
                           static_cast<double>(minutes) * 60.0 +
                           seconds;
  if (!std::isfinite(total_sec)) {
    throw std::runtime_error("CSV: invalid parsed time: '" + t0 + "'");
  }
  return ParsedTimeCell{total_sec, true};
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


struct ParsedChannelHeader {
  std::string clean_name;
  double scale_to_uv{1.0};
};

static void replace_all_inplace(std::string* s, const std::string& from, const std::string& to) {
  if (!s || from.empty()) return;
  size_t pos = 0;
  while ((pos = s->find(from, pos)) != std::string::npos) {
    s->replace(pos, from.size(), to);
    pos += to.size();
  }
}

static std::string normalize_unit_token(std::string u) {
  u = trim(std::move(u));
  if (u.empty()) return "";

  // Normalize UTF-8 micro signs to 'u' (both U+00B5 and U+03BC).
  replace_all_inplace(&u, u8"µ", "u");
  replace_all_inplace(&u, u8"μ", "u");

  u = to_lower(std::move(u));

  // Keep only alphanumeric to form a stable key.
  std::string t;
  t.reserve(u.size());
  for (unsigned char ch : u) {
    if (std::isalnum(ch) != 0) t.push_back(static_cast<char>(std::tolower(ch)));
  }
  if (t.empty()) return "";

  // Common physical units for EEG/physio channels.
  if (t == "uv" || t == "microv" || t == "microvolt" || t == "microvolts") return "uv";
  if (t == "mv" || t == "millivolt" || t == "millivolts") return "mv";
  if (t == "nv" || t == "nanovolt" || t == "nanovolts") return "nv";
  if (t == "v" || t == "volt" || t == "volts") return "v";
  return "";
}

static std::string strip_trailing_separators(std::string s) {
  s = trim(std::move(s));
  while (!s.empty()) {
    const char c = s.back();
    if (c == '-' || c == '_' || c == '/' || c == ':' ) {
      s.pop_back();
      continue;
    }
    break;
  }
  return trim(std::move(s));
}

static bool extract_bracket_suffix(const std::string& s, char open, char close,
                                   std::string* base, std::string* inside) {
  if (!base || !inside) return false;
  const std::string t = trim(s);
  if (t.size() < 3) return false;
  if (t.back() != close) return false;

  const size_t pos_open = t.rfind(open);
  if (pos_open == std::string::npos) return false;
  if (pos_open + 1 >= t.size()) return false;

  const std::string in = trim(t.substr(pos_open + 1, t.size() - pos_open - 2));
  if (in.empty()) return false;

  const std::string b = trim(t.substr(0, pos_open));
  if (b.empty()) return false;

  *base = b;
  *inside = in;
  return true;
}

static ParsedChannelHeader parse_channel_header(const std::string& raw) {
  // Many ASCII exports include a unit suffix, e.g.:
  //   "EEG1 (uV)", "Cz [mV]", "EEG2_uV"
  //
  // For downstream compatibility (channel matching, BrainVision/BIDS export),
  // we strip a recognized unit suffix from the channel name AND scale numeric
  // values into microvolts where possible.
  ParsedChannelHeader out;
  const std::string t = trim(strip_utf8_bom(raw));
  if (t.empty()) return out;

  std::string base_candidate;
  std::string unit_candidate;
  bool unit_from_brackets = false;

  // 1) Bracket suffixes at end: "(uV)" or "[uV]"
  if (extract_bracket_suffix(t, '(', ')', &base_candidate, &unit_candidate) ||
      extract_bracket_suffix(t, '[', ']', &base_candidate, &unit_candidate)) {
    unit_from_brackets = true;
  } else {
    // 2) Token suffix separated by whitespace/underscore/dash, e.g. "EEG2_uV" or "EEG2 uV"
    const size_t pos = t.find_last_of(" \t_-");
    if (pos != std::string::npos && pos + 1 < t.size()) {
      const std::string u = trim(t.substr(pos + 1));
      const std::string b = trim(t.substr(0, pos));
      if (!u.empty() && !b.empty()) {
        base_candidate = b;
        unit_candidate = u;
      }
    }
  }

  std::string ukey = normalize_unit_token(unit_candidate);
  if (!unit_from_brackets && ukey == "v") {
    // Avoid mis-parsing labels like "EOG-V" where "-V" means vertical, not volts.
    ukey.clear();
  }
  if (!ukey.empty()) {
    if (ukey == "uv") out.scale_to_uv = 1.0;
    else if (ukey == "mv") out.scale_to_uv = 1000.0;
    else if (ukey == "v") out.scale_to_uv = 1000000.0;
    else if (ukey == "nv") out.scale_to_uv = 0.001;

    // Clean up the channel name by removing the recognized unit suffix.
    out.clean_name = strip_trailing_separators(base_candidate);
    if (out.clean_name.empty()) out.clean_name = t;
  } else {
    out.clean_name = t;
    out.scale_to_uv = 1.0;
  }

  return out;
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

bool is_time_col_name(const std::string& s) {
  const std::string key = normalize_col_key(s);
  if (key.empty()) return false;
  if (key == "t" || key == "time" || key == "timestamp") return true;
  // Common variants: time_ms, time_s, time(sec), ...
  if (starts_with(key, "time")) return true;
  return false;
}

bool is_sample_col_name(const std::string& s) {
  const std::string key = normalize_col_key(s);
  if (key.empty()) return false;
  static const std::vector<std::string> exact = {
      "sample",
      "samples",
      "sampleno",
      "samplenr",
      "samplenumber",
      "sampleindex",
      "index",
      "idx",
      "frame",
      "row",
  };
  for (const auto& e : exact) {
    if (key == e) return true;
  }
  if (starts_with(key, "sample") && key.size() > 6) return true;
  return false;
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
      // BioTrace+ can export session segments as a dedicated column.
      // See BioTrace+ User Manual: Exporting session data -> "Include segments".
      "segment",
      "segments",
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
  const std::vector<std::string> prefixes = {"marker", "event", "trigger", "trig", "stim", "annotation", "segment"};
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

bool is_segment_column_key(const std::string& key) {
  // BioTrace+ distinguishes event markers (point events) from segments (time ranges).
  // When exporting to ASCII, the manual lists both "Include event markers" and
  // "Include segments" as separate options.
  if (key.empty()) return false;

  // Exact matches.
  if (key == "segment" || key == "segments") return true;

  // Conservative prefix match: segment1, segment2, ...
  if (starts_with(key, "segment") && key.size() > 7) {
    const std::string suf = key.substr(7);
    bool all_digits = true;
    for (char c : suf) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) return true;
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
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Failed to open CSV: " + path);

  // Header detection:
  // BioTrace+ (and other exporters) may emit one or more metadata lines before
  // the actual delimited header row. Some of those metadata lines may even be
  // delimited (e.g., "Client;TEST"), so a simple "first delimited line" rule is
  // too weak.
  //
  // Strategy:
  //  1) Read a bounded prefix of lines.
  //  2) Score candidate header rows and validate by looking at the following
  //     (non-comment) data row.
  const size_t kMaxScanLines = 20000;
  std::vector<std::string> prefix;
  prefix.reserve(256);
  {
    std::string line;
    while (prefix.size() < kMaxScanLines && std::getline(f, line)) {
      prefix.push_back(line);
    }
  }

  char delim = ',';
  std::vector<std::string> cols;
  size_t header_line_idx = static_cast<size_t>(-1);
  size_t time_col = static_cast<size_t>(-1);
  size_t sample_col = static_cast<size_t>(-1);
  size_t first_data_col = 0;
  bool has_time = false;
  bool time_in_ms = false;

  auto normalize_vals_to_header = [&](std::vector<std::string>* vals) {
    if (!vals) return;
    if (vals->size() < cols.size()) {
      vals->resize(cols.size());
    } else if (vals->size() > cols.size()) {
      while (vals->size() > cols.size() && trim(vals->back()).empty()) {
        vals->pop_back();
      }
    }
  };

  auto row_has_parseable_numeric = [&](const std::vector<std::string>& vals, size_t start_col, char d) -> bool {
    for (size_t i = start_col; i < vals.size(); ++i) {
      const std::string key = normalize_col_key(cols[i]);
      if (is_event_column_key(key)) continue;
      const std::string cell = trim(vals[i]);
      if (cell.empty() || is_nan_like(cell)) continue;
      try {
        (void)parse_double_csv_locale(cell, d);
        return true;
      } catch (const std::exception&) {
        // keep scanning other columns
      }
    }
    return false;
  };

  for (size_t i = 0; i < prefix.size(); ++i) {
    std::string t = trim(prefix[i]);
    if (is_comment_or_empty(t)) continue;
    t = strip_utf8_bom(std::move(t));

    const char d = detect_delim(t);
    auto cand_cols = split_csv_row(t, d);
    for (auto& x : cand_cols) x = trim(x);

    if (cand_cols.size() < 2) continue;
    if (!looks_like_header_row(cand_cols)) continue;

    // Detect axis columns.
    size_t cand_time_col = static_cast<size_t>(-1);
    size_t cand_sample_col = static_cast<size_t>(-1);
    size_t cand_first_data_col = 0;
    if (is_time_col_name(cand_cols[0])) {
      cand_time_col = 0;
      cand_first_data_col = 1;
    } else if (is_sample_col_name(cand_cols[0])) {
      cand_sample_col = 0;
      cand_first_data_col = 1;
      if (cand_cols.size() >= 2 && is_time_col_name(cand_cols[1])) {
        cand_time_col = 1;
        cand_first_data_col = 2;
      }
    }

    if (cand_cols.size() <= cand_first_data_col) continue;

    // Require at least one non-event data column.
    size_t n_data_cols = 0;
    for (size_t k = cand_first_data_col; k < cand_cols.size(); ++k) {
      if (!is_event_column_key(normalize_col_key(cand_cols[k]))) ++n_data_cols;
    }
    if (n_data_cols < 1) continue;

    // Validate against the next non-comment row in the prefix.
    size_t j = i + 1;
    for (; j < prefix.size(); ++j) {
      std::string u = trim(prefix[j]);
      if (is_comment_or_empty(u)) continue;

      auto vals = split_csv_row(prefix[j], d);
      // Temporarily set cols so normalize_vals_to_header uses the candidate.
      cols = cand_cols;
      normalize_vals_to_header(&vals);
      if (vals.size() != cols.size()) continue;

      // Time column should parse if we need it to infer fs.
      if (cand_time_col != static_cast<size_t>(-1) && fs_hz_ <= 0.0) {
        try {
          (void)parse_time_cell(vals[cand_time_col], d);
        } catch (const std::exception&) {
          continue;
        }
      }

      // At least one numeric data cell should parse.
      if (!row_has_parseable_numeric(vals, cand_first_data_col, d)) {
        continue;
      }

      // Candidate accepted.
      header_line_idx = i;
      delim = d;
      cols = std::move(cand_cols);
      time_col = cand_time_col;
      sample_col = cand_sample_col;
      first_data_col = cand_first_data_col;
      has_time = (time_col != static_cast<size_t>(-1));

      if (has_time) {
        const std::string tc = to_lower(cols[time_col]);
        if (tc.find("ms") != std::string::npos || tc.find("millis") != std::string::npos) {
          time_in_ms = true;
        }
      }
      break;
    }

    if (header_line_idx != static_cast<size_t>(-1)) break;
  }

  if (header_line_idx == static_cast<size_t>(-1) || cols.empty()) {
    throw std::runtime_error("CSV: missing header row");
  }

  if (cols.size() - first_data_col < 1) throw std::runtime_error("CSV: no data columns");

  // Detect marker/event columns (best-effort). These columns are excluded from
  // EEG data channels and converted into rec.events.
  std::vector<bool> is_event_col(cols.size(), false);
  std::vector<bool> is_segment_col(cols.size(), false);
  std::vector<size_t> event_cols;
  std::vector<std::string> event_col_names;
  std::vector<bool> event_is_segment;
  for (size_t i = first_data_col; i < cols.size(); ++i) {
    const std::string key = normalize_col_key(cols[i]);
    if (is_event_column_key(key)) {
      is_event_col[i] = true;
      const bool is_seg = is_segment_column_key(key);
      is_segment_col[i] = is_seg;
      event_cols.push_back(i);
      event_col_names.push_back(trim(cols[i]));
      event_is_segment.push_back(is_seg);
    }
  }

  EEGRecording rec;

  // Map original column index -> output channel index.
  // Also track per-column scaling to convert voltage-like units to microvolts.
  std::vector<int> out_idx(cols.size(), -1);
  std::vector<double> col_scale(cols.size(), 1.0);

  std::unordered_set<std::string> used_names;
  used_names.reserve(cols.size());

  auto make_unique_name = [&](std::string name) -> std::string {
    if (used_names.insert(name).second) return name;
    const std::string base = name;
    int k = 2;
    while (true) {
      std::string cand = base + "_" + std::to_string(k++);
      if (used_names.insert(cand).second) return cand;
    }
  };

  for (size_t i = 0; i < cols.size(); ++i) {
    if (has_time && i == time_col) continue;
    if (sample_col != static_cast<size_t>(-1) && i == sample_col) continue;
    if (is_event_col[i]) continue;

    const ParsedChannelHeader ph = parse_channel_header(cols[i]);
    std::string name = ph.clean_name;
    if (name.empty()) name = trim(cols[i]);
    if (name.empty()) {
      throw std::runtime_error("CSV: empty column name at index " + std::to_string(i));
    }

    name = make_unique_name(std::move(name));
    col_scale[i] = ph.scale_to_uv;

    out_idx[i] = static_cast<int>(rec.channel_names.size());
    rec.channel_names.push_back(std::move(name));
  }

  if (rec.channel_names.empty()) {
    throw std::runtime_error("CSV: no numeric data channels (only time/event columns?)");
  }

  rec.data.resize(rec.channel_names.size());

  // If fs is not provided and we have a time column, collect time samples to infer fs.
  std::vector<double> time_values;
  bool time_axis_is_hms = false;
  if (has_time && fs_hz_ <= 0.0) time_values.reserve(2048);

  // Track marker/event columns during row parsing.
  // BioTrace+ exports distinguish event markers (point events) and segments (time ranges).
  // We keep segment columns as "state" streams (emit duration events on transitions), while
  // marker columns are treated as point events (duration_sec=0) emitted on transitions into a
  // non-empty label.
  std::vector<std::string> cur_marker_label(event_cols.size());
  std::vector<size_t> cur_marker_start(event_cols.size(), 0);
  std::vector<PendingEvent> pending_events;
  pending_events.reserve(256);

  size_t n_marker_cols = 0;
  for (bool is_seg : event_is_segment) {
    if (!is_seg) ++n_marker_cols;
  }
  const bool disambiguate_marker_cols = (n_marker_cols > 1);

  size_t sample_idx = 0;

  auto parse_and_append_row = [&](const std::string& raw_in, size_t lineno) {
    std::string raw = raw_in;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::string t = trim(raw);
    if (is_comment_or_empty(t)) return;

    auto vals = split_csv_row(raw, delim);

    // Some exporters omit trailing empty cells (especially for marker/event columns),
    // or append one or more trailing delimiters. Be tolerant as long as any extra
    // fields are empty and any missing fields are implicitly empty.
    if (vals.size() < cols.size()) {
      vals.resize(cols.size());
    } else if (vals.size() > cols.size()) {
      while (vals.size() > cols.size() && trim(vals.back()).empty()) {
        vals.pop_back();
      }
    }

    if (vals.size() != cols.size()) {
      throw std::runtime_error("CSV: column count mismatch at line " + std::to_string(lineno) +
                               " (expected " + std::to_string(cols.size()) + ", got " +
                               std::to_string(vals.size()) + ")");
    }

    // Time axis (for fs inference): use the detected time column.
    if (has_time && fs_hz_ <= 0.0) {
      const ParsedTimeCell pt = parse_time_cell(vals[time_col], delim);
      time_values.push_back(pt.value);
      time_axis_is_hms = time_axis_is_hms || pt.is_hms;
    }

    // Numeric data columns.
    for (size_t i = first_data_col; i < vals.size(); ++i) {
      const int oi = out_idx[i];
      if (oi < 0) continue; // time, sample index, or event column
      const std::string cell = trim(vals[i]);
      float outv = 0.0f;
      if (cell.empty() || is_nan_like(cell)) {
        // Some exporters (e.g., BioTrace+ when "repeat slower channels" is
        // disabled) may leave cells empty for channels with lower sampling
        // rates. A practical default is forward-fill.
        auto& ch = rec.data[static_cast<size_t>(oi)];
        outv = ch.empty() ? 0.0f : ch.back();
      } else {
        const double v = parse_double_csv_locale(cell, delim) * col_scale[i];
        outv = static_cast<float>(v);
      }
      rec.data[static_cast<size_t>(oi)].push_back(outv);
    }

    // Event columns:
    //  - Segment columns: treat as a state signal and emit duration events on transitions.
    //  - Marker columns: emit a point event on transitions into a non-empty label.
    for (size_t k = 0; k < event_cols.size(); ++k) {
      const size_t ci = event_cols[k];
      std::string label = marker_cell_to_label(vals[ci], delim);

      const bool is_seg = event_is_segment[k];
      if (!label.empty() && !is_seg && disambiguate_marker_cols) {
        // Disambiguate if multiple marker columns exist.
        label = event_col_names[k] + ":" + label;
      }

      if (label != cur_marker_label[k]) {
        if (is_seg) {
          // Close previous segment.
          if (!cur_marker_label[k].empty()) {
            PendingEvent pe;
            pe.start_sample = cur_marker_start[k];
            pe.end_sample = sample_idx;
            pe.text = cur_marker_label[k];
            pending_events.push_back(std::move(pe));
          }

          // Start new segment.
          if (!label.empty()) {
            cur_marker_start[k] = sample_idx;
          }
        } else {
          // Marker/event: emit a point event at the transition into a non-empty label.
          if (!label.empty()) {
            PendingEvent pe;
            pe.start_sample = sample_idx;
            pe.end_sample = sample_idx; // point
            pe.text = label;
            pending_events.push_back(std::move(pe));
          }
        }

        cur_marker_label[k] = std::move(label);
      }
    }

    ++sample_idx;
  };

  // Parse data lines already present in the prefix buffer.
  for (size_t i = header_line_idx + 1; i < prefix.size(); ++i) {
    parse_and_append_row(prefix[i], i + 1);
  }

  // Continue streaming the rest of the file (if any).
  size_t lineno = prefix.size();
  std::string line;
  while (std::getline(f, line)) {
    ++lineno;
    parse_and_append_row(line, lineno);
  }

  if (rec.n_samples() == 0) throw std::runtime_error("CSV: no samples found");

  // Ensure equal lengths
  const size_t n = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != n) throw std::runtime_error("CSV: channel length mismatch");
  }

  // Close any trailing open *segment* events.
  for (size_t k = 0; k < cur_marker_label.size(); ++k) {
    if (!event_is_segment[k]) continue;
    if (cur_marker_label[k].empty()) continue;
    PendingEvent pe;
    pe.start_sample = cur_marker_start[k];
    pe.end_sample = n;
    pe.text = cur_marker_label[k];
    pending_events.push_back(std::move(pe));
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
    if (!time_axis_is_hms) {
      if (time_in_ms || dt_med > 1.0) dt_sec = dt_med / 1000.0;
    }

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
      if (pe.end_sample < pe.start_sample) continue;
      AnnotationEvent ev;
      ev.onset_sec = static_cast<double>(pe.start_sample) * inv_fs;
      if (pe.end_sample > pe.start_sample) {
        ev.duration_sec = static_cast<double>(pe.end_sample - pe.start_sample) * inv_fs;
      } else {
        ev.duration_sec = 0.0; // point event
      }
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
