#include "qeeg/ui_dashboard.hpp"

#include "qeeg/run_meta.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {

using qeeg::ends_with;
using qeeg::ensure_directory;
using qeeg::file_exists;
using qeeg::HttpRangeResult;
using qeeg::json_escape;
using qeeg::now_string_local;
using qeeg::parse_http_byte_range;
using qeeg::random_hex_token;
using qeeg::split_commandline_args;
using qeeg::starts_with;
using qeeg::to_lower;
using qeeg::trim;

struct Args {
  std::string root;
  std::string bin_dir;
  std::string host{"127.0.0.1"};
  int port{8765};
  int max_parallel{0}; // 0 = unlimited
  std::string api_token; // optional override (otherwise random)
  bool embed_help{true};
  bool scan_bin_dir{true};
  bool scan_run_meta{true};
  bool open_after{false};
  bool no_generate_ui{false};
};

static void print_help() {
  std::cout
    << "qeeg_ui_server_cli\n\n"
    << "Serve the QEEG Tools dashboard locally and expose a small local-only HTTP API\n"
    << "to run qeeg_*_cli executables from the browser UI.\n\n"
    << "Usage:\n"
    << "  qeeg_ui_server_cli --root <dir> --bin-dir <build/bin> [--host 127.0.0.1] [--port 8765] [--max-parallel N] [--open]\n\n"
    << "Options:\n"
    << "  --root DIR          Root directory to serve files from (required).\n"
    << "  --bin-dir DIR       Directory containing qeeg_*_cli executables (required).\n"
    << "  --host HOST         Bind address (default: 127.0.0.1).\n"
    << "  --port N            Port to listen on (default: 8765).\n"
    << "  --max-parallel N    Max concurrent jobs; extra runs are queued (default: 0 = unlimited).\n"
    << "  --api-token TOKEN   Override the random API token (advanced; useful for curl).\n"
    << "  --no-help           Generate UI without embedding --help outputs.\n"
    << "  --no-bin-scan       Do not scan --bin-dir for additional qeeg_*_cli tools.\n"
    << "  --no-scan           Do not scan --root for *_run_meta.json outputs.\n"
    << "  --no-generate-ui    Do not (re)generate <root>/qeeg_ui.html on startup.\n"
    << "  --open              Attempt to open the served dashboard URL in your browser.\n"
    << "  -h, --help          Show this help.\n\n"
    << "Security:\n"
    << "  - /api/* endpoints are loopback-only (127.0.0.1).\n"
    << "  - All /api endpoints except /api/status require X-QEEG-Token (printed on startup).\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--root" && i + 1 < argc) {
      a.root = argv[++i];
    } else if (arg == "--bin-dir" && i + 1 < argc) {
      a.bin_dir = argv[++i];
    } else if (arg == "--host" && i + 1 < argc) {
      a.host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      a.port = qeeg::to_int(argv[++i]);
    } else if (arg == "--max-parallel" && i + 1 < argc) {
      a.max_parallel = qeeg::to_int(argv[++i]);
    } else if (arg == "--api-token" && i + 1 < argc) {
      a.api_token = argv[++i];
    } else if (arg == "--no-help") {
      a.embed_help = false;
    } else if (arg == "--no-bin-scan") {
      a.scan_bin_dir = false;
    } else if (arg == "--no-scan") {
      a.scan_run_meta = false;
    } else if (arg == "--no-generate-ui") {
      a.no_generate_ui = true;
    } else if (arg == "--open") {
      a.open_after = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  return a;
}

static void try_open_browser_url(const std::string& url) {
#if defined(_WIN32)
  std::string cmd = "cmd /c start \"\" \"" + url + "\"";
  std::system(cmd.c_str());
#elif defined(__APPLE__)
  std::string cmd = "open \"" + url + "\"";
  std::system(cmd.c_str());
#else
  std::string cmd = "xdg-open \"" + url + "\"";
  std::system(cmd.c_str());
#endif
}

static std::string now_compact_local() {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmv, "%Y%m%d_%H%M%S");
  return oss.str();
}

static bool looks_like_qeeg_cli(const std::string& tool) {
  std::string base = tool;
  if (qeeg::ends_with(base, ".exe")) {
    base = base.substr(0, base.size() - 4);
  }
  if (!qeeg::starts_with(base, "qeeg_")) return false;
  if (!qeeg::ends_with(base, "_cli")) return false;
  if (qeeg::starts_with(base, "qeeg_test_")) return false;
  return true;
}

static std::filesystem::path resolve_exe_path(const std::filesystem::path& bin_dir,
                                              const std::string& tool) {
  std::filesystem::path p = bin_dir / tool;
  if (std::filesystem::exists(p)) return p;
  // Try adding .exe
  std::filesystem::path pe = p;
  pe += ".exe";
  if (std::filesystem::exists(pe)) return pe;
  return {};
}

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

static std::string trim_ws(const std::string& s);

static int hexval(char c) {
  if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
  if (c >= 'a' && c <= 'f') return 10 + static_cast<int>(c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + static_cast<int>(c - 'A');
  return -1;
}

static std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < s.size()) {
      const int h1 = hexval(s[i + 1]);
      const int h2 = hexval(s[i + 2]);
      if (h1 >= 0 && h2 >= 0) {
        out.push_back(static_cast<char>((h1 << 4) | h2));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

static std::string url_decode_path(const std::string& s) {
  // Percent-decode a URL *path* component.
  //
  // Important: unlike application/x-www-form-urlencoded query strings, the
  // path portion of a URL does NOT treat '+' as a space.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      const int h1 = hexval(s[i + 1]);
      const int h2 = hexval(s[i + 2]);
      if (h1 >= 0 && h2 >= 0) {
        out.push_back(static_cast<char>((h1 << 4) | h2));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

static std::map<std::string, std::string> parse_query_params(const std::string& qs) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < qs.size()) {
    size_t amp = qs.find('&', i);
    if (amp == std::string::npos) amp = qs.size();
    const std::string kv = qs.substr(i, amp - i);
    size_t eq = kv.find('=');
    std::string k = (eq == std::string::npos) ? kv : kv.substr(0, eq);
    std::string v = (eq == std::string::npos) ? std::string() : kv.substr(eq + 1);
    k = to_lower(trim_ws(url_decode(k)));
    v = url_decode(v);
    if (!k.empty()) out[k] = v;
    i = amp + 1;
  }
  return out;
}

static uintmax_t parse_u64(const std::string& s, uintmax_t fallback = 0) {
  uintmax_t v = 0;
  bool any = false;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return fallback;
    any = true;
    v = v * 10 + static_cast<uintmax_t>(c - '0');
  }
  return any ? v : fallback;
}

static std::string trim_ws(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
  return s.substr(b, e - b);
}

static bool parse_http_request(const std::string& raw, HttpRequest* out) {
  if (!out) return false;
  out->headers.clear();
  out->body.clear();

  size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;

  std::istringstream iss(raw.substr(0, header_end));
  std::string line;
  if (!std::getline(iss, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();

  {
    std::istringstream first(line);
    first >> out->method >> out->path;
    if (out->method.empty() || out->path.empty()) return false;
  }

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    const size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string key = to_lower(trim_ws(line.substr(0, c)));
    std::string val = trim_ws(line.substr(c + 1));
    if (!key.empty()) out->headers[key] = val;
  }

  // Body (if any) is everything after \r\n\r\n
  out->body = raw.substr(header_end + 4);
  return true;
}

static std::string http_status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "";
  }
}

static std::string content_type_for_path(const std::filesystem::path& p) {
  const std::string ext = to_lower(p.extension().u8string());
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".csv" || ext == ".tsv" || ext == ".txt") return "text/plain; charset=utf-8";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".webp") return "image/webp";
  if (ext == ".bmp") return "image/bmp";
  return "application/octet-stream";
}

// Best-effort: convert filesystem::file_time_type to a time_t (seconds since epoch).
//
// Note: std::filesystem::file_time_type may not use system_clock, so we do a
// relative conversion via now() values. This is a common portability pattern.
static bool file_mtime_time_t(const std::filesystem::path& p, std::time_t* out) {
  if (!out) return false;
  std::error_code ec;
  const auto ft = std::filesystem::last_write_time(p, ec);
  if (ec) return false;

  using namespace std::chrono;
  const auto sctp = time_point_cast<system_clock::duration>(
      ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
  *out = system_clock::to_time_t(sctp);
  return true;
}

static std::string format_http_date_gmt(std::time_t t) {
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tmv, "%a, %d %b %Y %H:%M:%S GMT");
  return oss.str();
}

static int month_from_http_abbrev(const std::string& m) {
  // Month is case-sensitive in IMF-fixdate, but accept any case for robustness.
  const std::string mm = to_lower(m);
  if (mm == "jan") return 1;
  if (mm == "feb") return 2;
  if (mm == "mar") return 3;
  if (mm == "apr") return 4;
  if (mm == "may") return 5;
  if (mm == "jun") return 6;
  if (mm == "jul") return 7;
  if (mm == "aug") return 8;
  if (mm == "sep") return 9;
  if (mm == "oct") return 10;
  if (mm == "nov") return 11;
  if (mm == "dec") return 12;
  return 0;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  // Howard Hinnant's civil calendar algorithm (public domain).
  // Returns days relative to 1970-01-01.
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400); // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

static bool parse_http_date_gmt(const std::string& s, std::time_t* out) {
  // Parse IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 9110).
  if (!out) return false;
  std::string v = trim(s);
  if (v.empty()) return false;

  // Strip day-of-week prefix if present.
  const size_t comma = v.find(',');
  if (comma != std::string::npos) {
    v = trim(v.substr(comma + 1));
  }

  // Split into: DD Mon YYYY HH:MM:SS GMT
  std::istringstream iss(v);
  std::string dd_s, mon_s, yyyy_s, time_s, tz_s;
  if (!(iss >> dd_s >> mon_s >> yyyy_s >> time_s >> tz_s)) return false;
  if (to_lower(tz_s) != "gmt") return false;

  int dd = 0;
  int yyyy = 0;
  try {
    dd = qeeg::to_int(dd_s);
    yyyy = qeeg::to_int(yyyy_s);
  } catch (...) {
    return false;
  }
  const int mon = month_from_http_abbrev(mon_s);
  if (mon <= 0) return false;

  int hh = 0, mm = 0, ss = 0;
  {
    // HH:MM:SS
    const size_t p1 = time_s.find(':');
    const size_t p2 = (p1 == std::string::npos) ? std::string::npos : time_s.find(':', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return false;
    try {
      hh = qeeg::to_int(time_s.substr(0, p1));
      mm = qeeg::to_int(time_s.substr(p1 + 1, p2 - (p1 + 1)));
      ss = qeeg::to_int(time_s.substr(p2 + 1));
    } catch (...) {
      return false;
    }
  }

  if (yyyy < 1970 || dd < 1 || dd > 31) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;

  const int64_t days = days_from_civil(yyyy, static_cast<unsigned>(mon), static_cast<unsigned>(dd));
  const int64_t secs = days * 86400 + static_cast<int64_t>(hh) * 3600 + static_cast<int64_t>(mm) * 60 + static_cast<int64_t>(ss);
  if (secs < 0) return false;
  *out = static_cast<std::time_t>(secs);
  return true;
}

static std::string strip_weak_etag(std::string s) {
  s = trim(s);
  if (starts_with(s, "W/") || starts_with(s, "w/")) {
    s = trim(s.substr(2));
  }
  return s;
}

static bool etag_matches(const std::string& a, const std::string& b) {
  return strip_weak_etag(a) == strip_weak_etag(b);
}

static bool if_none_match_allows_304(const std::string& if_none_match_value,
                                    const std::string& etag_value) {
  // If-None-Match can be "*" or a comma-separated list of ETags.
  const std::string v = trim(if_none_match_value);
  if (v.empty()) return false;
  if (v == "*") return true;

  std::istringstream iss(v);
  std::string tok;
  while (std::getline(iss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    if (etag_matches(tok, etag_value)) return true;
  }
  return false;
}

static std::string make_weak_etag(std::time_t mtime, uintmax_t size) {
  // Weak ETag based on modification time (seconds) and size.
  // This is dependency-free and sufficient for local caching/revalidation.
  return std::string("W/\"") + std::to_string(static_cast<long long>(mtime)) + "-" + std::to_string(size) + "\"";
}

static bool if_range_allows_range(const std::string& if_range_value,
                                 const std::string& etag_value,
                                 std::time_t mtime) {
  const std::string v = trim(if_range_value);
  if (v.empty()) return true;

  // If-Range may be an ETag (quoted) or an HTTP-date.
  if (!v.empty() && (v[0] == '"' || starts_with(v, "W/") || starts_with(v, "w/"))) {
    return etag_matches(v, etag_value);
  }
  std::time_t t = 0;
  if (!parse_http_date_gmt(v, &t)) {
    // Unknown format: be conservative and ignore Range.
    return false;
  }
  // Allow Range only if resource has not been modified since the provided date.
  return mtime <= t;
}


// Security headers for the built-in dashboard HTML. We keep 'unsafe-inline' for
// script/style because the dashboard HTML intentionally uses inline handlers and
// a single self-contained <script> block.
static const char* kDashboardCsp =
    "default-src 'self' data: blob:; "
    "img-src 'self' data: blob:; "
    "style-src 'self' 'unsafe-inline'; "
    "script-src 'self' 'unsafe-inline'; "
    "connect-src 'self'; "
    "base-uri 'self'; "
    "object-src 'none'; "
    "frame-ancestors 'none'";


static std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

static std::string url_escape_path(const std::string& s) {
  // Minimal URL percent-encoding for paths.
  //
  // Notes:
  // - We keep '/' so the browser navigates directories correctly.
  // - We normalize Windows path separators ('\') to URL separators ('/') to
  //   avoid broken links when native paths are embedded into href/src.
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c == '\\') c = '/';

    const bool ok =
        (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    if (ok) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}


static std::string format_local_time(std::filesystem::file_time_type tp) {
  // Convert std::filesystem::file_time_type -> local time string.
  // This is a best-effort conversion that works on common libstdc++/libc++
  // implementations in C++17.
  try {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        tp - std::filesystem::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sctp);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return oss.str();
  } catch (...) {
    return std::string();
  }
}

static int64_t file_time_to_unix_seconds(std::filesystem::file_time_type tp) {
  // Best-effort conversion of std::filesystem::file_time_type to Unix epoch seconds.
  // This uses the common C++17 technique of translating the file clock to system_clock.
  try {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        tp - std::filesystem::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sctp);
    return static_cast<int64_t>(tt);
  } catch (...) {
    return 0;
  }
}



// -----------------------------
// Minimal ZIP writer (store-only)
// -----------------------------
//
// The UI server sometimes needs to bundle a run directory for download from
// the browser. To keep the project dependency-free, we emit a classic ZIP
// archive using the "store" method (no compression) following PKWARE's
// APPNOTE format.
//
// Limitations (by design):
//   - No ZIP64 (individual files and archives must fit into 32-bit size fields)
//   - No compression (method 0)
//   - No encryption, no extra fields
//
// This is sufficient for typical run artifacts (CSV/JSON/SVG/logs).

struct ZipCdEntry {
  std::string name;
  uint32_t crc{0};
  uint32_t comp_size{0};
  uint32_t uncomp_size{0};
  uint16_t dos_time{0};
  uint16_t dos_date{0};
  uint32_t local_offset{0};
};

static uint32_t crc32_ieee_update(uint32_t crc, const unsigned char* data, size_t n) {
  static uint32_t table[256];
  static bool inited = false;
  if (!inited) {
    inited = true;
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        if (c & 1u) c = 0xEDB88320u ^ (c >> 1);
        else c >>= 1;
      }
      table[i] = c;
    }
  }
  uint32_t c = crc;
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c;
}

static uint32_t crc32_ieee(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  uint32_t c = 0xFFFFFFFFu;
  c = crc32_ieee_update(c, p, s.size());
  return c ^ 0xFFFFFFFFu;
}

static void zip_append_u16(std::string* out, uint16_t v) {
  if (!out) return;
  out->push_back(static_cast<char>(v & 0xFFu));
  out->push_back(static_cast<char>((v >> 8) & 0xFFu));
}

static void zip_append_u32(std::string* out, uint32_t v) {
  if (!out) return;
  out->push_back(static_cast<char>(v & 0xFFu));
  out->push_back(static_cast<char>((v >> 8) & 0xFFu));
  out->push_back(static_cast<char>((v >> 16) & 0xFFu));
  out->push_back(static_cast<char>((v >> 24) & 0xFFu));
}

