#include "qeeg/bdf_writer.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qeeg {

static void write_field(std::ofstream& f, const std::string& s, size_t width) {
  std::string out = s;
  if (out.size() > width) out = out.substr(0, width);
  if (out.size() < width) out.append(width - out.size(), ' ');
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!f) throw std::runtime_error("BDFWriter: failed writing header field");
}

static void write_bytes(std::ofstream& f, const void* data, size_t n) {
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
  if (!f) throw std::runtime_error("BDFWriter: failed writing bytes");
}

static std::string format_double_fixed_width(double v, size_t width) {
  // Header numeric fields are ASCII. Try a few fixed precisions and fall back to integer.
  for (int prec = 6; prec >= 0; --prec) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(prec) << v;
    std::string s = oss.str();
    if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.size() <= width) return s;
  }
  {
    long long iv = static_cast<long long>(std::llround(v));
    std::string s = std::to_string(iv);
    if (s.size() <= width) return s;
  }
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(0) << v;
  std::string s = oss.str();
  if (s.size() > width) s = s.substr(0, width);
  return s;
}

static void write_i24_le(std::ofstream& f, int32_t v) {
  // 24-bit two's complement, little endian (3 bytes per sample).
  const uint32_t u = static_cast<uint32_t>(v);
  char b[3];
  b[0] = static_cast<char>(u & 0xFFu);
  b[1] = static_cast<char>((u >> 8) & 0xFFu);
  b[2] = static_cast<char>((u >> 16) & 0xFFu);
  f.write(b, 3);
  if (!f) throw std::runtime_error("BDFWriter: failed writing sample data");
}

// ---- BDF+ TAL helpers (same conventions as EDF+) ----

static std::string format_tal_number(double v, bool force_plus) {
  if (!std::isfinite(v)) v = 0.0;
  if (std::fabs(v) < 1e-12) v = 0.0;

  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(6) << v;
  std::string s = oss.str();

  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }

  if (force_plus && !s.empty() && s.front() != '-') {
    s.insert(s.begin(), '+');
  }
  return s;
}

static std::string format_tal_onset(double onset_sec) {
  return format_tal_number(onset_sec, /*force_plus=*/true);
}

static std::string format_tal_duration(double dur_sec) {
  if (!std::isfinite(dur_sec) || dur_sec <= 0.0) return std::string();
  return format_tal_number(dur_sec, /*force_plus=*/false);
}

static std::string sanitize_tal_text(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char uc : in) {
    if (uc == 0x00 || uc == 0x14 || uc == 0x15 || uc == '\n' || uc == '\r' || uc == '\t') {
      out.push_back(' ');
    } else if (uc < 0x20) {
      out.push_back(' ');
    } else if (uc > 0x7E) {
      out.push_back('?');
    } else {
      out.push_back(static_cast<char>(uc));
    }
  }
  return trim(out);
}

static size_t estimate_tal_record_length(double record_onset_sec,
                                         const std::vector<AnnotationEvent>& events) {
  size_t len = 0;

  const std::string t0 = format_tal_onset(record_onset_sec);
  len += t0.size();
  len += 1; // 0x14

  for (const auto& ev : events) {
    const std::string txt = sanitize_tal_text(ev.text);
    if (txt.empty()) continue;

    const std::string onset = format_tal_onset(ev.onset_sec);
    const std::string dur = format_tal_duration(ev.duration_sec);

    len += onset.size();
    if (!dur.empty()) {
      len += 1; // 0x15
      len += dur.size();
    }
    len += 1;          // 0x14
    len += txt.size(); // text
    len += 1;          // 0x14
  }

  return len;
}

