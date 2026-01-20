# Verifies that qeeg_offline_app_cli sets QEEG_TOOLBOX for dispatched tools.
#
# This matters for *CLI file cross integration*: qeeg_pipeline_cli can spawn
# other qeeg_*_cli tools, and in an offline bundle you may only have
# qeeg_offline_app_cli (a multicall toolbox) available.
#
# Expected behavior:
#   qeeg_offline_app_cli qeeg_pipeline_cli --dry-run ...
# should print that the pipeline would run subtools via the toolbox executable.

if(NOT DEFINED QEEG_OFFLINE_APP)
  message(FATAL_ERROR "QEEG_OFFLINE_APP not set")
endif()

set(_tmp "${CMAKE_BINARY_DIR}/qeeg_test_offline_app_pipeline_auto_toolbox")
file(REMOVE_RECURSE "${_tmp}")
file(MAKE_DIRECTORY "${_tmp}")

# --- Build a tiny synthetic recording CSV (no time column) ---
set(_raw "${_tmp}/raw")
file(MAKE_DIRECTORY "${_raw}")

set(_csv "${_raw}/recording.csv")
file(WRITE "${_csv}" "C3,C4,Cz\n")
foreach(i RANGE 0 63)
  math(EXPR a "${i}")
  math(EXPR b "${i}*2")
  math(EXPR c "${i}*3")
  file(APPEND "${_csv}" "${a},${b},${c}\n")
endforeach()

set(_work "${_tmp}/work")

# Ensure the environment is clean so the behavior comes from the offline app.
unset(ENV{QEEG_TOOLBOX})

execute_process(
  COMMAND "${QEEG_OFFLINE_APP}" qeeg_pipeline_cli
          --input "${_raw}"
          --fs 250
          --outdir "${_work}"
          --dry-run
          --skip-preprocess
          --skip-bandratios
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "pipeline dry-run via offline app failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()

# The pipeline prints the command-lines it would run. With QEEG_TOOLBOX set,
# it should use qeeg_offline_app_cli as the launcher for sub-tools.
string(FIND "${_stderr}" "qeeg_offline_app_cli" _pos)
if(_pos EQUAL -1)
  message(FATAL_ERROR "Expected pipeline to launch tools via qeeg_offline_app_cli, but it did not.\nstderr=${_stderr}")
endif()

message(STATUS "Offline app auto toolbox env for pipeline OK")
