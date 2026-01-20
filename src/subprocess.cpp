#include "qeeg/subprocess.hpp"

#include "qeeg/utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>

#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>

#endif

namespace qeeg {

namespace {

#if defined(_WIN32)

static std::wstring utf8_to_wide_best_effort(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w;
  w.resize(static_cast<size_t>(n - 1));
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

static std::runtime_error win32_error(const std::string& context, DWORD err) {
  std::ostringstream oss;
  oss << context << " (win32_error=" << static_cast<unsigned long>(err) << ")";
  return std::runtime_error(oss.str());
}

#endif

} // namespace

SubprocessResult run_subprocess(const std::vector<std::string>& argv,
                                const std::string& cwd) {
  if (argv.empty() || argv[0].empty()) {
    throw std::runtime_error("run_subprocess: empty argv");
  }

#if defined(_WIN32)
  std::vector<std::string> argv_s = argv;
  const std::string cmd = join_commandline_args_win32(argv_s);

  const std::wstring cmd_w = utf8_to_wide_best_effort(cmd);
  std::vector<wchar_t> cmd_buf(cmd_w.begin(), cmd_w.end());
  cmd_buf.push_back(L'\0');

  const std::wstring cwd_w = utf8_to_wide_best_effort(cwd);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  // Inherit stdio.
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(
      nullptr,
      cmd_buf.data(),
      nullptr,
      nullptr,
      TRUE,
      0,
      nullptr,
      cwd_w.empty() ? nullptr : cwd_w.c_str(),
      &si,
      &pi);

  if (!ok) {
    const DWORD err = GetLastError();
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    throw win32_error("CreateProcess failed", err);
  }

  if (pi.hThread) CloseHandle(pi.hThread);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  if (!GetExitCodeProcess(pi.hProcess, &code)) {
    const DWORD err = GetLastError();
    CloseHandle(pi.hProcess);
    throw win32_error("GetExitCodeProcess failed", err);
  }
  CloseHandle(pi.hProcess);

  SubprocessResult r;
  r.exit_code = static_cast<int>(code);
  return r;

#else
  // Build argv array.
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  std::vector<std::string> argv_copy = argv;
  for (auto& s : argv_copy) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    throw std::runtime_error("run_subprocess: fork failed");
  }
  if (pid == 0) {
    // Child.
    if (!cwd.empty()) {
      (void)chdir(cwd.c_str());
    }
    execvp(cargv[0], cargv.data());
    // execvp failed.
    std::perror("execvp");
    std::exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    throw std::runtime_error("run_subprocess: waitpid failed");
  }

  SubprocessResult r;
  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  } else {
    r.exit_code = 1;
  }
  return r;
#endif
}

} // namespace qeeg
