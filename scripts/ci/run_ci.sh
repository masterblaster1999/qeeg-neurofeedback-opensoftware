#!/usr/bin/env bash
set -euo pipefail

preset="${1:-release}"

# CI knobs (optional):
#   QEEG_CI_JOBS                 - parallelism for build/test (default: CPU count)
#   QEEG_CI_INSTALL_PY_DEPS       - if set to "1", install scripts/ci/requirements-ci.txt
#   QEEG_CI_STRICT_OPTIONAL_TARGETS - if set to "1", fail if optional targets cannot run

if [[ -n "${QEEG_CI_JOBS:-}" ]]; then
  jobs="${QEEG_CI_JOBS}"
elif [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
  jobs="${CMAKE_BUILD_PARALLEL_LEVEL}"
else
  if command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
  else
    jobs=2
  fi
fi

# Cap auto-detected parallelism to keep CI stable on high-core machines.
if [[ -z "${QEEG_CI_JOBS:-}" && -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
  if [[ "${jobs}" =~ ^[0-9]+$ ]] && [[ "${jobs}" -gt 8 ]]; then
    jobs=8
  fi
fi

strict_optional="${QEEG_CI_STRICT_OPTIONAL_TARGETS:-0}"

log() {
  printf '[ci] %s\n' "$*"
}

die() {
  printf '[ci] error: %s\n' "$*" >&2
  exit 1
}

run() {
  log "$*"
  "$@"
}

install_py_deps() {
  local py=""
  if command -v python3 >/dev/null 2>&1; then
    py=python3
  elif command -v python >/dev/null 2>&1; then
    py=python
  else
    die "Python not found (needed for QEEG_CI_INSTALL_PY_DEPS=1)"
  fi

  run "${py}" -m pip install --upgrade pip
  run "${py}" -m pip install -r scripts/ci/requirements-ci.txt
}

if [[ "${QEEG_CI_INSTALL_PY_DEPS:-0}" == "1" ]]; then
  install_py_deps
fi

run cmake --preset "${preset}"
run cmake --build --preset "${preset}" --parallel "${jobs}"

# Run tests (if the preset has them enabled).
# '--output-on-failure' is also configured in CMakePresets.json but we pass it
# explicitly for robustness.
run ctest --preset "${preset}" --parallel "${jobs}" --output-on-failure

# Optional post-build validation targets.
# These are intentionally best-effort because some builds may disable them.
optional_targets=(
  qeeg_validate_schema_files
  qeeg_validate_nf_cli_json
  qeeg_validate_biotrace_json
  qeeg_selftest_nf_sessions_dashboard
  qeeg_selftest_rt_dashboard
)

for target in "${optional_targets[@]}"; do
  log "optional: cmake --build --preset ${preset} --target ${target}"
  if cmake --build --preset "${preset}" --parallel "${jobs}" --target "${target}"; then
    :
  else
    if [[ "${strict_optional}" == "1" ]]; then
      die "optional target '${target}' failed (set QEEG_CI_STRICT_OPTIONAL_TARGETS=0 to ignore)"
    fi
    log "note: optional target '${target}' not available (or failed)"
  fi
done
