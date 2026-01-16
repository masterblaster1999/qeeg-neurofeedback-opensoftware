#include "qeeg/utils.hpp"

#include <cassert>
#include <cctype>
#include <iostream>

int main() {
  {
    const std::string t = qeeg::random_hex_token(16);
    assert(t.size() == 32);
    for (unsigned char c : t) {
      assert(std::isxdigit(c) != 0);
    }
  }
  {
    const std::string t = qeeg::random_hex_token(1);
    assert(t.size() == 2);
    for (unsigned char c : t) {
      assert(std::isxdigit(c) != 0);
    }
  }
  {
    // 0 means "use a sensible default"
    const std::string t = qeeg::random_hex_token(0);
    assert(!t.empty());
    for (unsigned char c : t) {
      assert(std::isxdigit(c) != 0);
    }
  }

  std::cout << "ok\n";
  return 0;
}
