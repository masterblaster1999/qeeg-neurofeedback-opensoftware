#pragma once

#include <string>
#include <vector>

namespace qeeg {

// Minimal synchronous subprocess runner used by CLI orchestration tools.
//
// Goals:
//  - dependency-free (no Boost/process)
//  - cross-platform (Windows CreateProcess, POSIX fork/exec)
//  - deterministic exit codes
//
// Notes:
//  - stdout/stderr are inherited from the parent process.
//  - this helper does not capture output.
//  - argv[0] should be the executable path or name.
struct SubprocessResult {
  int exit_code{0};
};

SubprocessResult run_subprocess(const std::vector<std::string>& argv,
                                const std::string& cwd = std::string());

} // namespace qeeg
