#include "qeeg/csv_io.hpp"
#include "qeeg/event_ops.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace qeeg {

namespace {

static std::string trim_ws(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  size_t j = s.size();
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  return s.substr(i, j - i);
}

static std::string to_lower_ascii(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

static bool ends_with_ci(const std::string& s, const std::string& suf) {
  if (s.size() < suf.size()) return false;
  const size_t off = s.size() - suf.size();
  for (size_t i = 0; i < suf.size(); ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[off + i])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suf[i])));
    if (a != b) return false;
  }
  return true;
}

static std::vector<std::string> split_delimited_row(const std::string& line_in, char delim) {
  std::string line = line_in;
  if (!line.empty() && line.back() == '\r') line.pop_back();

  std::vector<std::string> out;
  std::string cell;
  cell.reserve(line.size());
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        // Doubled quote => literal quote.
        if (i + 1 < line.size() && line[i + 1] == '"') {
          cell.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        cell.push_back(c);
      }
    } else {
      if (c == '"') {
        in_quotes = true;
      } else if (c == delim) {
        out.push_back(cell);
        cell.clear();
      } else {
        cell.push_back(c);
      }
    }
  }
  out.push_back(cell);
  return out;
}

static size_t count_delim_outside_quotes(const std::string& line_in, char delim) {
  // Count delimiter occurrences that appear *outside* quoted fields.
  // This helps avoid being fooled by delimiter characters inside quotes.
  std::string line = line_in;
  if (!line.empty() && line.back() == '\r') line.pop_back();

  bool in_quotes = false;
  size_t n = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      // Escaped quote: "" inside quotes.
      if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
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

static char detect_delim(const std::string& header, const std::string& path) {
  // Respect extension hint first.
  if (ends_with_ci(path, ".tsv")) return '\t';

  // Auto-detect common delimiters: tab, comma, semicolon.
  const size_t n_tab = count_delim_outside_quotes(header, '\t');
  const size_t n_comma = count_delim_outside_quotes(header, ',');
  const size_t n_semi = count_delim_outside_quotes(header, ';');

  // Pick the delimiter with the highest count.
  // Tie-breaker is conservative: prefer comma, then semicolon, then tab.
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

  if (best_n == 0) {
    // Fall back to comma (most common) if we can't detect.
    return ',';
  }
  return best;
}

static int find_col(const std::vector<std::string>& header, const std::vector<std::string>& names) {
  for (size_t i = 0; i < header.size(); ++i) {
    const std::string h = to_lower_ascii(trim_ws(header[i]));
    for (const auto& want : names) {
      if (h == want) return static_cast<int>(i);
    }
  }
  return -1;
}

static bool parse_double_classic_strict(const std::string& s, double* out) {
  if (!out) return false;

  std::istringstream iss(s);
  iss.imbue(std::locale::classic());

  double v = 0.0;
  iss >> v;
  if (!iss) return false;

  // Allow trailing whitespace, but reject any other trailing characters.
  iss >> std::ws;
  if (!iss.eof()) return false;

  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

static bool parse_double_maybe(const std::string& s, char delim, double* out) {
  if (!out) return false;
  const std::string raw = trim_ws(s);
  const std::string low = to_lower_ascii(raw);
  if (low.empty() || low == "n/a" || low == "na") return false;

  // Primary parse: classic C locale (decimal point '.') regardless of process locale.
  if (parse_double_classic_strict(raw, out)) return true;

  // Locale-specific exports:
  // When the delimiter is not ',', numeric cells often use decimal commas
  // ("0,5") and sometimes thousands dots ("1.234,56").
  //
  // We only attempt comma-based parsing when the delimiter is NOT a comma, to
  // avoid ambiguity in comma-delimited CSVs.
  if (delim != ',') {
    // 1) Single decimal comma with no dot: "0,5" => "0.5"
    if (raw.find('.') == std::string::npos) {
      const size_t cpos = raw.find(',');
      if (cpos != std::string::npos && raw.find(',', cpos + 1) == std::string::npos) {
        std::string t = raw;
        t[cpos] = '.';
        if (parse_double_classic_strict(t, out)) return true;
      }
    }

    // 2) Thousands dots + decimal comma: "1.234,56" => "1234.56"
    const size_t comma = raw.find(',');
    if (comma != std::string::npos &&
        raw.find(',', comma + 1) == std::string::npos &&
        raw.find('.') != std::string::npos &&
        raw.rfind(',') > raw.find('.')) {
      std::string t;
      t.reserve(raw.size());
      for (char c : raw) {
        if (c == '.') continue; // drop thousands separators
        t.push_back(c);
      }
      std::replace(t.begin(), t.end(), ',', '.');
      if (parse_double_classic_strict(t, out)) return true;
    }
  }

  return false;
}

} // namespace

std::string csv_escape(const std::string& s) {
  bool need = false;
  for (char c : s) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      need = true;
      break;
    }
  }
  if (!need) return s;

  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

void write_events_csv(const std::string& path, const std::vector<AnnotationEvent>& events) {
  std::ofstream o(path);
  o.imbue(std::locale::classic());
  if (!o) throw std::runtime_error("Failed to write events CSV: " + path);

  o << "onset_sec,duration_sec,text\n";
  o << std::fixed << std::setprecision(6);
  for (const auto& ev : events) {
    o << ev.onset_sec << "," << ev.duration_sec << "," << csv_escape(ev.text) << "\n";
  }
}