static void zip_dos_datetime_from_tm(const std::tm& tmv, uint16_t* out_time, uint16_t* out_date) {
  if (out_time) *out_time = 0;
  if (out_date) *out_date = 0;

  int year = tmv.tm_year + 1900;
  int mon = tmv.tm_mon + 1;
  int day = tmv.tm_mday;
  int hour = tmv.tm_hour;
  int min = tmv.tm_min;
  int sec = tmv.tm_sec;

  if (year < 1980) year = 1980;
  if (year > 2107) year = 2107;
  if (mon < 1) mon = 1;
  if (mon > 12) mon = 12;
  if (day < 1) day = 1;
  if (day > 31) day = 31;
  if (hour < 0) hour = 0;
  if (hour > 23) hour = 23;
  if (min < 0) min = 0;
  if (min > 59) min = 59;
  if (sec < 0) sec = 0;
  if (sec > 59) sec = 59;

  const uint16_t dt = static_cast<uint16_t>(((hour & 31) << 11) | ((min & 63) << 5) | ((sec / 2) & 31));
  const uint16_t dd = static_cast<uint16_t>(((year - 1980) << 9) | ((mon & 15) << 5) | (day & 31));
  if (out_time) *out_time = dt;
  if (out_date) *out_date = dd;
}

static void zip_dos_datetime(std::filesystem::file_time_type tp, uint16_t* out_time, uint16_t* out_date) {
  try {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        tp - std::filesystem::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sctp);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    zip_dos_datetime_from_tm(tmv, out_time, out_date);
  } catch (...) {
    if (out_time) *out_time = 0;
    if (out_date) *out_date = 0;
  }
}

static void zip_dos_datetime_now(uint16_t* out_time, uint16_t* out_date) {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  zip_dos_datetime_from_tm(tmv, out_time, out_date);
}

static std::string zip_sanitize_component(std::string s) {
  // Keep filenames reasonably portable across unzip tools by keeping mostly
  // ASCII and replacing unusual characters.
  if (s.empty()) return "run";
  for (char& c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    const bool ok = (std::isalnum(uc) != 0) || c == '_' || c == '-' || c == '.';
    if (!ok) c = '_';
  }
  // Avoid leading dots (hidden files) in the synthetic folder name.
  while (!s.empty() && s.front() == '.') s.erase(s.begin());
  if (s.empty()) return "run";
  return s;
}

static std::string zip_normalize_relpath(std::string s) {
  s = trim(s);
  std::replace(s.begin(), s.end(), '\\', '/');
  while (!s.empty() && s.front() == '/') s.erase(s.begin());
  while (!s.empty() && s.back() == '/') s.pop_back();
  if (s.empty()) return {};

  // Reject obvious absolute / drive-like paths.
  if (s.find(':') != std::string::npos) return {};

  // Split and validate path segments.
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('/', i);
    if (j == std::string::npos) j = s.size();
    std::string part = s.substr(i, j - i);
    if (part.empty() || part == ".") {
      // skip
    } else if (part == "..") {
      return {};
    } else {
      parts.push_back(part);
    }
    i = j + 1;
  }
  if (parts.empty()) return {};

  std::string out;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k) out.push_back('/');
    out += parts[k];
  }
  return out;
}

static bool read_file_binary_bounded(const std::filesystem::path& p,
                                    uintmax_t max_bytes,
                                    std::string* out) {
  if (!out) return false;
  out->clear();

  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) return false;
  const uintmax_t sz = std::filesystem::file_size(p, ec);
  if (ec) return false;
  if (sz > max_bytes) return false;

  std::ifstream f(p, std::ios::binary);
  if (!f) return false;

  out->resize(static_cast<size_t>(sz));
  if (sz > 0) {
    f.read(&(*out)[0], static_cast<std::streamsize>(sz));
    if (!f) {
      out->clear();
      return false;
    }
  }
  return true;
}

static bool zip_add_store_entry(std::string* zip,
                               std::vector<ZipCdEntry>* cd,
                               const std::string& name,
                               const std::string& data,
                               uint16_t dos_time,
                               uint16_t dos_date,
                               std::string* out_error) {
  if (!zip || !cd) return false;

  if (name.empty()) {
    if (out_error) *out_error = "zip entry name is empty";
    return false;
  }
  if (name.size() > 0xFFFFu) {
    if (out_error) *out_error = "zip entry name too long";
    return false;
  }
  if (data.size() > 0xFFFFFFFFu) {
    if (out_error) *out_error = "zip entry too large (ZIP64 not supported)";
    return false;
  }
  if (zip->size() > 0xFFFFFFFFu) {
    if (out_error) *out_error = "zip archive too large (ZIP64 not supported)";
    return false;
  }

  const uint32_t local_off = static_cast<uint32_t>(zip->size());
  ZipCdEntry e;
  e.name = name;
  e.crc = crc32_ieee(data);
  e.comp_size = static_cast<uint32_t>(data.size());
  e.uncomp_size = static_cast<uint32_t>(data.size());
  e.dos_time = dos_time;
  e.dos_date = dos_date;
  e.local_offset = local_off;

  constexpr uint16_t kVersionNeeded = 20; // 2.0
  constexpr uint16_t kFlagsUtf8 = 0x0800; // UTF-8 filenames
  constexpr uint16_t kMethodStore = 0;

  // Local file header.
  zip_append_u32(zip, 0x04034b50u);
  zip_append_u16(zip, kVersionNeeded);
  zip_append_u16(zip, kFlagsUtf8);
  zip_append_u16(zip, kMethodStore);
  zip_append_u16(zip, e.dos_time);
  zip_append_u16(zip, e.dos_date);
  zip_append_u32(zip, e.crc);
  zip_append_u32(zip, e.comp_size);
  zip_append_u32(zip, e.uncomp_size);
  zip_append_u16(zip, static_cast<uint16_t>(e.name.size()));
  zip_append_u16(zip, 0); // extra length
  zip->append(e.name);
  zip->append(data);

  cd->push_back(std::move(e));
  return true;
}

static bool zip_finalize_store(std::string* zip,
                              const std::vector<ZipCdEntry>& cd,
                              std::string* out_error) {
  if (!zip) return false;

  if (cd.size() > 0xFFFFu) {
    if (out_error) *out_error = "too many zip entries";
    return false;
  }

  if (zip->size() > 0xFFFFFFFFu) {
    if (out_error) *out_error = "zip archive too large";
    return false;
  }

  constexpr uint16_t kVersionNeeded = 20;
  constexpr uint16_t kVersionMadeBy = 20;
  constexpr uint16_t kFlagsUtf8 = 0x0800;
  constexpr uint16_t kMethodStore = 0;

  const uint32_t cd_start = static_cast<uint32_t>(zip->size());

  // Central directory entries.
  for (const auto& e : cd) {
    if (e.name.size() > 0xFFFFu) {
      if (out_error) *out_error = "zip entry name too long";
      return false;
    }
    zip_append_u32(zip, 0x02014b50u);
    zip_append_u16(zip, kVersionMadeBy);
    zip_append_u16(zip, kVersionNeeded);
    zip_append_u16(zip, kFlagsUtf8);
    zip_append_u16(zip, kMethodStore);
    zip_append_u16(zip, e.dos_time);
    zip_append_u16(zip, e.dos_date);
    zip_append_u32(zip, e.crc);
    zip_append_u32(zip, e.comp_size);
    zip_append_u32(zip, e.uncomp_size);
    zip_append_u16(zip, static_cast<uint16_t>(e.name.size()));
    zip_append_u16(zip, 0); // extra
    zip_append_u16(zip, 0); // comment
    zip_append_u16(zip, 0); // disk start
    zip_append_u16(zip, 0); // internal attrs
    zip_append_u32(zip, 0); // external attrs
    zip_append_u32(zip, e.local_offset);
    zip->append(e.name);
  }

  const uint32_t cd_end = static_cast<uint32_t>(zip->size());
  const uint32_t cd_size = cd_end - cd_start;

  // End of central directory.
  zip_append_u32(zip, 0x06054b50u);
  zip_append_u16(zip, 0); // disk
  zip_append_u16(zip, 0); // cd disk
  zip_append_u16(zip, static_cast<uint16_t>(cd.size()));
  zip_append_u16(zip, static_cast<uint16_t>(cd.size()));
  zip_append_u32(zip, cd_size);
  zip_append_u32(zip, cd_start);
  zip_append_u16(zip, 0); // comment len

  return true;
}
static std::filesystem::path canonicalize_best_effort(const std::filesystem::path& p, bool is_dashboard = false) {
  std::error_code ec;
  std::filesystem::path c = std::filesystem::canonical(p, ec);
  if (!ec) return c;
  ec.clear();
  c = std::filesystem::weakly_canonical(p, ec);
  if (!ec) return c;
  ec.clear();
  c = std::filesystem::absolute(p, ec);
  if (!ec) return c;
  return p;
}

static bool path_is_within_root(const std::filesystem::path& root_canon,
                                const std::filesystem::path& candidate) {
  const std::filesystem::path c = canonicalize_best_effort(candidate);
  auto rit = root_canon.begin();
  auto cit = c.begin();
  for (; rit != root_canon.end() && cit != c.end(); ++rit, ++cit) {
    std::string r = rit->u8string();
    std::string v = cit->u8string();
#ifdef _WIN32
    r = to_lower(r);
    v = to_lower(v);
#endif
    if (r != v) return false;
  }
  return rit == root_canon.end();
}

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w;
  w.resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}
#endif

static std::string json_find_string_value(const std::string& s, const std::string& key) {
  // Tiny JSON string extractor (only for {"key":"value"}-style fields).
  const std::string needle = "\"" + key + "\"";
  const size_t pos = s.find(needle);
  if (pos == std::string::npos) return {};
  size_t i = s.find(':', pos + needle.size());
  if (i == std::string::npos) return {};
  ++i;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
  if (i >= s.size() || s[i] != '"') return {};
  ++i;
  std::string out;
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') break;
    if (c == '\\' && i < s.size()) {
      const char e = s[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(e); break;
      }
      continue;
    }
    out.push_back(c);
  }
  return out;
}

static bool json_find_bool_value(const std::string& s, const std::string& key, bool default_value) {
  // Tiny JSON boolean extractor.
  // Accepts:
  //   {"key":true} / {"key":false}
  //   {"key":"true"} / {"key":"false"}
  //   {"key":1} / {"key":0}
  const std::string needle = "\"" + key + "\"";
  const size_t pos = s.find(needle);
  if (pos == std::string::npos) return default_value;
  size_t i = s.find(':', pos + needle.size());
  if (i == std::string::npos) return default_value;
  ++i;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
  if (i >= s.size()) return default_value;
  if (s.compare(i, 4, "true") == 0) return true;
  if (s.compare(i, 5, "false") == 0) return false;
  if (s[i] == '1') return true;
  if (s[i] == '0') return false;
  // Handle quoted strings.
  if (s[i] == '"') {
    const std::string v = json_find_string_value(s, key);
    const std::string lv = to_lower(trim(v));
    if (lv == "true" || lv == "1" || lv == "yes" || lv == "y") return true;
    if (lv == "false" || lv == "0" || lv == "no" || lv == "n") return false;
  }
  return default_value;
}


static int json_find_int_value(const std::string& s, const std::string& key, int default_value) {
  // Tiny JSON integer extractor.
  // Accepts:
  //   {"key":123}
  //   {"key":"123"}
  //   {"key":-5}
  const std::string needle = "\"" + key + "\"";
  const size_t pos = s.find(needle);
  if (pos == std::string::npos) return default_value;
  size_t i = s.find(':', pos + needle.size());
  if (i == std::string::npos) return default_value;
  ++i;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
  if (i >= s.size()) return default_value;

  bool quoted = false;
  if (s[i] == '"') {
    quoted = true;
    ++i;
  }

  size_t j = i;
  if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) ++j;

  if (j <= i) return default_value;
  std::string num = s.substr(i, j - i);
  num = trim(num);

  try {
    return qeeg::to_int(num);
  } catch (...) {
    return default_value;
  }
}

static bool glob_match_ci(const std::string& pattern, const std::string& text) {
  // Case-insensitive glob match supporting '*' and '?'.
  // A minimal helper for workspace find; intended for typical ASCII paths.
  const char* p = pattern.c_str();
  const char* t = text.c_str();
  const char* star = nullptr;
  const char* star_text = nullptr;

  auto eq_ci = [](char a, char b) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) ==
           static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
  };

  while (*t) {
    if (*p == '?' || (*p && eq_ci(*p, *t))) {
      ++p;
      ++t;
      continue;
    }
    if (*p == '*') {
      star = p;
      ++p;
      star_text = t;
      continue;
    }
    if (star) {
      p = star + 1;
      ++star_text;
      t = star_text;
      continue;
    }
    return false;
  }

  while (*p == '*') ++p;
  return *p == '\0';
}

static std::string find_flag_value(const std::vector<std::string>& toks, const std::string& flag) {
  const std::string eq = flag + "=";
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i] == flag) {
      if (i + 1 < toks.size()) return toks[i + 1];
      return {};
    }
    if (starts_with(toks[i], eq)) {
      return toks[i].substr(eq.size());
    }
  }
  return {};
}

static std::string infer_input_path_from_args(const std::string& args) {
  // Best-effort extraction for common input flags used by qeeg_*_cli tools.
  // This is used only to populate ui_server_run_meta.json for nicer UI linking.
  const std::vector<std::string> toks = split_commandline_args(args);
  const std::vector<std::string> flags = {
      "--input",       // most tools
      "--bandpowers",  // qeeg_bandratios_cli
      "--dataset",     // qeeg_bids_scan_cli
      "--bids-root",   // export_derivatives_cli
      "--bids-file",   // export_derivatives_cli alt
  };
  for (const auto& f : flags) {
    const std::string v = find_flag_value(toks, f);
    if (!v.empty()) return v;
  }
  return {};
}

