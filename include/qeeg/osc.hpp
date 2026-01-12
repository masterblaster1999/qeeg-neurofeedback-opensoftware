#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qeeg {

// Minimal Open Sound Control (OSC) message builder + UDP sender.
//
// This is intentionally small and dependency-light:
// - Supports OSC Messages and Bundles
// - Supports common argument types:
//     * int32   ('i')
//     * int64   ('h')
//     * float32 ('f')
//     * float64 ('d')
//     * string  ('s')
//     * bool    ('T' / 'F') (no payload bytes)
//     * blob    ('b') (u32 size + raw bytes, padded to 4)
//
// OSC is frequently used to bridge real-time control signals between programs
// (e.g., Max/MSP, Pure Data, TouchOSC, Processing).

class OscMessage {
public:
  // OSC addresses should typically begin with '/', e.g. "/qeeg/state".
  explicit OscMessage(std::string address);

  const std::string& address() const { return address_; }

  void add_int32(int32_t v);
  void add_int64(int64_t v);
  void add_float32(float v);
  void add_float64(double v);
  void add_string(const std::string& v);
  void add_bool(bool v);

  // OSC "blob" type: raw bytes with a 32-bit length prefix.
  // The blob payload is padded to a 4-byte boundary as required by OSC.
  void add_blob(const void* data, size_t size);
  void add_blob(const std::vector<uint8_t>& data) { add_blob(data.data(), data.size()); }

  // Encode as an OSC Message byte array.
  // The returned size is always a multiple of 4 bytes (OSC alignment).
  std::vector<uint8_t> to_bytes() const;

private:
  std::string address_;
  std::string typetags_;        // begins with ','
  std::vector<uint8_t> args_;   // encoded arg bytes (padded where required)
};

// Minimal OSC Bundle builder.
//
// Bundles allow multiple OSC messages to be sent "atomically" in one UDP
// packet. OSC bundles also carry a 64-bit timetag.
//
// Notes:
// - The default timetag is 1, which is the conventional OSC "immediately"
//   timetag value.
// - add_bytes() can be used to add a pre-encoded OSC element (including
//   nested bundles) if you already have aligned bytes.
class OscBundle {
public:
  explicit OscBundle(uint64_t timetag = 1);

  uint64_t timetag() const { return timetag_; }
  void set_timetag(uint64_t timetag);
  void set_timetag_immediate();

  void clear();

  // Add an OSC Message as a bundle element.
  void add_message(const OscMessage& msg);

  // Add pre-encoded OSC element bytes (must be 4-byte aligned).
  void add_bytes(const std::vector<uint8_t>& element);

  // Encode as an OSC Bundle byte array.
  // The returned size is always a multiple of 4 bytes (OSC alignment).
  std::vector<uint8_t> to_bytes() const;

private:
  uint64_t timetag_{1};
  std::vector<std::vector<uint8_t>> elements_;
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
  bool send(const OscBundle& bundle);
  bool send_bytes(const std::vector<uint8_t>& bytes);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace qeeg
