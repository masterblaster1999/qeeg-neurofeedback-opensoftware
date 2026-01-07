#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace qeeg {

// Returns true if n is a power of two (and n > 0).
bool is_power_of_two(size_t n);

// Returns the smallest power of two >= n (n must be > 0).
size_t next_power_of_two(size_t n);

// In-place radix-2 FFT.
// - a.size() must be a power of two.
// - if inverse=true, computes inverse FFT (and divides by N).
void fft_inplace(std::vector<std::complex<double>>& a, bool inverse);

} // namespace qeeg
