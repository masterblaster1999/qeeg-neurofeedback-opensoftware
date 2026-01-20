# Verifies that CLIs can resolve --input when it points to a directory or a *_run_meta.json file.
#
# This is meant to test "CLI file cross integration": chaining tools by passing output directories
# or run-meta manifests, rather than hard-coding specific filenames.

if(NOT DEFINED QEEG_BANDPOWER)
  message(FATAL_ERROR "QEEG_BANDPOWER not set")
endif()
if(NOT DEFINED QEEG_PREPROCESS)
  message(FATAL_ERROR "QEEG_PREPROCESS not set")
endif()

set(_tmp "${CMAKE_BINARY_DIR}/qeeg_test_recording_input_resolution")
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

# Add a misleading output-like CSV name to ensure the resolver prefers the actual recording.
file(WRITE "${_raw}/bandpowers.csv" "channel,alpha\nC3,1\n")

# --- 1) Bandpower: pass the directory instead of the file ---
set(_out1 "${_tmp}/bp_from_dir")
execute_process(
  COMMAND "${QEEG_BANDPOWER}" --input "${_raw}" --fs 250 --outdir "${_out1}" --nperseg 64
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "qeeg_bandpower_cli failed (dir input). rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()
if(NOT EXISTS "${_out1}/bandpowers.csv")
  message(FATAL_ERROR "qeeg_bandpower_cli did not create expected bandpowers.csv in ${_out1}")
endif()

# --- 2) Preprocess: pass the directory instead of the file, then ensure it writes run-meta ---
set(_pre "${_tmp}/pre")
file(MAKE_DIRECTORY "${_pre}")
set(_pre_out "${_pre}/preprocessed.csv")
execute_process(
  COMMAND "${QEEG_PREPROCESS}" --input "${_raw}" --fs 250 --output "${_pre_out}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "qeeg_preprocess_cli failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()
if(NOT EXISTS "${_pre_out}")
  message(FATAL_ERROR "qeeg_preprocess_cli did not create expected output: ${_pre_out}")
endif()
if(NOT EXISTS "${_pre}/preprocess_run_meta.json")
  message(FATAL_ERROR "qeeg_preprocess_cli did not create preprocess_run_meta.json in ${_pre}")
endif()

# --- 3) Bandpower: pass the run-meta instead of the file ---
set(_out2 "${_tmp}/bp_from_meta")
execute_process(
  COMMAND "${QEEG_BANDPOWER}" --input "${_pre}/preprocess_run_meta.json" --fs 250 --outdir "${_out2}" --nperseg 64
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "qeeg_bandpower_cli failed (run-meta input). rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()
if(NOT EXISTS "${_out2}/bandpowers.csv")
  message(FATAL_ERROR "qeeg_bandpower_cli did not create expected bandpowers.csv in ${_out2}")
endif()

message(STATUS "CLI recording input resolution OK")
