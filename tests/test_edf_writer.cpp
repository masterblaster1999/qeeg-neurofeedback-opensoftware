#include "qeeg/edf_reader.hpp"
#include "qeeg/edf_writer.hpp"
#include "qeeg/types.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

static bool approx(float a, float b, float eps) {
  const float d = (a > b) ? (a - b) : (b - a);
  return d <= eps;
}

int main() {
  using namespace qeeg;

  EEGRecording rec;
  rec.channel_names = {"C3", "C4"};
  rec.fs_hz = 100.0;

  const size_t n = 250;
  rec.data.resize(2);
  rec.data[0].resize(n);
  rec.data[1].resize(n);

  const double two_pi = 6.2831853071795864769;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rec.fs_hz;
    rec.data[0][i] = static_cast<float>(100.0 * std::sin(two_pi * 10.0 * t));
    rec.data[1][i] = static_cast<float>(50.0 * std::cos(two_pi * 5.0 * t));
  }
  // Add a couple of events so we round-trip EDF+ annotations.
  rec.events.push_back(AnnotationEvent{0.5, 0.0, "Start"});
  rec.events.push_back(AnnotationEvent{1.2, 0.3, "Task"});


  const std::filesystem::path out = std::filesystem::temp_directory_path() / "qeeg_test_edf_writer_roundtrip.edf";

  EDFWriterOptions opts;
  opts.record_duration_seconds = 0.0; // single datarecord (no padding)
  opts.patient_id = "X";
  opts.recording_id = "qeeg-test";
  opts.physical_dimension = "uV";

  EDFWriter w;
  w.write(rec, out.string(), opts);

  EDFReader r;
  EEGRecording rec2 = r.read(out.string());

  assert(rec2.channel_names.size() == rec.channel_names.size());
  assert(rec2.n_samples() == rec.n_samples());
  assert(rec2.fs_hz == rec.fs_hz);

  // EDFReader should parse EDF+ "EDF Annotations" back into rec2.events.
  assert(rec2.events.size() == 2);

  auto find_event = [&](const std::string& text) -> const AnnotationEvent* {
    for (const auto& e : rec2.events) {
      if (e.text == text) return &e;
    }
    return nullptr;
  };

  const AnnotationEvent* e_start = find_event("Start");
  assert(e_start);
  assert(std::fabs(e_start->onset_sec - 0.5) < 1e-3);
  assert(std::fabs(e_start->duration_sec - 0.0) < 1e-3);

  const AnnotationEvent* e_task = find_event("Task");
  assert(e_task);
  assert(std::fabs(e_task->onset_sec - 1.2) < 1e-3);
  assert(std::fabs(e_task->duration_sec - 0.3) < 1e-3);

  float max_err = 0.0f;
  for (size_t ch = 0; ch < rec.data.size(); ++ch) {
    for (size_t i = 0; i < n; ++i) {
      const float e = std::fabs(rec2.data[ch][i] - rec.data[ch][i]);
      if (e > max_err) max_err = e;
      assert(approx(rec2.data[ch][i], rec.data[ch][i], 0.1f));
    }
  }

  std::filesystem::remove(out);

  std::cout << "OK (max_err=" << max_err << ")\n";
  return 0;
}
