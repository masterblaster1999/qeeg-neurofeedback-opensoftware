#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <random>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace qeeg {

static inline bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

static inline bool is_alnum(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && is_space(s[b])) ++b;
  size_t e = s.size();
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string strip_utf8_bom(std::string s) {
  // UTF-8 BOM bytes: EF BB BF
  if (s.size() >= 3) {
    const unsigned char b0 = static_cast<unsigned char>(s[0]);
    const unsigned char b1 = static_cast<unsigned char>(s[1]);
    const unsigned char b2 = static_cast<unsigned char>(s[2]);
    if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
      return s.substr(3);
    }
  }
  return s;
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  // Handle trailing empty field
  if (!s.empty() && s.back() == delim) out.emplace_back("");
  return out;
}

std::vector<std::string> split_commandline_args(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  cur.reserve(s.size());

  bool in_single = false;
  bool in_double = false;

  // Track whether we've started a token. This lets us preserve explicitly
  // empty quoted arguments like "" or '' (important for UIs that pass
  // optional flags where the value may be intentionally empty).
  bool token_started = false;

  auto flush = [&]() {
    if (token_started) {
      out.push_back(cur);
      cur.clear();
      token_started = false;
    }
  };

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];

    // Quote handling (best-effort, dependency-free).
    //
    // Single quotes: treat everything literally until the next '.
    // Double quotes: treat everything literally until the next ".
    //
    // Backslash behavior is intentionally conservative:
    // - Outside quotes, backslash is only treated as an escape when it
    //   precedes whitespace or a quote character. Otherwise it is preserved.
    //   This makes Windows-style paths like C:\\temp\\file.edf work without
    //   requiring the user to double-escape every backslash.
    // - Inside double quotes, backslash may escape ", \\, or whitespace.
    // - Inside single quotes, backslash is always literal (common shell rule).
    if (c == '\\' && !in_single) {
      if (i + 1 < s.size()) {
        const char n = s[i + 1];

        const bool n_is_space = is_space(n);
        const bool n_is_quote = (n == '\'' || n == '"');
        const bool n_is_backslash = (n == '\\');

        bool do_escape = false;
        if (in_double) {
          // In double quotes, allow escaping of quotes/backslash/whitespace.
          do_escape = n_is_space || n == '"' || n_is_backslash;
        } else {
          // Outside quotes, only escape whitespace or quotes.
          do_escape = n_is_space || n_is_quote;
        }

        if (do_escape) {
          // Consume the escaped character.
          ++i;
          cur.push_back(n);
          token_started = true;
          continue;
        }
      }

      // Default: preserve the backslash literally.
      cur.push_back('\\');
      token_started = true;
      continue;
    }

    if (!in_double && c == '\'') {
      token_started = true;
      in_single = !in_single;
      continue;
    }
    if (!in_single && c == '"') {
      token_started = true;
      in_double = !in_double;
      continue;
    }

    if (!in_single && !in_double && is_space(c)) {
      flush();
      continue;
    }

    cur.push_back(c);
    token_started = true;
  }

  flush();
  return out;
}

namespace {

static inline bool win32_needs_quotes(const std::string& s) {
  if (s.empty()) return true;
  for (unsigned char c : s) {
    if (std::isspace(c) != 0) return true;
    if (c == '"') return true;
  }
  return false;
}

static std::string win32_quote_arg(const std::string& arg) {
  // Quote an argument using rules compatible with the MSVC CRT's command line
  // parsing.
  //
  // Key behaviors:
  // - Surround with "" if the argument is empty or contains whitespace/quotes.
  // - Backslashes are literal except when immediately preceding a quote.
  // - When quoting, trailing backslashes before the closing quote must be
  //   doubled.
  //
  // This algorithm matches the widely-used approach implemented by Python's
  // subprocess.list2cmdline.

  if (!win32_needs_quotes(arg)) return arg;

  std::string out;
  out.reserve(arg.size() + 2);
  out.push_back('"');

  size_t bs = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++bs;
      continue;
    }

