Param(
  [Parameter(Position = 0)]
  [string]$Preset = "release"
)

# Local helper mirroring the GitHub Actions CI workflow (PowerShell edition).
#
# Usage (Windows):
#   powershell -ExecutionPolicy Bypass -File .\scripts\ci\run_ci.ps1
#   powershell -ExecutionPolicy Bypass -File .\scripts\ci\run_ci.ps1 debug

function Run-Step {
  Param(
    [Parameter(Mandatory = $true)]
    [string]$Label,
    [Parameter(Mandatory = $true)]
    [string]$Exe,
    [Parameter(Mandatory = $true)]
    [string[]]$Args
  )

  Write-Host $Label
  & $Exe @Args
  $code = $LASTEXITCODE
  if ($code -ne 0) {
    Write-Error "Step failed (exit $code): $Exe $($Args -join ' ')"
    exit $code
  }
}

Run-Step "[ci] configure: cmake --preset $Preset" "cmake" @("--preset", $Preset)
Run-Step "[ci] build: cmake --build --preset $Preset --parallel" "cmake" @("--build", "--preset", $Preset, "--parallel")
Run-Step "[ci] test: ctest --preset $Preset" "ctest" @("--preset", $Preset)

# Optional: validate machine-readable JSON outputs against schemas/.
# These convenience targets are added when CMake finds a Python3 interpreter.
$targets = @(
  "qeeg_validate_schema_files",
  "qeeg_validate_nf_cli_json",
  "qeeg_validate_biotrace_json",
  "qeeg_selftest_nf_sessions_dashboard",
  "qeeg_selftest_rt_dashboard"
)

foreach ($t in $targets) {
  Write-Host "[ci] validate (optional): $t"
  & cmake @("--build", "--preset", $Preset, "--target", $t)
  $code = $LASTEXITCODE
  if ($code -ne 0) {
    Write-Warning "[ci] note: '$t' not available in this build (or failed)."
  }
}
