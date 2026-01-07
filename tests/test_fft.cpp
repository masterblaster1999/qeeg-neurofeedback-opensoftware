#include "qeeg/fft.hpp"

#include <cassert>
#include <complex>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps=1e-9) {
  return (a > b ? a - b : b - a) <= eps;
}

int main() {
  using namespace qeeg;

  // FFT of impulse should be all ones
  {
    std::vector<std::complex<double>> a(4);
    a[0] = {1.0, 0.0};
    a[1] = {0.0, 0.0};
    a[2] = {0.0, 0.0};
    a[3] = {0.0, 0.0};

    fft_inplace(a, false);
    for (auto& x : a) {
      assert(approx(x.real(), 1.0, 1e-9));
      assert(approx(x.imag(), 0.0, 1e-9));
    }

    fft_inplace(a, true);
    assert(approx(a[0].real(), 1.0, 1e-9));
    assert(approx(a[1].real(), 0.0, 1e-9));
    assert(approx(a[2].real(), 0.0, 1e-9));
    assert(approx(a[3].real(), 0.0, 1e-9));
  }

  // FFT round-trip on random vector
  {
    std::vector<std::complex<double>> a(8);
    for (size_t i = 0; i < a.size(); ++i) a[i] = {static_cast<double>(i) * 0.123, -static_cast<double>(i) * 0.01};

    auto orig = a;
    fft_inplace(a, false);
    fft_inplace(a, true);

    for (size_t i = 0; i < a.size(); ++i) {
      assert(approx(a[i].real(), orig[i].real(), 1e-7));
      assert(approx(a[i].imag(), orig[i].imag(), 1e-7));
    }
  }

  std::cout << "All tests passed.\n";
  return 0;
}
