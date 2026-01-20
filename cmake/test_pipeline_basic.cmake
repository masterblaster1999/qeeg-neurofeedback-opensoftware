# Verifies that qeeg_pipeline_cli can run a small multi-step workflow.
#
# This is a higher-level integration test focused on "CLI file cross integration":
# the pipeline runs multiple qeeg_*_cli tools and passes outputs between them.

if(NOT DEFINED QEEG_PIPELINE)
  message(FATAL_ERROR "QEEG_PIPELINE not set")
endif()
if(NOT DEFINED QEEG_BIN_DIR)
  message(FATAL_ERROR "QEEG_BIN_DIR not set")
endif()

set(_tmp "${CMAKE_BINARY_DIR}/qeeg_test_pipeline_basic")
file(REMOVE_RECURSE "${_tmp}")
file(MAKE_DIRECTORY "${_tmp}")

# --- Build a tiny synthetic recording CSV (no time column) ---
set(_raw "${_tmp}/raw")
file(MAKE_DIRECTORY "${_raw}")

set(_csv "${_raw}/recording.csv")
file(WRITE "${_csv}" "C3,C4,Cz\n")
foreach(i RANGE 0 255)
  math(EXPR a "${i}")
  math(EXPR b "${i}*2")
  math(EXPR c "${i}*3")
  file(APPEND "${_csv}" "${a},${b},${c}\n")
endforeach()

set(_work "${_tmp}/work")

execute_process(
  COMMAND "${QEEG_PIPELINE}"
          --input "${_raw}"
          --fs 250
          --outdir "${_work}"
          --bin-dir "${QEEG_BIN_DIR}"
          --bandpower-args "--nperseg 64"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "qeeg_pipeline_cli failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()

if(NOT EXISTS "${_work}/01_preprocess/preprocessed.csv")
  message(FATAL_ERROR "Pipeline did not create expected preprocess output")
endif()
if(NOT EXISTS "${_work}/02_bandpower/bandpowers.csv")
  message(FATAL_ERROR "Pipeline did not create expected bandpowers.csv")
endif()
if(NOT EXISTS "${_work}/03_bandratios/bandratios.csv")
  message(FATAL_ERROR "Pipeline did not create expected bandratios.csv")
endif()
if(NOT EXISTS "${_work}/pipeline_run_meta.json")
  message(FATAL_ERROR "Pipeline did not create pipeline_run_meta.json")
endif()

message(STATUS "Pipeline basic workflow OK")