static std::vector<std::string> scan_run_dir_outputs(const std::filesystem::path& run_dir,
                                                     const std::filesystem::path& exclude,
                                                     size_t max_files) {
  std::vector<std::string> out;
  if (max_files == 0) max_files = 2000;

  std::error_code ec;
  if (!std::filesystem::exists(run_dir, ec) || !std::filesystem::is_directory(run_dir, ec)) return out;

  for (auto it = std::filesystem::recursive_directory_iterator(run_dir, ec);
       it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 6) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    const std::filesystem::path p = it->path();
    if (!exclude.empty() && p == exclude) continue;

    std::filesystem::path rel = std::filesystem::relative(p, run_dir, ec);
    if (ec || rel.empty()) {
      ec.clear();
      rel = p.filename();
    }
    const std::string s = rel.generic_u8string();
    if (s.empty()) continue;
    out.push_back(s);
    if (out.size() >= max_files) break;
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

struct RunJob {
  int id{0};
  std::string tool;
  std::string args;
  std::string run_dir_rel;
  std::string log_rel;
  std::string meta_rel;
  std::string input_path;
  std::string started;
  std::string status{"running"};
  int exit_code{0};
#ifdef _WIN32
  DWORD pid{0};
  HANDLE process_handle{nullptr};
#else
  pid_t pid{-1};
#endif
};

#ifdef _WIN32

static bool winsock_init_once() {
  static bool inited = false;
  static bool ok = false;
  if (inited) return ok;
  inited = true;
  WSADATA wsa;
  const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
  ok = (rc == 0);
  return ok;
}

static void socket_close(SOCKET s) {
  if (s != INVALID_SOCKET) closesocket(s);
}

#else

static void socket_close(int s) {
  if (s >= 0) close(s);
}

#endif

class UiServer {
 public:
  UiServer(std::filesystem::path root, std::filesystem::path bin_dir)
    : root_(std::move(root)), bin_dir_(std::move(bin_dir)) {
    root_canon_ = canonicalize_best_effort(root_);
  }

  void set_index_html(std::filesystem::path p) { index_html_ = std::move(p); }
  void set_host(std::string h) { host_ = std::move(h); }
  void set_port(int p) { port_ = p; }
  void set_api_token(std::string t) { api_token_ = std::move(t); }
  void set_max_parallel(int n) { max_parallel_ = (n < 0) ? 0 : n; }

  void run() {
#ifdef _WIN32
    if (!winsock_init_once()) {
      throw std::runtime_error("WSAStartup failed");
    }
#endif

    const std::string host = host_;
    const int port = port_;

    // Create socket.
#ifdef _WIN32
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) throw std::runtime_error("socket() failed");
#else
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) throw std::runtime_error("socket() failed");
#endif

    // Reuse address.
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      socket_close(srv);
      throw std::runtime_error("Invalid host address: " + host);
    }

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      socket_close(srv);
      throw std::runtime_error("bind() failed (is the port in use?)");
    }
    if (listen(srv, 16) != 0) {
      socket_close(srv);
      throw std::runtime_error("listen() failed");
    }

    std::cout << "Serving: http://" << host << ":" << port << "/\n";
    std::cout << "Root: " << root_.u8string() << "\n";
    std::cout << "Bin:  " << bin_dir_.u8string() << "\n";

    for (;;) {
      bool is_loopback = false;
#ifdef _WIN32
      sockaddr_in peer{};
      int peer_len = sizeof(peer);
      SOCKET c = accept(srv, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (c == INVALID_SOCKET) continue;
      // 127.0.0.1 only (best-effort IPv4 check).
      is_loopback = (peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
#else
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      int c = accept(srv, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (c < 0) continue;
      is_loopback = (peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
#endif
      try {
        handle_client(c, is_loopback);
      } catch (...) {
        // best-effort: ignore
      }
      socket_close(c);
    }
  }

 private:
  void handle_client(
#ifdef _WIN32
      SOCKET c,
      bool is_loopback
#else
      int c,
      bool is_loopback
#endif
  ) {
    std::string raw;
    raw.reserve(8192);

    // Read until header end.
    const size_t max_req = 2 * 1024 * 1024;
    char buf[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
      int n = recv(c, buf, static_cast<int>(sizeof(buf)), 0);
#else
      ssize_t n = recv(c, buf, sizeof(buf), 0);
#endif
      if (n <= 0) return;
      raw.append(buf, buf + n);
      if (raw.size() > max_req) {
        send_json(c, 413, "{\"error\":\"request too large\"}");
        return;
      }
    }

    HttpRequest req;
    if (!parse_http_request(raw, &req)) {
      send_json(c, 400, "{\"error\":\"bad request\"}");
      return;
    }

    // Split query string (if any) early so API routing can decide whether to
    // stream the request body (e.g., file upload) without buffering.
    std::string query_string;
    {
      const size_t qpos = req.path.find('?');
      if (qpos != std::string::npos) {
        query_string = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
      }
    }

    // URL-decode the request path (best-effort). This matters for browsing the
    // directory listing: links percent-encode spaces and other characters.
    req.path = url_decode_path(req.path);
    if (req.path.empty() || req.path[0] != '/') {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }
    // Reject embedded NUL bytes to avoid surprising filesystem behavior.
    if (req.path.find('\0') != std::string::npos) {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }

    // Determine Content-Length (if any). We parse as u64 to support large uploads.
    uintmax_t want = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
      want = parse_u64(trim_ws(it->second), 0);
    }

    const bool is_upload = (req.path == "/api/fs_upload");

    // Read remaining body if Content-Length says so.
    // For /api/fs_upload we stream directly to disk later.
    if (!is_upload) {
      if (want > static_cast<uintmax_t>(max_req)) {
        send_json(c, 413, "{\"error\":\"payload too large\"}");
        return;
      }
      const size_t want_sz = static_cast<size_t>(want);
      if (want_sz > 0 && req.body.size() < want_sz) {
        while (req.body.size() < want_sz) {
#ifdef _WIN32
          int n = recv(c, buf, static_cast<int>(sizeof(buf)), 0);
#else
          ssize_t n = recv(c, buf, sizeof(buf), 0);
#endif
          if (n <= 0) break;
          req.body.append(buf, buf + n);
          if (req.body.size() > max_req) {
            send_json(c, 413, "{\"error\":\"payload too large\"}");
            return;
          }
        }
      }
      if (want_sz > 0 && req.body.size() > want_sz) req.body.resize(want_sz);
    } else {
      // If the initial recv included more than the declared Content-Length,
      // truncate to avoid writing extra bytes.
      if (want > 0 && static_cast<uintmax_t>(req.body.size()) > want) {
        req.body.resize(static_cast<size_t>(want));
      }
    }

    if (starts_with(req.path, "/api/")) {
      // For safety, restrict API endpoints to loopback clients.
      if (!is_loopback) {
        send_json(c, 403, "{\"error\":\"api is loopback-only\"}");
        return;
      }
      // Additionally, reject browser cross-origin requests.
      if (!is_allowed_api_origin(req)) {
        send_json(c, 403, "{\"error\":\"origin not allowed\"}");
        return;
      }
      // All non-status API endpoints require the per-process token.
      if (req.path != "/api/status" && !has_valid_token(req)) {
        send_json(c, 403, "{\"error\":\"missing or invalid token\"}");
        return;
      }
    }

    if (req.path == "/api/status") {
      handle_status(c);
      return;
    }
    if (req.path == "/api/run") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_run(c, req.body);
      return;
    }
    if (req.path == "/api/runs") {
      handle_runs(c);
      return;
    }

    if (req.path == "/api/history") {
      if (req.method != "GET") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_history(c, query_string);
      return;
    }

    if (req.path == "/api/list") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_list(c, req.body);
      return;
    }

    if (req.path == "/api/find") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_find(c, req.body);
      return;
    }

    if (req.path == "/api/fs_upload") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_fs_upload(c, query_string, req.body, want);
      return;
    }

    // Workspace file operations (under --root).
    if (req.path == "/api/fs_mkdir") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_fs_mkdir(c, req.body);
      return;
    }

    if (req.path == "/api/fs_rename") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_fs_rename(c, req.body);
      return;
    }

    if (req.path == "/api/fs_trash") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_fs_trash(c, req.body);
      return;
    }

    if (req.path == "/api/delete_run") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_delete_run(c, req.body);
      return;
    }


    if (req.path == "/api/note") {
      if (req.method == "GET") {
        handle_note_get(c, query_string);
        return;
      }
      if (req.method == "POST") {
        handle_note_set(c, req.body);
        return;
      }
      send_json(c, 405, "{\"error\":\"method not allowed\"}");
      return;
    }




    if (req.path == "/api/presets") {
      if (req.method == "GET") {
        handle_presets_get(c);
        return;
      }
      if (req.method == "POST") {
        handle_presets_set(c, req.body);
        return;
      }
      send_json(c, 405, "{\"error\":\"method not allowed\"}");
      return;
    }

    {
      int id = 0;
      if (try_parse_id_path(req.path, "/api/job/", &id)) {
        if (req.method != "GET") {
          send_json(c, 405, "{\"error\":\"method not allowed\"}");
          return;
        }
        handle_job(c, id);
        return;
      }
      if (try_parse_id_path(req.path, "/api/zip/", &id)) {
        if (req.method != "GET") {
          send_json(c, 405, "{\"error\":\"method not allowed\"}");
          return;
        }
        handle_zip(c, id);
        return;
      }
      if (try_parse_id_path(req.path, "/api/log2/", &id)) {
        if (req.method != "GET") {
          send_json(c, 405, "{\"error\":\"method not allowed\"}");
          return;
        }
        handle_log_delta(c, id, query_string);
        return;
      }
      if (try_parse_id_path(req.path, "/api/log/", &id)) {
        if (req.method != "GET") {
          send_json(c, 405, "{\"error\":\"method not allowed\"}");
          return;
        }
        handle_log_tail(c, id);
        return;
      }
      if (try_parse_id_path(req.path, "/api/kill/", &id)) {
        if (req.method != "POST") {
          send_json(c, 405, "{\"error\":\"method not allowed\"}");
          return;
        }
        handle_kill(c, id);
        return;
      }
    }

    if (req.path == "/" || req.path == "/index.html") {
      if (req.method != "GET" && req.method != "HEAD") {
        send_text(c, 405, "method not allowed\n");
        return;
      }
      serve_file(c, index_html_, req, true);
      return;
    }

    // Static file: map URL path to <root>/<path>
    if (!req.path.empty() && req.path[0] == '/') {
      std::filesystem::path rel;
      try {
        rel = std::filesystem::u8path(req.path.substr(1));
      } catch (...) {
        send_text(c, 400, "bad path\n");
        return;
      }
      // Prevent ".." traversal.
      if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        send_text(c, 403, "forbidden\n");
        return;
      }
      for (const auto& part : rel) {
        if (part == "..") {
          send_text(c, 403, "forbidden\n");
          return;
        }
      }
      std::filesystem::path p = root_ / rel;

      // Prevent escaping the served root through symlinks: resolve canonical paths and ensure
      // the final target remains under the canonical root directory.
      if (!path_is_within_root(root_canon_, p)) {
        send_text(c, 403, "forbidden\n");
        return;
      }
      if (std::filesystem::is_directory(p)) {
        if (req.method != "GET" && req.method != "HEAD") {
          send_text(c, 405, "method not allowed\n");
          return;
        }
        // Try index.html inside.
        std::filesystem::path idx = p / "index.html";
        if (std::filesystem::exists(idx)) {
          serve_file(c, idx, req);
          return;
        }

        // No index file: render a simple directory listing so users can
        // browse run outputs (e.g., ui_runs/<timestamp>_<tool>_idX/).
        serve_directory_listing(c, p, req.path, req.method == "HEAD");
        return;
      }
      if (std::filesystem::exists(p)) {
        if (req.method != "GET" && req.method != "HEAD") {
          send_text(c, 405, "method not allowed\n");
          return;
        }
        serve_file(c, p, req);
        return;
      }
    }

    send_text(c, 404, "not found\n");
  }

  void handle_status(
#ifdef _WIN32
      SOCKET c
#else
      int c
#endif
  ) {
    std::ostringstream oss;
    oss << "{\"ok\":true,\"time\":\"" << qeeg::json_escape(qeeg::now_string_local()) << "\"";
    if (!api_token_.empty()) {
      oss << ",\"token\":\"" << qeeg::json_escape(api_token_) << "\"";
    }
    oss << "}";
    send_json(c, 200, oss.str());
  }

  bool is_allowed_api_origin(const HttpRequest& req) const {
    // Browsers send Origin on cross-origin requests. For this local server,
    // we only allow the expected loopback origins for API calls.
    const auto it = req.headers.find("origin");
    if (it == req.headers.end()) return true; // non-browser clients

    const std::string origin = trim(it->second);
    if (origin.empty()) return true;

    const std::string a1 = std::string("http://127.0.0.1:") + std::to_string(port_);
    const std::string a2 = std::string("http://localhost:") + std::to_string(port_);
    const std::string a3 = std::string("http://") + host_ + ":" + std::to_string(port_);
    return origin == a1 || origin == a2 || origin == a3;
  }

  bool has_valid_token(const HttpRequest& req) const {
    if (api_token_.empty()) return true;
    const auto it = req.headers.find("x-qeeg-token");
    if (it == req.headers.end()) return false;
    return trim(it->second) == api_token_;
  }

  static bool try_parse_id_path(const std::string& path, const std::string& prefix, int* out_id) {
    if (!out_id) return false;
    if (!starts_with(path, prefix)) return false;
    std::string s = path.substr(prefix.size());
    // Allow a trailing slash.
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty()) return false;
    for (char c : s) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    try {
      *out_id = qeeg::to_int(s);
      return true;
    } catch (...) {
      return false;
    }
  }


  size_t count_active_jobs() const {
    size_t n = 0;
    for (const auto& j : jobs_) {
      if (j.status == "running" || j.status == "stopping") ++n;
    }
    return n;
  }

  size_t parallel_limit() const {
    if (max_parallel_ <= 0) return static_cast<size_t>(-1);
    return static_cast<size_t>(max_parallel_);
  }

  static void append_text_line_best_effort(const std::filesystem::path& p, const std::string& line) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f) return;
    f << line;
    if (!line.empty() && line.back() != '\n') f << '\n';
  }

  bool start_job_process(RunJob* job, std::string* out_error) {
    if (out_error) out_error->clear();
    if (!job) {
      if (out_error) *out_error = "null job";
      return false;
    }

    const std::filesystem::path exe = resolve_exe_path(bin_dir_, job->tool);
    if (exe.empty()) {
      if (out_error) *out_error = "tool not found in bin-dir";
      return false;
    }

    const std::filesystem::path run_dir = root_ / std::filesystem::u8path(job->run_dir_rel);
    const std::filesystem::path log_path = root_ / std::filesystem::u8path(job->log_rel);
    qeeg::ensure_directory(run_dir.u8string());

#ifdef _WIN32
    // Build a single command line string for CreateProcess.
    //
    // IMPORTANT: Windows processes receive a *single* command line string and most C/C++
    // runtimes (including MSVC's) apply special parsing rules for backslashes immediately
    // preceding double quotes. If we naively concatenate raw strings, paths like:
    //   C:\path with space\
    // can be mis-parsed (trailing backslash escapes the closing quote).
    //
    // To make behavior consistent with the POSIX fork/exec path, we:
    //  1) split the UI-provided args string into tokens using split_commandline_args()
    //  2) re-quote each token using join_commandline_args_win32()
    std::vector<std::string> argv_s;
    argv_s.push_back(exe.u8string());
    for (const auto& t : qeeg::split_commandline_args(job->args)) {
      argv_s.push_back(t);
    }
    const std::string cmd = qeeg::join_commandline_args_win32(argv_s);

    const std::wstring cmd_w = utf8_to_wide(cmd);
    std::vector<wchar_t> cmd_buf(cmd_w.begin(), cmd_w.end());
    cmd_buf.push_back(L'\0');

    const std::wstring cwd_w = utf8_to_wide(root_.u8string());
    const std::wstring log_w = utf8_to_wide(log_path.u8string());

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hLog = CreateFileW(log_w.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      if (out_error) {
        std::ostringstream oss;
        oss << "failed to open log file (win32_error=" << err << ")";
        *out_error = oss.str();
      }
      return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        NULL,
        cmd_buf.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        cwd_w.empty() ? NULL : cwd_w.c_str(),
        &si,
        &pi);

    // Parent does not need the thread handle.
    if (pi.hThread) CloseHandle(pi.hThread);
    // Close our copy of the log handle; the child inherits its own handle.
    CloseHandle(hLog);

    if (!ok) {
      const DWORD err = GetLastError();
      if (pi.hProcess) CloseHandle(pi.hProcess);
      if (out_error) {
        std::ostringstream oss;
        oss << "CreateProcess failed (win32_error=" << err << ")";
        *out_error = oss.str();
      }
      return false;
    }

    job->pid = pi.dwProcessId;
    job->process_handle = pi.hProcess;
    return true;
#else
    std::vector<std::string> argv_s;
    argv_s.push_back(exe.u8string());
    for (const auto& t : qeeg::split_commandline_args(job->args)) {
      argv_s.push_back(t);
    }

    // Build argv array.
    std::vector<char*> argv;
    argv.reserve(argv_s.size() + 1);
    for (auto& s : argv_s) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
      if (out_error) *out_error = "fork failed";
      return false;
    }
    if (pid == 0) {
      // Child: redirect stdout/stderr to log file.
      FILE* f = std::fopen(log_path.u8string().c_str(), "wb");
      if (f) {
        int fd = fileno(f);
        if (fd >= 0) {
          dup2(fd, 1);
          dup2(fd, 2);
        }
      }
      // Work in root so relative paths make sense.
      chdir(root_.u8string().c_str());
      execv(argv[0], argv.data());
      // execv failed.
      std::perror("execv");
      std::exit(127);
    }

    // Parent.
    job->pid = pid;
    return true;
