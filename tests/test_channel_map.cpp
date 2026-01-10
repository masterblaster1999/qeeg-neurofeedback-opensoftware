#include "qeeg/channel_map.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
  using namespace qeeg;

  EEGRecording rec;
  rec.fs_hz = 256.0;
  rec.channel_names = {"ExG1", "ExG2", "Fp1"};
  rec.data = {
      {1.0f, 2.0f}, // ExG1
      {3.0f, 4.0f}, // ExG2
      {5.0f, 6.0f}  // Fp1
  };

  const std::string map_path = "test_channel_map.csv";
  {
    std::ofstream o(map_path);
    assert(o);
    o << "old,new\n";
    o << "ExG1,C3\n";
    o << "exg2,C4\n";
    o << "fp1,DROP\n";
  }

  ChannelMap m = load_channel_map_file(map_path);
  apply_channel_map(&rec, m);

  assert(rec.channel_names.size() == 2);
  assert(rec.data.size() == 2);

  assert(rec.channel_names[0] == "C3");
  assert(rec.channel_names[1] == "C4");

  assert(rec.data[0].size() == 2);
  assert(rec.data[1].size() == 2);

  assert(rec.data[0][0] == 1.0f);
  assert(rec.data[1][1] == 4.0f);

  std::remove(map_path.c_str());

  // Duplicate detection
  EEGRecording rec2;
  rec2.fs_hz = 256.0;
  rec2.channel_names = {"ExG1", "ExG2"};
  rec2.data = {{1.0f}, {2.0f}};

  const std::string map_path2 = "test_channel_map_dup.csv";
  {
    std::ofstream o(map_path2);
    assert(o);
    o << "ExG1,C3\n";
    o << "ExG2,C3\n"; // duplicate mapped name => should throw
  }

  bool threw = false;
  try {
    ChannelMap m2 = load_channel_map_file(map_path2);
    apply_channel_map(&rec2, m2);
  } catch (const std::exception&) {
    threw = true;
  }
  assert(threw);

  std::remove(map_path2.c_str());

  std::cout << "ChannelMap test passed.\n";
  return 0;
}
