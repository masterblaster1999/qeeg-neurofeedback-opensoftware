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

bool write_text_file(const std::string& path, const std::string& content) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  std::error_code ec;
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }

  std::ofstream out(p, std::ios::binary);
  if (!out) return false;
  if (!content.empty()) out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  out.close();
  return static_cast<bool>(out);
}

namespace {

static std::filesystem::path make_tmp_path_same_dir(const std::filesystem::path& target) {
  const std::filesystem::path dir = target.has_parent_path() ? target.parent_path()
                                                             : std::filesystem::path();
  const std::string base = target.filename().u8string();
  const std::string name = base + ".tmp." + random_hex_token(8);
  return dir.empty() ? std::filesystem::u8path(name)
                     : (dir / std::filesystem::u8path(name));
}

} // namespace

bool write_text_file_atomic(const std::string& path, const std::string& content) {
  const std::filesystem::path target = std::filesystem::u8path(path);

  // Ensure destination directory exists.
  std::error_code ec;
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
  }

  // Create a temporary file in the same directory (best-effort).
  std::filesystem::path tmp = make_tmp_path_same_dir(target);
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (!std::filesystem::exists(tmp, ec)) break;
    tmp = make_tmp_path_same_dir(target);
  }

  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return false;
    if (!content.empty()) out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    out.close();
    if (!out) {
      std::error_code rm_ec;
      std::filesystem::remove(tmp, rm_ec);
      return false;
    }
  }

  // Rename into place.
  ec.clear();
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    // On some platforms (notably Windows), rename may fail if the destination
    // exists. Best-effort fallback: remove existing destination and rename.
    std::error_code rm_ec;
    std::filesystem::remove(target, rm_ec);
    ec.clear();
    std::filesystem::rename(tmp, target, ec);
  }

  if (ec) {
    // Best-effort cleanup.
    std::error_code rm_ec;
    std::filesystem::remove(tmp, rm_ec);
    return false;
  }

  return true;
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

static std::string ctime_safe(std::time_t t) {
#if defined(_WIN32)
  // MSVC deprecates ctime() in favor of the bounds-checked ctime_s().
  char buf[26] = {};
  if (ctime_s(buf, sizeof(buf), &t) != 0) return std::string();
  std::string s(buf);
#else
  // POSIX thread-safe variant.
  char buf[26] = {};
  if (ctime_r(&t, buf) == nullptr) return std::string();
  std::string s(buf);
#endif

  // ctime_s/ctime_r append a trailing newline.
  if (!s.empty() && s.back() == '\n') s.pop_back();
  return s;
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
    // Fallback for the extremely unlikely case localtime_s/localtime_r fails.
    // Keep logs/UI usable without relying on deprecated ctime().
    return ctime_safe(t);
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


namespace {

static bool parse_2dig(const std::string& s, size_t pos, int* out) {
  if (!out) return false;
  if (pos + 2 > s.size()) return false;
  const char a = s[pos];
  const char b = s[pos + 1];
  if (a < '0' || a > '9' || b < '0' || b > '9') return false;
  *out = (a - '0') * 10 + (b - '0');
  return true;
}

static bool parse_4dig(const std::string& s, size_t pos, int* out) {
  if (!out) return false;
  if (pos + 4 > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < 4; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v;
  return true;
}

static bool is_leap_year(int y) {
  // Gregorian leap year rules.
  if (y % 4 != 0) return false;
  if (y % 100 != 0) return true;
  return (y % 400 == 0);
}

static int days_in_month(int y, int m) {
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2) return mdays[1] + (is_leap_year(y) ? 1 : 0);
  return mdays[m - 1];
}

static bool valid_civil(int y, int m, int d) {
  if (m < 1 || m > 12) return false;
  const int dim = days_in_month(y, m);
  if (dim <= 0) return false;
  return d >= 1 && d <= dim;
}

// Convert a civil date to days since Unix epoch (1970-01-01).
//
// This uses Howard Hinnant's well-known, dependency-free civil date algorithm.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  // Shift March-based year to simplify leap handling.
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400); // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ?  -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
  // 719468 is the number of days from 0000-03-01 to 1970-01-01.
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

} // namespace

