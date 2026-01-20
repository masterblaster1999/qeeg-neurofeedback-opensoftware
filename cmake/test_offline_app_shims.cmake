# CTest helper: verify qeeg_offline_app_cli can install and uninstall shims
# in a temporary directory.

if (NOT DEFINED QEEG_OFFLINE_APP)
  message(FATAL_ERROR "QEEG_OFFLINE_APP is not set")
endif()

# Put the temp dir next to the executable so it's always writable.
get_filename_component(_exe_dir "${QEEG_OFFLINE_APP}" DIRECTORY)
set(_shim_dir "${_exe_dir}/_qeeg_shim_test")

file(REMOVE_RECURSE "${_shim_dir}")
file(MAKE_DIRECTORY "${_shim_dir}")

if (CMAKE_HOST_WIN32)
  set(_shim_file "${_shim_dir}/qeeg_version_cli.exe")
else()
  set(_shim_file "${_shim_dir}/qeeg_version_cli")
endif()

execute_process(
  COMMAND "${QEEG_OFFLINE_APP}" --install-shims "${_shim_dir}" --tool qeeg_version_cli --force
  RESULT_VARIABLE _rv_install
  OUTPUT_VARIABLE _out_install
  ERROR_VARIABLE _err_install
)

if (NOT _rv_install EQUAL 0)
  message(FATAL_ERROR "install-shims failed (rv=${_rv_install})\nstdout:\n${_out_install}\nstderr:\n${_err_install}")
endif()

if (NOT EXISTS "${_shim_file}")
  message(FATAL_ERROR "expected shim was not created: ${_shim_file}\nstdout:\n${_out_install}\nstderr:\n${_err_install}")
endif()

execute_process(
  COMMAND "${QEEG_OFFLINE_APP}" --uninstall-shims "${_shim_dir}" --tool qeeg_version_cli --force
  RESULT_VARIABLE _rv_uninstall
  OUTPUT_VARIABLE _out_uninstall
  ERROR_VARIABLE _err_uninstall
)

if (NOT _rv_uninstall EQUAL 0)
  message(FATAL_ERROR "uninstall-shims failed (rv=${_rv_uninstall})\nstdout:\n${_out_uninstall}\nstderr:\n${_err_uninstall}")
endif()

if (EXISTS "${_shim_file}")
  message(FATAL_ERROR "expected shim to be removed but it still exists: ${_shim_file}\nstdout:\n${_out_uninstall}\nstderr:\n${_err_uninstall}")
endif()

file(REMOVE_RECURSE "${_shim_dir}")
