#include "qeeg/ui_dashboard.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
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
using qeeg::json_escape;
using qeeg::now_string_local;
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
    << "  qeeg_ui_server_cli --root <dir> --bin-dir <build/bin> [--host 127.0.0.1] [--port 8765] [--open]\n\n"
    << "Options:\n"
    << "  --root DIR          Root directory to serve files from (required).\n"
    << "  --bin-dir DIR       Directory containing qeeg_*_cli executables (required).\n"
    << "  --host HOST         Bind address (default: 127.0.0.1).\n"
    << "  --port N            Port to listen on (default: 8765).\n"
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
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
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

struct RunJob {
  int id{0};
  std::string tool;
  std::string args;
  std::string run_dir_rel;
  std::string log_rel;
  std::string started;
  std::string status{"running"};
  int exit_code{0};
#ifndef _WIN32
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
    : root_(std::move(root)), bin_dir_(std::move(bin_dir)) {}

  void set_index_html(std::filesystem::path p) { index_html_ = std::move(p); }
  void set_host(std::string h) { host_ = std::move(h); }
  void set_port(int p) { port_ = p; }
  void set_api_token(std::string t) { api_token_ = std::move(t); }

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

    // Read remaining body if Content-Length says so.
    size_t want = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
      try { want = static_cast<size_t>(qeeg::to_int(it->second)); } catch (...) { want = 0; }
    }
    if (want > 0 && req.body.size() < want) {
      while (req.body.size() < want) {
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
      if (req.body.size() > want) req.body.resize(want);
    }

    // Strip query string.
    const size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) req.path = req.path.substr(0, qpos);

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

    if (req.path == "/api/list") {
      if (req.method != "POST") {
        send_json(c, 405, "{\"error\":\"method not allowed\"}");
        return;
      }
      handle_list(c, req.body);
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
      serve_file(c, index_html_);
      return;
    }

    // Static file: map URL path to <root>/<path>
    if (!req.path.empty() && req.path[0] == '/') {
      std::filesystem::path rel = std::filesystem::u8path(req.path.substr(1));
      // Prevent ".." traversal.
      for (const auto& part : rel) {
        if (part == "..") {
          send_text(c, 403, "forbidden\n");
          return;
        }
      }
      std::filesystem::path p = root_ / rel;
      if (std::filesystem::is_directory(p)) {
        // Try index.html inside.
        std::filesystem::path idx = p / "index.html";
        if (std::filesystem::exists(idx)) {
          serve_file(c, idx);
          return;
        }
      }
      if (std::filesystem::exists(p)) {
        serve_file(c, p);
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

  void update_jobs() {
#ifndef _WIN32
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
    }
#endif
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
          << "}";
    }
    oss << "]}";
    send_json(c, 200, oss.str());
  }


  struct FsEntry {
    std::string name;
    std::string path;  // relative to root
    bool is_dir{false};
    uintmax_t size{0};
  };

  void handle_list(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::string& body) {
    const std::string dir_raw = json_find_string_value(body, "dir");

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
    if (rel.is_absolute()) {
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
      e.path = (rel / it->path().filename()).generic_u8string();
      e.is_dir = it->is_directory(ec);
      if (!e.is_dir) {
        const uintmax_t sz = std::filesystem::file_size(it->path(), ec);
        if (!ec) e.size = sz;
        ec.clear();
      }
      entries.push_back(std::move(e));
      if (entries.size() >= kMaxEntries) break;
    }

    std::sort(entries.begin(), entries.end(),
              [](const FsEntry& a, const FsEntry& b) {
                if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
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
          << "}";
    }
    oss << "]}";
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
    std::ostringstream oss;
    oss << "{\"id\":" << j->id
        << ",\"tool\":\"" << qeeg::json_escape(j->tool) << "\""
        << ",\"args\":\"" << qeeg::json_escape(j->args) << "\""
        << ",\"started\":\"" << qeeg::json_escape(j->started) << "\""
        << ",\"status\":\"" << qeeg::json_escape(j->status) << "\""
        << ",\"exit_code\":" << j->exit_code
        << ",\"run_dir\":\"" << qeeg::json_escape(j->run_dir_rel) << "\""
        << ",\"log\":\"" << qeeg::json_escape(j->log_rel) << "\"" << "}";
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
    const std::string tail = read_file_tail_bytes(p, 64 * 1024);
    send_text(c, 200, tail, "text/plain; charset=utf-8");
  }

  void handle_kill(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int id) {
#ifdef _WIN32
    (void)id;
    send_json(c, 501, "{\"error\":\"kill API not implemented on Windows in this build\"}");
    return;
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

#ifdef _WIN32
    (void)args;
    send_json(c, 501, "{\"error\":\"run API not implemented on Windows in this build\"}");
    return;
#else
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
    const std::string run_dir_rel = std::string("ui_runs/") + (stamp + "_" + safe_tool + "_id" + std::to_string(job_id));
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

    std::vector<std::string> argv_s;
    argv_s.push_back(exe.u8string());
    for (const auto& t : qeeg::split_commandline_args(expanded_args)) {
      argv_s.push_back(t);
    }

    // Build argv array.
    std::vector<char*> argv;
    argv.reserve(argv_s.size() + 1);
    for (auto& s : argv_s) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
      send_json(c, 500, "{\"error\":\"fork failed\"}");
      return;
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

    // Parent: record job and respond.
    RunJob job;
    job.id = job_id;
    job.tool = tool;
    job.args = expanded_args;
    job.started = qeeg::now_string_local();
    job.run_dir_rel = run_dir_rel;
    job.log_rel = run_dir_rel + "/run.log";
    job.pid = pid;
    jobs_.push_back(job);

    std::ostringstream oss;
    oss << "{\"ok\":true,\"id\":" << job.id
        << ",\"run_dir\":\"" << qeeg::json_escape(job.run_dir_rel) << "\""
        << ",\"log\":\"" << qeeg::json_escape(job.log_rel) << "\"" << "}";
    send_json(c, 200, oss.str());
#endif
  }

  void send_text(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int code, const std::string& body, const std::string& content_type = "text/plain; charset=utf-8") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << http_status_text(code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    const std::string hdr = oss.str();
    send_all(c, hdr.data(), hdr.size());
    send_all(c, body.data(), body.size());
  }

  void send_json(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      int code, const std::string& json) {
    send_text(c, code, json, "application/json; charset=utf-8");
  }

  void send_all(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const char* data, size_t n) {
    if (!data || n == 0) return;
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      int rc = send(c, data + off, static_cast<int>(n - off), 0);
#else
      ssize_t rc = send(c, data + off, n - off, 0);
#endif
      if (rc <= 0) break;
      off += static_cast<size_t>(rc);
    }
  }

  void serve_file(
#ifdef _WIN32
      SOCKET c,
#else
      int c,
#endif
      const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) {
      send_text(c, 404, "not found\n");
      return;
    }

    // Read file into memory (best-effort; keep reasonably bounded).
    const uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec || sz > (50ull * 1024ull * 1024ull)) {
      send_text(c, 413, "file too large\n");
      return;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) {
      send_text(c, 404, "not found\n");
      return;
    }

    std::string body;
    body.resize(static_cast<size_t>(sz));
    if (sz > 0) {
      f.read(&body[0], static_cast<std::streamsize>(sz));
      if (!f) {
        send_text(c, 500, "failed to read file\n");
        return;
      }
    }

    send_text(c, 200, body, content_type_for_path(p));
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path bin_dir_;
  std::filesystem::path index_html_;
  std::string host_{"127.0.0.1"};
  int port_{8765};
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