bool parse_iso8601_to_utc_millis(const std::string& ts, int64_t* out_utc_ms) {
  if (!out_utc_ms) return false;
  *out_utc_ms = 0;

  // Minimum length for: YYYY-MM-DDTHH:MM:SSZ
  if (ts.size() < 20) return false;

  int year = 0;
  int mon = 0;
  int day = 0;
  int hh = 0;
  int mm = 0;
  int ss = 0;

  if (!parse_4dig(ts, 0, &year) || ts[4] != '-') return false;
  if (!parse_2dig(ts, 5, &mon) || ts[7] != '-') return false;
  if (!parse_2dig(ts, 8, &day)) return false;

  const char t = ts[10];
  if (t != 'T' && t != 't') return false;

  if (!parse_2dig(ts, 11, &hh) || ts[13] != ':') return false;
  if (!parse_2dig(ts, 14, &mm) || ts[16] != ':') return false;
  if (!parse_2dig(ts, 17, &ss)) return false;

  if (!valid_civil(year, mon, day)) return false;
  if (hh < 0 || hh > 23) return false;
  if (mm < 0 || mm > 59) return false;
  if (ss < 0 || ss > 59) return false;

  size_t i = 19;
  int millis = 0;

  // Optional fractional seconds.
  if (i < ts.size() && ts[i] == '.') {
    ++i;
    if (i >= ts.size()) return false;
    if (ts[i] < '0' || ts[i] > '9') return false;

    int mult = 100;
    size_t nd = 0;
    while (i < ts.size()) {
      const char c = ts[i];
      if (c < '0' || c > '9') break;
      if (nd < 3) {
        millis += (c - '0') * mult;
        mult /= 10;
      }
      ++nd;
      ++i;
    }
  }

  if (i >= ts.size()) return false;

  // Time zone spec.
  int offset_seconds = 0;
  const char z = ts[i];
  if ((z == 'Z' || z == 'z') && i + 1 == ts.size()) {
    offset_seconds = 0;
    i += 1;
  } else if ((z == '+' || z == '-') && i + 6 == ts.size() && ts[i + 3] == ':') {
    int oh = 0;
    int om = 0;
    if (!parse_2dig(ts, i + 1, &oh)) return false;
    if (!parse_2dig(ts, i + 4, &om)) return false;
    if (oh < 0 || oh > 23) return false;
    if (om < 0 || om > 59) return false;
    offset_seconds = oh * 3600 + om * 60;
    if (z == '-') offset_seconds = -offset_seconds;
    i += 6;
  } else {
    return false;
  }

  if (i != ts.size()) return false;

  const int64_t days = days_from_civil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
  const int64_t local_seconds = days * 86400 + static_cast<int64_t>(hh) * 3600 + static_cast<int64_t>(mm) * 60 + static_cast<int64_t>(ss);

  // ISO-8601 numeric UTC offsets are defined such that:
  //   local_time = utc_time + offset
  // so:
  //   utc_time = local_time - offset
  const int64_t utc_seconds = local_seconds - static_cast<int64_t>(offset_seconds);

  *out_utc_ms = utc_seconds * 1000 + static_cast<int64_t>(millis);
  return true;
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

static void json_skip_ws(const std::string& s, size_t* i) {
  while (i && *i < s.size() && std::isspace(static_cast<unsigned char>(s[*i])) != 0) {
    ++(*i);
  }
}

static bool json_parse_hex4(const std::string& s, size_t pos, unsigned* out) {
  if (!out) return false;
  if (pos + 4 > s.size()) return false;
  unsigned v = 0;
  for (size_t k = 0; k < 4; ++k) {
    const char c = s[pos + k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(10 + (c - 'a'));
    else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(10 + (c - 'A'));
    else return false;
  }
  *out = v;
  return true;
}

static void json_append_utf8(unsigned codepoint, std::string* out) {
  if (!out) return;

  // JSON \u escapes are UTF-16 code units. Surrogate codepoints are not valid
  // scalar values in UTF-8; treat them as replacement characters.
  if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
    codepoint = 0xFFFDu; // U+FFFD replacement
  }

  if (codepoint <= 0x7Fu) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
    out->push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else if (codepoint <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
    out->push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
    out->push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  }
}

static bool json_parse_string(const std::string& s, size_t* i, std::string* out) {
  if (!i || *i >= s.size() || s[*i] != '"') return false;
  ++(*i);
  std::string r;
  r.reserve(64);

  while (*i < s.size()) {
    const char c = s[*i];
    ++(*i);

    if (c == '"') {
      if (out) *out = r;
      return true;
    }

    if (c == '\\') {
      if (*i >= s.size()) return false;
      const char e = s[*i];
      ++(*i);
      switch (e) {
        case '"': r.push_back('"'); break;
        case '\\': r.push_back('\\'); break;
        case '/': r.push_back('/'); break;
        case 'b': r.push_back('\b'); break;
        case 'f': r.push_back('\f'); break;
        case 'n': r.push_back('\n'); break;
        case 'r': r.push_back('\r'); break;
        case 't': r.push_back('\t'); break;
        case 'u': {
          // \uXXXX (UTF-16 code unit). Combine surrogate pairs when present.
          unsigned cp = 0;
          if (!json_parse_hex4(s, *i, &cp)) return false;
          *i += 4;

          // High surrogate?
          if (cp >= 0xD800u && cp <= 0xDBFFu) {
            // Look for a following low surrogate escape sequence: \uYYYY
            if ((*i + 6) <= s.size() && s[*i] == '\\' && s[*i + 1] == 'u') {
              unsigned low = 0;
              if (json_parse_hex4(s, *i + 2, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
                // Consume the second escape.
                *i += 6;
                const unsigned hi = cp;
                const unsigned codepoint = 0x10000u + ((hi - 0xD800u) << 10) + (low - 0xDC00u);
                json_append_utf8(codepoint, &r);
                break;
              }
            }
            // Invalid/missing low surrogate: replacement char.
            json_append_utf8(0xFFFDu, &r);
            break;
          }

          // Orphan low surrogate?
          if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            json_append_utf8(0xFFFDu, &r);
            break;
          }

          json_append_utf8(cp, &r);
          break;
        }
        default:
          // Unknown escape: keep the literal char.
          r.push_back(e);
          break;
      }
      continue;
    }

    r.push_back(c);
  }
  return false;
}

static bool json_find_value_pos_top_level(const std::string& s,
                                         const std::string& key,
                                         size_t* out_pos) {
  // Find a JSON object member matching `key` and return the position of its value.
  //
  // We intentionally avoid naive substring search for \"<key>\" because that can
  // match occurrences inside JSON string values.
  //
  // We also restrict matches to the *top-level* object (depth 1) so that nested
  // objects cannot shadow keys unexpectedly.
  int depth = 0;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];

    if (c == '"') {
      const size_t token_start = i;
      std::string tok;
      if (!json_parse_string(s, &i, &tok)) {
        // Malformed string: advance to avoid an infinite loop.
        i = token_start + 1;
        continue;
      }

      if (depth == 1 && tok == key) {
        size_t j = i;
        json_skip_ws(s, &j);
        if (j < s.size() && s[j] == ':') {
          ++j;
          json_skip_ws(s, &j);
          if (out_pos) *out_pos = j;
          return true;
        }
      }
      continue;
    }

    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth > 0) --depth;
    }

    ++i;
  }
  return false;
}

} // namespace