#endif
  }

  void maybe_start_queued_jobs() {
    const size_t limit = parallel_limit();
    size_t active = count_active_jobs();
    if (active >= limit) return;

    for (auto& j : jobs_) {
      if (active >= limit) break;
      if (j.status != "queued") continue;

      std::string err;
      if (start_job_process(&j, &err)) {
        j.status = "running";
        append_text_line_best_effort(root_ / std::filesystem::u8path(j.run_dir_rel) / "command.txt",
                                     std::string("launched: ") + now_string_local());
        ++active;
      } else {
        j.status = "error";
        j.exit_code = 127;
        append_text_line_best_effort(root_ / std::filesystem::u8path(j.log_rel),
                                     std::string("ERROR: failed to start queued job: ") + err);
        finalize_ui_run_meta(&j);
      }
    }
  }

  void update_jobs() {
#ifdef _WIN32
    for (auto& j : jobs_) {
      if (j.status != "running" && j.status != "stopping") continue;
      if (!j.process_handle) continue;
      DWORD code = STILL_ACTIVE;
      if (GetExitCodeProcess(j.process_handle, &code) == 0) continue;
      if (code == STILL_ACTIVE) continue;
      j.exit_code = static_cast<int>(code);
      if (j.status == "stopping") {
        j.status = "killed";
      } else {
        j.status = (j.exit_code == 0) ? "finished" : "error";
      }
      CloseHandle(j.process_handle);
      j.process_handle = nullptr;

      finalize_ui_run_meta(&j);
    }
#else
    for (auto& j : jobs_) {
      if (j.status != "running" && j.status != "stopping") continue;
      int st = 0;
      const pid_t rc = waitpid(j.pid, &st, WNOHANG);
      if (rc == 0) continue;
      if (rc < 0) continue;
      if (WIFEXITED(st)) {
        j.exit_code = WEXITSTATUS(st);
        if (j.status == "stopping") {
          j.status = "killed";
        } else {
          j.status = (j.exit_code == 0) ? "finished" : "error";
        }
      } else if (WIFSIGNALED(st)) {
        j.exit_code = 128 + WTERMSIG(st);
        if (j.status == "stopping") {
          j.status = "killed";
        } else {
          j.status = "error";
        }
      } else {
        j.status = (j.status == "stopping") ? "killed" : "finished";
      }

      finalize_ui_run_meta(&j);
    }
#endif

    // If concurrency limiting is enabled, start queued jobs when slots free up.
    maybe_start_queued_jobs();
  }

  void finalize_ui_run_meta(RunJob* j) {
    if (!j) return;

    // Write a small status file and refresh ui_server_run_meta.json so the
    // dashboard can discover artifacts produced by UI-launched jobs.
    const std::filesystem::path run_dir = root_ / std::filesystem::u8path(j->run_dir_rel);
    std::error_code ec;
    if (!std::filesystem::exists(run_dir, ec) || !std::filesystem::is_directory(run_dir, ec)) return;

    const std::string meta_rel = j->meta_rel.empty() ? (j->run_dir_rel + "/ui_server_run_meta.json") : j->meta_rel;
    const std::filesystem::path meta_abs = root_ / std::filesystem::u8path(meta_rel);

    // Record final status in a human-readable text file (previewable in UI).
    const std::filesystem::path exit_path = run_dir / "exit_status.txt";
    {
      std::ofstream f(exit_path, std::ios::binary);
      if (f) {
        f << "tool: " << j->tool << "\n";
        f << "args: " << j->args << "\n";
        f << "started: " << j->started << "\n";
        f << "finished: " << qeeg::now_string_local() << "\n";
        f << "status: " << j->status << "\n";
        f << "exit_code: " << j->exit_code << "\n";
      }
    }

    std::vector<std::string> outputs = scan_run_dir_outputs(run_dir, meta_abs, 2000);

    // Ensure a few canonical artifacts are included if they exist.
    auto ensure = [&](const std::string& rel) {
      const std::filesystem::path p = run_dir / std::filesystem::u8path(rel);
      std::error_code e2;
      if (std::filesystem::exists(p, e2)) {
        outputs.push_back(rel);
      }
    };
    ensure("run.log");
    ensure("command.txt");
    ensure("exit_status.txt");

    std::sort(outputs.begin(), outputs.end());
    outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());

    // Best-effort: refresh the meta file (overwrites timestamp).
    qeeg::write_run_meta_json(meta_abs.u8string(), j->tool, j->run_dir_rel, j->input_path, outputs);
  }

  void handle_runs(
#ifdef _WIN32
      SOCKET c
#else
      int c
#endif
  ) {
    update_jobs();
    std::ostringstream oss;
    oss << "{\"runs\":[";
    for (size_t i = 0; i < jobs_.size(); ++i) {
      const auto& j = jobs_[i];
      if (i) oss << ',';
      oss << "{\"id\":" << j.id
          << ",\"tool\":\"" << qeeg::json_escape(j.tool) << "\""
          << ",\"args\":\"" << qeeg::json_escape(j.args) << "\""
          << ",\"started\":\"" << qeeg::json_escape(j.started) << "\""
          << ",\"status\":\"" << qeeg::json_escape(j.status) << "\""
          << ",\"exit_code\":" << j.exit_code
          << ",\"run_dir\":\"" << qeeg::json_escape(j.run_dir_rel) << "\""
          << ",\"log\":\"" << qeeg::json_escape(j.log_rel) << "\""
          << ",\"meta\":\"" << qeeg::json_escape(j.meta_rel) << "\""
          << ",\"input_path\":\"" << qeeg::json_escape(j.input_path) << "\""
          << "}";
    }
    oss << "]}";
    send_json(c, 200, oss.str());
  }


  static std::string parse_kv_line_value(const std::string& text, const std::string& key_lower) {
    // Extract a simple "key: value" line from a small text file (command.txt / exit_status.txt).
    // Comparison is case-insensitive on the key.
    if (key_lower.empty()) return {};
    std::string needle = to_lower(key_lower);
    if (!needle.empty() && needle.back() != ':') needle.push_back(':');

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim_ws(line);
      if (t.empty()) continue;
      const std::string lo = to_lower(t);
      if (!starts_with(lo, needle)) continue;
      return trim_ws(t.substr(needle.size()));
    }
    return {};
  }

  static std::string extract_args_from_command_value(const std::string& command_value) {
    // command_value is the value part of:
    //   command: "<exe>" <args...>
    // Return the <args...> portion.
    std::string s = trim_ws(command_value);
    if (s.empty()) return {};
    if (s.front() == '"') {
      const size_t q2 = s.find('"', 1);
      if (q2 == std::string::npos) return {};
      return trim_ws(s.substr(q2 + 1));
    }
    // Best-effort fallback: split on first whitespace.
    const size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) return {};
    return trim_ws(s.substr(sp + 1));
  }

  void handle_history(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& query_string) {
    update_jobs();

    // Query params:
    //   limit=<N>  (default 40, max 200)
    //   tool=<name> (optional filter)
    const std::map<std::string, std::string> qp = parse_query_params(query_string);
    size_t limit = 40;
    {
      auto it = qp.find("limit");
      if (it != qp.end()) {
        const uintmax_t v = parse_u64(it->second, static_cast<uintmax_t>(limit));
        if (v > 0) limit = static_cast<size_t>(std::min<uintmax_t>(v, 200));
      }
    }
    std::string tool_filter;
    {
      auto it = qp.find("tool");
      if (it != qp.end()) tool_filter = trim(it->second);
    }

    const std::filesystem::path ui_runs = root_ / std::filesystem::u8path("ui_runs");
    std::error_code ec;
    if (!std::filesystem::exists(ui_runs, ec) || !std::filesystem::is_directory(ui_runs, ec)) {
      send_json(c, 200, "{\"ok\":true,\"runs\":[]}");
      return;
    }

    // Collect run directories (we sort by directory name, which starts with a compact timestamp).
    std::vector<std::string> dir_names;
    for (auto it = std::filesystem::directory_iterator(ui_runs, ec);
         it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (ec) break;
      if (!it->is_directory(ec)) {
        ec.clear();
        continue;
      }
      const std::string name = it->path().filename().u8string();
      if (name.empty()) continue;
      dir_names.push_back(name);
    }
    std::sort(dir_names.begin(), dir_names.end(),
              [](const std::string& a, const std::string& b) { return a > b; });

    std::ostringstream oss;
    oss << "{\"ok\":true,\"runs\":[";
    bool first = true;
    size_t emitted = 0;

    for (const auto& name : dir_names) {
      if (emitted >= limit) break;
      const std::string run_dir_rel = std::string("ui_runs/") + name;
      const std::filesystem::path run_dir_abs = ui_runs / std::filesystem::u8path(name);
      if (!path_is_within_root(root_canon_, run_dir_abs)) continue;

      const std::filesystem::path meta_abs = run_dir_abs / "ui_server_run_meta.json";
      const std::filesystem::path cmd_abs = run_dir_abs / "command.txt";
      const std::filesystem::path exit_abs = run_dir_abs / "exit_status.txt";

      std::string tool;
      std::string input_path;
      if (std::filesystem::exists(meta_abs, ec) && std::filesystem::is_regular_file(meta_abs, ec)) {
        tool = qeeg::read_run_meta_tool(meta_abs.u8string());
        input_path = qeeg::read_run_meta_input_path(meta_abs.u8string());
      }
      ec.clear();

      std::string started;
      std::string args;
      std::string command_rel;
      {
        std::string cmd_txt;
        if (path_is_within_root(root_canon_, cmd_abs) && read_file_binary_bounded(cmd_abs, 128 * 1024, &cmd_txt)) {
          if (tool.empty()) tool = parse_kv_line_value(cmd_txt, "tool");
          started = parse_kv_line_value(cmd_txt, "started");
          const std::string cmd_value = parse_kv_line_value(cmd_txt, "command");
          args = extract_args_from_command_value(cmd_value);
          command_rel = run_dir_rel + "/command.txt";
        }
      }

      if (!tool_filter.empty() && tool != tool_filter) continue;

      std::string status;
      int exit_code = 0;

      // Prefer live in-memory job state if the current server session launched this run.
      RunJob* live = nullptr;
      for (auto& j : jobs_) {
        if (j.run_dir_rel == run_dir_rel) {
          live = &j;
          break;
        }
      }
      if (live) {
        status = live->status;
        exit_code = live->exit_code;
      } else {
        std::string exit_txt;
        if (path_is_within_root(root_canon_, exit_abs) && read_file_binary_bounded(exit_abs, 128 * 1024, &exit_txt)) {
          status = parse_kv_line_value(exit_txt, "status");
          const std::string ec_s = parse_kv_line_value(exit_txt, "exit_code");
          if (!ec_s.empty()) {
            try {
              exit_code = qeeg::to_int(ec_s);
            } catch (...) {
              exit_code = 0;
            }
          }
          if (started.empty()) started = parse_kv_line_value(exit_txt, "started");
        }
      }

      const std::string meta_rel = run_dir_rel + "/ui_server_run_meta.json";
      const std::string log_rel = run_dir_rel + "/run.log";

      if (!first) oss << ',';
      first = false;

      oss << '{'
          << "\"run_dir\":\"" << qeeg::json_escape(run_dir_rel) << "\""
          << ",\"tool\":\"" << qeeg::json_escape(tool) << "\""
          << ",\"args\":\"" << qeeg::json_escape(args) << "\""
          << ",\"started\":\"" << qeeg::json_escape(started) << "\""
          << ",\"status\":\"" << qeeg::json_escape(status) << "\""
          << ",\"exit_code\":" << exit_code
          << ",\"meta\":\"" << qeeg::json_escape(meta_rel) << "\""
          << ",\"log\":\"" << qeeg::json_escape(log_rel) << "\""
          << ",\"command\":\"" << qeeg::json_escape(command_rel) << "\""
          << ",\"input_path\":\"" << qeeg::json_escape(input_path) << "\""
          << '}';
      ++emitted;
    }

    oss << "]}";
    send_json(c, 200, oss.str());
  }


  struct FsEntry {
    std::string name;
    std::string path;  // relative to root
    bool is_dir{false};
    uintmax_t size{0};
    int64_t mtime{0};
  };

  void handle_list(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string dir_raw = json_find_string_value(body, "dir");
    const bool show_hidden = json_find_bool_value(body, "show_hidden", false);
    const bool sort_desc = json_find_bool_value(body, "desc", false);
    std::string sort_mode = to_lower(trim(json_find_string_value(body, "sort")));
    if (sort_mode != "size" && sort_mode != "mtime" && sort_mode != "name") sort_mode = "name";

    auto normalize = [](std::string s) {
      s = trim(s);
      // Convert backslashes to slashes so callers can use either style.
      std::replace(s.begin(), s.end(), '\\', '/');
      while (!s.empty() && s.front() == '/') s.erase(s.begin());
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (s == ".") s.clear();
      return s;
    };

    const std::string dir_norm = normalize(dir_raw);
    std::filesystem::path rel = dir_norm.empty() ? std::filesystem::path() : std::filesystem::u8path(dir_norm);

    // Prevent absolute paths and traversal outside the served root.
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
      send_json(c, 403, "{\"error\":\"absolute paths not allowed\"}");
      return;
    }
    for (const auto& part : rel) {
      if (part == "..") {
        send_json(c, 403, "{\"error\":\"path traversal not allowed\"}");
        return;
      }
    }

    const std::filesystem::path abs = root_ / rel;
    std::error_code ec;
    if (!std::filesystem::exists(abs, ec) || !std::filesystem::is_directory(abs, ec)) {
      send_json(c, 404, "{\"error\":\"dir not found\"}");
      return;
    }

    // Prevent escaping the served root through symlinks.
    if (!path_is_within_root(root_canon_, abs)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    std::vector<FsEntry> entries;
    entries.reserve(256);

    // Keep responses bounded.
    const size_t kMaxEntries = 2000;
    for (auto it = std::filesystem::directory_iterator(abs, ec);
         it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (ec) break;
      FsEntry e;
      e.name = it->path().filename().u8string();
      if (!show_hidden && !e.name.empty() && e.name[0] == '.') {
        continue;
      }
      e.path = (rel / it->path().filename()).generic_u8string();
      e.is_dir = it->is_directory(ec);

      // Best-effort modified time.
      {
        std::error_code ec2;
        const auto ft = std::filesystem::last_write_time(it->path(), ec2);
        if (!ec2) e.mtime = file_time_to_unix_seconds(ft);
      }

      if (!e.is_dir) {
        const uintmax_t sz = std::filesystem::file_size(it->path(), ec);
        if (!ec) e.size = sz;
        ec.clear();
      }
      entries.push_back(std::move(e));
      if (entries.size() >= kMaxEntries) break;
    }

    std::sort(entries.begin(), entries.end(),
              [&](const FsEntry& a, const FsEntry& b) {
                // Keep dirs first for better navigation.
                if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;

                if (sort_mode == "size") {
                  if (a.size != b.size) return sort_desc ? (a.size > b.size) : (a.size < b.size);
                } else if (sort_mode == "mtime") {
                  if (a.mtime != b.mtime) return sort_desc ? (a.mtime > b.mtime) : (a.mtime < b.mtime);
                }
                return to_lower(a.name) < to_lower(b.name);
              });

    std::ostringstream oss;
    oss << "{\"ok\":true,\"dir\":\"" << json_escape(rel.generic_u8string()) << "\",\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& e = entries[i];
      if (i) oss << ',';
      oss << "{"
          << "\"name\":\"" << json_escape(e.name) << "\""
          << ",\"path\":\"" << json_escape(e.path) << "\""
          << ",\"type\":\"" << (e.is_dir ? "dir" : "file") << "\""
          << ",\"size\":" << e.size
          << ",\"mtime\":" << e.mtime
          << "}";
    }
    oss << "]}";
    send_json(c, 200, oss.str());
  }

  void handle_find(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string dir_raw = json_find_string_value(body, "dir");
    std::string q_raw = json_find_string_value(body, "q");
    const bool show_hidden = json_find_bool_value(body, "show_hidden", false);
    int max_results = json_find_int_value(body, "max_results", 200);
    int max_depth = json_find_int_value(body, "max_depth", 8);
    std::string want_type = to_lower(trim(json_find_string_value(body, "type")));
    if (want_type != "file" && want_type != "dir" && want_type != "any") want_type = "any";

    q_raw = trim(q_raw);
    if (q_raw.empty()) {
      send_json(c, 400, "{\"error\":\"missing q\"}");
      return;
    }

    if (max_results < 1) max_results = 1;
    if (max_results > 2000) max_results = 2000;
    if (max_depth < 0) max_depth = 0;
    if (max_depth > 64) max_depth = 64;

    // Normalize dir (relative to root).
    auto normalize = [](std::string s) {
      s = trim(s);
      // Convert backslashes to slashes so callers can use either style.
      std::replace(s.begin(), s.end(), '\\', '/');
      while (!s.empty() && s.front() == '/') s.erase(s.begin());
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (s == ".") s.clear();
      return s;
    };

    const std::string dir_norm = normalize(dir_raw);
    std::filesystem::path rel = dir_norm.empty() ? std::filesystem::path() : std::filesystem::u8path(dir_norm);

    // Prevent absolute paths and traversal outside the served root.
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
      send_json(c, 403, "{\"error\":\"absolute paths not allowed\"}");
      return;
    }
    for (const auto& part : rel) {
      if (part == "..") {
        send_json(c, 403, "{\"error\":\"path traversal not allowed\"}");
        return;
      }
    }

    const std::filesystem::path abs = root_ / rel;
    std::error_code ec;
    if (!std::filesystem::exists(abs, ec) || !std::filesystem::is_directory(abs, ec)) {
      send_json(c, 404, "{\"error\":\"dir not found\"}");
      return;
    }

    // Prevent escaping the served root through symlinks.
    if (!path_is_within_root(root_canon_, abs)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    // Normalize query: treat backslashes as slashes for consistency.
    std::string q = q_raw;
    std::replace(q.begin(), q.end(), '\\', '/');

    const bool use_glob = (q.find('*') != std::string::npos) || (q.find('?') != std::string::npos);
    const bool q_has_sep = (q.find('/') != std::string::npos);
    const std::string q_lc = to_lower(q);

    std::vector<FsEntry> results;
    results.reserve(static_cast<size_t>(std::min(256, max_results)));

    size_t scanned = 0;
    bool truncated = false;
    const size_t kMaxScanned = 200000; // hard cap to keep requests bounded

    const auto t0 = std::chrono::steady_clock::now();

    std::filesystem::recursive_directory_iterator it(abs, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;

    for (; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }

      if (++scanned >= kMaxScanned) {
        truncated = true;
        break;
      }

      const std::filesystem::path p = it->path();
      const std::string name = p.filename().u8string();

      // Determine type without following symlinks.
      std::error_code ec2;
      const auto st = it->symlink_status(ec2);
      const bool is_symlink = (!ec2 && st.type() == std::filesystem::file_type::symlink);
      const bool is_dir = (!ec2 && st.type() == std::filesystem::file_type::directory);

      // Do not recurse into symlinks.
      if (is_symlink) {
        it.disable_recursion_pending();
      }

      // Hide dotfiles unless explicitly requested.
      if (!show_hidden && !name.empty() && name[0] == '.') {
        if (is_dir) it.disable_recursion_pending();
        continue;
      }

      // Depth cap: when at or beyond max_depth, avoid recursing deeper.
      if (is_dir && it.depth() >= max_depth) {
        it.disable_recursion_pending();
      }

      if (want_type == "file" && is_dir) continue;
      if (want_type == "dir" && !is_dir) continue;

      // Convert absolute path to a root-relative path (for UI links).
      std::filesystem::path relp = p.lexically_relative(root_);
      std::string rels = relp.generic_u8string();
      if (rels.empty()) rels = name;

      bool match = false;
      if (use_glob) {
        // If the pattern includes a '/', match against the full rel path. Otherwise match just the name.
        const std::string& hay = q_has_sep ? rels : name;
        match = glob_match_ci(q, hay);
      } else {
        const std::string n_lc = to_lower(name);
        const std::string p_lc = to_lower(rels);
        match = (n_lc.find(q_lc) != std::string::npos) || (p_lc.find(q_lc) != std::string::npos);
      }
      if (!match) continue;

      FsEntry e;
      e.name = name;
      e.path = rels;
      e.is_dir = is_dir;

      // Best-effort modified time.
      {
        std::error_code ec3;
        const auto ft = std::filesystem::last_write_time(p, ec3);
        if (!ec3) e.mtime = file_time_to_unix_seconds(ft);
      }

      if (!e.is_dir && !is_symlink) {
        std::error_code ec4;
        const uintmax_t sz = std::filesystem::file_size(p, ec4);
        if (!ec4) e.size = sz;
      }

      results.push_back(std::move(e));
      if (results.size() >= static_cast<size_t>(max_results)) {
        truncated = true;
        break;
      }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::sort(results.begin(), results.end(), [&](const FsEntry& a, const FsEntry& b) {
      if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
      return to_lower(a.path) < to_lower(b.path);
    });

    std::ostringstream oss;
    oss << "{\"ok\":true,\"dir\":\"" << json_escape(rel.generic_u8string())
        << "\",\"q\":\"" << json_escape(q_raw)
        << "\",\"scanned\":" << scanned
        << ",\"elapsed_ms\":" << elapsed_ms
        << ",\"truncated\":" << (truncated ? "true" : "false")
        << ",\"results\":[";

    for (size_t i = 0; i < results.size(); ++i) {
      const auto& e = results[i];
      if (i) oss << ',';
      oss << '{'
          << "\"name\":\"" << json_escape(e.name) << "\""
          << ",\"path\":\"" << json_escape(e.path) << "\""
          << ",\"type\":\"" << (e.is_dir ? "dir" : "file") << "\""
          << ",\"size\":" << e.size
          << ",\"mtime\":" << e.mtime
          << '}';
    }
    oss << "]}";

    send_json(c, 200, oss.str());
  }


  // ---- Workspace file operations (under --root) ----
  static bool is_valid_single_name(const std::string& raw, std::string* err) {
    const std::string name = trim(raw);
    if (name.empty()) {
      if (err) *err = "empty name";
      return false;
    }
    if (name == "." || name == "..") {
      if (err) *err = "invalid name";
      return false;
    }
    // Disallow path separators and control characters.
    for (char ch : name) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (ch == '/' || ch == '\\') {
        if (err) *err = "name must not contain path separators";
        return false;
      }
      if (c < 32) {
        if (err) *err = "name contains control characters";
        return false;
      }
    }
    // Avoid excessively long single components.
    if (name.size() > 255) {
      if (err) *err = "name too long";
      return false;
    }
    return true;
  }

  static std::string normalize_rel_string(std::string s) {
    s = trim(s);
    // Convert backslashes to slashes so callers can use either style.
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s == ".") s.clear();
    return s;
  }

  static bool validate_rel_path(const std::filesystem::path& rel, std::string* err) {
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
      if (err) *err = "absolute paths not allowed";
      return false;
    }
    for (const auto& part : rel) {
      if (part == "..") {
        if (err) *err = "path traversal not allowed";
        return false;
      }
    }
    return true;
  }

  void handle_fs_upload(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& query_string,
      const std::string& initial_body,
      uintmax_t content_length) {
    // Upload a single file into the workspace root.
    //
    // Endpoint:
    //   POST /api/fs_upload?dir=<rel>&name=<filename>&overwrite=0|1
    // Body:
    //   raw file bytes
    //
    // We intentionally keep this endpoint simple: no multipart parsing, and
    // we require Content-Length so we can stream exactly N bytes.

    const uintmax_t kMaxUploadBytes = 1024ull * 1024ull * 1024ull; // 1 GiB

    if (content_length == 0) {
      send_json(c, 411, "{\"error\":\"missing Content-Length\"}");
      return;
    }
    if (content_length > kMaxUploadBytes) {
      send_json(c, 413, "{\"error\":\"upload too large\"}");
      return;
    }

    const std::map<std::string, std::string> qp = parse_query_params(query_string);
    std::string dir_raw;
    std::string name_raw;
    bool overwrite = false;
    {
      auto it = qp.find("dir");
      if (it != qp.end()) dir_raw = it->second;
      auto itn = qp.find("name");
      if (itn != qp.end()) name_raw = itn->second;
      auto ito = qp.find("overwrite");
      if (ito != qp.end()) {
        const std::string v = to_lower(trim(ito->second));
        overwrite = (v == "1" || v == "true" || v == "yes" || v == "y");
      }
    }

    std::string err;
    if (!is_valid_single_name(name_raw, &err)) {
      send_json(c, 400, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }
    const std::string name = trim(name_raw);

    const std::string dir_norm = normalize_rel_string(dir_raw);
    std::filesystem::path rel_dir;
    try {
      rel_dir = dir_norm.empty() ? std::filesystem::path() : std::filesystem::u8path(dir_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad dir\"}");
      return;
    }
    if (!validate_rel_path(rel_dir, &err)) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }

    const std::filesystem::path abs_dir = root_ / rel_dir;
    std::error_code ec;
    if (!std::filesystem::exists(abs_dir, ec) || !std::filesystem::is_directory(abs_dir, ec)) {
      send_json(c, 404, "{\"error\":\"dir not found\"}");
      return;
    }
    if (!path_is_within_root(root_canon_, abs_dir)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    std::filesystem::path rel_new;
    try {
      rel_new = rel_dir / std::filesystem::u8path(name);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad name\"}");
      return;
    }
    if (!validate_rel_path(rel_new, &err) || rel_new.empty()) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err.empty() ? "path not allowed" : err) + "\"}");
      return;
    }

    const std::filesystem::path abs_new = root_ / rel_new;
    if (!path_is_within_root(root_canon_, abs_new)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    // Validate destination (if it exists).
    bool dest_exists = false;
    {
      ec.clear();
      dest_exists = std::filesystem::exists(abs_new, ec) && !ec;
      ec.clear();
      const auto st = std::filesystem::symlink_status(abs_new, ec);
      if (!ec && std::filesystem::is_symlink(st)) {
        send_json(c, 403, "{\"error\":\"refusing to overwrite symlink\"}");
        return;
      }
      ec.clear();
      if (dest_exists && std::filesystem::is_directory(abs_new, ec)) {
        send_json(c, 409, "{\"error\":\"destination is a directory\"}");
        return;
      }
      ec.clear();
      if (dest_exists && !std::filesystem::is_regular_file(abs_new, ec)) {
        // Includes sockets, devices, etc.
        send_json(c, 409, "{\"error\":\"destination is not a regular file\"}");
        return;
      }
      if (dest_exists && !overwrite) {
        send_json(c, 409, "{\"error\":\"destination exists\"}");
        return;
      }
    }

    // Create a temporary file in the same directory so the final rename is
    // best-effort atomic.
    std::filesystem::path rel_tmp;
    std::filesystem::path abs_tmp;
    {
      std::string tmp_name = name + ".upload_tmp_" + random_hex_token(8);
      for (int attempt = 0; attempt < 8; ++attempt) {
        try {
          rel_tmp = rel_dir / std::filesystem::u8path(tmp_name);
        } catch (...) {
          rel_tmp = rel_dir / std::filesystem::path(name + ".upload_tmp");
        }
        abs_tmp = root_ / rel_tmp;
        if (!path_is_within_root(root_canon_, abs_tmp)) {
          send_json(c, 403, "{\"error\":\"path not allowed\"}");
          return;
        }
        ec.clear();
        if (!std::filesystem::exists(abs_tmp, ec)) break;
        tmp_name = name + ".upload_tmp_" + random_hex_token(8);
      }
      ec.clear();
      if (std::filesystem::exists(abs_tmp, ec)) {
        send_json(c, 500, "{\"error\":\"cannot create temp file\"}");
        return;
      }
    }

    std::ofstream f(abs_tmp, std::ios::binary);
    if (!f) {
      send_json(c, 500, "{\"error\":\"cannot open temp file\"}");
      return;
    }

    uintmax_t written = 0;
    const uintmax_t init_take_u64 = std::min<uintmax_t>(static_cast<uintmax_t>(initial_body.size()), content_length);
    const size_t init_take = static_cast<size_t>(init_take_u64);
    if (init_take > 0) {
      f.write(initial_body.data(), static_cast<std::streamsize>(init_take));
      if (!f) {
        f.close();
        std::error_code ec2;
        std::filesystem::remove(abs_tmp, ec2);
        send_json(c, 500, "{\"error\":\"write failed\"}");
        return;
      }
      written += init_take_u64;
    }

    char ubuf[64 * 1024];
    while (written < content_length) {
      const uintmax_t remain = content_length - written;
      const size_t want_read = static_cast<size_t>(std::min<uintmax_t>(remain, sizeof(ubuf)));
#ifdef _WIN32
      int n = recv(c, ubuf, static_cast<int>(want_read), 0);
#else
      ssize_t n = recv(c, ubuf, want_read, 0);
#endif
      if (n <= 0) break;
      f.write(ubuf, static_cast<std::streamsize>(n));
      if (!f) {
        break;
      }
      written += static_cast<uintmax_t>(n);
      if (written > content_length) {
        // Shouldn't happen because we clamp want_read, but keep safe.
        written = content_length;
        break;
      }
    }

    f.flush();
    f.close();

    if (written != content_length) {
      std::error_code ec2;
      std::filesystem::remove(abs_tmp, ec2);
      send_json(c, 400, "{\"error\":\"upload truncated\"}");
      return;
    }

    // Overwrite behavior: remove existing destination before rename.
    if (dest_exists && overwrite) {
      ec.clear();
      std::filesystem::remove(abs_new, ec);
      if (ec) {
        std::error_code ec2;
        std::filesystem::remove(abs_tmp, ec2);
        send_json(c, 500, std::string("{\"error\":\"cannot overwrite: ") + json_escape(ec.message()) + "\"}");
        return;
      }
    }

    ec.clear();
    std::filesystem::rename(abs_tmp, abs_new, ec);
    if (ec) {
      std::error_code ec2;
      std::filesystem::remove(abs_tmp, ec2);
      send_json(c, 500, std::string("{\"error\":\"finalize failed: ") + json_escape(ec.message()) + "\"}");
      return;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"path\":\"" << json_escape(rel_new.generic_u8string()) << "\""
        << ",\"bytes\":" << written
        << ",\"overwritten\":" << (dest_exists && overwrite ? "true" : "false")
        << "}";
    send_json(c, 200, oss.str());
  }

  void handle_fs_mkdir(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string dir_raw = json_find_string_value(body, "dir");
    const std::string name_raw = json_find_string_value(body, "name");

    std::string err;
    if (!is_valid_single_name(name_raw, &err)) {
      send_json(c, 400, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }
    const std::string name = trim(name_raw);

    const std::string dir_norm = normalize_rel_string(dir_raw);
    std::filesystem::path rel_dir;
    try {
      rel_dir = dir_norm.empty() ? std::filesystem::path() : std::filesystem::u8path(dir_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad dir\"}");
      return;
    }
    if (!validate_rel_path(rel_dir, &err)) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }

    const std::filesystem::path abs_dir = root_ / rel_dir;
    std::error_code ec;
    if (!std::filesystem::exists(abs_dir, ec) || !std::filesystem::is_directory(abs_dir, ec)) {
      send_json(c, 404, "{\"error\":\"dir not found\"}");
      return;
    }
    if (!path_is_within_root(root_canon_, abs_dir)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    std::filesystem::path rel_new;
    try {
      rel_new = rel_dir / std::filesystem::u8path(name);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad name\"}");
      return;
    }
    if (!validate_rel_path(rel_new, &err)) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }

    const std::filesystem::path abs_new = root_ / rel_new;
    if (!path_is_within_root(root_canon_, abs_new)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    ec.clear();
    if (std::filesystem::exists(abs_new, ec)) {
      send_json(c, 409, "{\"error\":\"already exists\"}");
      return;
    }
    ec.clear();
    if (!std::filesystem::create_directory(abs_new, ec) || ec) {
      send_json(c, 500, std::string("{\"error\":\"mkdir failed: ") + json_escape(ec.message()) + "\"}");
      return;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"path\":\"" << json_escape(rel_new.generic_u8string()) << "\"}";
    send_json(c, 200, oss.str());
  }

  void handle_fs_rename(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string path_raw = json_find_string_value(body, "path");
    const std::string new_name_raw = json_find_string_value(body, "new_name");

    const std::string path_norm = normalize_rel_string(path_raw);
    if (path_norm.empty()) {
      send_json(c, 400, "{\"error\":\"missing path\"}");
      return;
    }

    std::string err;
    std::filesystem::path rel_old;
    try {
      rel_old = std::filesystem::u8path(path_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }
    if (!validate_rel_path(rel_old, &err) || rel_old.empty()) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err.empty() ? "path not allowed" : err) + "\"}");
      return;
    }

    if (!is_valid_single_name(new_name_raw, &err)) {
      send_json(c, 400, std::string("{\"error\":\"") + json_escape(err) + "\"}");
      return;
    }
    const std::string new_name = trim(new_name_raw);

    const std::filesystem::path abs_old = root_ / rel_old;
    std::error_code ec;
    if (!std::filesystem::exists(abs_old, ec)) {
      send_json(c, 404, "{\"error\":\"path not found\"}");
      return;
    }
    if (!path_is_within_root(root_canon_, abs_old)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    const std::filesystem::path rel_parent = rel_old.parent_path();
    std::filesystem::path rel_new;
    try {
      rel_new = rel_parent / std::filesystem::u8path(new_name);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad new_name\"}");
      return;
    }
    if (!validate_rel_path(rel_new, &err) || rel_new.empty()) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err.empty() ? "dest not allowed" : err) + "\"}");
      return;
    }

    const std::filesystem::path abs_new = root_ / rel_new;
    if (!path_is_within_root(root_canon_, abs_new)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    ec.clear();
    if (std::filesystem::exists(abs_new, ec)) {
      send_json(c, 409, "{\"error\":\"destination exists\"}");
      return;
    }

    ec.clear();
    std::filesystem::rename(abs_old, abs_new, ec);
    if (ec) {
      send_json(c, 500, std::string("{\"error\":\"rename failed: ") + json_escape(ec.message()) + "\"}");
      return;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"from\":\"" << json_escape(rel_old.generic_u8string()) << "\""
        << ",\"path\":\"" << json_escape(rel_new.generic_u8string()) << "\""
        << "}";
    send_json(c, 200, oss.str());
  }

  void handle_fs_trash(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string path_raw = json_find_string_value(body, "path");
    const std::string path_norm = normalize_rel_string(path_raw);
    if (path_norm.empty()) {
      send_json(c, 400, "{\"error\":\"missing path\"}");
      return;
    }

    std::string err;
    std::filesystem::path rel_old;
    try {
      rel_old = std::filesystem::u8path(path_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }
    if (!validate_rel_path(rel_old, &err) || rel_old.empty()) {
      send_json(c, 403, std::string("{\"error\":\"") + json_escape(err.empty() ? "path not allowed" : err) + "\"}");
      return;
    }

    const std::filesystem::path abs_old = root_ / rel_old;
    std::error_code ec;
    if (!std::filesystem::exists(abs_old, ec)) {
      send_json(c, 404, "{\"error\":\"path not found\"}");
      return;
    }
    if (!path_is_within_root(root_canon_, abs_old)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    // Create the trash folder under root (best-effort).
    const std::filesystem::path trash_rel = std::filesystem::u8path(".qeeg_trash");
    const std::filesystem::path trash_abs = root_ / trash_rel;
    ec.clear();
    if (!std::filesystem::exists(trash_abs, ec)) {
      ec.clear();
      std::filesystem::create_directory(trash_abs, ec);
    }
    ec.clear();
    if (!std::filesystem::exists(trash_abs, ec) || !std::filesystem::is_directory(trash_abs, ec)) {
      send_json(c, 500, "{\"error\":\"cannot create .qeeg_trash\"}");
      return;
    }
    if (!path_is_within_root(root_canon_, trash_abs)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    const std::string base = rel_old.filename().u8string();
    if (base.empty()) {
      send_json(c, 400, "{\"error\":\"invalid path\"}");
      return;
    }

    std::string dest_name = now_compact_local() + "_" + base;
    std::filesystem::path rel_new = trash_rel / std::filesystem::u8path(dest_name);
    std::filesystem::path abs_new = root_ / rel_new;

    // Avoid collisions.
    for (int attempt = 0; attempt < 6; ++attempt) {
      ec.clear();
      if (!std::filesystem::exists(abs_new, ec)) break;
      dest_name = now_compact_local() + "_" + base + "_" + random_hex_token(4);
      rel_new = trash_rel / std::filesystem::u8path(dest_name);
      abs_new = root_ / rel_new;
    }

    if (!path_is_within_root(root_canon_, abs_new)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    ec.clear();
    std::filesystem::rename(abs_old, abs_new, ec);
    if (ec) {
      send_json(c, 500, std::string("{\"error\":\"trash failed: ") + json_escape(ec.message()) + "\"}");
      return;
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"from\":\"" << json_escape(rel_old.generic_u8string()) << "\""
        << ",\"path\":\"" << json_escape(rel_new.generic_u8string()) << "\""
        << "}";
    send_json(c, 200, oss.str());
  }




  void handle_note_get(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& query_string) {
    const std::map<std::string, std::string> qp = parse_query_params(query_string);
    std::string run_dir_raw;
    auto it = qp.find("run_dir");
    if (it != qp.end()) run_dir_raw = it->second;
    if (run_dir_raw.empty()) {
      auto it2 = qp.find("path");
      if (it2 != qp.end()) run_dir_raw = it2->second;
    }
    if (run_dir_raw.empty()) {
      send_json(c, 400, "{\"error\":\"missing run_dir\"}");
      return;
    }

    auto normalize = [](std::string s) {
      s = trim(s);
      std::replace(s.begin(), s.end(), '\\', '/');
      while (!s.empty() && s.front() == '/') s.erase(s.begin());
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (s == ".") s.clear();
      return s;
    };

    const std::string run_dir_norm = normalize(run_dir_raw);
    if (run_dir_norm.empty()) {
      send_json(c, 400, "{\"error\":\"empty run_dir\"}");
      return;
    }

    // Notes are restricted to ui_runs/<run>/note.md.
    if (run_dir_norm == "ui_runs" || !starts_with(run_dir_norm, "ui_runs/")) {
      send_json(c, 403, "{\"error\":\"notes are restricted to ui_runs/<run>\"}");
      return;
    }

    std::filesystem::path rel;
    try {
      rel = std::filesystem::u8path(run_dir_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }

    if (rel.is_absolute()) {
      send_json(c, 403, "{\"error\":\"absolute paths not allowed\"}");
      return;
    }

    for (const auto& part : rel) {
      const std::string p = part.u8string();
      if (p == "..") {
        send_json(c, 403, "{\"error\":\"path traversal not allowed\"}");
        return;
      }
    }

    const std::filesystem::path run_abs = root_ / rel;
    std::error_code ec;
    if (!std::filesystem::exists(run_abs, ec) || !std::filesystem::is_directory(run_abs, ec)) {
      send_json(c, 404, "{\"error\":\"run_dir not found\"}");
      return;
    }

    if (!path_is_within_root(root_canon_, run_abs)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    constexpr size_t kMaxNote = 128 * 1024;
    const std::filesystem::path note_abs = run_abs / "note.md";
    const std::string note_rel = (rel / "note.md").generic_u8string();

    bool exists = false;
    bool truncated = false;
    std::string text;

    ec.clear();
    if (std::filesystem::exists(note_abs, ec)) {
      if (std::filesystem::is_directory(note_abs, ec)) {
        send_json(c, 400, "{\"error\":\"note.md is a directory\"}");
        return;
      }
      if (std::filesystem::is_symlink(note_abs, ec)) {
        send_json(c, 403, "{\"error\":\"note.md symlink not allowed\"}");
        return;
      }

      // Prevent symlink escapes even if note.md exists as a link.
      if (!path_is_within_root(root_canon_, note_abs)) {
        send_json(c, 403, "{\"error\":\"forbidden\"}");
        return;
      }

      exists = true;
      const uintmax_t sz = std::filesystem::file_size(note_abs, ec);
      if (ec) {
        send_json(c, 500, "{\"error\":\"cannot stat note\"}");
        return;
      }
      const size_t take = (sz > kMaxNote) ? kMaxNote : static_cast<size_t>(sz);
      truncated = (sz > kMaxNote);

      std::ifstream f(note_abs, std::ios::binary);
      if (!f) {
        send_json(c, 500, "{\"error\":\"cannot read note\"}");
        return;
      }
      text.resize(take);
      if (take > 0) {
        f.read(&text[0], static_cast<std::streamsize>(take));
        if (!f) {
          send_json(c, 500, "{\"error\":\"cannot read note\"}");
          return;
        }
      }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"run_dir\":\"" << json_escape(run_dir_norm) << "\""
        << ",\"note\":\"" << json_escape(note_rel) << "\""
        << ",\"exists\":" << (exists ? "true" : "false")
        << ",\"truncated\":" << (truncated ? "true" : "false")
        << ",\"text\":\"" << json_escape(text) << "\""
        << "}";
    send_json(c, 200, oss.str());
  }

  void handle_note_set(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    std::string run_dir_raw = json_find_string_value(body, "run_dir");
    if (run_dir_raw.empty()) run_dir_raw = json_find_string_value(body, "path");
    const std::string text = json_find_string_value(body, "text");

    if (run_dir_raw.empty()) {
      send_json(c, 400, "{\"error\":\"missing run_dir\"}");
      return;
    }

    auto normalize = [](std::string s) {
      s = trim(s);
      std::replace(s.begin(), s.end(), '\\', '/');
      while (!s.empty() && s.front() == '/') s.erase(s.begin());
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (s == ".") s.clear();
      return s;
    };

    const std::string run_dir_norm = normalize(run_dir_raw);
    if (run_dir_norm.empty()) {
      send_json(c, 400, "{\"error\":\"empty run_dir\"}");
      return;
    }

    if (run_dir_norm == "ui_runs" || !starts_with(run_dir_norm, "ui_runs/")) {
      send_json(c, 403, "{\"error\":\"notes are restricted to ui_runs/<run>\"}");
      return;
    }

    constexpr size_t kMaxNote = 128 * 1024;
    if (text.size() > kMaxNote) {
      send_json(c, 413, "{\"error\":\"note too large (max 128KB)\"}");
      return;
    }

    std::filesystem::path rel;
    try {
      rel = std::filesystem::u8path(run_dir_norm);
    } catch (...) {
      send_json(c, 400, "{\"error\":\"bad path\"}");
      return;
    }

    if (rel.is_absolute()) {
      send_json(c, 403, "{\"error\":\"absolute paths not allowed\"}");
      return;
    }

    for (const auto& part : rel) {
      const std::string p = part.u8string();
      if (p == "..") {
        send_json(c, 403, "{\"error\":\"path traversal not allowed\"}");
        return;
      }
    }

    const std::filesystem::path run_abs = root_ / rel;
    std::error_code ec;
    if (!std::filesystem::exists(run_abs, ec) || !std::filesystem::is_directory(run_abs, ec)) {
      send_json(c, 404, "{\"error\":\"run_dir not found\"}");
      return;
    }

    if (!path_is_within_root(root_canon_, run_abs)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    const std::filesystem::path note_abs = run_abs / "note.md";
    const std::string note_rel = (rel / "note.md").generic_u8string();

    ec.clear();
    if (std::filesystem::exists(note_abs, ec) && std::filesystem::is_symlink(note_abs, ec)) {
      send_json(c, 403, "{\"error\":\"note.md symlink not allowed\"}");
      return;
    }

    if (!path_is_within_root(root_canon_, note_abs)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    std::ofstream f(note_abs, std::ios::binary | std::ios::trunc);
    if (!f) {
      send_json(c, 500, "{\"error\":\"cannot write note\"}");
      return;
    }

    if (!text.empty()) {
      f.write(text.data(), static_cast<std::streamsize>(text.size()));
      if (!f) {
        send_json(c, 500, "{\"error\":\"cannot write note\"}");
        return;
      }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"run_dir\":\"" << json_escape(run_dir_norm) << "\""
        << ",\"note\":\"" << json_escape(note_rel) << "\""
        << ",\"bytes\":" << static_cast<uintmax_t>(text.size())
        << "}";
    send_json(c, 200, oss.str());
  }







  void handle_presets_get(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& /*query_string*/ = std::string()) {
    // Persist UI presets under the served root so they survive browser refreshes and
    // can be shared across machines.
    constexpr uintmax_t kMaxPresets = 512 * 1024; // 512KB

    const std::filesystem::path p = root_ / "qeeg_ui_presets.json";
    if (!path_is_within_root(root_canon_, p)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(p, ec);
    if (ec) {
      send_json(c, 500, "{\"error\":\"cannot stat presets\"}");
      return;
    }

    std::string data;
    if (exists) {
      if (std::filesystem::is_directory(p, ec)) {
        send_json(c, 400, "{\"error\":\"presets is a directory\"}");
        return;
      }
      if (std::filesystem::is_symlink(p, ec)) {
        send_json(c, 403, "{\"error\":\"presets symlink not allowed\"}");
        return;
      }
      if (!read_file_binary_bounded(p, kMaxPresets, &data)) {
        send_json(c, 413, "{\"error\":\"presets too large or unreadable\"}");
        return;
      }
    }

    std::string json = trim(data);
    if (json.find('\0') != std::string::npos) json.clear();

    // Validate minimal JSON (we only accept an object).
    char first = 0;
    for (char ch : json) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) continue;
      first = ch;
      break;
    }
    if (json.empty() || first != '{') {
      json = "{}";
    }

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"exists\":" << (exists ? "true" : "false")
        << ",\"bytes\":" << static_cast<uintmax_t>(data.size())
        << ",\"presets\":" << json
        << "}";
    send_json(c, 200, oss.str());
  }

  void handle_presets_set(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    constexpr uintmax_t kMaxPresets = 512 * 1024; // 512KB

    std::string json = trim(body);
    if (json.empty()) json = "{}"; // allow clearing

    if (json.find('\0') != std::string::npos) {
      send_json(c, 400, "{\"error\":\"invalid presets payload\"}");
      return;
    }

    if (static_cast<uintmax_t>(json.size()) > kMaxPresets) {
      send_json(c, 413, "{\"error\":\"presets too large (max 512KB)\"}");
      return;
    }

    char first = 0;
    for (char ch : json) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) continue;
      first = ch;
      break;
    }
    if (first != '{') {
      send_json(c, 400, "{\"error\":\"presets must be a JSON object\"}");
      return;
    }

    const std::filesystem::path p = root_ / "qeeg_ui_presets.json";
    if (!path_is_within_root(root_canon_, p)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
      if (std::filesystem::is_directory(p, ec)) {
        send_json(c, 400, "{\"error\":\"presets is a directory\"}");
        return;
      }
      if (std::filesystem::is_symlink(p, ec)) {
        send_json(c, 403, "{\"error\":\"presets symlink not allowed\"}");
        return;
      }
    }

    // Write via a temp file + rename for best-effort atomicity.
    std::filesystem::path tmp = p;
    tmp += ".tmp";

    if (!path_is_within_root(root_canon_, tmp)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    ec.clear();
    if (std::filesystem::exists(tmp, ec)) {
      std::filesystem::remove(tmp, ec);
      ec.clear();
    }

    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) {
        send_json(c, 500, "{\"error\":\"cannot write presets\"}");
        return;
      }
      if (!json.empty()) {
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!f) {
          f.close();
          std::error_code ec2;
          std::filesystem::remove(tmp, ec2);
          send_json(c, 500, "{\"error\":\"cannot write presets\"}");
          return;
        }
      }
    }

#ifdef _WIN32
    // On Windows, std::filesystem::rename may fail if destination exists.
    ec.clear();
    if (std::filesystem::exists(p, ec)) {
      ec.clear();
      std::filesystem::remove(p, ec);
      ec.clear();
    }
#endif

    ec.clear();
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
      // Fallback: copy + delete temp.
      ec.clear();
      std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec);
      std::error_code ec2;
      std::filesystem::remove(tmp, ec2);
      if (ec) {
        send_json(c, 500, "{\"error\":\"cannot finalize presets\"}");
        return;
      }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"bytes\":" << static_cast<uintmax_t>(json.size()) << "}";
    send_json(c, 200, oss.str());
  }


  void handle_delete_run(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    update_jobs();

    std::string run_dir_raw = json_find_string_value(body, "run_dir");
    if (run_dir_raw.empty()) run_dir_raw = json_find_string_value(body, "path");
    if (run_dir_raw.empty()) {
      send_json(c, 400, "{\"error\":\"missing run_dir\"}");
      return;
    }

    auto normalize = [](std::string s) {
      s = trim(s);
      // Convert backslashes to slashes so callers can use either style.
      std::replace(s.begin(), s.end(), '\\', '/');
      while (!s.empty() && s.front() == '/') s.erase(s.begin());
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (s == ".") s.clear();
      return s;
    };

    const std::string run_dir_norm = normalize(run_dir_raw);
    if (run_dir_norm.empty()) {
      send_json(c, 400, "{\"error\":\"empty run_dir\"}");
      return;
    }

    // Safety: only allow deleting per-run folders under ui_runs/.
    if (run_dir_norm == "ui_runs" || !starts_with(run_dir_norm, "ui_runs/")) {
      send_json(c, 403, "{\"error\":\"delete is restricted to ui_runs/<run>\"}");
      return;
    }

    std::filesystem::path rel = std::filesystem::u8path(run_dir_norm);

    // Prevent absolute paths and traversal outside the served root.
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
      send_json(c, 403, "{\"error\":\"absolute paths not allowed\"}");
      return;
    }
    for (const auto& part : rel) {
      if (part == "..") {
        send_json(c, 403, "{\"error\":\"path traversal not allowed\"}");
        return;
      }
    }

    const std::filesystem::path abs = root_ / rel;
    std::error_code ec;
    if (!std::filesystem::exists(abs, ec)) {
      send_json(c, 404, "{\"error\":\"run_dir not found\"}");
      return;
    }
    if (!std::filesystem::is_directory(abs, ec)) {
      send_json(c, 400, "{\"error\":\"run_dir is not a directory\"}");
      return;
    }

    // Prevent escaping the served root through symlinks.
    if (!path_is_within_root(root_canon_, abs)) {
      send_json(c, 403, "{\"error\":\"path not allowed\"}");
      return;
    }

    // Don't allow deleting active jobs.
    for (const auto& j : jobs_) {
      if (j.run_dir_rel == run_dir_norm &&
          (j.status == "running" || j.status == "stopping" || j.status == "queued")) {
        send_json(c, 409, "{\"error\":\"run is active\"}");
        return;
      }
    }

    const uintmax_t removed = std::filesystem::remove_all(abs, ec);
    if (ec) {
      std::ostringstream oss;
      oss << "{\"error\":\"delete failed\",\"detail\":\"" << qeeg::json_escape(ec.message()) << "\"}";
      send_json(c, 500, oss.str());
      return;
    }

    // Remove any matching jobs from the live list (safe because we rejected active statuses).
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [&](const RunJob& j) { return j.run_dir_rel == run_dir_norm; }),
                jobs_.end());

    std::ostringstream oss;
    oss << "{\"ok\":true,\"removed\":" << removed << "}";
    send_json(c, 200, oss.str());
  }


  RunJob* find_job(int id) {
    for (auto& j : jobs_) {
      if (j.id == id) return &j;
    }
    return nullptr;
  }

  void handle_job(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id) {
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_json(c, 404, "{\"error\":\"job not found\"}");
      return;
    }

    int queue_pos = 0;
    int queue_len = 0;
    if (j->status == "queued") {
      for (const auto& x : jobs_) {
        if (x.status == "queued") {
          ++queue_len;
          if (x.id == j->id) queue_pos = queue_len;
        }
      }
    }

    std::ostringstream oss;
    oss << "{\"id\":" << j->id
        << ",\"tool\":\"" << qeeg::json_escape(j->tool) << "\"" 
        << ",\"args\":\"" << qeeg::json_escape(j->args) << "\"" 
        << ",\"started\":\"" << qeeg::json_escape(j->started) << "\"" 
        << ",\"status\":\"" << qeeg::json_escape(j->status) << "\"" 
        << ",\"exit_code\":" << j->exit_code
        << ",\"run_dir\":\"" << qeeg::json_escape(j->run_dir_rel) << "\"" 
        << ",\"log\":\"" << qeeg::json_escape(j->log_rel) << "\"" 
        << ",\"meta\":\"" << qeeg::json_escape(j->meta_rel) << "\"" 
        << ",\"input_path\":\"" << qeeg::json_escape(j->input_path) << "\"" ;

    if (queue_len > 0) {
      oss << ",\"queue_pos\":" << queue_pos
          << ",\"queue_len\":" << queue_len;
    }

    oss << "}";
    send_json(c, 200, oss.str());
  }


  static std::string read_file_tail_bytes(const std::filesystem::path& p, size_t max_bytes) {
    if (max_bytes == 0) max_bytes = 64 * 1024;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) return {};

    const uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) return {};

    std::ifstream f(p, std::ios::binary);
    if (!f) return {};

    uintmax_t off = 0;
    if (sz > max_bytes) off = sz - static_cast<uintmax_t>(max_bytes);
    if (off > 0) {
      f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
      if (!f) return {};
    }

    const size_t want = static_cast<size_t>(std::min<uintmax_t>(sz, max_bytes));
    std::string out;
    out.resize(want);
    if (want > 0) {
      f.read(&out[0], static_cast<std::streamsize>(want));
      const std::streamsize got = f.gcount();
      if (got < 0) return {};
      out.resize(static_cast<size_t>(got));
    }
    return out;
  }

  void handle_log_delta(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id,
      const std::string& query_string) {
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_json(c, 404, "{\"error\":\"job not found\"}");
      return;
    }

    const std::filesystem::path p = root_ / std::filesystem::u8path(j->log_rel);
    if (!path_is_within_root(root_canon_, p)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    // Parse query parameters:
    //   offset=<u64>  (byte offset)
    //   max=<u64>     (max bytes to return, capped)
    const std::map<std::string, std::string> qp = parse_query_params(query_string);
    uintmax_t offset = 0;
    size_t max_bytes = 64 * 1024;
    {
      auto it = qp.find("offset");
      if (it != qp.end()) offset = parse_u64(it->second, 0);
    }
    {
      auto it = qp.find("max");
      if (it != qp.end()) {
        const uintmax_t mv = parse_u64(it->second, static_cast<uintmax_t>(max_bytes));
        if (mv > 0) max_bytes = static_cast<size_t>(std::min<uintmax_t>(mv, 1024ull * 1024ull));
      }
    }
    // Keep responses small (the UI will poll).
    const size_t hard_cap = 256 * 1024;
    if (max_bytes > hard_cap) max_bytes = hard_cap;

    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) {
      send_json(c, 200, "{\"ok\":true,\"offset\":0,\"size\":0,\"eof\":true,\"truncated\":false,\"text\":\"\"}");
      return;
    }

    const uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) {
      send_json(c, 500, "{\"error\":\"file_size failed\"}");
      return;
    }

    bool truncated = false;
    if (offset == 0 && sz > static_cast<uintmax_t>(max_bytes)) {
      // Treat offset=0 as "give me the tail" for large logs.
      offset = sz - static_cast<uintmax_t>(max_bytes);
      truncated = true;
    }
    if (offset > sz) offset = sz;

    const uintmax_t remain = sz - offset;
    const size_t want = static_cast<size_t>(std::min<uintmax_t>(remain, static_cast<uintmax_t>(max_bytes)));

    std::ifstream f(p, std::ios::binary);
    if (!f) {
      send_json(c, 500, "{\"error\":\"open failed\"}");
      return;
    }
    if (offset > 0) {
      f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
      if (!f) {
        send_json(c, 500, "{\"error\":\"seek failed\"}");
        return;
      }
    }

    std::string chunk;
    chunk.resize(want);
    if (want > 0) {
      f.read(&chunk[0], static_cast<std::streamsize>(want));
      const std::streamsize got = f.gcount();
      if (got < 0) chunk.clear();
      else chunk.resize(static_cast<size_t>(got));
    } else {
      chunk.clear();
    }

    const uintmax_t next = offset + static_cast<uintmax_t>(chunk.size());
    const bool eof = (next >= sz);

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"offset\":" << next
        << ",\"size\":" << sz
        << ",\"eof\":" << (eof ? "true" : "false")
        << ",\"truncated\":" << (truncated ? "true" : "false")
        << ",\"text\":\"" << qeeg::json_escape(chunk) << "\"";
    oss << "}";
    send_json(c, 200, oss.str());
  }

  void handle_log_tail(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id) {
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_text(c, 404, "job not found\n");
      return;
    }
    const std::filesystem::path p = root_ / std::filesystem::u8path(j->log_rel);
    if (!path_is_within_root(root_canon_, p)) {
      send_text(c, 403, "forbidden\n");
      return;
    }
    const std::string tail = read_file_tail_bytes(p, 64 * 1024);
    send_text(c, 200, tail, "text/plain; charset=utf-8");
  }


  void handle_zip(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id) {
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_json(c, 404, "{\"error\":\"job not found\"}");
      return;
    }

    const std::filesystem::path run_dir = root_ / std::filesystem::u8path(j->run_dir_rel);
    std::error_code ec;
    if (!std::filesystem::exists(run_dir, ec) || !std::filesystem::is_directory(run_dir, ec)) {
      send_json(c, 404, "{\"error\":\"run_dir not found\"}");
      return;
    }

    if (!path_is_within_root(root_canon_, run_dir)) {
      send_json(c, 403, "{\"error\":\"forbidden\"}");
      return;
    }

    // Gather candidate artifact paths.
    const std::filesystem::path meta_abs = root_ / std::filesystem::u8path(j->meta_rel);
    std::vector<std::string> rels;
    ec.clear();
    if (!j->meta_rel.empty() && std::filesystem::exists(meta_abs, ec) && std::filesystem::is_regular_file(meta_abs, ec)) {
      rels = qeeg::read_run_meta_outputs(meta_abs.u8string());
    }
    ec.clear();
    if (rels.empty()) {
      // Fallback: scan run dir (bounded) so users can download partial outputs even
      // while a job is still running.
      rels = scan_run_dir_outputs(run_dir, meta_abs, 2000);
    }

    // Ensure a few canonical artifacts (if present).
    auto ensure_rel = [&](const std::string& rel) {
      const std::filesystem::path p = run_dir / std::filesystem::u8path(rel);
      std::error_code e2;
      if (std::filesystem::exists(p, e2)) rels.push_back(rel);
    };
    ensure_rel("run.log");
    ensure_rel("command.txt");
    ensure_rel("exit_status.txt");

    std::sort(rels.begin(), rels.end());
    rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

    const std::string folder_raw = run_dir.filename().u8string();
    const std::string folder = zip_sanitize_component(folder_raw.empty() ? ("run_" + std::to_string(id)) : folder_raw);
    const uintmax_t kMaxFile = 25ull * 1024ull * 1024ull;
    const uintmax_t kMaxTotal = 80ull * 1024ull * 1024ull;

    std::string zip;
    std::vector<ZipCdEntry> cd;
    cd.reserve(std::min<size_t>(rels.size() + 4, 2048));

    uintmax_t total_uncompressed = 0;
    std::vector<std::string> skipped;

    auto add_notice = [&](const std::string& msg) {
      if (!msg.empty()) skipped.push_back(msg);
    };

    for (const auto& rel0 : rels) {
      const std::string rel = zip_normalize_relpath(rel0);
      if (rel.empty()) continue;

      // We'll add meta explicitly at the end under a stable name.
      if (rel == "ui_server_run_meta.json") continue;

      const std::filesystem::path abs = run_dir / std::filesystem::u8path(rel);
      if (!path_is_within_root(root_canon_, abs)) {
        add_notice(rel + " (outside root)");
        continue;
      }

      ec.clear();
      if (!std::filesystem::exists(abs, ec) || std::filesystem::is_directory(abs, ec)) {
        ec.clear();
        continue;
      }

      const uintmax_t sz = std::filesystem::file_size(abs, ec);
      if (ec) {
        ec.clear();
        continue;
      }

      if (sz > kMaxFile) {
        add_notice(rel + " (skipped: file too large)");
        continue;
      }

      if (total_uncompressed + sz > kMaxTotal) {
        add_notice(rel + " (skipped: archive size limit)");
        continue;
      }

      std::string data;
      if (!read_file_binary_bounded(abs, kMaxFile, &data)) {
        add_notice(rel + " (skipped: read failed)");
        continue;
      }

      total_uncompressed += static_cast<uintmax_t>(data.size());

      uint16_t dt = 0, dd = 0;
      const auto mtime = std::filesystem::last_write_time(abs, ec);
      ec.clear();
      zip_dos_datetime(mtime, &dt, &dd);

      const std::string zip_name = folder + "/" + rel;
      std::string err;
      if (!zip_add_store_entry(&zip, &cd, zip_name, data, dt, dd, &err)) {
        std::ostringstream oss;
        oss << "{\"error\":\"zip build failed\",\"detail\":\"" << qeeg::json_escape(err) << "\"}";
        send_json(c, 500, oss.str());
        return;
      }
    }

    // Include the job meta file itself (if present).
    ec.clear();
    if (!j->meta_rel.empty() && std::filesystem::exists(meta_abs, ec) && std::filesystem::is_regular_file(meta_abs, ec)) {
      const uintmax_t sz = std::filesystem::file_size(meta_abs, ec);
      if (!ec && sz <= kMaxFile && total_uncompressed + sz <= kMaxTotal && path_is_within_root(root_canon_, meta_abs)) {
        std::string data;
        if (read_file_binary_bounded(meta_abs, kMaxFile, &data)) {
          total_uncompressed += static_cast<uintmax_t>(data.size());
          uint16_t dt = 0, dd = 0;
          const auto mtime = std::filesystem::last_write_time(meta_abs, ec);
          ec.clear();
          zip_dos_datetime(mtime, &dt, &dd);
          std::string err;
          (void)zip_add_store_entry(&zip, &cd, folder + "/ui_server_run_meta.json", data, dt, dd, &err);
        }
      }
      ec.clear();
    }

    // If we skipped anything, include a small note inside the ZIP.
    if (!skipped.empty()) {
      std::ostringstream note;
      note << "Some files were not included in this archive (safety limits).\n\n";
      for (const auto& s : skipped) note << " - " << s << "\n";
      note << "\n";
      note << "Max per-file: " << (kMaxFile / (1024 * 1024)) << " MiB\n";
      note << "Max total:    " << (kMaxTotal / (1024 * 1024)) << " MiB\n";
      uint16_t dt = 0, dd = 0;
      zip_dos_datetime_now(&dt, &dd);
      std::string err;
      (void)zip_add_store_entry(&zip, &cd, folder + "/_ZIP_NOTICE.txt", note.str(), dt, dd, &err);
    }

    std::string zip_err;
    if (!zip_finalize_store(&zip, cd, &zip_err)) {
      std::ostringstream oss;
      oss << "{\"error\":\"zip finalize failed\",\"detail\":\"" << qeeg::json_escape(zip_err) << "\"}";
      send_json(c, 500, oss.str());
      return;
    }

    // Serve as a download.
    const std::string filename = zip_sanitize_component(folder) + ".zip";
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n";
    hdr << "Content-Type: application/zip\r\n";
    hdr << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n";
    hdr << "Cache-Control: no-store\r\n";
    hdr << "Content-Length: " << zip.size() << "\r\n";
    hdr << "Connection: close\r\n";
    hdr << "X-Content-Type-Options: nosniff\r\n";
    hdr << "X-Frame-Options: DENY\r\n";
    hdr << "Referrer-Policy: no-referrer\r\n";
    hdr << "Cross-Origin-Resource-Policy: same-origin\r\n";
    hdr << "\r\n";
    const std::string h = hdr.str();
    send_all(c, h.data(), h.size());
    send_all(c, zip.data(), zip.size());
  }


  void handle_kill(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id) {
#ifdef _WIN32
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_json(c, 404, "{\"error\":\"job not found\"}");
      return;
    }
    if (j->status == "queued") {
      j->status = "canceled";
      j->exit_code = 130;
      finalize_ui_run_meta(j);
      send_json(c, 200, "{\"ok\":true,\"status\":\"canceled\"}");
      return;
    }
    if (j->status != "running" && j->status != "stopping") {
      std::ostringstream oss;
      oss << "{\"ok\":true,\"status\":\"" << qeeg::json_escape(j->status) << "\"}";
      send_json(c, 200, oss.str());
      return;
    }
    if (!j->process_handle) {
      send_json(c, 500, "{\"error\":\"no process handle\"}");
      return;
    }
    if (TerminateProcess(j->process_handle, 1) == 0) {
      send_json(c, 500, "{\"error\":\"terminate failed\"}");
      return;
    }
    j->status = "stopping";
    send_json(c, 200, "{\"ok\":true,\"status\":\"stopping\"}");
