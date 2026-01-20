# CTest helper: verify qeeg_offline_app_cli --install-shims --dry-run
# does not create directories or files.

if (NOT DEFINED QEEG_OFFLINE_APP)
  message(FATAL_ERROR "QEEG_OFFLINE_APP is not set")
endif()

# Put the temp dir next to the executable so we don't rely on system temp locations.
get_filename_component(_exe_dir "${QEEG_OFFLINE_APP}" DIRECTORY)
set(_shim_dir "${_exe_dir}/_qeeg_shim_test_dryrun")

# Ensure it does not exist before the test.
file(REMOVE_RECURSE "${_shim_dir}")

if (CMAKE_HOST_WIN32)
  set(_shim_file "${_shim_dir}/qeeg_version_cli.exe")
else()
  set(_shim_file "${_shim_dir}/qeeg_version_cli")
endif()

execute_process(
  COMMAND "${QEEG_OFFLINE_APP}" --install-shims "${_shim_dir}" --tool qeeg_version_cli --force --dry-run
  RESULT_VARIABLE _rv
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)

if (NOT _rv EQUAL 0)
  message(FATAL_ERROR "install-shims --dry-run failed (rv=${_rv})\nstdout:\n${_out}\nstderr:\n${_err}")
endif()

# Dry-run must not create the directory or shim.
if (EXISTS "${_shim_dir}")
  message(FATAL_ERROR "dry-run unexpectedly created directory: ${_shim_dir}\nstdout:\n${_out}\nstderr:\n${_err}")
endif()

if (EXISTS "${_shim_file}")
  message(FATAL_ERROR "dry-run unexpectedly created shim file: ${_shim_file}\nstdout:\n${_out}\nstderr:\n${_err}")
endif()

# Clean up in case of partial side effects.
file(REMOVE_RECURSE "${_shim_dir}")
