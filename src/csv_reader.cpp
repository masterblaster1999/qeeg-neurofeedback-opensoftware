#include "qeeg/csv_reader.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
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

enum class TextEncoding {
  kUtf8,
  kUtf16LE,
  kUtf16BE,
};

struct DetectedTextEncoding {
  TextEncoding encoding{TextEncoding::kUtf8};
  size_t bom_bytes{0};
};

static bool looks_like_zip_container_prefix(const unsigned char* b, size_t n) {
  // ZIP files start with the signature "PK" plus a 2-byte record type.
  // Common signatures:
  //   PK 03 04  (local file header)
  //   PK 01 02  (central directory header)
  //   PK 05 06  (end of central directory)
  //   PK 07 08  (data descriptor)
  if (n < 4) return false;
  if (b[0] != static_cast<unsigned char>('P') || b[1] != static_cast<unsigned char>('K')) return false;
  const unsigned char t0 = b[2];
  const unsigned char t1 = b[3];
  return (t0 == 0x03 && t1 == 0x04) || (t0 == 0x01 && t1 == 0x02) ||
         (t0 == 0x05 && t1 == 0x06) || (t0 == 0x07 && t1 == 0x08);
}

static bool looks_like_binary_blob_prefix_utf8(const unsigned char* b, size_t n) {
  const size_t sample = std::min<size_t>(static_cast<size_t>(n), 4096);
  if (sample == 0) return false;

  size_t nul = 0;
  size_t ctrl = 0;

  for (size_t i = 0; i < sample; ++i) {
    const unsigned char c = b[i];
    if (c == 0x00) {
      ++nul;
      continue;
    }

    // Count ASCII control bytes (excluding common whitespace).
    if (c < 0x09) {
      ++ctrl;
      continue;
    }
    if (c == 0x0B) {  // vertical tab
      ++ctrl;
      continue;
    }
    if (c == 0x7F) {
      ++ctrl;
      continue;
    }
    if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0C && c != 0x0D) {
      ++ctrl;
      continue;
    }
  }

  // NUL bytes are extremely unlikely in a UTF-8/ASCII delimited text export.
  // (They *do* occur frequently in UTF-16, which is handled by encoding sniffing
  // before this function is consulted.)
  const double nul_ratio = static_cast<double>(nul) / static_cast<double>(sample);
  if (nul_ratio >= 0.01) return true;

  const double ctrl_ratio = static_cast<double>(ctrl) / static_cast<double>(sample);
  // Text exports are also very unlikely to contain many control characters.
  return ctrl_ratio > 0.15;
}

static DetectedTextEncoding detect_text_encoding(const unsigned char* b, size_t n) {
  // Read a small prefix to detect BOMs and (heuristically) UTF-16 without a BOM.
  // Some Windows exporters save "Unicode text" (UTF-16) without a BOM.
  // BioTrace+/NeXus ASCII exports are usually mostly-ASCII (digits, delimiters,
  // and electrode names), so UTF-16 often shows up as a high ratio of NUL bytes
  // in either the odd or even byte positions.
  if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    return {TextEncoding::kUtf8, 3};
  }
  if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
    return {TextEncoding::kUtf16LE, 2};
  }
  if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
    return {TextEncoding::kUtf16BE, 2};
  }

  // Heuristic UTF-16 detection without BOM.
  //
  // For ASCII-heavy text in UTF-16:
  //  - UTF-16LE: bytes look like [c,0,c,0,...]  => NULs on odd indices.
  //  - UTF-16BE: bytes look like [0,c,0,c,...]  => NULs on even indices.
  // Keep this threshold fairly small so we can still detect tiny UTF-16
  // snippets (e.g., short test exports) without misclassifying them as
  // binary just because they contain NUL bytes.
  if (n >= 16) {
    size_t even_zeros = 0;
    size_t odd_zeros = 0;
    size_t even_n = 0;
    size_t odd_n = 0;

    for (size_t i = 0; i < n; ++i) {
      if ((i & 1u) == 0u) {
        ++even_n;
        if (b[i] == 0x00) ++even_zeros;
      } else {
        ++odd_n;
        if (b[i] == 0x00) ++odd_zeros;
      }
    }

    const double total_zero_ratio =
        (static_cast<double>(even_zeros + odd_zeros) / static_cast<double>(n));
    const double even_zero_ratio =
        (even_n > 0) ? (static_cast<double>(even_zeros) / static_cast<double>(even_n)) : 0.0;
    const double odd_zero_ratio =
        (odd_n > 0) ? (static_cast<double>(odd_zeros) / static_cast<double>(odd_n)) : 0.0;

    // Require both:
    //  - Many NULs overall (typical for UTF-16 text).
    //  - A strong asymmetry between even/odd byte positions.
    // This helps avoid misclassifying arbitrary binary files.
    if (total_zero_ratio >= 0.20) {
      if (odd_zero_ratio >= 0.40 && even_zero_ratio <= 0.10) {
        return {TextEncoding::kUtf16LE, 0};
      }
      if (even_zero_ratio >= 0.40 && odd_zero_ratio <= 0.10) {
        return {TextEncoding::kUtf16BE, 0};
      }
    }
  }

  return {TextEncoding::kUtf8, 0};
}

