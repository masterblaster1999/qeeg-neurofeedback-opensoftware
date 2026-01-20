# Verify qeeg_bandratios_cli can accept --bandpowers as a directory (CLI chaining).
#
# This test is intentionally lightweight and dependency-free.

if(NOT DEFINED QEEG_BANDRATIOS)
  message(FATAL_ERROR "QEEG_BANDRATIOS not set")
endif()

# Use a deterministic temp directory under the build tree.
set(_tmp "${CMAKE_BINARY_DIR}/qeeg_test_bandratios_input")
file(REMOVE_RECURSE "${_tmp}")
file(MAKE_DIRECTORY "${_tmp}")

set(_in_dir "${_tmp}/out_bp")
file(MAKE_DIRECTORY "${_in_dir}")

# Minimal bandpowers.csv (wide format) compatible with qeeg_bandratios_cli.
file(WRITE "${_in_dir}/bandpowers.csv" "channel,theta,beta,alpha\nC3,2,1,5\nC4,4,2,6\n")

set(_out_dir "${_tmp}/out_ratios")
file(REMOVE_RECURSE "${_out_dir}")

execute_process(
  COMMAND "${QEEG_BANDRATIOS}" --bandpowers "${_in_dir}" --outdir "${_out_dir}" --ratio theta/beta
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "qeeg_bandratios_cli failed (rc=${_rc})\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()

if(NOT EXISTS "${_out_dir}/bandratios.csv")
  message(FATAL_ERROR "Missing expected output: ${_out_dir}/bandratios.csv")
endif()

file(READ "${_out_dir}/bandratios.csv" _csv)
string(FIND "${_csv}" "theta_over_beta" _idx)
if(_idx EQUAL -1)
  message(FATAL_ERROR "bandratios.csv missing expected ratio column 'theta_over_beta'\n${_csv}")
endif()

message(STATUS "qeeg_bandratios_cli directory input OK")