#else
    update_jobs();
    RunJob* j = find_job(id);
    if (!j) {
      send_json(c, 404, "{\"error\":\"job not found\"}");
      return;
    }
    if (j->status != "running" && j->status != "stopping") {
      std::ostringstream oss;
      oss << "{\"ok\":true,\"status\":\"" << qeeg::json_escape(j->status) << "\"}";
      send_json(c, 200, oss.str());
      return;
    }
    if (j->pid <= 0) {
      send_json(c, 500, "{\"error\":\"no pid\"}");
      return;
    }
    if (kill(j->pid, SIGTERM) != 0) {
      send_json(c, 500, "{\"error\":\"kill failed\"}");
      return;
    }
    j->status = "stopping";
    send_json(c, 200, "{\"ok\":true,\"status\":\"stopping\"}");
#endif
  }


  void handle_run(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string tool = json_find_string_value(body, "tool");
    const std::string args = json_find_string_value(body, "args");
    if (tool.empty()) {
      send_json(c, 400, "{\"error\":\"missing tool\"}");
      return;
    }
    if (!looks_like_qeeg_cli(tool)) {
      send_json(c, 403, "{\"error\":\"tool not allowed\"}");
      return;
    }

    const std::filesystem::path exe = resolve_exe_path(bin_dir_, tool);
    if (exe.empty()) {
      send_json(c, 404, "{\"error\":\"tool not found in bin-dir\"}");
      return;
    }

    // Keep job statuses fresh so our concurrency limiter has an accurate view.
    update_jobs();

    const int job_id = ++next_job_id_;

    auto sanitize_component = [](std::string s) {
      for (char& c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '_' || c == '-') continue;
        c = '_';
      }
      return s;
    };
    auto replace_all_inplace = [](std::string* s, const std::string& from, const std::string& to) {
      if (!s || from.empty()) return;
      size_t pos = 0;
      while ((pos = s->find(from, pos)) != std::string::npos) {
        s->replace(pos, from.size(), to);
        pos += to.size();
      }
    };

    // Create run directory under root.
    const std::string stamp = now_compact_local();
    const std::string safe_tool = sanitize_component(qeeg::to_lower(tool));
    const std::string run_dir_rel =
        std::string("ui_runs/") + (stamp + "_" + safe_tool + "_id" + std::to_string(job_id));
    const std::filesystem::path run_dir = root_ / std::filesystem::u8path(run_dir_rel);
    qeeg::ensure_directory(run_dir.u8string());
    const std::filesystem::path log_path = run_dir / "run.log";

    // Allow simple placeholders for convenience.
    //
    // Example:
    //   --outdir {{RUN_DIR}}/out_map
    //   --outdir {{RUN_DIR_ABS}}/out_map
    std::string expanded_args = args;
    replace_all_inplace(&expanded_args, "{{RUN_DIR}}", run_dir_rel);
    replace_all_inplace(&expanded_args, "{{RUN_DIR_ABS}}", run_dir.u8string());

    // Write lightweight per-run metadata so qeeg_ui_cli can auto-discover
    // UI-launched runs and surface their artifacts.
    const std::string input_path = infer_input_path_from_args(expanded_args);
    const std::string meta_rel = run_dir_rel + "/ui_server_run_meta.json";
    const std::filesystem::path meta_path = run_dir / "ui_server_run_meta.json";
    const std::filesystem::path cmd_path = run_dir / "command.txt";

    // Touch the log so the UI can link it immediately (even if the job is queued).
    {
      std::ofstream(log_path, std::ios::binary);
    }

    const std::string started = qeeg::now_string_local();
    {
      std::ofstream f(cmd_path, std::ios::binary);
      if (f) {
        f << "tool: " << tool << "\n";
        f << "started: " << started << "\n";
        f << "cwd: " << root_.u8string() << "\n";
        f << "command: \"" << exe.u8string() << "\"";
        if (!expanded_args.empty()) f << ' ' << expanded_args;
        f << "\n";
        if (!input_path.empty()) f << "input_path: " << input_path << "\n";
      }
    }

    {
      const std::vector<std::string> outputs = {"run.log", "command.txt"};
      qeeg::write_run_meta_json(meta_path.u8string(), tool, run_dir_rel, input_path, outputs);
    }

    // Decide whether we can start immediately or should queue.
    const size_t limit = parallel_limit();
    const size_t active = count_active_jobs();
    const bool can_start_now = (active < limit);

    RunJob job;
    job.id = job_id;
    job.tool = tool;
    job.args = expanded_args;
    job.started = started;
    job.run_dir_rel = run_dir_rel;
    job.log_rel = run_dir_rel + "/run.log";
    job.meta_rel = meta_rel;
    job.input_path = input_path;

    if (!can_start_now) {
      job.status = "queued";
      jobs_.push_back(job);

      // Best-effort: include queue position so the UI can show it immediately.
      int qpos = 0;
      int qlen = 0;
      for (const auto& x : jobs_) {
        if (x.status == "queued") {
          ++qlen;
          if (x.id == job.id) qpos = qlen;
        }
      }

      std::ostringstream oss;
      oss << "{\"ok\":true,\"id\":" << job.id
          << ",\"status\":\"queued\""
          << ",\"queue_pos\":" << qpos
          << ",\"queue_len\":" << qlen
          << ",\"run_dir\":\"" << qeeg::json_escape(job.run_dir_rel) << "\""
          << ",\"log\":\"" << qeeg::json_escape(job.log_rel) << "\""
          << ",\"meta\":\"" << qeeg::json_escape(job.meta_rel) << "\""
          << ",\"input_path\":\"" << qeeg::json_escape(job.input_path) << "\""
          << "}";
      send_json(c, 200, oss.str());
      return;
    }

    std::string err;
    if (!start_job_process(&job, &err)) {
      append_text_line_best_effort(log_path, std::string("ERROR: failed to start job: ") + err);
      std::ostringstream oss;
      oss << "{\"error\":\"failed to start job\",\"detail\":\"" << qeeg::json_escape(err) << "\"}";
      send_json(c, 500, oss.str());
      return;
    }

    job.status = "running";
    jobs_.push_back(job);

    // For consistency with queued jobs, record the actual launch timestamp too.
    append_text_line_best_effort(cmd_path, std::string("launched: ") + qeeg::now_string_local());

    std::ostringstream oss;
    oss << "{\"ok\":true,\"id\":" << job.id
        << ",\"status\":\"" << qeeg::json_escape(job.status) << "\""
        << ",\"run_dir\":\"" << qeeg::json_escape(job.run_dir_rel) << "\""
        << ",\"log\":\"" << qeeg::json_escape(job.log_rel) << "\""
        << ",\"meta\":\"" << qeeg::json_escape(job.meta_rel) << "\""
        << ",\"input_path\":\"" << qeeg::json_escape(job.input_path) << "\""
        << "}";
    send_json(c, 200, oss.str());
  }



  void send_headers(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int code,
      const std::string& content_type,
      uintmax_t content_length,
      const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
      bool no_store = false) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << http_status_text(code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << content_length << "\r\n";
    oss << "Connection: close\r\n";
    if (no_store) oss << "Cache-Control: no-store\r\n";
    oss << "X-Content-Type-Options: nosniff\r\n";
    oss << "X-Frame-Options: DENY\r\n";
    oss << "Referrer-Policy: no-referrer\r\n";
    oss << "Cross-Origin-Resource-Policy: same-origin\r\n";
    for (const auto& kv : extra_headers) {
      if (!kv.first.empty()) oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "\r\n";
    const std::string hdr = oss.str();
    (void)send_all(c, hdr.data(), hdr.size());
  }

  void send_text(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int code, const std::string& body,
      const std::string& content_type = "text/plain; charset=utf-8",
      const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
      bool no_store = false) {
    send_headers(c, code, content_type, static_cast<uintmax_t>(body.size()), extra_headers, no_store);
    (void)send_all(c, body.data(), body.size());
  }

  void send_json(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int code, const std::string& json) {
    send_text(c, code, json, "application/json; charset=utf-8", {}, true);
  }


  bool send_all(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const char* data, size_t n) {
    if (!data || n == 0) return true;
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      int rc = send(c, data + off, static_cast<int>(n - off), 0);
#else
      ssize_t rc = send(c, data + off, n - off, 0);
#endif
      if (rc <= 0) return false;
      off += static_cast<size_t>(rc);
    }
    return true;
  }


  void serve_directory_listing(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::filesystem::path& dir_abs,
      const std::string& url_path,
      bool head_only = false) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_abs, ec) || !std::filesystem::is_directory(dir_abs, ec)) {
      send_text(c, 404, "not found\n");
      return;
    }

    // Prevent symlink escapes.
    if (!path_is_within_root(root_canon_, dir_abs)) {
      send_text(c, 403, "forbidden\n");
      return;
    }

    struct Ent {
      std::string name;
      std::filesystem::path abs;
      bool is_dir{false};
      uintmax_t size{0};
      std::filesystem::file_time_type mtime{};
    };

    std::vector<Ent> ents;
    ents.reserve(256);
    bool truncated = false;
    const size_t kMaxEntries = 2000;

    for (auto it = std::filesystem::directory_iterator(dir_abs, ec);
         it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (ec) break;
      if (ents.size() >= kMaxEntries) {
        truncated = true;
        break;
      }

      const std::filesystem::path p = it->path();
      // Skip entries that would escape the served root.
      if (!path_is_within_root(root_canon_, p)) continue;

      Ent e;
      e.abs = p;
      e.name = p.filename().u8string();
      e.is_dir = it->is_directory(ec);
      ec.clear();
      if (!e.is_dir) {
        const uintmax_t sz = std::filesystem::file_size(p, ec);
        if (!ec) e.size = sz;
        ec.clear();
      }
      e.mtime = std::filesystem::last_write_time(p, ec);
      ec.clear();

      ents.push_back(std::move(e));
    }

    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
      if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
      return to_lower(a.name) < to_lower(b.name);
    });

    auto human_size = [](uintmax_t n) {
      const char* units[] = {"B", "KB", "MB", "GB", "TB"};
      double v = static_cast<double>(n);
      size_t u = 0;
      while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
      }
      std::ostringstream oss;
      if (u == 0) {
        oss << static_cast<uintmax_t>(v) << ' ' << units[u];
      } else {
        oss << std::fixed << std::setprecision(v >= 10.0 ? 1 : 2) << v << ' ' << units[u];
      }
      return oss.str();
    };

    // Compute links.
    const std::filesystem::path rel_dir = std::filesystem::relative(dir_abs, root_, ec);
    ec.clear();

    std::string parent_href;
    if (rel_dir.empty() || rel_dir == std::filesystem::path(".")) {
      parent_href.clear();
    } else {
      std::filesystem::path parent_rel = rel_dir.parent_path();
      std::string s = parent_rel.generic_u8string();
      if (s.empty() || s == ".") parent_href = "/";
      else parent_href = "/" + url_escape_path(s) + "/";
    }

    std::ostringstream html;
    html << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    html << "<title>Index of " << html_escape(url_path) << "</title>";
    html << "<style>";
    html << "body{font-family:system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:18px;color:#111}";
    html << "code{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;font-size:12px}";
    html << "table{width:100%;border-collapse:collapse;margin-top:10px}";
    html << "th,td{padding:8px 10px;border-bottom:1px solid #ddd;font-size:13px;vertical-align:top}";
    html << "th{text-align:left;color:#555;text-transform:uppercase;letter-spacing:.06em;font-size:11px}";
    html << "a{text-decoration:none;color:#0b57d0} a:hover{text-decoration:underline}";
    html << ".muted{color:#666;font-size:12px}";
    html << "</style></head><body>";
    html << "<h2>Index of <code>" << html_escape(url_path) << "</code></h2>";
    html << "<div class=\"muted\">Served from <code>" << html_escape(root_.u8string())
         << "</code></div>";
    if (!parent_href.empty()) {
      html << "<p><a href=\"" << html_escape(parent_href) << "\">&uarr; Parent directory</a></p>";
    }
    html << "<table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Modified</th></tr></thead><tbody>";

    for (const auto& e : ents) {
      std::filesystem::path rel = std::filesystem::relative(e.abs, root_, ec);
      ec.clear();
      std::string rel_s = rel.generic_u8string();
      if (rel_s.empty()) rel_s = e.name;
      std::string href = "/" + url_escape_path(rel_s);
      std::string display = e.name;
      const char* type = e.is_dir ? "dir" : "file";
      if (e.is_dir) {
        href += "/";
        display += "/";
      }
      const std::string mtime = format_local_time(e.mtime);
      html << "<tr><td><a href=\"" << html_escape(href) << "\">" << html_escape(display)
           << "</a></td><td>" << type << "</td><td>";
      if (e.is_dir) html << "";
      else html << html_escape(human_size(e.size));
      html << "</td><td><code>" << html_escape(mtime) << "</code></td></tr>";
    }
    html << "</tbody></table>";
    if (truncated) {
      html << "<p class=\"muted\">Truncated to " << kMaxEntries << " entries.</p>";
    }
    html << "</body></html>";

    const std::string body = html.str();

    std::vector<std::pair<std::string, std::string>> extra;
    extra.emplace_back("Content-Security-Policy", kDashboardCsp);
    if (head_only) {
      send_headers(c, 200, "text/html; charset=utf-8", static_cast<uintmax_t>(body.size()), extra, true);
      return;
    }
    send_text(c, 200, body, "text/html; charset=utf-8", extra, true);
  }

  void serve_file(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::filesystem::path& p,
      const HttpRequest& req,
      bool is_dashboard = false) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) {
      send_text(c, 404, "not found\n");
      return;
    }

    // Stream the file instead of reading it all into memory. This avoids
    // rejecting large EEG outputs (e.g., EDF/BDF) and keeps RAM usage bounded.
    const uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) {
      send_text(c, 404, "not found\n");
      return;
    }

    std::time_t mtime = 0;
    if (!file_mtime_time_t(p, &mtime)) {
      // Still serve the file, but omit validators.
      mtime = 0;
    }

    const bool head_only = (req.method == "HEAD");
    const std::string ct = content_type_for_path(p);

    // Cache validators for conditional requests.
    const std::string etag = make_weak_etag(mtime, sz);
    const std::string last_modified = (mtime > 0) ? format_http_date_gmt(mtime) : std::string();

    std::vector<std::pair<std::string, std::string>> extra;
    extra.emplace_back("Accept-Ranges", "bytes");
    if (!etag.empty()) extra.emplace_back("ETag", etag);
    if (!last_modified.empty()) extra.emplace_back("Last-Modified", last_modified);

    bool no_store = false;
    if (is_dashboard && starts_with(ct, "text/html")) {
      extra.emplace_back("Content-Security-Policy", kDashboardCsp);
      no_store = true;
    } else {
      // Allow caching but require revalidation to avoid stale run outputs.
      extra.emplace_back("Cache-Control", "no-cache");
    }

    // Conditional GET/HEAD: If-None-Match has precedence over If-Modified-Since.
    const auto inm_it = req.headers.find("if-none-match");
    if (inm_it != req.headers.end()) {
      if (if_none_match_allows_304(inm_it->second, etag)) {
        send_headers(c, 304, ct, 0, extra, no_store);
        return;
      }
    } else {
      const auto ims_it = req.headers.find("if-modified-since");
      if (ims_it != req.headers.end() && mtime > 0) {
        std::time_t ims = 0;
        if (parse_http_date_gmt(ims_it->second, &ims)) {
          if (mtime <= ims) {
            send_headers(c, 304, ct, 0, extra, no_store);
            return;
          }
        }
      }
    }

    // Optional: handle a single Range request. If-Range can force a full response.
    uintmax_t rstart = 0;
    uintmax_t rend = 0;
    const auto range_it = req.headers.find("range");
    if (range_it != req.headers.end()) {
      const auto ifr_it = req.headers.find("if-range");
      if (ifr_it != req.headers.end() && mtime > 0) {
        if (!if_range_allows_range(ifr_it->second, etag, mtime)) {
          // Ignore Range and fall through to full response.
        } else {
          const HttpRangeResult rr = parse_http_byte_range(range_it->second, sz, &rstart, &rend);
          if (rr == HttpRangeResult::kUnsatisfiable) {
            std::vector<std::pair<std::string, std::string>> h = extra;
            h.emplace_back("Content-Range", std::string("bytes */") + std::to_string(sz));
            const std::string body = "range not satisfiable\n";
            send_headers(c, 416, "text/plain; charset=utf-8", static_cast<uintmax_t>(body.size()), h, true);
            if (!head_only) (void)send_all(c, body.data(), body.size());
            return;
          }
          if (rr == HttpRangeResult::kSatisfiable) {
            const uintmax_t clen = (rend - rstart) + 1;
            std::vector<std::pair<std::string, std::string>> h = extra;
            h.emplace_back(
                "Content-Range",
                std::string("bytes ") + std::to_string(rstart) + "-" + std::to_string(rend) + "/" + std::to_string(sz));
            send_headers(c, 206, ct, clen, h, no_store);
            if (head_only) return;

            std::ifstream f(p, std::ios::binary);
            if (!f) {
              send_text(c, 404, "not found\n");
              return;
            }

            // Seek to start and stream the requested range.
            f.clear();
            f.seekg(static_cast<std::streamoff>(rstart));
            char buf[64 * 1024];
            uintmax_t remaining = clen;
            while (f && remaining > 0) {
              const size_t want = static_cast<size_t>(std::min<uintmax_t>(remaining, sizeof(buf)));
              f.read(buf, static_cast<std::streamsize>(want));
              const std::streamsize got = f.gcount();
              if (got <= 0) break;
              if (!send_all(c, buf, static_cast<size_t>(got))) break;
              remaining -= static_cast<uintmax_t>(got);
            }
            return;
          }
          // kInvalid: ignore and fall through to full response.
        }
      }

      // If-Range not present (or mtime unavailable): treat like a normal Range request.
      if (ifr_it == req.headers.end() || mtime == 0) {
        const HttpRangeResult rr = parse_http_byte_range(range_it->second, sz, &rstart, &rend);
        if (rr == HttpRangeResult::kUnsatisfiable) {
          std::vector<std::pair<std::string, std::string>> h = extra;
          h.emplace_back("Content-Range", std::string("bytes */") + std::to_string(sz));
          const std::string body = "range not satisfiable\n";
          send_headers(c, 416, "text/plain; charset=utf-8", static_cast<uintmax_t>(body.size()), h, true);
          if (!head_only) (void)send_all(c, body.data(), body.size());
          return;
        }
        if (rr == HttpRangeResult::kSatisfiable) {
          const uintmax_t clen = (rend - rstart) + 1;
          std::vector<std::pair<std::string, std::string>> h = extra;
          h.emplace_back(
              "Content-Range",
              std::string("bytes ") + std::to_string(rstart) + "-" + std::to_string(rend) + "/" + std::to_string(sz));
          send_headers(c, 206, ct, clen, h, no_store);
          if (head_only) return;

          std::ifstream f(p, std::ios::binary);
          if (!f) {
            send_text(c, 404, "not found\n");
            return;
          }

          // Seek to start and stream the requested range.
          f.clear();
          f.seekg(static_cast<std::streamoff>(rstart));
          char buf[64 * 1024];
          uintmax_t remaining = clen;
          while (f && remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uintmax_t>(remaining, sizeof(buf)));
            f.read(buf, static_cast<std::streamsize>(want));
            const std::streamsize got = f.gcount();
            if (got <= 0) break;
            if (!send_all(c, buf, static_cast<size_t>(got))) break;
            remaining -= static_cast<uintmax_t>(got);
          }
          return;
        }
        // kInvalid: ignore and fall through to full response.
      }
    }

    // Full response.
    send_headers(c, 200, ct, sz, extra, no_store);
    if (head_only) return;

    std::ifstream f(p, std::ios::binary);
    if (!f) {
      send_text(c, 404, "not found\n");
      return;
    }

    // Send the body in chunks.
    char buf[64 * 1024];
    while (f) {
      f.read(buf, static_cast<std::streamsize>(sizeof(buf)));
      const std::streamsize got = f.gcount();
      if (got <= 0) break;
      if (!send_all(c, buf, static_cast<size_t>(got))) break;
    }
  }

 private:

  std::filesystem::path root_;
  std::filesystem::path root_canon_;
  std::filesystem::path bin_dir_;
  std::filesystem::path index_html_;
  std::string host_{"127.0.0.1"};
  int port_{8765};
  int max_parallel_{0};
  std::string api_token_;

  std::vector<RunJob> jobs_;
  int next_job_id_{0};
};

} // namespace

