#include "qeeg/utils.hpp"

#include "test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

static std::string u8_fffd() {
  // U+FFFD replacement character in UTF-8.
  return std::string("\xEF\xBF\xBD");
}

int main() {
  std::vector<std::string> out;
  std::string err;

  TEST_CHECK(qeeg::json_parse_string_array("[]", &out, &err));
  TEST_CHECK(out.empty());

  TEST_CHECK(qeeg::json_parse_string_array(" [  \n\t  ] ", &out, &err));
  TEST_CHECK(out.empty());

  TEST_CHECK(qeeg::json_parse_string_array("[\"a\",\"b\"]", &out, &err));
  TEST_CHECK(out.size() == 2);
  TEST_CHECK(out[0] == "a");
  TEST_CHECK(out[1] == "b");

  TEST_CHECK(qeeg::json_parse_string_array("[\"a\\n\\t\\\\b\"]", &out, &err));
  TEST_CHECK(out.size() == 1);
  TEST_CHECK(out[0] == std::string("a\n\t\\b"));

  // UTF-16 surrogate pair: U+1F600 (grinning face)
  TEST_CHECK(qeeg::json_parse_string_array("[\"\\uD83D\\uDE00\"]", &out, &err));
  TEST_CHECK(out.size() == 1);
  TEST_CHECK(out[0] == std::string("\xF0\x9F\x98\x80"));

  // Orphan high surrogate: replacement
  TEST_CHECK(qeeg::json_parse_string_array("[\"\\uD83D\"]", &out, &err));
  TEST_CHECK(out.size() == 1);
  TEST_CHECK(out[0] == u8_fffd());

  // Orphan low surrogate: replacement
  TEST_CHECK(qeeg::json_parse_string_array("[\"\\uDE00\"]", &out, &err));
  TEST_CHECK(out.size() == 1);
  TEST_CHECK(out[0] == u8_fffd());

  // Reject non-string elements
  err.clear();
  TEST_CHECK(!qeeg::json_parse_string_array("[1]", &out, &err));
  TEST_CHECK(!err.empty());

  // Reject unterminated arrays
  err.clear();
  TEST_CHECK(!qeeg::json_parse_string_array("[\"a\"", &out, &err));
  TEST_CHECK(!err.empty());

  // Reject trailing non-whitespace
  err.clear();
  TEST_CHECK(!qeeg::json_parse_string_array("[\"a\"] x", &out, &err));
  TEST_CHECK(!err.empty());

  std::cout << "ok\n";
  return 0;
}
