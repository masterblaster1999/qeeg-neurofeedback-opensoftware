#include "qeeg/preprocess.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>

static float max_abs(const std::vector<float>& x) {
  float m = 0.0f;
  for (float v : x) {
    const float a = std::fabs(v);
    if (a > m) m = a;
  }
  return m;
}

int main() {
  // Simple CAR sanity check: two identical channels should become ~0.
  qeeg::EEGRecording rec;
  rec.fs_hz = 250.0;
  rec.channel_names = {"C1", "C2"};
  const size_t N = 1000;
  rec.data.resize(2);
  rec.data[0].resize(N);
  rec.data[1].resize(N);
  for (size_t i = 0; i < N; ++i) {
    const float v = static_cast<float>(std::sin(2.0 * 3.141592653589793238462643383279502884 * 10.0 * (static_cast<double>(i) / rec.fs_hz)));
    rec.data[0][i] = v;
    rec.data[1][i] = v;
  }

  qeeg::PreprocessOptions opt;
  opt.average_reference = true;
  qeeg::preprocess_recording_inplace(rec, opt);

  if (max_abs(rec.data[0]) > 1e-6f || max_abs(rec.data[1]) > 1e-6f) {
    std::cerr << "CAR failed: max_abs ch0=" << max_abs(rec.data[0])
              << " ch1=" << max_abs(rec.data[1]) << "\n";
    return 1;
  }

  // Streaming CAR should also zero out identical channels.
  std::vector<std::vector<float>> block(2);
  block[0] = {1.0f, 2.0f, 3.0f, 4.0f};
  block[1] = {1.0f, 2.0f, 3.0f, 4.0f};

  qeeg::StreamingPreprocessor sp(2, 250.0, opt);
  sp.process_block(&block);

  if (max_abs(block[0]) > 1e-6f || max_abs(block[1]) > 1e-6f) {
    std::cerr << "Streaming CAR failed: max_abs ch0=" << max_abs(block[0])
              << " ch1=" << max_abs(block[1]) << "\n";
    return 1;
  }

  std::cout << "OK\n";
  return 0;
}
