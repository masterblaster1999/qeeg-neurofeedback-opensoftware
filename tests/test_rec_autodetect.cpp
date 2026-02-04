#include "qeeg/reader.hpp"

#include "qeeg/bdf_writer.hpp"
#include "qeeg/edf_writer.hpp"

#include "test_support.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static qeeg::EEGRecording make_demo_recording() {
  qeeg::EEGRecording rec;
  rec.fs_hz = 256.0;
  rec.channel_names = {"Cz", "Pz"};

  const int n = 256; // exactly 1 second at 256 Hz
  rec.data.resize(rec.channel_names.size());
  for (auto& ch : rec.data) ch.resize(static_cast<size_t>(n));

  for (int i = 0; i < n; ++i) {
    // Keep values small to avoid clipping and to keep quantization error low.
    rec.data[0][static_cast<size_t>(i)] = static_cast<float>(i % 50) - 25.0f;
    rec.data[1][static_cast<size_t>(i)] = static_cast<float>((i % 20) - 10) * 0.5f;
  }

  return rec;
}

static bool approx_equal(float a, float b, float tol) {
  return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= static_cast<double>(tol);
}

static void remove_if_exists(const std::string& path) {
  (void)std::remove(path.c_str());
}

static void check_recording_matches(const qeeg::EEGRecording& got, const qeeg::EEGRecording& expected,
                                    float tol) {
  assert(got.n_channels() == expected.n_channels());
  assert(got.n_samples() == expected.n_samples());
  assert(got.channel_names == expected.channel_names);
  assert(got.fs_hz == expected.fs_hz);

  // Spot-check samples across the recording.
  const std::vector<size_t> idx = {0, 1, 2, 10, 42, 100, 200, expected.n_samples() - 1};
  for (size_t ch = 0; ch < expected.n_channels(); ++ch) {
    for (size_t i : idx) {
      const float a = got.data[ch][i];
      const float b = expected.data[ch][i];
      if (!approx_equal(a, b, tol)) {
        std::cerr << "Mismatch at ch=" << ch << " i=" << i << " got=" << a << " expected=" << b << "\n";
        std::exit(1);
      }
    }
  }
}

int main() {
  using namespace qeeg;

  const EEGRecording src = make_demo_recording();

  // 1) BDF content saved with a .rec extension (common in some BioTrace+/NeXus workflows)
  {
    const std::string path = "test_tmp_rec_autodetect_bdf_as_rec.rec";
    remove_if_exists(path);

    BDFWriter w;
    BDFWriterOptions opts;
    opts.record_duration_seconds = 1.0;
    w.write(src, path, opts);

    EEGRecording got = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
    check_recording_matches(got, src, /*tol=*/1e-3f);

    remove_if_exists(path);
  }

  // 2) EDF content saved with a .rec extension
  {
    const std::string path = "test_tmp_rec_autodetect_edf_as_rec.rec";
    remove_if_exists(path);

    EDFWriter w;
    EDFWriterOptions opts;
    opts.record_duration_seconds = 1.0;
    w.write(src, path, opts);

    EEGRecording got = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
    check_recording_matches(got, src, /*tol=*/1e-2f);

    remove_if_exists(path);
  }

  // 3) BDF content saved with a .edf extension (misnamed export)
  {
    const std::string path = "test_tmp_rec_autodetect_bdf_as_edf.edf";
    remove_if_exists(path);

    BDFWriter w;
    BDFWriterOptions opts;
    opts.record_duration_seconds = 1.0;
    w.write(src, path, opts);

    EEGRecording got = read_recording_auto(path, /*fs_hz_for_csv=*/0.0);
    check_recording_matches(got, src, /*tol=*/1e-3f);

    remove_if_exists(path);
  }

  return 0;
}
