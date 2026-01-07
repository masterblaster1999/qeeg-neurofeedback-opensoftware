#include "qeeg/osc.hpp"

#include <cassert>
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

  std::cout << "All tests passed.\n";
  return 0;
}