static std::vector<uint8_t> build_tal_record_bytes(double record_onset_sec,
                                                   const std::vector<AnnotationEvent>& events_in,
                                                   size_t nbytes) {
  std::vector<AnnotationEvent> events = events_in;
  std::sort(events.begin(), events.end(), [](const AnnotationEvent& a, const AnnotationEvent& b) {
    if (a.onset_sec != b.onset_sec) return a.onset_sec < b.onset_sec;
    if (a.duration_sec != b.duration_sec) return a.duration_sec < b.duration_sec;
    return a.text < b.text;
  });

  std::vector<uint8_t> out(nbytes, 0);
  size_t pos = 0;

  auto push_byte = [&](uint8_t b) {
    if (pos >= out.size()) {
      throw std::runtime_error(
          "BDFWriter: annotation record overflow (increase annotation_samples_per_record)");
    }
    out[pos++] = b;
  };

  auto push_str = [&](const std::string& s) {
    for (unsigned char uc : s) {
      push_byte(static_cast<uint8_t>(uc));
    }
  };

  // Per-record timekeeping marker (empty text).
  push_str(format_tal_onset(record_onset_sec));
  push_byte(0x14);

  for (const auto& ev : events) {
    const std::string txt = sanitize_tal_text(ev.text);
    if (txt.empty()) continue;

    push_str(format_tal_onset(ev.onset_sec));
    const std::string dur = format_tal_duration(ev.duration_sec);
    if (!dur.empty()) {
      push_byte(0x15);
      push_str(dur);
    }
    push_byte(0x14);
    push_str(txt);
    push_byte(0x14);
  }

  return out;
}

static size_t round_up(size_t x, size_t multiple) {
  if (multiple == 0) return x;
  const size_t r = x % multiple;
  return (r == 0) ? x : (x + (multiple - r));
}

