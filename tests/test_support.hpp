#pragma once

// Test support helpers.
//
// We intentionally avoid relying on <cassert>'s assert() semantics because
// most Release builds define NDEBUG, which would normally compile assert()
// away and silently turn tests into no-ops.
//
// Instead, tests include this header and continue to use assert(expr) as
// before, but we override the macro to be *always-on* and to fail fast with a
// clear message on stderr.
//
// This approach keeps Release builds warning-free on MSVC (avoids /DNDEBUG vs
// /UNDEBUG command line overrides) while preserving meaningful unit tests.

#include <cassert>  // bring in the standard macro (and its header guard)

#include <cstdlib>
#include <iostream>

namespace qeeg_test {

inline void fail(const char* expr, const char* file, int line) {
  std::cerr << "Test assertion failed: " << expr << " (" << file << ":" << line << ")\n";
  std::exit(1);
}

} // namespace qeeg_test

#ifndef QEEG_TEST_ASSERT
#define QEEG_TEST_ASSERT(expr) \
  (static_cast<bool>(expr) ? (void)0 : ::qeeg_test::fail(#expr, __FILE__, __LINE__))
#endif

// Provide a consistent test assertion in all build types.
//
// If <cassert> already defined assert() as a no-op (because NDEBUG is set), we
// replace it here. If it did not, we still replace it so that the failure mode
// is uniform across platforms (no interactive dialogs on Windows).
#ifdef assert
#undef assert
#endif
#define assert(expr) QEEG_TEST_ASSERT(expr)

// Convenience alias for readability in new tests.
#ifndef TEST_CHECK
#define TEST_CHECK(expr) QEEG_TEST_ASSERT(expr)
#endif