static void append_utf8_codepoint(uint32_t cp, std::string* out) {
  if (!out) return;

  // Treat invalid scalar values and surrogates as replacement.
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;

  if (cp <= 0x7Fu) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

struct Utf16StreamState {
  bool little_endian{true};
  bool eof{false};
  std::vector<unsigned char> buf;
  size_t pos{0};

  // A single code-unit pushback slot (used for CRLF and invalid surrogate recovery).
  bool has_pending{false};
  uint16_t pending{0};

  void fill(std::ifstream& f) {
    if (eof) return;

    // Compact buffer: drop already-consumed bytes.
    if (pos > 0) {
      if (pos >= buf.size()) {
        buf.clear();
      } else {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::vector<unsigned char>::difference_type>(pos));
      }
      pos = 0;
    }

    unsigned char tmp[1 << 16];
    f.read(reinterpret_cast<char*>(tmp), sizeof(tmp));
    const std::streamsize n = f.gcount();
    if (n <= 0) {
      eof = true;
      return;
    }

    buf.insert(buf.end(), tmp, tmp + n);

    if (f.eof()) eof = true;
  }

  bool next_code_unit(std::ifstream& f, uint16_t* out) {
    if (has_pending) {
      has_pending = false;
      if (out) *out = pending;
      return true;
    }

    while (true) {
      if (pos + 2 <= buf.size()) {
        const unsigned char b0 = buf[pos];
        const unsigned char b1 = buf[pos + 1];
        pos += 2;

        const uint16_t v = little_endian
                               ? static_cast<uint16_t>(static_cast<uint16_t>(b0) |
                                                       (static_cast<uint16_t>(b1) << 8))
                               : static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8) |
                                                       static_cast<uint16_t>(b1));
        if (out) *out = v;
        return true;
      }

      if (eof) return false;
      fill(f);

      if (eof && pos + 2 > buf.size()) return false;
    }
  }
};

static bool getline_any(std::ifstream& f, TextEncoding enc, Utf16StreamState* u16, std::string* out) {
  if (!out) return false;

  if (enc == TextEncoding::kUtf8) {
    return static_cast<bool>(std::getline(f, *out));
  }

  if (!u16) return false;
  out->clear();

  bool got_any = false;
  uint16_t cu = 0;

  while (true) {
    if (!u16->next_code_unit(f, &cu)) {
      return got_any;
    }

    got_any = true;

    // Newline handling (CR, LF, CRLF).
    if (cu == 0x000Au) { // LF
      return true;
    }
    if (cu == 0x000Du) { // CR
      // Consume a following LF if present.
      uint16_t next = 0;
      if (u16->next_code_unit(f, &next)) {
        if (next != 0x000Au) {
          u16->has_pending = true;
          u16->pending = next;
        }
      }
      return true;
    }

    // UTF-16 decoding.
    if (cu >= 0xD800u && cu <= 0xDBFFu) {
      // High surrogate.
      uint16_t lo = 0;
      if (u16->next_code_unit(f, &lo)) {
        if (lo >= 0xDC00u && lo <= 0xDFFFu) {
          const uint32_t codepoint =
              0x10000u + ((static_cast<uint32_t>(cu - 0xD800u) << 10) | static_cast<uint32_t>(lo - 0xDC00u));
          append_utf8_codepoint(codepoint, out);
        } else {
          // Invalid surrogate pair: emit replacement and push back 'lo'.
          append_utf8_codepoint(0xFFFDu, out);
          u16->has_pending = true;
          u16->pending = lo;
        }
      } else {
        append_utf8_codepoint(0xFFFDu, out);
        return true;
      }
      continue;
    }

    if (cu >= 0xDC00u && cu <= 0xDFFFu) {
      // Orphan low surrogate.
      append_utf8_codepoint(0xFFFDu, out);
      continue;
    }

    append_utf8_codepoint(static_cast<uint32_t>(cu), out);
  }
}

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

