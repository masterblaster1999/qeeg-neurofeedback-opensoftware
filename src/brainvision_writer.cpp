#include "qeeg/brainvision_writer.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace qeeg {

namespace fs = std::filesystem;

static void write_le_i16(std::ofstream& os, int16_t v) {
  char b[2];
  b[0] = static_cast<char>(v & 0xFF);
  b[1] = static_cast<char>((v >> 8) & 0xFF);
  os.write(b, 2);
}

static void write_le_f32(std::ofstream& os, float f) {
  static_assert(sizeof(float) == 4, "float must be 32-bit");
  uint32_t u = 0;
  std::memcpy(&u, &f, sizeof(float));
  char b[4];
  b[0] = static_cast<char>(u & 0xFF);
  b[1] = static_cast<char>((u >> 8) & 0xFF);
  b[2] = static_cast<char>((u >> 16) & 0xFF);
  b[3] = static_cast<char>((u >> 24) & 0xFF);
  os.write(b, 4);
}

static std::string sanitize_marker_text(std::string s) {
  // Marker fields are comma-separated. Be conservative and avoid commas/newlines.
  for (char& c : s) {
    if (c == '\r' || c == '\n') c = ' ';
    if (c == ',') c = ';';
  }
  return trim(s);
}

static fs::path ensure_vhdr_extension(fs::path p) {
  if (p.extension() != ".vhdr") {
    p.replace_extension(".vhdr");
  }
  return p;
}