std::string json_find_string_value(const std::string& s, const std::string& key) {
  size_t pos = 0;
  if (!json_find_value_pos_top_level(s, key, &pos)) return {};
  if (pos >= s.size() || s[pos] != '"') return {};
  std::string out;
  if (!json_parse_string(s, &pos, &out)) return {};
  return out;
}

bool json_find_bool_value(const std::string& s, const std::string& key, bool default_value) {
  // Accepts:
  //   {"key":true} / {"key":false}
  //   {"key":"true"} / {"key":"false"}
  //   {"key":1} / {"key":0}
  size_t i = 0;
  if (!json_find_value_pos_top_level(s, key, &i)) return default_value;
  if (i >= s.size()) return default_value;

  auto is_delim = [](char c) {
    return c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c)) != 0;
  };

  if (s.compare(i, 4, "true") == 0 && (i + 4 == s.size() || is_delim(s[i + 4]))) return true;
  if (s.compare(i, 5, "false") == 0 && (i + 5 == s.size() || is_delim(s[i + 5]))) return false;
  if (s[i] == '1' && (i + 1 == s.size() || is_delim(s[i + 1]))) return true;
  if (s[i] == '0' && (i + 1 == s.size() || is_delim(s[i + 1]))) return false;

  if (s[i] == '"') {
    std::string v;
    size_t j = i;
    if (!json_parse_string(s, &j, &v)) return default_value;
    const std::string lv = to_lower(trim(v));
    if (lv == "true" || lv == "1" || lv == "yes" || lv == "y") return true;
    if (lv == "false" || lv == "0" || lv == "no" || lv == "n") return false;
  }

  return default_value;
}

