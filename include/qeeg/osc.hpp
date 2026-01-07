#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qeeg {

// Minimal Open Sound Control (OSC) message builder + UDP sender.
//
// This is intentionally small and dependency-light:
// - Supports OSC Messages (not Bundles)
// - Supports argument types: int32, float32, string
//
// OSC is frequently used to bridge real-time control signals between programs
// (e.g., Max/MSP, Pure Data, TouchOSC, Processing).

class OscMessage {
public:
  // OSC addresses should typically begin with '/', e.g. "/qeeg/state".
  explicit OscMessage(std::string address);

  const std::string& address() const { return address_; }

  void add_int32(int32_t v);
  void add_float32(float v);
  void add_string(const std::string& v);

  // Encode as an OSC Message byte array.
  // The returned size is always a multiple of 4 bytes (OSC alignment).
  std::vector<uint8_t> to_bytes() const;

private:
  std::string address_;
  std::string typetags_;         // begins with ','
  std::vector<uint8_t> args_;    // encoded arg bytes (padded where required)
};

class OscUdpClient {
public:
  OscUdpClient(const std::string& host, int port);
  ~OscUdpClient();

  OscUdpClient(const OscUdpClient&) = delete;
  OscUdpClient& operator=(const OscUdpClient&) = delete;

  bool ok() const;
  std::string last_error() const;

  bool send(const OscMessage& msg);
  bool send_bytes(const std::vector<uint8_t>& bytes);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace qeeg
