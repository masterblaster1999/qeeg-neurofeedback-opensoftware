#include "qeeg/osc.hpp"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace qeeg {
namespace {

static void append_padded_string(std::vector<uint8_t>* out, const std::string& s) {
  if (!out) return;
  out->insert(out->end(), s.begin(), s.end());
  out->push_back(0);
  while ((out->size() % 4) != 0) out->push_back(0);
}

static void append_u32_be(std::vector<uint8_t>* out, uint32_t v) {
  if (!out) return;
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 0) & 0xFF));
}

static void append_u64_be(std::vector<uint8_t>* out, uint64_t v) {
  if (!out) return;
  out->push_back(static_cast<uint8_t>((v >> 56) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 48) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 40) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 32) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 0) & 0xFF));
}

static void append_i32_be(std::vector<uint8_t>* out, int32_t v) {
  append_u32_be(out, static_cast<uint32_t>(v));
}

static void append_f32_be(std::vector<uint8_t>* out, float v) {
  static_assert(sizeof(float) == 4, "float must be 32-bit");
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  append_u32_be(out, bits);
}

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
  if (s != INVALID_SOCKET) {
    closesocket(s);
  }
}

#else

static void socket_close(int s) {
  if (s >= 0) {
    close(s);
  }
}

#endif

} // namespace

OscMessage::OscMessage(std::string address)
  : address_(std::move(address)), typetags_(",") {
  if (address_.empty() || address_[0] != '/') {
    throw std::runtime_error("OscMessage: address must start with '/'");
  }
}

void OscMessage::add_int32(int32_t v) {
  typetags_.push_back('i');
  append_i32_be(&args_, v);
}

void OscMessage::add_float32(float v) {
  typetags_.push_back('f');
  append_f32_be(&args_, v);
}

void OscMessage::add_string(const std::string& v) {
  typetags_.push_back('s');
  append_padded_string(&args_, v);
}

std::vector<uint8_t> OscMessage::to_bytes() const {
  std::vector<uint8_t> out;
  out.reserve(address_.size() + typetags_.size() + args_.size() + 32);

  append_padded_string(&out, address_);
  append_padded_string(&out, typetags_);
  out.insert(out.end(), args_.begin(), args_.end());

  while ((out.size() % 4) != 0) out.push_back(0);
  return out;
}

OscBundle::OscBundle(uint64_t timetag) : timetag_(timetag) {}

void OscBundle::set_timetag(uint64_t timetag) {
  timetag_ = timetag;
}

void OscBundle::set_timetag_immediate() {
  timetag_ = 1;
}

void OscBundle::clear() {
  elements_.clear();
}

void OscBundle::add_message(const OscMessage& msg) {
  elements_.push_back(msg.to_bytes());
}

void OscBundle::add_bytes(const std::vector<uint8_t>& element) {
  if (!element.empty() && (element.size() % 4) != 0) {
    throw std::runtime_error("OscBundle: element size must be a multiple of 4 bytes");
  }
  elements_.push_back(element);
}

std::vector<uint8_t> OscBundle::to_bytes() const {
  std::vector<uint8_t> out;
  out.reserve(16 + elements_.size() * 32);

  // OSC bundle header.
  append_padded_string(&out, "#bundle");
  append_u64_be(&out, timetag_);

  // Elements: [u32 size][element bytes]
  for (const auto& el : elements_) {
    append_u32_be(&out, static_cast<uint32_t>(el.size()));
    out.insert(out.end(), el.begin(), el.end());
  }

  while ((out.size() % 4) != 0) out.push_back(0);
  return out;
}

struct OscUdpClient::Impl {
#ifdef _WIN32
  SOCKET sock{INVALID_SOCKET};
#else
  int sock{-1};
#endif

  sockaddr_storage addr{};
  socklen_t addr_len{0};

  bool ok{false};
  std::string last_error;
};

OscUdpClient::OscUdpClient(const std::string& host, int port)
  : impl_(std::make_unique<Impl>()) {
  if (port <= 0 || port > 65535) {
    impl_->last_error = "OscUdpClient: port must be in [1, 65535]";
    impl_->ok = false;
    return;
  }

#ifdef _WIN32
  if (!winsock_init_once()) {
    impl_->last_error = "OscUdpClient: WSAStartup failed";
    impl_->ok = false;
    return;
  }
#endif

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  const int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0 || !res) {
#ifdef _WIN32
    impl_->last_error = std::string("OscUdpClient: getaddrinfo failed: ") + gai_strerrorA(rc);
#else
    impl_->last_error = std::string("OscUdpClient: getaddrinfo failed: ") + gai_strerror(rc);
#endif
    impl_->ok = false;
    return;
  }

  bool opened = false;
  for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
#ifdef _WIN32
    SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (s == INVALID_SOCKET) continue;
#else
    int s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (s < 0) continue;
#endif

    std::memset(&impl_->addr, 0, sizeof(impl_->addr));
    std::memcpy(&impl_->addr, it->ai_addr, static_cast<size_t>(it->ai_addrlen));
    impl_->addr_len = static_cast<socklen_t>(it->ai_addrlen);

    impl_->sock = s;
    opened = true;
    break;
  }

  freeaddrinfo(res);

  if (!opened) {
    impl_->last_error = "OscUdpClient: failed to open UDP socket";
    impl_->ok = false;
    return;
  }

  impl_->ok = true;
}

OscUdpClient::~OscUdpClient() {
  if (!impl_) return;
  socket_close(impl_->sock);
#ifdef _WIN32
  impl_->sock = INVALID_SOCKET;
#else
  impl_->sock = -1;
#endif
}

bool OscUdpClient::ok() const {
  return impl_ && impl_->ok;
}

std::string OscUdpClient::last_error() const {
  return impl_ ? impl_->last_error : std::string();
}

bool OscUdpClient::send(const OscMessage& msg) {
  return send_bytes(msg.to_bytes());
}

bool OscUdpClient::send(const OscBundle& bundle) {
  return send_bytes(bundle.to_bytes());
}

bool OscUdpClient::send_bytes(const std::vector<uint8_t>& bytes) {
  if (!ok()) return false;
  if (bytes.empty()) return true;
  if ((bytes.size() % 4) != 0) {
    impl_->last_error = "OscUdpClient: OSC packet size must be a multiple of 4 bytes";
    return false;
  }

#ifdef _WIN32
  const int sent = sendto(
    impl_->sock,
    reinterpret_cast<const char*>(bytes.data()),
    static_cast<int>(bytes.size()),
    0,
    reinterpret_cast<const sockaddr*>(&impl_->addr),
    impl_->addr_len);
  if (sent == SOCKET_ERROR) {
    impl_->last_error = "OscUdpClient: sendto failed";
    return false;
  }
#else
  const ssize_t sent = sendto(
    impl_->sock,
    bytes.data(),
    bytes.size(),
    0,
    reinterpret_cast<const sockaddr*>(&impl_->addr),
    impl_->addr_len);
  if (sent < 0) {
    impl_->last_error = "OscUdpClient: sendto failed";
    return false;
  }
#endif

  return true;
}

} // namespace qeeg
