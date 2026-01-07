#include "qeeg/fft.hpp"

#include <cmath>
#include <stdexcept>

namespace qeeg {

bool is_power_of_two(size_t n) {
  return n != 0 && (n & (n - 1)) == 0;
}

size_t next_power_of_two(size_t n) {
  if (n == 0) throw std::runtime_error("next_power_of_two: n must be > 0");
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

static size_t reverse_bits(size_t x, unsigned bits) {
  size_t y = 0;
  for (unsigned i = 0; i < bits; ++i) {
    y = (y << 1) | (x & 1);
    x >>= 1;
  }
  return y;
}

void fft_inplace(std::vector<std::complex<double>>& a, bool inverse) {
  const size_t n = a.size();
  if (!is_power_of_two(n)) {
    throw std::runtime_error("fft_inplace: size must be a power of two");
  }

  // bit-reversal permutation
  unsigned bits = 0;
  while ((1u << bits) < n) ++bits;

  for (size_t i = 0; i < n; ++i) {
    size_t j = reverse_bits(i, bits);
    if (j > i) std::swap(a[i], a[j]);
  }

  const double pi = std::acos(-1.0);
  for (size_t len = 2; len <= n; len <<= 1) {
    double ang = 2.0 * pi / static_cast<double>(len);
    if (!inverse) ang = -ang;
    std::complex<double> wlen(std::cos(ang), std::sin(ang));

    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        std::complex<double> u = a[i + j];
        std::complex<double> v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }

  if (inverse) {
    for (auto& x : a) x /= static_cast<double>(n);
  }
}

} // namespace qeeg