namespace {

static std::string tsv_sanitize_cell(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\t' || c == '\n' || c == '\r') out.push_back(' ');
    else out.push_back(c);
  }
  return out;
}

} // namespace

void write_events_tsv(const std::string& path, const std::vector<AnnotationEvent>& events) {
  std::ofstream o(path);
  o.imbue(std::locale::classic());
  if (!o) throw std::runtime_error("Failed to write events TSV: " + path);

  o << "onset\tduration\ttrial_type\n";
  o << std::fixed << std::setprecision(6);
  for (const auto& ev : events) {
    o << ev.onset_sec << '\t' << ev.duration_sec << '\t' << tsv_sanitize_cell(ev.text) << "\n";
  }
}

void write_events_tsv(const std::string& path, const EEGRecording& rec) {
  write_events_tsv(path, rec.events);
}


std::vector<AnnotationEvent> read_events_table(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Failed to read events table: " + path);

  std::string header_line;
  // Skip leading blank/comment lines.
  while (std::getline(f, header_line)) {
    if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
    std::string t = trim_ws(header_line);
    if (t.empty()) continue;
    if (!t.empty() && t[0] == '#') continue;
    // Some Windows CSV exports prefix the first header cell with a UTF-8 BOM.
    t = strip_utf8_bom(std::move(t));
    header_line = t;
    break;
  }
  if (header_line.empty()) return {};

  const char delim = detect_delim(header_line, path);
  const auto header = split_delimited_row(header_line, delim);

  // Column discovery.
  const int col_onset = find_col(header, {"onset", "onset_sec"});
  const int col_dur = find_col(header, {"duration", "duration_sec"});
  const int col_txt = find_col(header, {"trial_type", "text", "label", "event"});
  const int col_val = find_col(header, {"value"});

  if (col_onset < 0) {
    throw std::runtime_error("Events table missing required onset column (onset/onset_sec): " + path);
  }
  // Duration is required by BIDS, but allow absent duration by treating as 0.

  std::vector<AnnotationEvent> events;
  events.reserve(256);

  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim_ws(line);
    if (t.empty()) continue;
    if (!t.empty() && t[0] == '#') continue;

    auto row = split_delimited_row(t, delim);
    // Normalize row length vs header.
    if (row.size() < header.size()) row.resize(header.size());
    while (row.size() > header.size() && !row.empty() && trim_ws(row.back()).empty()) {
      row.pop_back();
    }
    if (row.size() < header.size()) row.resize(header.size());

    double onset = std::numeric_limits<double>::quiet_NaN();
    if (!parse_double_maybe(row[static_cast<size_t>(col_onset)], delim, &onset)) {
      continue; // invalid onset
    }

    double dur = 0.0;
    if (col_dur >= 0) {
      double tmp = 0.0;
      if (parse_double_maybe(row[static_cast<size_t>(col_dur)], delim, &tmp)) dur = tmp;
    }
    if (!(dur >= 0.0) || !std::isfinite(dur)) dur = 0.0;

    std::string txt;
    if (col_txt >= 0) {
      txt = row[static_cast<size_t>(col_txt)];
    } else if (col_val >= 0) {
      txt = row[static_cast<size_t>(col_val)];
    }
    txt = trim_ws(txt);

    events.push_back({onset, dur, txt});
  }

  // Normalize + de-duplicate for deterministic downstream merges.
  // (This is intentionally conservative: only *exact* duplicates are removed after
  // microsecond quantization and text trimming.)
  deduplicate_events(&events);
  return events;
}

void write_events_csv(const std::string& path, const EEGRecording& rec) {
  write_events_csv(path, rec.events);
}

void write_recording_csv(const std::string& path, const EEGRecording& rec, bool write_time_column) {
  if (rec.fs_hz <= 0.0) {
    throw std::runtime_error("write_recording_csv: rec.fs_hz must be > 0");
  }
  if (rec.n_channels() == 0) {
    throw std::runtime_error("write_recording_csv: recording has no channels");
  }

  const size_t N = rec.n_samples();
  for (const auto& ch : rec.data) {
    if (ch.size() != N) {
      throw std::runtime_error("write_recording_csv: channel length mismatch");
    }
  }

  std::ofstream o(path);
  o.imbue(std::locale::classic());
  if (!o) throw std::runtime_error("Failed to write CSV: " + path);

  // Header
  bool first = true;
  if (write_time_column) {
    o << "time";
    first = false;
  }
  for (const auto& name : rec.channel_names) {
    if (!first) o << ",";
    o << csv_escape(name);
    first = false;
  }
  o << "\n";

  o << std::fixed << std::setprecision(6);
  const double inv_fs = 1.0 / rec.fs_hz;

  for (size_t s = 0; s < N; ++s) {
    bool col_first = true;
    if (write_time_column) {
      o << (static_cast<double>(s) * inv_fs);
      col_first = false;
    }
    for (size_t ch = 0; ch < rec.n_channels(); ++ch) {
      if (!col_first) o << ",";
      o << rec.data[ch][s];
      col_first = false;
    }
    o << "\n";
  }
}

} // namespace qeeg