    if (c == '"') {
      // Escape all backslashes, then escape the quote.
      out.append(bs * 2 + 1, '\\');
      out.push_back('"');
      bs = 0;
      continue;
    }

    // Normal character: emit any pending backslashes literally.
    if (bs) {
      out.append(bs, '\\');
      bs = 0;
    }
    out.push_back(c);
  }

  // End of arg: if we're quoting, escape trailing backslashes so the closing
  // quote is not consumed.
  if (bs) out.append(bs * 2, '\\');
  out.push_back('"');
  return out;
}

} // namespace

std::string join_commandline_args_win32(const std::vector<std::string>& argv) {
  std::string out;
  // Rough sizing: args + spaces.
  size_t total = 0;
  for (const auto& a : argv) total += a.size() + 1;
  out.reserve(total);

  bool first = true;
  for (const auto& a : argv) {
    if (!first) out.push_back(' ');
    first = false;
    out += win32_quote_arg(a);
  }
  return out;
}

std::string random_hex_token(size_t n_bytes) {
  if (n_bytes == 0) n_bytes = 16;
  static const char* kHex = "0123456789abcdef";

  // Best-effort entropy source.
  //
  // Prefer std::random_device directly rather than seeding a PRNG like mt19937.
  // For local-only tooling this is usually sufficient, and keeps the token
  // generation logic dependency-free.
  std::random_device rd;

  std::string out;
  out.reserve(n_bytes * 2);

  size_t produced = 0;
  while (produced < n_bytes) {
    const std::random_device::result_type r = rd();
    for (size_t k = 0; k < sizeof(r) && produced < n_bytes; ++k) {
      const unsigned char b = static_cast<unsigned char>((r >> (8 * k)) & 0xFFu);
      out.push_back(kHex[(b >> 4) & 0x0F]);
      out.push_back(kHex[b & 0x0F]);
      ++produced;
    }
  }

  return out;
}