void BrainVisionWriter::write(const EEGRecording& rec, const std::string& vhdr_path_in,
                             const BrainVisionWriterOptions& opts) {
  if (rec.fs_hz <= 0.0) {
    throw std::runtime_error("BrainVisionWriter: rec.fs_hz must be > 0");
  }
  if (rec.data.empty() || rec.channel_names.empty()) {
    throw std::runtime_error("BrainVisionWriter: recording has no channels");
  }
  if (rec.channel_names.size() != rec.data.size()) {
    throw std::runtime_error("BrainVisionWriter: channel_names size does not match data size");
  }
  const size_t n_channels = rec.n_channels();
  const size_t n_samples = rec.n_samples();
  for (size_t ch = 0; ch < n_channels; ++ch) {
    if (rec.data[ch].size() != n_samples) {
      throw std::runtime_error("BrainVisionWriter: channels have inconsistent sample counts");
    }
  }

  fs::path vhdr_path = ensure_vhdr_extension(fs::path(vhdr_path_in));
  fs::path outdir = vhdr_path.parent_path();
  if (!outdir.empty()) {
    fs::create_directories(outdir);
  }

  const std::string base_name = vhdr_path.stem().string();
  fs::path eeg_path = outdir / (base_name + ".eeg");
  fs::path vmrk_path = outdir / (base_name + ".vmrk");

  const std::string eeg_file_ref = eeg_path.filename().string();
  const std::string vmrk_file_ref = vmrk_path.filename().string();

  const long long sampling_interval_us_ll = std::llround(1e6 / rec.fs_hz);
  if (sampling_interval_us_ll <= 0 || sampling_interval_us_ll > std::numeric_limits<int>::max()) {
    throw std::runtime_error("BrainVisionWriter: invalid SamplingInterval derived from fs_hz");
  }
  const int sampling_interval_us = static_cast<int>(sampling_interval_us_ll);

  // Derive per-channel resolution if using INT_16.
  std::vector<double> resolution(n_channels, 1.0);
  if (opts.binary_format == BrainVisionBinaryFormat::Int16) {
    const double eps = 1e-9;
    if (opts.int16_target_max_digital <= 0) {
      throw std::runtime_error("BrainVisionWriter: int16_target_max_digital must be > 0");
    }
    if (opts.int16_resolution > 0.0) {
      std::fill(resolution.begin(), resolution.end(), opts.int16_resolution);
    } else {
      for (size_t ch = 0; ch < n_channels; ++ch) {
        double max_abs = 0.0;
        for (size_t i = 0; i < n_samples; ++i) {
          max_abs = std::max(max_abs, std::abs(static_cast<double>(rec.data[ch][i])));
        }
        if (max_abs <= 0.0) {
          resolution[ch] = 1.0;
        } else {
          resolution[ch] = std::max(eps, max_abs / static_cast<double>(opts.int16_target_max_digital));
        }
      }
    }
  }

  // --- Write EEG binary file (.eeg) ---
  {
    std::ofstream os(eeg_path, std::ios::binary);
    if (!os) {
      throw std::runtime_error("BrainVisionWriter: failed to open EEG file for writing: " + eeg_path.string());
    }

    if (opts.binary_format == BrainVisionBinaryFormat::Float32) {
      for (size_t i = 0; i < n_samples; ++i) {
        for (size_t ch = 0; ch < n_channels; ++ch) {
          write_le_f32(os, rec.data[ch][i]);
        }
      }
    } else {
      for (size_t i = 0; i < n_samples; ++i) {
        for (size_t ch = 0; ch < n_channels; ++ch) {
          const double r = resolution[ch];
          double dv = (r > 0.0) ? (static_cast<double>(rec.data[ch][i]) / r) : 0.0;
          long long q = std::llround(dv);
          if (q < -32768) q = -32768;
          if (q > 32767) q = 32767;
          write_le_i16(os, static_cast<int16_t>(q));
        }
      }
    }
  }

  // --- Write header (.vhdr) ---
  {
    std::ofstream os(vhdr_path);
    if (!os) {
      throw std::runtime_error("BrainVisionWriter: failed to open header file for writing: " + vhdr_path.string());
    }

    os << "Brain Vision Data Exchange Header File Version 1.0\n\n";
    os << "[Common Infos]\n";
    os << "Codepage=" << opts.codepage << "\n";
    os << "DataFile=" << eeg_file_ref << "\n";
    os << "MarkerFile=" << vmrk_file_ref << "\n";
    os << "DataFormat=BINARY\n";
    os << "DataOrientation=MULTIPLEXED\n";
    os << "DataType=TIMEDOMAIN\n";
    os << "NumberOfChannels=" << n_channels << "\n";
    os << "SamplingInterval=" << sampling_interval_us << "\n\n";

    os << "[Binary Infos]\n";
    if (opts.binary_format == BrainVisionBinaryFormat::Float32) {
      os << "BinaryFormat=IEEE_FLOAT_32\n\n";
    } else {
      os << "BinaryFormat=INT_16\n\n";
    }

    os << "[Channel Infos]\n";
    // BrainVision expects per-channel resolution even for float32.
    os.setf(std::ios::fixed);
    os.precision(10);
    for (size_t ch = 0; ch < n_channels; ++ch) {
      const double r = (opts.binary_format == BrainVisionBinaryFormat::Int16) ? resolution[ch] : 1.0;
      os << "Ch" << (ch + 1) << "=" << rec.channel_names[ch] << ",," << r << "," << opts.unit << "\n";
    }

    os << "\n[Comment]\n";
    os << "Generated by qeeg BrainVisionWriter\n";
  }

  // --- Write marker file (.vmrk) ---
  {
    std::ofstream os(vmrk_path);
    if (!os) {
      throw std::runtime_error("BrainVisionWriter: failed to open marker file for writing: " + vmrk_path.string());
    }

    os << "Brain Vision Data Exchange Marker File Version 1.0\n\n";
    os << "[Common Infos]\n";
    os << "Codepage=" << opts.codepage << "\n";
    os << "DataFile=" << eeg_file_ref << "\n\n";

    os << "[Marker Infos]\n";
    int mk = 1;
    if (opts.write_new_segment_marker) {
      os << "Mk" << mk++ << "=New Segment,,1,1,0\n";
    }

    if (opts.write_events) {
      for (const auto& ev : rec.events) {
        long long pos = std::llround(ev.onset_sec * rec.fs_hz) + 1;
        if (pos < 1) pos = 1;
        if (pos > static_cast<long long>(n_samples)) pos = static_cast<long long>(n_samples);

        long long pts = std::llround(ev.duration_sec * rec.fs_hz);
        if (pts < 1) pts = 1;

        std::string desc = sanitize_marker_text(ev.text);
        os << "Mk" << mk++ << "=Comment," << desc << "," << pos << "," << pts << ",0\n";
      }
    }
  }
}

} // namespace qeeg
