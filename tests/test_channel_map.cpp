#include "qeeg/channel_map.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
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

  // Empty "new" value should keep the original channel name (use new=DROP to drop).
  EEGRecording rec_keep;
  rec_keep.fs_hz = 256.0;
  rec_keep.channel_names = {"ExG1", "ExG2", "Fp1"};
  rec_keep.data = {
      {1.0f, 2.0f}, // ExG1
      {3.0f, 4.0f}, // ExG2
      {5.0f, 6.0f}  // Fp1
  };

  const std::string map_path_keep = "test_channel_map_keep_empty.csv";
  {
    std::ofstream o(map_path_keep);
    assert(o);
    o << "old,new\n";
    o << "ExG1,\n";     // keep ExG1
    o << "ExG2,C4\n";   // rename
    o << "Fp1,DROP\n";  // drop
  }

  ChannelMap m_keep = load_channel_map_file(map_path_keep);
  apply_channel_map(&rec_keep, m_keep);

  assert(rec_keep.channel_names.size() == 2);
  assert(rec_keep.channel_names[0] == "ExG1");
  assert(rec_keep.channel_names[1] == "C4");
  assert(rec_keep.data.size() == 2);
  assert(rec_keep.data[0][0] == 1.0f);
  assert(rec_keep.data[1][1] == 4.0f);

  std::remove(map_path_keep.c_str());

  // UTF-8 BOM on the first line should not break header detection or mapping.
  EEGRecording rec_bom;
  rec_bom.fs_hz = 256.0;
  rec_bom.channel_names = {"ExG1", "ExG2", "Fp1"};
  rec_bom.data = {
      {1.0f, 2.0f},
      {3.0f, 4.0f},
      {5.0f, 6.0f},
  };

  const std::string map_path_bom = "test_channel_map_bom.csv";
  {
    std::ofstream o(map_path_bom, std::ios::binary);
    assert(o);
    o << "\xEF\xBB\xBFold,new\n";
    o << "ExG1,C3\n";
    o << "ExG2,DROP\n";
  }

  ChannelMap m_bom = load_channel_map_file(map_path_bom);
  apply_channel_map(&rec_bom, m_bom);

  assert(rec_bom.channel_names.size() == 2);
  assert(rec_bom.channel_names[0] == "C3");
  assert(rec_bom.channel_names[1] == "Fp1");
  assert(rec_bom.data.size() == 2);
  assert(rec_bom.data[0][0] == 1.0f);
  assert(rec_bom.data[1][1] == 6.0f);

  std::remove(map_path_bom.c_str());

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

  // UTF-8 filenames should work (important on Windows).
  {
    const std::filesystem::path dir = std::filesystem::u8path(u8"tmp_\xC2\xB5_channel_map");
    const std::filesystem::path map_p = dir / std::filesystem::u8path(u8"map_\xC2\xB5.csv");
    const std::filesystem::path tmpl_p = dir / std::filesystem::u8path(u8"template_\xC2\xB5.csv");

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    assert(!ec);

    // Template writer should handle UTF-8 paths.
    write_channel_map_template(tmpl_p.u8string(), rec);

    {
      std::ofstream o(map_p, std::ios::binary);
      assert(o);
      o << "old,new\n";
      o << "ExG1,C3\n";
    }

    EEGRecording rec_u8;
    rec_u8.fs_hz = 256.0;
    rec_u8.channel_names = {"ExG1"};
    rec_u8.data = {{1.0f, 2.0f}};

    ChannelMap m_u8 = load_channel_map_file(map_p.u8string());
    apply_channel_map(&rec_u8, m_u8);
    assert(rec_u8.channel_names.size() == 1);
    assert(rec_u8.channel_names[0] == "C3");

    std::filesystem::remove_all(dir, ec);
  }

  std::cout << "ChannelMap test passed.\n";
  return 0;
}
