Param(
  [Parameter(Position = 0)]
  [string]$Preset = "release"
)

$ErrorActionPreference = "Stop"

function Write-Log {
  Param([string]$Message)
  Write-Host "[ci] $Message"
}

function Get-Jobs {
  if ($env:QEEG_CI_JOBS) { return [int]$env:QEEG_CI_JOBS }
  if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { return [int]$env:CMAKE_BUILD_PARALLEL_LEVEL }
  $v = [Environment]::ProcessorCount
  if ($v -gt 8) { $v = 8 }
  return $v
}

function Resolve-Python {
  $cmd = Get-Command python -ErrorAction SilentlyContinue
  if ($cmd) { return @("python") }

  $cmd = Get-Command py -ErrorAction SilentlyContinue
  if ($cmd) {
    # Prefer Python 3 if available.
    return @("py", "-3")
  }

  throw "Python not found (needed for QEEG_CI_INSTALL_PY_DEPS=1)"
}

function Install-PyDeps {
  $py = Resolve-Python

  Write-Log ("{0} -m pip install --upgrade pip" -f ($py -join " "))
  & $py -m pip install --upgrade pip

  Write-Log ("{0} -m pip install -r scripts/ci/requirements-ci.txt" -f ($py -join " "))
  & $py -m pip install -r scripts/ci/requirements-ci.txt
}

$jobs = Get-Jobs
$strictOptional = ($env:QEEG_CI_STRICT_OPTIONAL_TARGETS -eq "1")

if ($env:QEEG_CI_INSTALL_PY_DEPS -eq "1") {
  Install-PyDeps
}

Write-Log "cmake --preset $Preset"
cmake --preset $Preset

Write-Log "cmake --build --preset $Preset --parallel $jobs"
cmake --build --preset $Preset --parallel $jobs

Write-Log "ctest --preset $Preset --parallel $jobs --output-on-failure"
ctest --preset $Preset --parallel $jobs --output-on-failure

# Optional post-build validation targets.
$optionalTargets = @(
  "qeeg_validate_schema_files",
  "qeeg_validate_nf_cli_json",
  "qeeg_validate_biotrace_json",
  "qeeg_selftest_nf_sessions_dashboard",
  "qeeg_selftest_rt_dashboard"
)

foreach ($target in $optionalTargets) {
  Write-Log "optional: cmake --build --preset $Preset --target $target"
  try {
    cmake --build --preset $Preset --parallel $jobs --target $target
  } catch {
    if ($strictOptional) {
      throw "optional target '$target' failed (set QEEG_CI_STRICT_OPTIONAL_TARGETS=0 to ignore)"
    }
    Write-Log "note: optional target '$target' not available (or failed)"
  }
}