bool is_export_footer_line(const std::string& line) {
  // BioTrace+ ASCII exports often end with a footer marker like:
  //   "<end of exported RAW data>"
  //   "<Unbearbeitete Daten exportiert>"
  // These are not data rows and should be ignored.
  std::string t = trim(strip_utf8_bom(line));
  if (t.size() >= 2 && t.front() == '<' && t.back() == '>') return true;
  return false;
}


static bool looks_like_header_row(const std::vector<std::string>& cols) {
  // Best-effort: require at least one alphabetic character near the *front* of the row,
  // not just anywhere.
  //
  // Rationale:
  // - Some headerless exports include a trailing segment/marker text column (e.g. "Baseline"),
  //   which would otherwise make a *data* row look like a header.
  // - Real headers almost always have an axis label (Time/Sample/...) and/or a channel name
  //   in the first two columns.
  const size_t kCheck = std::min<size_t>(cols.size(), 2);
  for (size_t i = 0; i < kCheck; ++i) {
    for (unsigned char ch : cols[i]) {
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

  // Also handle a legacy single-byte micro sign (0xB5) that can appear in
  // Windows-1252 / ISO-8859-1 exports (common in some BioTrace+/NeXus ASCII files).
  // We do this *after* the UTF-8 replacements so we don't corrupt valid UTF-8 sequences.
  for (char& ch : u) {
    if (static_cast<unsigned char>(ch) == 0xB5) ch = 'u';
  }

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
  // Common non-English variants seen in exports.
  //  - German: Zeit
  //  - Dutch: Tijd
  //  - French: Temps
  //  - Spanish: Tiempo
  if (key == "zeit" || key == "tijd" || key == "temps" || key == "tiempo") return true;
  // Common variants: time_ms, time_s, time(sec), ...
  if (starts_with(key, "time")) return true;
  if (starts_with(key, "zeit")) return true;
  if (starts_with(key, "tijd")) return true;
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
      // BioTrace+/NeXus exports sometimes label the sample counter as "Beispiele".
      "beispiele",
  };
  for (const auto& e : exact) {
    if (key == e) return true;
  }
  if (starts_with(key, "sample") && key.size() > 6) return true;
  return false;
}

bool is_fs_meta_key(const std::string& key) {
  // Detect common metadata keys that specify sampling rate.
  // We use a conservative alphanumeric-only key (normalize_col_key).
  if (key.empty()) return false;

  static const std::vector<std::string> exact = {
      "samplerate",
      "samplingrate",
      "samplingfrequency",
      "samplingfreq",
      "fshz",
      "fs",
      // German
      "abtastrate",
      "abtastfrequenz",
  };
  for (const auto& e : exact) {
    if (key == e) return true;
  }

  // Prefix matches: "samplingratehz", "samplerateinhz", ...
  if (starts_with(key, "samplerate") || starts_with(key, "samplingrate") ||
      starts_with(key, "samplingfrequency") || starts_with(key, "abtastrate") ||
      starts_with(key, "abtastfrequenz") || starts_with(key, "fshz")) {
    return true;
  }

  return false;
}

bool extract_first_number_token(const std::string& s, std::string* out_token) {
  if (!out_token) return false;
  out_token->clear();

  const std::string t = trim(s);
  bool in = false;
  size_t start = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(t[i]);
    const bool is_digit = (std::isdigit(ch) != 0);
    const bool is_num_char = is_digit || t[i] == '.' || t[i] == ',';
    if (!in) {
      // Only start a token on a digit to avoid capturing a leading '.' from text.
      if (is_digit) {
        in = true;
        start = i;
      }
    } else {
      if (!is_num_char) {
        *out_token = t.substr(start, i - start);
        return !out_token->empty();
      }
    }
  }
  if (in) {
    *out_token = t.substr(start);
    return !out_token->empty();
  }
  return false;
}

