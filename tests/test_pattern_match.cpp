#include "qeeg/pattern.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace qeeg;

  // Wildcards
  assert(wildcard_match("abc", "a?c", true));
  assert(wildcard_match("abcdef", "a*ef", true));
  assert(!wildcard_match("abcdef", "a*eg", true));

  // Case-insensitive wildcard
  assert(wildcard_match("StimA", "*stima*", false));
  assert(!wildcard_match("StimA", "*stima*", true));

  // Regex (ECMAScript) search
  {
    const std::regex re_cs = compile_regex("Stim[0-9]+", true);
    assert(regex_search("Stim12", re_cs));
    assert(!regex_search("stim12", re_cs));
  }
  {
    const std::regex re_ci = compile_regex("Stim[0-9]+", false);
    assert(regex_search("Stim12", re_ci));
    assert(regex_search("stim12", re_ci));
  }

  // Invalid regex should throw a runtime_error.
  bool threw = false;
  try {
    (void)compile_regex("(unclosed", true);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);

  std::cout << "test_pattern_match OK\n";
  return 0;
}
