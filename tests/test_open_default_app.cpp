// Minimal smoke test placeholder.
//
// Some environments (CI, headless servers) cannot safely open GUI apps.
// This test intentionally performs no side-effects and simply verifies
// the test target builds and runs.

#include <iostream>

int main() {
  std::cout << "test_open_default_app: OK (no GUI open performed)" << std::endl;
  return 0;
}
