#include "qeeg/osc.hpp"

#include "test_support.hpp"
#include <cstdint>
#include <iostream>
#include <vector>

static void expect_bytes(const std::vector<uint8_t>& b,
                         size_t off,
                         const std::vector<uint8_t>& exp) {
  assert(off + exp.size() <= b.size());
  for (size_t i = 0; i < exp.size(); ++i) {
    assert(b[off + i] == exp[i]);
  }
}

int main() {
  using namespace qeeg;

  // Example: /test ,if 1 0.5
  {
    OscMessage m("/test");
    m.add_int32(1);
    m.add_float32(0.5f);
    const std::vector<uint8_t> bytes = m.to_bytes();

    // /test\0 padded to 8 bytes
    // ,if\0 padded to 4 bytes
    // args: int32 + float32
    assert(bytes.size() == 20);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0, { '/', 't', 'e', 's', 't', 0, 0, 0 });
    expect_bytes(bytes, 8, { ',', 'i', 'f', 0 });

    // int32 1 big-endian
    expect_bytes(bytes, 12, { 0x00, 0x00, 0x00, 0x01 });

    // float32 0.5 big-endian == 0x3F000000
    expect_bytes(bytes, 16, { 0x3F, 0x00, 0x00, 0x00 });
  }

  // String padding test: /s ,s "hi"
  {
    OscMessage m("/s");
    m.add_string("hi");
    const std::vector<uint8_t> bytes = m.to_bytes();

    assert(bytes.size() == 12);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0, { '/', 's', 0, 0 });
    expect_bytes(bytes, 4, { ',', 's', 0, 0 });
    expect_bytes(bytes, 8, { 'h', 'i', 0, 0 });
  }

  // Bool test: /b ,T (no payload bytes)
  {
    OscMessage m("/b");
    m.add_bool(true);
    const std::vector<uint8_t> bytes = m.to_bytes();

    assert(bytes.size() == 8);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0, { '/', 'b', 0, 0 });
    expect_bytes(bytes, 4, { ',', 'T', 0, 0 });
  }

  // Int64 test: /h ,h 1
  {
    OscMessage m("/h");
    m.add_int64(1);
    const std::vector<uint8_t> bytes = m.to_bytes();

    assert(bytes.size() == 16);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0, { '/', 'h', 0, 0 });
    expect_bytes(bytes, 4, { ',', 'h', 0, 0 });

    // int64 1 big-endian
    expect_bytes(bytes, 8, { 0, 0, 0, 0, 0, 0, 0, 1 });
  }

  // Float64 test: /d ,d 0.5
  {
    OscMessage m("/d");
    m.add_float64(0.5);
    const std::vector<uint8_t> bytes = m.to_bytes();

    assert(bytes.size() == 16);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0, { '/', 'd', 0, 0 });
    expect_bytes(bytes, 4, { ',', 'd', 0, 0 });

    // float64 0.5 big-endian == 0x3FE0000000000000
    expect_bytes(bytes, 8, { 0x3F, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
  }

  // Blob test: /blob ,b <len=3> 0x01 0x02 0x03
  {
    const std::vector<uint8_t> payload = { 1, 2, 3 };
    OscMessage m("/blob");
    m.add_blob(payload);
    const std::vector<uint8_t> bytes = m.to_bytes();

    assert(bytes.size() == 20);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0,  { '/', 'b', 'l', 'o', 'b', 0, 0, 0 });
    expect_bytes(bytes, 8,  { ',', 'b', 0, 0 });
    expect_bytes(bytes, 12, { 0, 0, 0, 3 });
    expect_bytes(bytes, 16, { 1, 2, 3, 0 });
  }

  // Bundle test: #bundle <timetag> [size][message]...
  {
    OscMessage a("/a");
    a.add_int32(1);

    OscMessage b("/b");
    b.add_string("hi");

    OscBundle bundle; // default timetag = 1 ("immediately")
    bundle.add_message(a);
    bundle.add_message(b);

    const std::vector<uint8_t> bytes = bundle.to_bytes();

    // Bundle layout:
    //  - "#bundle\0" (8 bytes)
    //  - timetag (8 bytes, big-endian)
    //  - element 1: u32 size + message bytes
    //  - element 2: u32 size + message bytes

    // Both messages are 12 bytes here, so total = 8 + 8 + (4+12)*2 = 48.
    assert(bytes.size() == 48);
    assert((bytes.size() % 4) == 0);

    expect_bytes(bytes, 0,  { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0 });
    expect_bytes(bytes, 8,  { 0, 0, 0, 0, 0, 0, 0, 1 });

    // Element 1 size (12)
    expect_bytes(bytes, 16, { 0, 0, 0, 12 });
    // Element 1 message (/a ,i 1)
    expect_bytes(bytes, 20, { '/', 'a', 0, 0, ',', 'i', 0, 0, 0, 0, 0, 1 });

    // Element 2 size (12)
    expect_bytes(bytes, 32, { 0, 0, 0, 12 });
    // Element 2 message (/b ,s "hi")
    expect_bytes(bytes, 36, { '/', 'b', 0, 0, ',', 's', 0, 0, 'h', 'i', 0, 0 });
  }

  std::cout << "All tests passed.\n";
  return 0;
}
