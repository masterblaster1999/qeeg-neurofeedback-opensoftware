#include "qeeg/svg_utils.hpp"

#include <cassert>
#include <iostream>

int main() {
  using qeeg::svg_escape;
  using qeeg::url_escape;

  {
    const std::string s = "<tag attr=\"a&b\">O'Reilly</tag>";
    const std::string e = svg_escape(s);
    // Basic XML entity escaping
    assert(e.find("&lt;") != std::string::npos);
    assert(e.find("&gt;") != std::string::npos);
    assert(e.find("&quot;") != std::string::npos);
    assert(e.find("&amp;") != std::string::npos);
    assert(e.find("&apos;") != std::string::npos);
  }

  {
    const std::string p = "file name (1).svg";
    const std::string u = url_escape(p);
    // Spaces must be encoded.
    assert(u.find(' ') == std::string::npos);
    assert(u.find("%20") != std::string::npos);
  }

  std::cout << "test_svg_utils OK\n";
  return 0;
}