double infer_fs_hz_from_metadata(const std::vector<std::string>& prefix) {
  // Best-effort: scan header/metadata lines for a sampling-rate declaration.
  // BioTrace+/NeXus ASCII exports sometimes provide fs in a metadata row, e.g.:
  //   "Sample Rate;250"
  //   "Sampling frequency (Hz): 256"
  //   "Abtastrate; 512 Hz"
  // We only use this when the caller did not pass --fs and we cannot reliably infer
  // from a time column.
  for (const auto& raw : prefix) {
    std::string line = trim(strip_utf8_bom(raw));
    if (is_comment_or_empty(line)) continue;
    if (is_export_footer_line(line)) continue;

    auto try_parse_kv = [&](const std::string& k, const std::string& v) -> double {
      const std::string key = normalize_col_key(k);
      if (!is_fs_meta_key(key)) return 0.0;
      std::string token;
      if (!extract_first_number_token(v, &token)) return 0.0;
      double fs = 0.0;
      try {
        fs = parse_double_relaxed_decimal(token);
      } catch (const std::exception&) {
        return 0.0;
      }
      if (!(fs > 0.0) || !std::isfinite(fs)) return 0.0;
      const std::string vlow = to_lower(v);
      if (vlow.find("khz") != std::string::npos) fs *= 1000.0;
      // Treat "sps" (samples per second) as Hz.
      if (fs > 0.0 && fs <= 100000.0) return fs;
      return 0.0;
    };

    // 1) Delimited key/value.
    {
      const char d = detect_delim(line);
      auto parts = split_csv_row(line, d);
      for (auto& x : parts) x = trim(x);
      if (parts.size() >= 2) {
        std::string v = parts[1];
        // Some exports may split units into separate columns (e.g., "250";"Hz").
        for (size_t i = 2; i < parts.size(); ++i) {
          const std::string extra = trim(parts[i]);
          if (!extra.empty()) v += " " + extra;
        }
        const double fs = try_parse_kv(parts[0], v);
        if (fs > 0.0) return fs;
      }
    }

    // 2) "key: value" / "key=value".
    {
      size_t pos = line.find(':');
      if (pos == std::string::npos) pos = line.find('=');
      if (pos != std::string::npos) {
        const std::string k = line.substr(0, pos);
        const std::string v = line.substr(pos + 1);
        const double fs = try_parse_kv(k, v);
        if (fs > 0.0) return fs;
      }
    }

    // 3) Fallback: keyword anywhere in line.
    const std::string allkey = normalize_col_key(line);
    if (allkey.find("samplerate") != std::string::npos || allkey.find("samplingrate") != std::string::npos ||
        allkey.find("samplingfrequency") != std::string::npos || allkey.find("abtastrate") != std::string::npos ||
        allkey.find("abtastfrequenz") != std::string::npos) {
      std::string token;
      if (!extract_first_number_token(line, &token)) continue;
      try {
        double fs = parse_double_relaxed_decimal(token);
        if (!(fs > 0.0) || !std::isfinite(fs)) continue;
        const std::string vlow = to_lower(line);
        if (vlow.find("khz") != std::string::npos) fs *= 1000.0;
        if (fs > 0.0 && fs <= 100000.0) return fs;
      } catch (const std::exception&) {
        continue;
      }
    }
  }
  return 0.0;
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
    const long long r = std::llround(v);
    if (std::fabs(v - static_cast<double>(r)) <= 1e-6) {
      return std::to_string(r);
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

  // Sniff the first bytes to detect containers / binary blobs early. This helps
  // when a user accidentally passes a compressed/session container (e.g., some
  // BioTrace+/NeXus exports) instead of a plain delimited text export.
  unsigned char sniff[4096];
  // MSVC warns about implicit narrowing when the fill value is an int literal.
  // Use an explicit unsigned-char zero to keep builds warning-free.
  std::fill(std::begin(sniff), std::end(sniff), static_cast<unsigned char>(0));
  f.read(reinterpret_cast<char*>(sniff), sizeof(sniff));
  const std::streamsize sniff_n = f.gcount();
  f.clear();
  f.seekg(0, std::ios::beg);

  if (looks_like_zip_container_prefix(sniff, static_cast<size_t>(sniff_n))) {
    throw std::runtime_error(
        "Input appears to be a ZIP container (not a plain delimited text export): " + path +
        "\nIf this is a BioTrace+/NeXus session container (.bcd/.mbd/.m2k/.zip), extract an embedded EDF/BDF/ASCII export first.\n"
        "Try: python3 scripts/biotrace_extract_container.py --input <container> --outdir extracted\n"
        "     (alias: --container)\n"
        "Or:  python3 scripts/biotrace_run_nf.py --container <container> --outdir <outdir> -- <qeeg_nf_cli args>");
  }

  // Encoding detection: some BioTrace+/NeXus ASCII exports are saved as UTF-16 ("Unicode" text on Windows).
  // Support UTF-8/ASCII and UTF-16LE/BE with a BOM, and heuristically detect UTF-16 without a BOM.
  const DetectedTextEncoding det = detect_text_encoding(sniff, static_cast<size_t>(sniff_n));

  if (det.encoding == TextEncoding::kUtf8 &&
      looks_like_binary_blob_prefix_utf8(sniff, static_cast<size_t>(sniff_n))) {
    throw std::runtime_error(
        "Input appears to be binary (not a delimited text export): " + path +
        "\nIf this is a proprietary BioTrace+/NeXus format, export to an open format (EDF/BDF/ASCII) first.");
  }

  if (det.bom_bytes > 0) {
    f.seekg(static_cast<std::streamoff>(det.bom_bytes), std::ios::beg);
  }
  const TextEncoding enc = det.encoding;
  Utf16StreamState u16;
  if (enc == TextEncoding::kUtf16BE) u16.little_endian = false;

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
    while (prefix.size() < kMaxScanLines && getline_any(f, enc, &u16, &line)) {
      prefix.push_back(line);
    }
  }

  // Some BioTrace+/NeXus ASCII exports include the sampling rate as a metadata
  // line before the header row. If the caller does not provide --fs, we can
  // use this as a fallback when the time axis is absent or too coarse.
  const double meta_fs_hz = (fs_hz_ <= 0.0) ? infer_fs_hz_from_metadata(prefix) : 0.0;

char delim = ',';
std::vector<std::string> cols;
size_t header_line_idx = static_cast<size_t>(-1);
size_t data_start_idx = static_cast<size_t>(-1);
size_t time_col = static_cast<size_t>(-1);
size_t sample_col = static_cast<size_t>(-1);
size_t first_data_col = 0;
bool has_time = false;
bool time_in_ms = false;

auto normalize_vals_to_ncols = [&](std::vector<std::string>* vals, size_t ncols) {
  if (!vals) return;
  if (vals->size() < ncols) {
    vals->resize(ncols);
  } else if (vals->size() > ncols) {
    while (vals->size() > ncols && trim(vals->back()).empty()) {
      vals->pop_back();
    }
  }
};

auto normalize_vals_to_header = [&](std::vector<std::string>* vals) {
  normalize_vals_to_ncols(vals, cols.size());
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
  if (is_export_footer_line(t)) continue;
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
    if (is_export_footer_line(u)) continue;

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
    data_start_idx = i + 1;
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

if (data_start_idx == static_cast<size_t>(-1) || cols.empty()) {
  // Headerless fallback: handle simple ASCII exports that omit column names.
  // Common in some BioTrace+ exports and ad-hoc recordings where the file is
  // just: <time-or-sample><delim><value>[<delim>...]
  //
  // We attempt to infer the delimiter, generate synthetic channel names, and
  // (best-effort) treat sparse string columns as event/segment streams.
  size_t first_data_idx = static_cast<size_t>(-1);
  for (size_t i = 0; i < prefix.size(); ++i) {
    std::string t = trim(prefix[i]);
    if (is_comment_or_empty(t)) continue;
    if (is_export_footer_line(t)) continue;
    first_data_idx = i;
    break;
  }
  if (first_data_idx == static_cast<size_t>(-1)) {
    throw std::runtime_error("CSV: missing header row (file appears empty)");
  }

  // Collect a small sample of lines for delimiter and column inference.
  const size_t kMaxSampleLines = 512;
  std::vector<std::string> sample_lines;
  sample_lines.reserve(128);
  for (size_t i = first_data_idx; i < prefix.size() && sample_lines.size() < kMaxSampleLines; ++i) {
    std::string t = trim(prefix[i]);
    if (is_comment_or_empty(t)) continue;
    if (is_export_footer_line(t)) continue;
    sample_lines.push_back(prefix[i]);
  }

  struct DelimEval {
    char d{','};
    size_t max_cols{0};
    size_t axis_ok{0};
    size_t col1_ok{0};
    size_t col1_bad{0};
    long long score{std::numeric_limits<long long>::min()};
  };

  auto eval_delim = [&](char d) -> DelimEval {
    DelimEval e;
    e.d = d;
    for (const auto& line : sample_lines) {
      auto vals = split_csv_row(line, d);
      for (auto& x : vals) x = trim(x);

      if (vals.size() < 2) continue;
      e.max_cols = std::max(e.max_cols, vals.size());

      // Axis column parseability.
      try {
        (void)parse_time_cell(vals[0], d);
        ++e.axis_ok;
      } catch (const std::exception&) {
        // ignore
      }

      // Second column should usually be numeric for a real signal channel.
      if (vals.size() >= 2) {
        const std::string c1 = trim(vals[1]);
        if (!c1.empty() && !is_nan_like(c1)) {
          try {
            (void)parse_double_csv_locale(c1, d);
            ++e.col1_ok;
          } catch (const std::exception&) {
            ++e.col1_bad;
          }
        }
      }
    }

    // Prefer delimiters that:
    //  - yield at least 2 columns
    //  - parse the 2nd column as numeric often
    //  - avoid parse failures on the 2nd column
    e.score = static_cast<long long>(e.max_cols) * 10 +
              static_cast<long long>(e.col1_ok) * 1000 +
              static_cast<long long>(e.axis_ok) * 10 -
              static_cast<long long>(e.col1_bad) * 2000;
    return e;
  };

  DelimEval best = eval_delim(',');
  for (char d : std::vector<char>{';', '\t'}) {
    DelimEval cand = eval_delim(d);
    if (cand.score > best.score) best = cand;
  }

  if (best.max_cols < 2) {
    throw std::runtime_error("CSV: missing header row");
  }

  delim = best.d;
  const size_t ncols = best.max_cols;

  // Infer whether the first column is a time axis or a sample index.
  std::vector<double> axis_vals;
  axis_vals.reserve(sample_lines.size());
  bool any_hms = false;
  for (const auto& line : sample_lines) {
    auto vals = split_csv_row(line, delim);
    for (auto& x : vals) x = trim(x);
    normalize_vals_to_ncols(&vals, ncols);

    const std::string c0 = trim(vals[0]);
    if (c0.find(':') != std::string::npos) any_hms = true;
    try {
      const ParsedTimeCell pt = parse_time_cell(c0, delim);
      axis_vals.push_back(pt.value);
    } catch (const std::exception&) {
      // skip unparseable axis cells during inference
    }
  }

  bool axis_is_sample = false;
  if (!any_hms && axis_vals.size() >= 3) {
    // If the axis looks like a contiguous integer counter with dt=1, treat it
    // as a sample index rather than a time axis (BioTrace+ can export "time"
    // in samples).
    size_t n_int = 0;
    for (double v : axis_vals) {
      if (!std::isfinite(v)) continue;
      const double r = std::round(v);
      if (std::fabs(v - r) <= 1e-9) ++n_int;
    }
    const double int_ratio = static_cast<double>(n_int) / static_cast<double>(axis_vals.size());

    std::vector<double> dts;
    dts.reserve(axis_vals.size() - 1);
    for (size_t i = 1; i < axis_vals.size(); ++i) {
      const double dt = axis_vals[i] - axis_vals[i - 1];
      if (dt > 0.0 && std::isfinite(dt)) dts.push_back(dt);
    }
    const double dt_med = dts.empty() ? std::numeric_limits<double>::quiet_NaN() : median_inplace(&dts);

    const bool starts_near_zero = (std::fabs(axis_vals.front()) <= 1e-6) ||
                                 (std::fabs(axis_vals.front() - 1.0) <= 1e-6);

    if (int_ratio >= 0.95 && starts_near_zero && std::isfinite(dt_med) && std::fabs(dt_med - 1.0) <= 1e-9) {
      axis_is_sample = true;
    }
  }

  // Analyze non-axis columns to identify empty placeholders and event streams.
  // This is useful for BioTrace+ exports where one or more trailing columns
  // are reserved for event markers/segments.
  const size_t data0 = 1;
  std::vector<size_t> nonempty(ncols, 0);
  std::vector<size_t> numeric_ok(ncols, 0);
  std::vector<size_t> numeric_bad(ncols, 0);
  std::vector<size_t> numeric_nonzero(ncols, 0);
  std::vector<size_t> numeric_nonint_nonzero(ncols, 0);
  std::vector<std::vector<long long>> int_codes(ncols);
  for (auto& v : int_codes) v.reserve(8);

  auto add_code = [&](std::vector<long long>* codes, long long c) {
    if (!codes) return;
    for (long long x : *codes) {
      if (x == c) return;
    }
    if (codes->size() < 32) codes->push_back(c);
  };

  for (const auto& line : sample_lines) {
    auto vals = split_csv_row(line, delim);
    for (auto& x : vals) x = trim(x);
    normalize_vals_to_ncols(&vals, ncols);

    for (size_t j = data0; j < ncols; ++j) {
      const std::string cell = trim(vals[j]);
      if (cell.empty() || is_nan_like(cell)) continue;
      ++nonempty[j];

      try {
        const double v = parse_double_csv_locale(cell, delim);
        ++numeric_ok[j];
        if (std::isfinite(v) && std::fabs(v) > 1e-12) {
          ++numeric_nonzero[j];
          const long long r = std::llround(v);
          if (std::fabs(v - static_cast<double>(r)) <= 1e-6) {
            add_code(&int_codes[j], r);
          } else {
            ++numeric_nonint_nonzero[j];
          }
        }
      } catch (const std::exception&) {
        ++numeric_bad[j];
      }
    }
  }

  // Decide which columns are numeric channels vs event/segment streams.
  std::vector<bool> col_is_event(ncols, false);
  std::vector<bool> col_is_segment(ncols, false);

  for (size_t j = data0; j < ncols; ++j) {
    if (nonempty[j] == 0) {
      // Always empty placeholder -> ignore by treating as an event column.
      col_is_event[j] = true;
      col_is_segment[j] = false;
      continue;
    }

    if (numeric_ok[j] == 0 && numeric_bad[j] > 0) {
      // Pure string column -> event/segment.
      col_is_event[j] = true;
    } else if (numeric_bad[j] > 0) {
      // Mixed numeric + strings: treat as event to avoid parse failures.
      const double bad_ratio = static_cast<double>(numeric_bad[j]) / static_cast<double>(nonempty[j]);
      if (bad_ratio >= 0.2) col_is_event[j] = true;
    } else {
      // Pure numeric column. Heuristic: if it is mostly zeros with a small set
      // of integer-like non-zero codes, treat as a marker stream.
      if (numeric_ok[j] > 0 && numeric_nonzero[j] > 0) {
        const double nz_ratio = static_cast<double>(numeric_nonzero[j]) / static_cast<double>(numeric_ok[j]);
        if (nz_ratio <= 0.1 && numeric_nonint_nonzero[j] == 0 && int_codes[j].size() <= 32) {
          col_is_event[j] = true;
        }
      }
    }
  }

  // Segment vs marker: if a string label persists over multiple consecutive
  // samples, classify as a segment stream.
  for (size_t j = data0; j < ncols; ++j) {
    if (!col_is_event[j] || nonempty[j] == 0) continue;

    std::string cur;
    size_t run = 0;
    size_t max_run = 0;

    for (const auto& line : sample_lines) {
      auto vals = split_csv_row(line, delim);
      for (auto& x : vals) x = trim(x);
      normalize_vals_to_ncols(&vals, ncols);

      const std::string label = marker_cell_to_label(vals[j], delim);
      if (label.empty()) {
        cur.clear();
        run = 0;
        continue;
      }
      if (label == cur) {
        ++run;
      } else {
        cur = label;
        run = 1;
      }
      max_run = std::max(max_run, run);
    }

    if (max_run > 1) {
      col_is_segment[j] = true;
    }
  }

  // Construct a synthetic header row.
  cols.clear();
  cols.resize(ncols);
  cols[0] = axis_is_sample ? "sample" : "time";

  size_t ch_idx = 1;
  size_t ev_idx = 1;
  size_t seg_idx = 1;
  for (size_t j = 1; j < ncols; ++j) {
    if (!col_is_event[j]) {
      cols[j] = "Ch" + std::to_string(ch_idx++);
      continue;
    }

    if (col_is_segment[j]) {
      cols[j] = (seg_idx == 1) ? "segment" : ("segment" + std::to_string(seg_idx));
      ++seg_idx;
    } else {
      cols[j] = (ev_idx == 1) ? "event" : ("event" + std::to_string(ev_idx));
      ++ev_idx;
    }
  }

  // Initialize axis interpretation from the synthetic header.
  if (axis_is_sample) {
    sample_col = 0;
    time_col = static_cast<size_t>(-1);
    has_time = false;
    first_data_col = 1;
  } else {
    time_col = 0;
    sample_col = static_cast<size_t>(-1);
    has_time = true;
    first_data_col = 1;
    time_in_ms = false;
  }

  data_start_idx = first_data_idx;
  header_line_idx = static_cast<size_t>(-1); // indicates synthetic header
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
    if (is_export_footer_line(t)) return;

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
  for (size_t i = data_start_idx; i < prefix.size(); ++i) {
    parse_and_append_row(prefix[i], i + 1);
  }

  // Continue streaming the rest of the file (if any).
  size_t lineno = prefix.size();
  std::string line;
  while (getline_any(f, enc, &u16, &line)) {
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
    auto use_meta_or_throw = [&](const std::string& reason) -> double {
      if (meta_fs_hz > 0.0 && std::isfinite(meta_fs_hz)) return meta_fs_hz;
      throw std::runtime_error(
          reason +
          "\nCSVReader: pass --fs <Hz> (or export a high-resolution time column, e.g. time_ms / hh:mm:ss.sss).");
    };

    if (!has_time) {
      // Header uses a sample index axis (or the file is headerless) and no time column exists.
      rec.fs_hz = use_meta_or_throw(
          "CSV: sampling rate not provided and no time axis found to infer it from (sample index axis detected).");
    } else if (time_values.size() < 2) {
      rec.fs_hz = use_meta_or_throw("CSV: need at least 2 time samples to infer fs");
    } else {
      const size_t n_pairs = time_values.size() - 1;
      std::vector<double> dts;
      dts.reserve(n_pairs);

      for (size_t i = 1; i < time_values.size(); ++i) {
        const double dt = time_values[i] - time_values[i - 1];
        if (dt > 0.0 && std::isfinite(dt)) dts.push_back(dt);
      }

      const double pos_ratio = (n_pairs > 0)
                                   ? (static_cast<double>(dts.size()) / static_cast<double>(n_pairs))
                                   : 0.0;
      if (dts.empty()) {
        rec.fs_hz = use_meta_or_throw("CSV: time axis is not increasing, so fs cannot be inferred from it");
      } else {
        double dt_med = median_inplace(&dts);
        if (!(dt_med > 0.0) || !std::isfinite(dt_med)) {
          rec.fs_hz = use_meta_or_throw("CSV: failed to infer dt from time column");
        } else {
          // Detect numeric time axes that are actually sample indices (0,1,2,...) exported as "Time"/"Zeit".
          bool time_name_hints_seconds = false;
          if (time_col != static_cast<size_t>(-1)) {
            const std::string tc = to_lower(cols[time_col]);
            if (tc.find("sec") != std::string::npos || tc.find("second") != std::string::npos) {
              time_name_hints_seconds = true;
            }
            if (tc.find("time_s") != std::string::npos || tc.find("_s") != std::string::npos ||
                tc.find("(s)") != std::string::npos || tc.find("[s]") != std::string::npos) {
              time_name_hints_seconds = true;
            }
          }

          bool axis_looks_like_sample = false;
          if (!time_axis_is_hms && !time_in_ms && !time_name_hints_seconds) {
            // If the axis looks like a contiguous integer counter with dt=1, treat it as a sample index.
            size_t n_int = 0;
            for (double v : time_values) {
              if (!std::isfinite(v)) continue;
              const double r = std::round(v);
              if (std::fabs(v - r) <= 1e-9) ++n_int;
            }
            const double int_ratio = static_cast<double>(n_int) / static_cast<double>(time_values.size());
            const bool starts_near_zero = (std::fabs(time_values.front()) <= 1e-6) ||
                                         (std::fabs(time_values.front() - 1.0) <= 1e-6);
            if (int_ratio >= 0.95 && starts_near_zero && std::fabs(dt_med - 1.0) <= 1e-9) {
              axis_looks_like_sample = true;
              // If metadata indicates a truly low sampling rate (e.g., 1 Hz), treat the axis as seconds.
              if (meta_fs_hz > 0.0 && meta_fs_hz <= 2.0) axis_looks_like_sample = false;
            }
          }

          // If many consecutive steps are non-increasing, the time axis is likely too coarse
          // (e.g., hh:mm:ss without sub-second resolution) to infer fs.
          const bool axis_too_coarse = (pos_ratio < 0.5);

          if (axis_looks_like_sample || axis_too_coarse) {
            rec.fs_hz = use_meta_or_throw(
                axis_looks_like_sample
                    ? "CSV: time column looks like a sample index (0,1,2,...) so fs cannot be inferred from it"
                    : "CSV: time column appears too coarse/non-monotonic to infer fs reliably");
          } else {
            // Units:
            // - If the header name hints at ms, use that.
            // - Otherwise, use a pragmatic EEG-specific heuristic: if dt is > 1.0, the
            //   file is likely using milliseconds (dt=4 for 250Hz, etc).
            double dt_sec = dt_med;
            if (!time_axis_is_hms) {
              if (time_in_ms || dt_med > 1.0) dt_sec = dt_med / 1000.0;
            }

            if (!(dt_sec > 0.0) || !std::isfinite(dt_sec)) {
              rec.fs_hz = use_meta_or_throw("CSV: invalid inferred dt_sec from time column");
            } else {
              rec.fs_hz = 1.0 / dt_sec;
              if (!(rec.fs_hz > 0.0) || !std::isfinite(rec.fs_hz)) {
                rec.fs_hz = use_meta_or_throw("CSV: failed to infer fs from time column");
              }
            }
          }
        }
      }
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