std::vector<std::string> split_csv_row(const std::string& row, char delim) {
  // Minimal CSV parser for one physical line.
  //
  // Supports quoted fields with "" escaping.
  // This is intentionally permissive about whitespace after closing quotes.

  std::vector<std::string> out;
  std::string field;
  field.reserve(row.size());

  bool in_quotes = false;
  bool after_closing_quote = false;

  for (size_t i = 0; i < row.size(); ++i) {
    char c = row[i];

    // Common Windows line endings: getline() strips '\n' but not '\r'.
    if (!in_quotes && c == '\r') {
      // Ignore stray CR outside quotes.
      continue;
    }

    if (in_quotes) {
      if (c == '"') {
        // Escaped quote: ""
        if ((i + 1) < row.size() && row[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
          after_closing_quote = true;
        }
      } else {
        field.push_back(c);
      }
      continue;
    }

    // Not in quotes.
    if (after_closing_quote) {
      // RFC4180 requires next char to be delimiter or end-of-line; be tolerant
      // and allow whitespace before the delimiter.
      if (c == delim) {
        out.push_back(field);
        field.clear();
        after_closing_quote = false;
        continue;
      }
      if (is_space(c)) {
        continue;
      }
      // Unexpected char after closing quote: treat it as literal.
      after_closing_quote = false;
      field.push_back(c);
      continue;
    }

    if (c == delim) {
      out.push_back(field);
      field.clear();
      continue;
    }

    if (c == '"') {
      // Start quote if the field is empty or only whitespace.
      bool only_ws = field.empty();
      if (!only_ws) {
        only_ws = true;
        for (char fc : field) {
          if (!is_space(fc)) {
            only_ws = false;
            break;
          }
        }
      }
      if (only_ws) {
        field.clear();
        in_quotes = true;
        continue;
      }
      // Otherwise: quote inside unquoted field.
      field.push_back(c);
      continue;
    }

    field.push_back(c);
  }

  if (in_quotes) {
    throw std::runtime_error("split_csv_row: unterminated quoted field");
  }

  // Flush last field.
  out.push_back(field);
  return out;
}

void convert_csv_file_to_tsv(const std::string& csv_path, const std::string& tsv_path) {
  std::ifstream in(csv_path, std::ios::binary);
  if (!in) throw std::runtime_error("Failed to open CSV: " + csv_path);

  std::ofstream out(tsv_path, std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write TSV: " + tsv_path);

  std::string line;
  bool first_line = true;
  while (std::getline(in, line)) {
    if (first_line) {
      line = strip_utf8_bom(line);
      first_line = false;
    }

    const std::vector<std::string> fields = split_csv_row(line, ',');
    for (size_t i = 0; i < fields.size(); ++i) {
      std::string cell = fields[i];
      // TSV delimiter safety: replace any literal tab characters inside cells.
      for (char& c : cell) {
        if (c == '\t') c = ' ';
      }
      out << cell;
      if (i + 1 < fields.size()) out << '\t';
    }
    out << '\n';
  }
}


std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string normalize_channel_name(std::string s) {
  // NOTE: We keep this intentionally conservative and dependency-free.
  // It is used for lookups and matching, not for displaying labels.

  // Remove BOM defensively in case labels come from CSV headers.
  s = strip_utf8_bom(std::move(s));
  s = trim(s);
  if (s.empty()) return s;

  // Lowercase first so prefix/suffix checks are case-insensitive.
  s = to_lower(std::move(s));

  // Many EEG formats (notably EDF) store labels like "EEG Fp1-REF".
  // For robust matching against montages and CLI user input, we build an
  // alphanumeric-only key and then strip common modality prefixes/suffixes.
  std::string t;
  t.reserve(s.size());
  for (char c : s) {
    if (is_alnum(c)) t.push_back(c);
  }
  if (t.empty()) return t;

  // Strip common leading modality tokens.
  // Order matters: strip longer prefixes first.
  //
  // NOTE: Some devices use labels like "EEG1"/"EEG2" (digits immediately after the prefix).
  // In those cases we *do not* strip the prefix, since it would collapse distinct channels
  // down to bare digits and break matching/uniqueness.
  const std::vector<std::string> prefixes = {"seeg", "ieeg", "eeg"};
  for (const auto& p : prefixes) {
    if (starts_with(t, p) && t.size() > p.size()) {
      const char next = t[p.size()];
      if (std::isalpha(static_cast<unsigned char>(next)) != 0) {
        t = t.substr(p.size());
      }
      break;
    }
  }

  // Strip common reference suffixes.
  const std::vector<std::string> suffixes = {"reference", "ref"};
  for (const auto& suf : suffixes) {
    if (ends_with(t, suf) && t.size() > suf.size()) {
      t = t.substr(0, t.size() - suf.size());
      break;
    }
  }

  if (t.empty()) return t;

  // Common 10-20 legacy aliases.
  // Older: T3 T4 T5 T6; Newer: T7 T8 P7 P8.
  if (t == "t3") return "t7";
  if (t == "t4") return "t8";
  if (t == "t5") return "p7";
  if (t == "t6") return "p8";
  return t;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int to_int(const std::string& s) {
  try {
    const std::string t = trim(s);
    size_t idx = 0;
    int v = std::stoi(t, &idx, 10);
    if (idx == 0) throw std::invalid_argument("no digits");
    // Be strict: reject trailing garbage like "12abc".
    // (Trailing whitespace is already removed by trim().)
    if (idx != t.size()) throw std::invalid_argument("trailing characters");
    return v;
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse int from '" + s + "': " + e.what());
  }
}

double to_double(const std::string& s) {
  try {
    const std::string t = trim(s);
    if (t.empty()) throw std::invalid_argument("empty");

    auto parse_classic = [](const std::string& x, double* out) -> bool {
      if (!out) return false;
      std::istringstream iss(x);
      iss.imbue(std::locale::classic());
      double v = 0.0;
      iss >> v;
      if (!iss) return false;
      // Allow trailing whitespace, but reject any other trailing characters.
      iss >> std::ws;
      if (!iss.eof()) return false;
      *out = v;
      return true;
    };

    double v = 0.0;
    if (parse_classic(t, &v)) return v;

    // Common locale pitfall: users may provide a decimal comma (e.g. "0,5").
    // In the default C locale, stod("0,5") silently parses "0" and ignores
    // the remainder. We instead either parse correctly or throw.
    if (t.find('.') == std::string::npos) {
      const size_t cpos = t.find(',');
      if (cpos != std::string::npos) {
        // Only apply this fallback when there's exactly one comma.
        if (t.find(',', cpos + 1) == std::string::npos) {
          std::string tc = t;
          tc[cpos] = '.';
          if (parse_classic(tc, &v)) return v;
        }
      }
    }

    throw std::invalid_argument("invalid");
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to parse double from '" + s + "': " + e.what());
  }
}

bool file_exists(const std::string& path) {
  return std::filesystem::exists(std::filesystem::u8path(path));
}

void ensure_directory(const std::string& path) {
  std::filesystem::create_directories(std::filesystem::u8path(path));
}

namespace {

static bool localtime_safe(std::time_t t, std::tm* out) {
#if defined(_WIN32)
  return out && localtime_s(out, &t) == 0;
#else
  return out && localtime_r(&t, out) != nullptr;
#endif
}

static bool gmtime_safe(std::time_t t, std::tm* out) {
#if defined(_WIN32)
  return out && gmtime_s(out, &t) == 0;
#else
  return out && gmtime_r(&t, out) != nullptr;
#endif
}

static long utc_offset_seconds(std::time_t t) {
  std::tm local_tm{};
  std::tm gm_tm{};
  if (!localtime_safe(t, &local_tm) || !gmtime_safe(t, &gm_tm)) return 0;

  // mktime() interprets its input tm as *local time*. By converting both the
  // local and UTC broken-down times with mktime(), we can compute the local UTC
  // offset (including DST) for this instant.
  std::tm gm_as_local = gm_tm;
  gm_as_local.tm_isdst = -1;

  const std::time_t local_tt = std::mktime(&local_tm);
  const std::time_t gm_local_tt = std::mktime(&gm_as_local);
  if (local_tt == (std::time_t)-1 || gm_local_tt == (std::time_t)-1) return 0;

  const double diff = std::difftime(local_tt, gm_local_tt);
  return static_cast<long>(diff);
}

static std::string format_utc_offset(long offset_seconds) {
  char sign = '+';
  if (offset_seconds < 0) {
    sign = '-';
    offset_seconds = -offset_seconds;
  }

  const long total_minutes = offset_seconds / 60;
  const long hh = total_minutes / 60;
  const long mm = total_minutes % 60;

  std::ostringstream oss;
  oss << sign
      << std::setw(2) << std::setfill('0') << hh
      << ":"
      << std::setw(2) << std::setfill('0') << mm;
  return oss.str();
}

} // namespace

std::string now_string_local() {
  const std::time_t t = std::time(nullptr);

  std::tm tm{};
  if (!localtime_safe(t, &tm)) {
    // Fallback: ctime() is not thread-safe, but returning *something* is more
    // helpful than an empty string for logs/UI.
    std::string s = std::ctime(&t);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
  }

  // ISO-8601 local time with numeric UTC offset, e.g.
  //   2026-01-15T13:37:42-05:00
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
      << format_utc_offset(utc_offset_seconds(t));
  return oss.str();
}

std::string now_string_utc() {
  const std::time_t t = std::time(nullptr);

  std::tm tm{};
  if (!gmtime_safe(t, &tm)) return std::string();

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string json_escape(const std::string& s) {
  // Minimal JSON string escape.
  //
  // Notes:
  // - We assume the input is valid UTF-8 if it contains non-ASCII bytes.
  // - Control characters are escaped; other bytes are preserved.
  std::ostringstream oss;
  oss << std::hex << std::uppercase;
  for (unsigned char uc : s) {
    const char c = static_cast<char>(uc);
    switch (c) {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (uc < 0x20) {
          // JSON requires control chars to be escaped.
          oss << "\\u" << std::setw(4) << std::setfill('0') << static_cast<int>(uc);
          // Reset stream state for subsequent formatting.
          oss << std::setw(0) << std::setfill(' ');
        } else {
          oss << c;
        }
        break;
    }
  }
  return oss.str();
}


namespace {

static bool parse_uintmax_strict(const std::string& s, uintmax_t* out) {
  if (!out) return false;
  const std::string t = qeeg::trim(s);
  if (t.empty()) return false;

  uintmax_t v = 0;
  const uintmax_t kMax = std::numeric_limits<uintmax_t>::max();

  for (char c : t) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    const uintmax_t digit = static_cast<uintmax_t>(c - '0');

    // Overflow check for: v = v * 10 + digit
    if (v > (kMax - digit) / 10) return false;
    v = v * 10 + digit;
  }

  *out = v;
  return true;
}

static bool starts_with_ci(const std::string& s, const std::string& prefix_lower_ascii) {
  // prefix_lower_ascii must already be lowercase ASCII.
  if (s.size() < prefix_lower_ascii.size()) return false;
  for (size_t i = 0; i < prefix_lower_ascii.size(); ++i) {
    const unsigned char sc = static_cast<unsigned char>(s[i]);
    const unsigned char pc = static_cast<unsigned char>(prefix_lower_ascii[i]);
    if (static_cast<unsigned char>(std::tolower(sc)) != pc) return false;
  }
  return true;
}

} // namespace


HttpRangeResult parse_http_byte_range(const std::string& range_header,
                                      uintmax_t resource_size,
                                      uintmax_t* out_start,
                                      uintmax_t* out_end) {
  if (out_start) *out_start = 0;
  if (out_end) *out_end = 0;

  const std::string raw = trim(range_header);
  if (raw.empty()) return HttpRangeResult::kNone;
  if (resource_size == 0) return HttpRangeResult::kUnsatisfiable;

  // Only support the standard bytes range-unit.
  if (!starts_with_ci(raw, "bytes=")) return HttpRangeResult::kInvalid;

  std::string spec = trim(raw.substr(6));
  if (spec.empty()) return HttpRangeResult::kInvalid;

  // We intentionally do NOT support multiple ranges (comma-separated), since
  // that would require multipart/byteranges responses.
  if (spec.find(',') != std::string::npos) return HttpRangeResult::kInvalid;

  const size_t dash = spec.find('-');
  if (dash == std::string::npos) return HttpRangeResult::kInvalid;

  const std::string a = trim(spec.substr(0, dash));
  const std::string b = trim(spec.substr(dash + 1));
  if (a.empty() && b.empty()) return HttpRangeResult::kInvalid;

  uintmax_t start = 0;
  uintmax_t end = 0;

  if (a.empty()) {
    // Suffix range: bytes=-N (last N bytes)
    uintmax_t suffix = 0;
    if (!parse_uintmax_strict(b, &suffix)) return HttpRangeResult::kInvalid;
    if (suffix == 0) return HttpRangeResult::kUnsatisfiable;
    if (suffix >= resource_size) {
      start = 0;
    } else {
      start = resource_size - suffix;
    }
    end = resource_size - 1;
  } else {
    // Start or start-end.
    if (!parse_uintmax_strict(a, &start)) return HttpRangeResult::kInvalid;
    if (start >= resource_size) return HttpRangeResult::kUnsatisfiable;

    if (b.empty()) {
      end = resource_size - 1;
    } else {
      if (!parse_uintmax_strict(b, &end)) return HttpRangeResult::kInvalid;
      if (end < start) return HttpRangeResult::kInvalid;
      if (end >= resource_size) end = resource_size - 1;
    }
  }

  if (end < start) return HttpRangeResult::kInvalid;
  if (!out_start || !out_end) return HttpRangeResult::kInvalid;
  *out_start = start;
  *out_end = end;
  return HttpRangeResult::kSatisfiable;
}

} // namespace qeeg