int main(int argc, char** argv) {
  try {
    const Args a = parse_args(argc, argv);
    if (a.root.empty() || a.bin_dir.empty()) {
      std::cerr << "qeeg_ui_server_cli: --root and --bin-dir are required (see --help)\n";
      return 2;
    }

    const std::filesystem::path root = std::filesystem::u8path(a.root);
    const std::filesystem::path bin_dir = std::filesystem::u8path(a.bin_dir);

    const std::filesystem::path ui_html = root / "qeeg_ui.html";
    if (!a.no_generate_ui) {
      qeeg::UiDashboardArgs u;
      u.root = a.root;
      u.output_html = ui_html.u8string();
      u.bin_dir = a.bin_dir;
      u.embed_help = a.embed_help;
      u.scan_bin_dir = a.scan_bin_dir;
      u.scan_run_meta = a.scan_run_meta;
      u.title = "QEEG Tools UI";
      qeeg::write_qeeg_tools_ui_html(u);
      std::cout << "(re)generated UI: " << ui_html.u8string() << "\n";
    }

    UiServer s(root, bin_dir);
    s.set_host(a.host);
    s.set_port(a.port);
    s.set_max_parallel(a.max_parallel);
    s.set_index_html(ui_html);

    const std::string token = a.api_token.empty() ? random_hex_token(16) : a.api_token;
    s.set_api_token(token);

    std::cout << "API token (required for /api/* except /api/status): " << token << "\n";
    std::cout << "Example curl: curl -H 'X-QEEG-Token: " << token << "' "
              << "http://" << a.host << ':' << a.port << "/api/runs\n";

    const std::string url = std::string("http://") + a.host + ":" + std::to_string(a.port) + "/";
    if (a.open_after) {
      try_open_browser_url(url);
    }

    s.run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