void BDFWriter::write(const EEGRecording& rec, const std::string& path, const BDFWriterOptions& opts) {
  if (rec.channel_names.empty() || rec.data.empty()) {
    throw std::runtime_error("BDFWriter: recording has no channels/data");
  }
  if (rec.channel_names.size() != rec.data.size()) {
    throw std::runtime_error("BDFWriter: channel_names size != data size");
  }
  if (!(rec.fs_hz > 0.0)) {
    throw std::runtime_error("BDFWriter: invalid sampling rate");
  }

  const int ns_data = static_cast<int>(rec.data.size());
  const size_t n_samples = rec.n_samples();
  if (n_samples == 0) throw std::runtime_error("BDFWriter: recording has zero samples");

  for (int ch = 0; ch < ns_data; ++ch) {
    if (rec.data[static_cast<size_t>(ch)].size() != n_samples) {
      throw std::runtime_error("BDFWriter: all channels must have the same number of samples");
    }
  }

  const bool want_annotations = opts.write_bdfplus_annotations && !rec.events.empty();

  // Determine record duration + samples per record for DATA channels.
  double record_duration = opts.record_duration_seconds;
  int data_samples_per_record = 0;
  int num_records = 0;

  if (record_duration <= 0.0) {
    // Single record, no padding.
    record_duration = static_cast<double>(n_samples) / rec.fs_hz;
    if (!(record_duration > 0.0)) {
      throw std::runtime_error("BDFWriter: invalid duration derived from n_samples/fs");
    }
    data_samples_per_record = static_cast<int>(n_samples);
    num_records = 1;
  } else {
    const double spr_d = rec.fs_hz * record_duration;
    data_samples_per_record = static_cast<int>(std::llround(spr_d));
    if (std::fabs(spr_d - static_cast<double>(data_samples_per_record)) > 1e-6) {
      throw std::runtime_error(
          "BDFWriter: fs_hz * record_duration_seconds must be an integer (or use record_duration_seconds<=0)");
    }
    if (data_samples_per_record <= 0) throw std::runtime_error("BDFWriter: invalid samples_per_record");
    num_records = static_cast<int>((n_samples + static_cast<size_t>(data_samples_per_record) - 1) /
                                   static_cast<size_t>(data_samples_per_record));
    if (num_records <= 0) throw std::runtime_error("BDFWriter: invalid num_records");
  }

  // Group events by datarecord index.
  std::vector<std::vector<AnnotationEvent>> events_by_record;
  if (want_annotations) {
    events_by_record.resize(static_cast<size_t>(num_records));
    for (const auto& ev : rec.events) {
      if (!std::isfinite(ev.onset_sec)) continue;
      int r = 0;
      if (num_records > 1 && record_duration > 0.0) {
        r = static_cast<int>(std::floor(ev.onset_sec / record_duration));
      }
      if (r < 0) r = 0;
      if (r >= num_records) r = num_records - 1;
      events_by_record[static_cast<size_t>(r)].push_back(ev);
    }
  }

  // Determine annotation samples per record (each sample encodes 1 TAL byte in low 8 bits).
  int ann_samples_per_record = 0;
  if (want_annotations) {
    const size_t kMinAnn = 80;  // conservative minimum
    const size_t kRoundTo = 16; // nice alignment; not required

    if (opts.annotation_samples_per_record > 0) {
      ann_samples_per_record = opts.annotation_samples_per_record;
    } else {
      size_t max_need = 0;
      for (int r = 0; r < num_records; ++r) {
        const double t0 = static_cast<double>(r) * record_duration;
        const size_t need = estimate_tal_record_length(t0, events_by_record[static_cast<size_t>(r)]);
        max_need = std::max(max_need, need);
      }
      size_t chosen = std::max(max_need, kMinAnn);
      chosen = round_up(chosen, kRoundTo);
      if (chosen > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("BDFWriter: annotation_samples_per_record too large");
      }
      ann_samples_per_record = static_cast<int>(chosen);
    }

    for (int r = 0; r < num_records; ++r) {
      const double t0 = static_cast<double>(r) * record_duration;
      const size_t need = estimate_tal_record_length(t0, events_by_record[static_cast<size_t>(r)]);
      if (need > static_cast<size_t>(ann_samples_per_record)) {
        throw std::runtime_error(
            "BDFWriter: annotation_samples_per_record too small for events; set a larger value or use auto (0)");
      }
    }
  }

  const int ns_total = ns_data + (want_annotations ? 1 : 0);
  const int header_bytes = 256 + ns_total * 256;

  // BDF uses 24-bit signed integers.
  const int32_t dig_min = -8388608;
  const int32_t dig_max = 8388607;

  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(ns_total));
  for (int ch = 0; ch < ns_data; ++ch) labels.push_back(rec.channel_names[static_cast<size_t>(ch)]);
  if (want_annotations) labels.emplace_back("BDF Annotations");

  std::vector<std::string> phys_dim;
  phys_dim.reserve(static_cast<size_t>(ns_total));
  for (int ch = 0; ch < ns_data; ++ch) phys_dim.push_back(opts.physical_dimension);
  if (want_annotations) phys_dim.emplace_back("");

  std::vector<int> samples_per_record;
  samples_per_record.reserve(static_cast<size_t>(ns_total));
  for (int ch = 0; ch < ns_data; ++ch) samples_per_record.push_back(data_samples_per_record);
  if (want_annotations) samples_per_record.push_back(ann_samples_per_record);

  // Optional sanity: BDF+ format description recommends max 15MB per datarecord.
  // bytes_per_record = sum(samples_per_record) * 3 bytes.
  {
    std::uintmax_t bpr = 0;
    for (int s = 0; s < ns_total; ++s) {
      bpr += static_cast<std::uintmax_t>(samples_per_record[static_cast<size_t>(s)]) * 3u;
    }
    const std::uintmax_t kMaxBpr = 15u * 1024u * 1024u;
    if (bpr > kMaxBpr) {
      throw std::runtime_error("BDFWriter: datarecord too large (>15MB). Reduce record_duration_seconds or channels.");
    }
  }

  std::vector<double> phys_min(ns_total, 0.0), phys_max(ns_total, 0.0);
  std::vector<double> scale(ns_total, 1.0), offset(ns_total, 0.0);

  for (int ch = 0; ch < ns_data; ++ch) {
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < n_samples; ++i) {
      const double v = static_cast<double>(rec.data[static_cast<size_t>(ch)][i]);
      if (!std::isfinite(v)) continue;
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) {
      mn = -1.0;
      mx = 1.0;
    }
    if (mx < mn) std::swap(mx, mn);
    double pmin = mn;
    double pmax = mx;
    double range = pmax - pmin;
    if (!(range > 0.0)) {
      const double c = 0.5 * (pmin + pmax);
      pmin = c - 1.0;
      pmax = c + 1.0;
      range = pmax - pmin;
    } else {
      const double pad = range * std::max(0.0, opts.physical_padding_fraction);
      pmin -= pad;
      pmax += pad;
      range = pmax - pmin;
      if (!(range > 0.0)) {
        const double c = 0.5 * (pmin + pmax);
        pmin = c - 1.0;
        pmax = c + 1.0;
        range = pmax - pmin;
      }
    }

    phys_min[ch] = pmin;
    phys_max[ch] = pmax;

    const double dig_range = static_cast<double>(dig_max) - static_cast<double>(dig_min);
    scale[ch] = range / dig_range;
    offset[ch] = pmin - static_cast<double>(dig_min) * scale[ch];
    if (!(scale[ch] > 0.0)) throw std::runtime_error("BDFWriter: internal scale error");
  }

  if (want_annotations) {
    const int ann = ns_total - 1;
    phys_min[ann] = static_cast<double>(dig_min);
    phys_max[ann] = static_cast<double>(dig_max);
    scale[ann] = 1.0;
    offset[ann] = 0.0;
  }

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("BDFWriter: failed to open for writing: " + path);

  // --- Fixed header (256 bytes) ---
  // BDF uses a non-ASCII version field: 0xFF followed by "BIOSEMI".
  const unsigned char version[8] = {0xFFu, 'B', 'I', 'O', 'S', 'E', 'M', 'I'};
  write_bytes(f, version, sizeof(version));
  write_field(f, opts.patient_id, 80);
  write_field(f, opts.recording_id, 80);
  write_field(f, opts.start_date_dd_mm_yy, 8);
  write_field(f, opts.start_time_hh_mm_ss, 8);
  write_field(f, std::to_string(header_bytes), 8);

  // Reserved field:
  // - plain BDF: typically starts with "24BIT"
  // - BDF+: starts with "BDF+C" (continuous) or "BDF+D" (discontinuous)
  if (want_annotations) {
    write_field(f, "BDF+C", 44);
  } else {
    write_field(f, "24BIT", 44);
  }

  write_field(f, std::to_string(num_records), 8);
  write_field(f, format_double_fixed_width(record_duration, 8), 8);
  write_field(f, std::to_string(ns_total), 4);

  // --- Per-signal header (ns_total * 256 bytes), stored field-by-field ---
  for (int s = 0; s < ns_total; ++s) write_field(f, labels[static_cast<size_t>(s)], 16); // label
  for (int s = 0; s < ns_total; ++s) write_field(f, "", 80); // transducer
  for (int s = 0; s < ns_total; ++s) write_field(f, phys_dim[static_cast<size_t>(s)], 8); // phys dim
  for (int s = 0; s < ns_total; ++s) write_field(f, format_double_fixed_width(phys_min[s], 8), 8); // phys min
  for (int s = 0; s < ns_total; ++s) write_field(f, format_double_fixed_width(phys_max[s], 8), 8); // phys max
  for (int s = 0; s < ns_total; ++s) write_field(f, std::to_string(dig_min), 8); // dig min
  for (int s = 0; s < ns_total; ++s) write_field(f, std::to_string(dig_max), 8); // dig max
  for (int s = 0; s < ns_total; ++s) write_field(f, "", 80); // prefiltering
  for (int s = 0; s < ns_total; ++s) write_field(f, std::to_string(samples_per_record[static_cast<size_t>(s)]), 8);
  for (int s = 0; s < ns_total; ++s) write_field(f, "", 32); // reserved per signal

  // --- Data records ---
  const size_t spr_data = static_cast<size_t>(data_samples_per_record);

  for (int r = 0; r < num_records; ++r) {
    const size_t base = static_cast<size_t>(r) * spr_data;

    // EEG data channels.
    for (int ch = 0; ch < ns_data; ++ch) {
      const auto& x = rec.data[static_cast<size_t>(ch)];
      for (size_t i = 0; i < spr_data; ++i) {
        const size_t idx = base + i;
        double phys = 0.0;
        if (idx < n_samples) {
          phys = static_cast<double>(x[idx]);
          if (!std::isfinite(phys)) phys = 0.0;
        }
        const double d = (phys - offset[ch]) / scale[ch];
        long long di = static_cast<long long>(std::llround(d));
        if (di < dig_min) di = dig_min;
        if (di > dig_max) di = dig_max;
        write_i24_le(f, static_cast<int32_t>(di));
      }
    }

    // BDF+ annotation channel (if present) should come last.
    if (want_annotations) {
      const double t0 = static_cast<double>(r) * record_duration;
      const auto& evs = events_by_record[static_cast<size_t>(r)];
      const std::vector<uint8_t> bytes =
          build_tal_record_bytes(t0, evs, static_cast<size_t>(ann_samples_per_record));
      for (uint8_t b : bytes) {
        const int32_t dig = static_cast<int32_t>(static_cast<uint32_t>(b));
        write_i24_le(f, dig);
      }
    }
  }
}

} // namespace qeeg
