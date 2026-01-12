#include "qeeg/brainvision_reader.hpp"

#include "qeeg/utils.hpp"

#include "qeeg/event_ops.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace qeeg {

struct BVChannelInfo {
  std::string name;
  double resolution{1.0};
  std::string unit{"uV"};
};

struct BVHeader {
  std::string data_file;
  std::string marker_file;
  std::string data_format{"BINARY"};
  std::string data_orientation{"MULTIPLEXED"};
  int number_of_channels{0};
  int sampling_interval_us{0}; // microseconds
  std::string binary_format{"IEEE_FLOAT_32"};
  std::map<int, BVChannelInfo> channels_by_index; // 1-based index
};

static inline bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

static inline std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static inline std::string bv_unescape_commas(std::string s) {
  // BrainVision .vhdr/.vmrk convention: literal commas inside fields may be encoded as \"\\1\".
  // Decode \"\\1\" back to ','.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '1') {
      out.push_back(',');
      ++i;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

static inline double parse_double_or(const std::string& s, double fallback) {
  std::string t = trim(s);
  if (t.empty()) return fallback;
  try {
    return std::stod(t);
  } catch (...) {
    return fallback;
  }
}

static inline long long parse_ll_or(const std::string& s, long long fallback) {
  std::string t = trim(s);
  if (t.empty()) return fallback;
  try {
    return std::stoll(t);
  } catch (...) {
    return fallback;
  }
}

static inline bool try_parse_ll_strict(const std::string& s, long long* out) {
  const std::string t = trim(s);
  if (t.empty()) return false;
  try {
    size_t idx = 0;
    const long long v = std::stoll(t, &idx, 10);
    if (idx != t.size()) return false;
    if (out) *out = v;
    return true;
  } catch (...) {
    return false;
  }
}

static inline int parse_int_or(const std::string& s, int fallback) {
  std::string t = trim(s);
  if (t.empty()) return fallback;
  try {
    return std::stoi(t);
  } catch (...) {
    return fallback;
  }
}

static inline double unit_to_microvolt_scale(std::string unit) {
  unit = lower_copy(trim(unit));
  // tolerate UTF-8 micro sign and ASCII fallbacks.
  if (unit == "uv" || unit == "µv" || unit == "μv") return 1.0;
  if (unit == "mv") return 1000.0;
  if (unit == "v") return 1e6;
  if (unit == "nv") return 0.001;
  return 1.0; // unknown: assume already in microvolts
}

static inline uint16_t read_le_u16(std::istream& is) {
  unsigned char b[2];
  is.read(reinterpret_cast<char*>(b), 2);
  return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

static inline int16_t read_le_i16(std::istream& is) {
  return static_cast<int16_t>(read_le_u16(is));
}

static inline float read_le_f32(std::istream& is) {
  unsigned char b[4];
  is.read(reinterpret_cast<char*>(b), 4);
  uint32_t u = (uint32_t)b[0]
             | ((uint32_t)b[1] << 8)
             | ((uint32_t)b[2] << 16)
             | ((uint32_t)b[3] << 24);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

static BVHeader parse_vhdr(const fs::path& vhdr_path) {
  std::ifstream is(vhdr_path);
  if (!is) {
    throw std::runtime_error("BrainVisionReader: failed to open header: " + vhdr_path.string());
  }

  BVHeader h;
  std::string section;

  std::string line;
  bool first_line = true;
  while (std::getline(is, line)) {
    if (first_line) {
      line = strip_utf8_bom(line);
      first_line = false;
    }

    line = trim(line);
    if (line.empty()) continue;
    if (line[0] == ';') continue;

    if (line.front() == '[' && line.back() == ']') {
      section = lower_copy(trim(line.substr(1, line.size() - 2)));
      continue;
    }

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = lower_copy(trim(line.substr(0, eq)));
    std::string val = trim(line.substr(eq + 1));

    if (section == "common infos") {
      if (key == "datafile") h.data_file = val;
      else if (key == "markerfile") h.marker_file = val;
      else if (key == "dataformat") h.data_format = val;
      else if (key == "dataorientation") h.data_orientation = val;
      else if (key == "numberofchannels") h.number_of_channels = parse_int_or(val, 0);
      else if (key == "samplinginterval") h.sampling_interval_us = parse_int_or(val, 0);
    } else if (section == "binary infos") {
      if (key == "binaryformat") h.binary_format = val;
    } else if (section == "channel infos") {
      if (key.size() >= 3 && key[0] == 'c' && key[1] == 'h') {
        int idx = parse_int_or(key.substr(2), 0);
        if (idx <= 0) continue;

        // Format: ChN=<Name>,<Reference>,<Resolution>,<Unit>[,...]
        std::vector<std::string> fields = split(val, ',');
        BVChannelInfo ci;
        if (!fields.empty()) ci.name = bv_unescape_commas(trim(fields[0]));
        if (fields.size() >= 3) ci.resolution = parse_double_or(fields[2], 1.0);
        if (fields.size() >= 4) ci.unit = trim(fields[3]);
        if (ci.name.empty()) ci.name = "Ch" + std::to_string(idx);
        if (ci.unit.empty()) ci.unit = "uV";
        if (ci.resolution <= 0.0) ci.resolution = 1.0;
        h.channels_by_index[idx] = ci;
      }
    }
  }

  if (h.data_file.empty()) {
    throw std::runtime_error("BrainVisionReader: missing DataFile in [Common Infos]");
  }
  if (h.number_of_channels <= 0) {
    // derive from channel info if present
    if (!h.channels_by_index.empty()) {
      h.number_of_channels = h.channels_by_index.rbegin()->first;
    }
  }
  if (h.number_of_channels <= 0) {
    throw std::runtime_error("BrainVisionReader: missing/invalid NumberOfChannels");
  }
  if (h.sampling_interval_us <= 0) {
    throw std::runtime_error("BrainVisionReader: missing/invalid SamplingInterval (microseconds)");
  }

  return h;
}

struct BVMarker {
  std::string type;
  std::string desc;
  long long pos{0}; // 1-based sample index
  long long len{0}; // in samples
  int channel{0};
};

static bool split_right_commas(const std::string& s, int n, std::string* left,
                               std::vector<std::string>* tail) {
  if (!left || !tail) return false;
  if (n <= 0) return false;

  std::string cur = s;
  tail->clear();
  tail->reserve(static_cast<size_t>(n));

  for (int i = 0; i < n; ++i) {
    const size_t comma = cur.rfind(',');
    if (comma == std::string::npos) return false;
    tail->push_back(trim(cur.substr(comma + 1)));
    cur = cur.substr(0, comma);
  }

  std::reverse(tail->begin(), tail->end());
  *left = trim(cur);
  return true;
}

// Parse a BrainVision marker value:
//   <type>, <description>, <position>, <points>, <channel number>[, <date>]
//
// This function is robust to commas in the description by splitting numeric fields from the right.
static bool parse_bv_marker_value(const std::string& val, BVMarker* mk) {
  if (!mk) return false;

  mk->type.clear();
  mk->desc.clear();
  mk->pos = 0;
  mk->len = 0;
  mk->channel = 0;

  const std::string v = trim(val);
  if (v.empty()) return false;

  const size_t first_comma = v.find(',');
  mk->type = trim((first_comma == std::string::npos) ? v : v.substr(0, first_comma));
  const std::string rest = (first_comma == std::string::npos) ? std::string() : v.substr(first_comma + 1);

  auto finalize = [&]() {
    mk->type = bv_unescape_commas(mk->type);
    mk->desc = bv_unescape_commas(mk->desc);
  };

  if (rest.empty()) {
    finalize();
    return true;
  }

  // BrainVision may optionally add a date field, and it is only evaluated for "New Segment" markers.
  // We try to detect this extra field without being confused by commas inside the description.
  const bool is_new_segment = ieq(trim(mk->type), "New Segment");
  bool has_date_field = false;
  if (is_new_segment) {
    const size_t last_comma = rest.rfind(',');
    if (last_comma != std::string::npos) {
      const std::string last_tok = trim(rest.substr(last_comma + 1));
      long long as_ll = 0;
      // Heuristic: channel numbers are typically small (0 meaning "all channels").
      // If the last token is not a small integer, treat it as a date field.
      if (!try_parse_ll_strict(last_tok, &as_ll) || as_ll > 10000 || as_ll < -10000) {
        has_date_field = true;
      }
    }
  }

  std::string desc;
  std::vector<std::string> tail;

  auto parse_with_tail = [&](int n_tail) -> bool {
    if (!split_right_commas(rest, n_tail, &desc, &tail)) return false;
    if (static_cast<int>(tail.size()) != n_tail) return false;

    // For n_tail == 3: tail = [position, points, channel]
    // For n_tail == 4: tail = [position, points, channel, date]
    mk->desc = desc;
    mk->pos = parse_ll_or(tail[0], 0);
    mk->len = parse_ll_or(tail[1], 0);
    mk->channel = parse_int_or(tail[2], 0);
    return true;
  };

  if (has_date_field) {
    // If this fails (e.g., because the description itself created an extra comma), fall back.
    if (parse_with_tail(4)) { finalize(); return true; }
  }
  if (parse_with_tail(3)) { finalize(); return true; }

  // Fallback to the original naive split for malformed lines.
  const std::vector<std::string> fields = split(v, ',');
  if (fields.size() >= 1) mk->type = trim(fields[0]);
  if (fields.size() >= 2) mk->desc = trim(fields[1]);
  if (fields.size() >= 3) mk->pos = parse_ll_or(fields[2], 0);
  if (fields.size() >= 4) mk->len = parse_ll_or(fields[3], 0);
  if (fields.size() >= 5) mk->channel = parse_int_or(fields[4], 0);
  finalize();
  return true;
}

static std::vector<BVMarker> parse_vmrk(const fs::path& vmrk_path) {
  std::vector<BVMarker> out;
  std::ifstream is(vmrk_path);
  if (!is) {
    return out;
  }

  std::string section;
  std::string line;
  bool first_line = true;
  while (std::getline(is, line)) {
    if (first_line) {
      line = strip_utf8_bom(line);
      first_line = false;
    }

    line = trim(line);
    if (line.empty()) continue;
    if (line[0] == ';') continue;

    if (line.front() == '[' && line.back() == ']') {
      section = lower_copy(trim(line.substr(1, line.size() - 2)));
      continue;
    }

    if (section != "marker infos") continue;

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = lower_copy(trim(line.substr(0, eq)));
    if (key.size() < 2 || key[0] != 'm' || key[1] != 'k') continue;

    std::string val = trim(line.substr(eq + 1));

    BVMarker mk;
    if (!parse_bv_marker_value(val, &mk)) continue;

    if (mk.pos > 0) {
      out.push_back(mk);
    }
  }

  // Sort by position (mk keys are usually ordered but not guaranteed).
  std::sort(out.begin(), out.end(), [](const BVMarker& a, const BVMarker& b) {
    if (a.pos != b.pos) return a.pos < b.pos;
    return a.len < b.len;
  });

  return out;
}

EEGRecording BrainVisionReader::read(const std::string& vhdr_path_str) {
  fs::path vhdr_path = fs::path(vhdr_path_str);

  BVHeader h = parse_vhdr(vhdr_path);

  const fs::path base_dir = vhdr_path.parent_path();

  fs::path data_path = fs::path(h.data_file);
  if (data_path.is_relative()) data_path = base_dir / data_path;

  fs::path marker_path;
  if (!h.marker_file.empty()) {
    marker_path = fs::path(h.marker_file);
    if (marker_path.is_relative()) marker_path = base_dir / marker_path;
  }

  std::string orientation = lower_copy(trim(h.data_orientation));
  std::string binary_format = lower_copy(trim(h.binary_format));
  std::string data_format = lower_copy(trim(h.data_format));

  if (data_format != "binary") {
    throw std::runtime_error("BrainVisionReader: only DataFormat=BINARY is supported (got: " + h.data_format + ")");
  }

  enum class BinFmt { Int16, Float32 };
  BinFmt fmt;
  size_t bytes_per_value = 0;
  if (binary_format == "int_16") {
    fmt = BinFmt::Int16;
    bytes_per_value = 2;
  } else if (binary_format == "ieee_float_32") {
    fmt = BinFmt::Float32;
    bytes_per_value = 4;
  } else {
    throw std::runtime_error("BrainVisionReader: unsupported BinaryFormat: " + h.binary_format);
  }

  if (!(orientation == "multiplexed" || orientation == "vectorized")) {
    throw std::runtime_error("BrainVisionReader: unsupported DataOrientation: " + h.data_orientation +
                             " (expected MULTIPLEXED or VECTORIZED)");
  }

  if (!fs::exists(data_path)) {
    throw std::runtime_error("BrainVisionReader: EEG binary file does not exist: " + data_path.string());
  }

  const uintmax_t file_bytes = fs::file_size(data_path);
  const uintmax_t frame_bytes = static_cast<uintmax_t>(h.number_of_channels) * bytes_per_value;
  if (frame_bytes == 0) {
    throw std::runtime_error("BrainVisionReader: invalid frame size derived from header");
  }
  if (file_bytes % frame_bytes != 0) {
    std::ostringstream oss;
    oss << "BrainVisionReader: EEG binary size (" << file_bytes
        << " bytes) is not divisible by (n_channels * bytes_per_value) = " << frame_bytes
        << " (n_channels=" << h.number_of_channels << ", bytes_per_value=" << bytes_per_value << ")";
    throw std::runtime_error(oss.str());
  }
  const size_t n_samples = static_cast<size_t>(file_bytes / frame_bytes);

  EEGRecording rec;
  rec.fs_hz = 1e6 / static_cast<double>(h.sampling_interval_us);
  rec.channel_names.resize(static_cast<size_t>(h.number_of_channels));
  rec.data.resize(static_cast<size_t>(h.number_of_channels));

  // Prepare per-channel scaling.
  std::vector<double> per_ch_mul_uV(static_cast<size_t>(h.number_of_channels), 1.0);
  for (int ch = 1; ch <= h.number_of_channels; ++ch) {
    BVChannelInfo ci;
    auto it = h.channels_by_index.find(ch);
    if (it != h.channels_by_index.end()) {
      ci = it->second;
    } else {
      ci.name = "Ch" + std::to_string(ch);
      ci.resolution = 1.0;
      ci.unit = "uV";
    }
    rec.channel_names[static_cast<size_t>(ch - 1)] = ci.name;

    const double unit_scale = unit_to_microvolt_scale(ci.unit);
    per_ch_mul_uV[static_cast<size_t>(ch - 1)] = ci.resolution * unit_scale;
  }

  for (auto& v : rec.data) {
    v.assign(n_samples, 0.0f);
  }

  std::ifstream is(data_path, std::ios::binary);
  if (!is) {
    throw std::runtime_error("BrainVisionReader: failed to open EEG binary for reading: " + data_path.string());
  }

  if (orientation == "multiplexed") {
    for (size_t i = 0; i < n_samples; ++i) {
      for (int ch = 0; ch < h.number_of_channels; ++ch) {
        float raw = 0.0f;
        if (fmt == BinFmt::Float32) {
          raw = read_le_f32(is);
        } else {
          raw = static_cast<float>(read_le_i16(is));
        }
        rec.data[static_cast<size_t>(ch)][i] = static_cast<float>(raw * per_ch_mul_uV[static_cast<size_t>(ch)]);
      }
    }
  } else { // vectorized
    for (int ch = 0; ch < h.number_of_channels; ++ch) {
      for (size_t i = 0; i < n_samples; ++i) {
        float raw = 0.0f;
        if (fmt == BinFmt::Float32) {
          raw = read_le_f32(is);
        } else {
          raw = static_cast<float>(read_le_i16(is));
        }
        rec.data[static_cast<size_t>(ch)][i] = static_cast<float>(raw * per_ch_mul_uV[static_cast<size_t>(ch)]);
      }
    }
  }

  // Parse markers to events (best-effort).
  if (!marker_path.empty() && fs::exists(marker_path)) {
    std::vector<BVMarker> markers = parse_vmrk(marker_path);

    for (const auto& mk : markers) {
      // BrainVision marker positions are 1-based.
      const double onset_sec = (static_cast<double>(mk.pos) - 1.0) / rec.fs_hz;
      // BrainVision stores marker length in *data points* ("points").
      // In practice, point/impulse markers typically have a length of 1 data point.
      // Map len<=1 to duration=0 to better preserve "instantaneous" events across formats.
      const double duration_sec = (mk.len > 1) ? (static_cast<double>(mk.len) / rec.fs_hz) : 0.0;

      // "New Segment" is often a record-start marker.
      if (ieq(mk.type, "New Segment") && trim(mk.desc).empty()) {
        continue;
      }

      AnnotationEvent ev;
      ev.onset_sec = onset_sec;
      ev.duration_sec = duration_sec;

      // Keep the description if present. If missing, fall back to type.
      const std::string desc = trim(mk.desc);
      if (!desc.empty()) {
        ev.text = desc;
      } else {
        ev.text = trim(mk.type);
      }

      // If it's not a Comment marker, prefix with the marker type so tools can distinguish.
      if (!ev.text.empty() && !ieq(mk.type, "Comment") && !ieq(mk.type, "New Segment")) {
        ev.text = trim(mk.type) + ":" + ev.text;
      }

      rec.events.push_back(ev);
    }

    // Deterministic ordering (segments before point markers at the same onset).
    sort_events(&rec.events);
  }

  return rec;
}

} // namespace qeeg
