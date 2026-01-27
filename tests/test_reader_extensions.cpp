#include "qeeg/reader.hpp"

#include "test_support.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // 1) .txt should be treated like CSV when it has a time column.
  const std::string path = "test_reader_extensions.txt";
  {
    std::ofstream o(path);
    assert(o);
    o << "time,C1,C2\n";
    o << "0.00,1,2\n";
    o << "0.01,3,4\n";
    o << "0.02,5,6\n";
  }

  EEGRecording rec = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
  assert(rec.channel_names.size() == 2);
  assert(rec.channel_names[0] == "C1");
  assert(rec.channel_names[1] == "C2");
  assert(rec.data.size() == 2);
  assert(rec.data[0].size() == 3);
  assert(approx(rec.fs_hz, 100.0));

  std::remove(path.c_str());

  // 2) .bcd/.mbd should error with a helpful message.
  bool threw = false;
  try {
    (void)read_recording_auto("dummy.bcd", 0.0);
  } catch (const std::exception& e) {
    threw = true;
    const std::string msg = e.what();
    assert(msg.find(".bcd/.mbd") != std::string::npos);
  }
  assert(threw);

  std::cout << "Reader extension test passed.\n";
  return 0;
}
