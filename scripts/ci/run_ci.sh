#!/usr/bin/env bash
set -euo pipefail

# Local helper mirroring the GitHub Actions CI workflow.
#
# Usage:
#   ./scripts/ci/run_ci.sh            # defaults to the 'release' CMake preset
#   ./scripts/ci/run_ci.sh debug      # run the 'debug' preset

preset="${1:-release}"

echo "[ci] configure: cmake --preset ${preset}"
cmake --preset "${preset}"

echo "[ci] build: cmake --build --preset ${preset} --parallel"
cmake --build --preset "${preset}" --parallel

echo "[ci] test: ctest --preset ${preset}"
ctest --preset "${preset}"

# Optional: validate machine-readable JSON outputs against schemas/ and run extra smoke-test targets (best effort).
# These convenience targets are added when CMake finds a Python3 interpreter.
for target in \
  qeeg_validate_schema_files \
  qeeg_validate_nf_cli_json \
  qeeg_validate_biotrace_json \
  qeeg_selftest_nf_sessions_dashboard \
  qeeg_selftest_rt_dashboard; do
  echo "[ci] validate (optional): ${target}"
  if ! cmake --build --preset "${preset}" --target "${target}"; then
    echo "[ci] note: '${target}' not available in this build (or failed)." >&2
  fi
done