int json_find_int_value(const std::string& s, const std::string& key, int default_value) {
  // Accepts:
  //   {"key":123}
  //   {"key":"123"}
  //   {"key":-5}
  size_t i = 0;
  if (!json_find_value_pos_top_level(s, key, &i)) return default_value;
  if (i >= s.size()) return default_value;

  std::string num;
  if (s[i] == '"') {
    size_t j = i;
    if (!json_parse_string(s, &j, &num)) return default_value;
    num = trim(num);
  } else {
    size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) ++j;
    if (j <= i) return default_value;
    num = s.substr(i, j - i);
    num = trim(num);
  }

  try {
    return qeeg::to_int(num);
  } catch (...) {
    return default_value;
  }
}

bool normalize_rel_path_safe(const std::string& raw, std::string* out_norm) {
  if (!out_norm) return false;
  *out_norm = std::string();

  std::string s = trim(raw);
  if (s.empty()) return false;

  // Embedded NUL bytes are not valid in filesystem paths.
  if (s.find('\0') != std::string::npos) return false;

  // Allow both slash styles; normalize to URL/posix-style slashes.
  std::replace(s.begin(), s.end(), '\\', '/');

  // Reject Windows drive prefixes like "C:" early (even if followed by '/').
  if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) != 0 && s[1] == ':') {
    return false;
  }

  // Strip any leading slashes so "/abs" cannot become an absolute path when joined.
  while (!s.empty() && (s.front() == '/' || s.front() == '\\')) s.erase(s.begin());

  // Strip trailing slashes so directory paths like "outdir/" are accepted.
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();

  std::filesystem::path p;
  try {
    p = std::filesystem::u8path(s);
  } catch (...) {
    return false;
  }

  if (p.empty()) return false;
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;

  for (const auto& part : p) {
    const std::string comp = part.u8string();
    if (comp.empty()) return false;
    if (comp == "..") return false;
  }

  // Normalize "." segments lexically (no filesystem access).
  const std::filesystem::path n = p.lexically_normal();
  if (n.empty()) return false;

  const std::string norm = n.generic_u8string();
  if (norm.empty() || norm == ".") return false;
  if (norm.find('\0') != std::string::npos) return false;

  // Defense in depth: re-parse and re-check the normalized path.
  std::filesystem::path pn;
  try {
    pn = std::filesystem::u8path(norm);
  } catch (...) {
    return false;
  }
  if (pn.empty()) return false;
  if (pn.is_absolute() || pn.has_root_name() || pn.has_root_directory()) return false;
  for (const auto& part : pn) {
    const std::string comp = part.u8string();
    if (comp.empty()) return false;
    if (comp == "..") return false;
  }

  *out_norm = norm;
  return true;
}

std::string url_encode_path(const std::string& path) {
  // RFC 3986 percent-encoding (best-effort) for URL *paths*.
  //
  // We keep alphanumerics and the unreserved set "-._~" plus '/'.
  // Everything else (including spaces, '#', '?', '%' etc.) is percent-encoded.
  //
  // On Windows, file paths commonly use '\\' separators. When such paths are
  // accidentally embedded into URLs, browsers will treat '\\' as a literal
  // character (or we would encode it as %5C), which breaks navigation.
  // Normalize '\\' to '/' to keep links working cross-platform.
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (unsigned char uc : path) {
    char c = static_cast<char>(uc);
    if (c == '\\') c = '/';
    const bool unreserved =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved || c == '/') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[(uc >> 4) & 0xF]);
      out.push_back(kHex[uc & 0xF]);
    }
  }
  return out;
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
