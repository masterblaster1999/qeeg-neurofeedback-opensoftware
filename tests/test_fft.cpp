#include "qeeg/fft.hpp"

#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>

// NOTE:
//   We intentionally avoid <cassert>'s assert() for test conditions. Many
//   builds (including CMake Release) compile with -DNDEBUG, which compiles
//   assert() away entirely. That can lead to tests that do nothing in Release
//   *and* to "unused variable" warnings that become hard errors under
//   -Werror.
static void test_check(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::cerr << "Test failure: " << expr << " (" << file << ":" << line << ")\n";
    std::exit(1);
  }
}

#define TEST_CHECK(expr) test_check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

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
      TEST_CHECK(approx(x.real(), 1.0, 1e-9));
      TEST_CHECK(approx(x.imag(), 0.0, 1e-9));
    }

    fft_inplace(a, true);
    TEST_CHECK(approx(a[0].real(), 1.0, 1e-9));
    TEST_CHECK(approx(a[1].real(), 0.0, 1e-9));
    TEST_CHECK(approx(a[2].real(), 0.0, 1e-9));
    TEST_CHECK(approx(a[3].real(), 0.0, 1e-9));
  }

  // FFT round-trip on random vector
  {
    std::vector<std::complex<double>> a(8);
    for (size_t i = 0; i < a.size(); ++i) a[i] = {static_cast<double>(i) * 0.123, -static_cast<double>(i) * 0.01};

    auto orig = a;
    fft_inplace(a, false);
    fft_inplace(a, true);

    for (size_t i = 0; i < a.size(); ++i) {
      TEST_CHECK(approx(a[i].real(), orig[i].real(), 1e-7));
      TEST_CHECK(approx(a[i].imag(), orig[i].imag(), 1e-7));
    }
  }

  std::cout << "All tests passed.\n";
  return 0;
}
