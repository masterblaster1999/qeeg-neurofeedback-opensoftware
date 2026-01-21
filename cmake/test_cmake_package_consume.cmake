# Verifies that `cmake --install` produces a working CMake package config that
# can be consumed by a downstream project via `find_package(qeeg CONFIG REQUIRED)`.
#
# This is a lightweight integration test for:
#   - install(TARGETS ...) + install(EXPORT ...)
#   - qeegConfig.cmake / qeegConfigVersion.cmake
#   - exported target name qeeg::qeeg
#
# The test is intentionally dependency-free and does not require any external
# package manager.

# Optional inputs (passed from CTest):
#   - QEEG_CONFIG: multi-config build configuration (e.g., Release)
#   - QEEG_BUILD_TYPE: single-config build type (e.g., Release/Debug)

set(_tmp "${CMAKE_BINARY_DIR}/qeeg_test_cmake_package_consume")
file(REMOVE_RECURSE "${_tmp}")
file(MAKE_DIRECTORY "${_tmp}")

set(_prefix "${_tmp}/install")
file(MAKE_DIRECTORY "${_prefix}")

# Determine config args for multi-config generators.
set(_config_args "")
if(DEFINED QEEG_CONFIG AND NOT QEEG_CONFIG STREQUAL "")
  set(_config_args --config "${QEEG_CONFIG}")
endif()

# For single-config builds, try to keep the consumer build type consistent.
set(_consumer_build_type "")
if(DEFINED QEEG_BUILD_TYPE AND NOT QEEG_BUILD_TYPE STREQUAL "")
  set(_consumer_build_type "${QEEG_BUILD_TYPE}")
elseif(DEFINED QEEG_CONFIG AND NOT QEEG_CONFIG STREQUAL "")
  set(_consumer_build_type "${QEEG_CONFIG}")
endif()

# --- Install this build into a private prefix ---
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${_prefix}" ${_config_args}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "cmake --install failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()

# Ensure generated version metadata header is present in the install tree.
#
# This header is required for non-CMake consumers (e.g. pkg-config) to obtain
# the correct project version from <qeeg/version.hpp>.
file(GLOB_RECURSE _ver_cfg_candidates "${_prefix}/*/qeeg/version_config.hpp")
if(NOT _ver_cfg_candidates)
  message(FATAL_ERROR "Installed tree is missing qeeg/version_config.hpp under prefix: ${_prefix}")
endif()


# --- (Optional) Verify documentation installation ---
#
# If QEEG_INSTALL_DOCS is enabled, we expect top-level docs (LICENSE, README, etc.)
# to be installed under CMAKE_INSTALL_DOCDIR.
set(_want_docs FALSE)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _docs_cache_lines
       REGEX "^QEEG_INSTALL_DOCS:BOOL=")
  if(_docs_cache_lines)
    list(GET _docs_cache_lines 0 _docs_line)
    string(REPLACE "QEEG_INSTALL_DOCS:BOOL=" "" _docs_val "${_docs_line}")
    if(_docs_val STREQUAL "ON" OR _docs_val STREQUAL "1" OR _docs_val STREQUAL "TRUE" OR _docs_val STREQUAL "YES")
      set(_want_docs TRUE)
    endif()
  endif()
endif()

if(_want_docs)
  file(GLOB_RECURSE _citation_candidates "${_prefix}/*/CITATION.cff")
  if(NOT _citation_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_DOCS=ON but CITATION.cff was not found under install prefix: ${_prefix}")
  endif()
  file(GLOB_RECURSE _license_candidates "${_prefix}/*/LICENSE")
  if(NOT _license_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_DOCS=ON but LICENSE was not found under install prefix: ${_prefix}")
  endif()

  # JSON Schemas for machine-readable CLI outputs should also be installed when docs are enabled.
  file(GLOB_RECURSE _schema_candidates "${_prefix}/*/qeeg_nf_cli_print_config.schema.json")
  if(NOT _schema_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_DOCS=ON but qeeg_nf_cli_print_config.schema.json was not found under install prefix: ${_prefix}")
  endif()

endif()

# --- (Optional) Verify shell completion installation ---
#
# If QEEG_INSTALL_COMPLETIONS is enabled and the CLI tools are built, we expect
# completion files to be installed for select CLI tools (currently:
# qeeg_offline_app_cli, qeeg_version_cli, and qeeg_nf_cli) for:
#   - bash (bash-completion)
#   - zsh
#   - fish
set(_want_completions FALSE)
set(_want_cli FALSE)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _comp_cache_lines
       REGEX "^QEEG_INSTALL_COMPLETIONS:BOOL=")
  if(_comp_cache_lines)
    list(GET _comp_cache_lines 0 _comp_line)
    string(REPLACE "QEEG_INSTALL_COMPLETIONS:BOOL=" "" _comp_val "${_comp_line}")
    if(_comp_val STREQUAL "ON" OR _comp_val STREQUAL "1" OR _comp_val STREQUAL "TRUE" OR _comp_val STREQUAL "YES")
      set(_want_completions TRUE)
    endif()
  endif()

  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _cli_cache_lines
       REGEX "^QEEG_BUILD_CLI:BOOL=")
  if(_cli_cache_lines)
    list(GET _cli_cache_lines 0 _cli_line)
    string(REPLACE "QEEG_BUILD_CLI:BOOL=" "" _cli_val "${_cli_line}")
    if(_cli_val STREQUAL "ON" OR _cli_val STREQUAL "1" OR _cli_val STREQUAL "TRUE" OR _cli_val STREQUAL "YES")
      set(_want_cli TRUE)
    endif()
  endif()
endif()

# Bash completion is uncommon on Windows; skip validation even if the option was set.
if(CMAKE_HOST_WIN32)
  set(_want_completions FALSE)
endif()

if(_want_completions AND _want_cli)
  file(GLOB_RECURSE _bash_completion_candidates "${_prefix}/*/bash-completion/completions/qeeg_offline_app_cli")
  if(NOT _bash_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but bash completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _bash_version_completion_candidates "${_prefix}/*/bash-completion/completions/qeeg_version_cli")
  if(NOT _bash_version_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_version_cli bash completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _bash_nf_completion_candidates "${_prefix}/*/bash-completion/completions/qeeg_nf_cli")
  if(NOT _bash_nf_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_nf_cli bash completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _zsh_completion_candidates "${_prefix}/*/zsh/site-functions/_qeeg_offline_app_cli")
  if(NOT _zsh_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but zsh completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _zsh_version_completion_candidates "${_prefix}/*/zsh/site-functions/_qeeg_version_cli")
  if(NOT _zsh_version_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_version_cli zsh completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _zsh_nf_completion_candidates "${_prefix}/*/zsh/site-functions/_qeeg_nf_cli")
  if(NOT _zsh_nf_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_nf_cli zsh completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _fish_completion_candidates "${_prefix}/*/fish/vendor_completions.d/qeeg_offline_app_cli.fish")
  if(NOT _fish_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but fish completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _fish_version_completion_candidates "${_prefix}/*/fish/vendor_completions.d/qeeg_version_cli.fish")
  if(NOT _fish_version_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_version_cli fish completion file was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _fish_nf_completion_candidates "${_prefix}/*/fish/vendor_completions.d/qeeg_nf_cli.fish")
  if(NOT _fish_nf_completion_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_COMPLETIONS=ON but qeeg_nf_cli fish completion file was not found under install prefix: ${_prefix}")
  endif()
endif()


# --- (Optional) Verify man page installation ---
#
# If QEEG_INSTALL_MANPAGES is enabled and the CLI tools are built, we expect
# man pages for select CLI entrypoints to be installed under <prefix>/share/man/man1.
set(_want_manpages FALSE)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _man_cache_lines
       REGEX "^QEEG_INSTALL_MANPAGES:BOOL=")
  if(_man_cache_lines)
    list(GET _man_cache_lines 0 _man_line)
    string(REPLACE "QEEG_INSTALL_MANPAGES:BOOL=" "" _man_val "${_man_line}")
    if(_man_val STREQUAL "ON" OR _man_val STREQUAL "1" OR _man_val STREQUAL "TRUE" OR _man_val STREQUAL "YES")
      set(_want_manpages TRUE)
    endif()
  endif()
endif()

# man pages are not a Windows convention; skip validation even if the option was set.
if(CMAKE_HOST_WIN32)
  set(_want_manpages FALSE)
endif()

if(_want_manpages AND _want_cli)
  file(GLOB_RECURSE _man_offline_candidates "${_prefix}/*/man1/qeeg_offline_app_cli.1")
  if(NOT _man_offline_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_MANPAGES=ON but qeeg_offline_app_cli man page was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _man_version_candidates "${_prefix}/*/man1/qeeg_version_cli.1")
  if(NOT _man_version_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_MANPAGES=ON but qeeg_version_cli man page was not found under install prefix: ${_prefix}")
  endif()

  file(GLOB_RECURSE _man_nf_candidates "${_prefix}/*/man1/qeeg_nf_cli.1")
  if(NOT _man_nf_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_MANPAGES=ON but qeeg_nf_cli man page was not found under install prefix: ${_prefix}")
  endif()
endif()

# --- (Optional) Verify pkg-config metadata ---
#
# Some users/package managers rely on pkg-config even when a CMake config is
# provided. If QEEG_INSTALL_PKGCONFIG was enabled during the build, we expect a
# qeeg.pc file to be installed under <prefix>/*/pkgconfig.
set(_want_pkgconfig FALSE)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _pc_cache_lines
       REGEX "^QEEG_INSTALL_PKGCONFIG:BOOL=")
  if(_pc_cache_lines)
    list(GET _pc_cache_lines 0 _pc_line)
    string(REPLACE "QEEG_INSTALL_PKGCONFIG:BOOL=" "" _pc_val "${_pc_line}")
    if(_pc_val STREQUAL "ON" OR _pc_val STREQUAL "1" OR _pc_val STREQUAL "TRUE" OR _pc_val STREQUAL "YES")
      set(_want_pkgconfig TRUE)
    endif()
  endif()
endif()

# pkg-config is uncommon on Windows; skip validation even if the option was set.
if(CMAKE_HOST_WIN32)
  set(_want_pkgconfig FALSE)
endif()

if(_want_pkgconfig)
  file(GLOB_RECURSE _pc_candidates "${_prefix}/*/pkgconfig/qeeg.pc")
  if(NOT _pc_candidates)
    message(FATAL_ERROR "QEEG_INSTALL_PKGCONFIG=ON but qeeg.pc was not found under install prefix: ${_prefix}")
  endif()

  list(GET _pc_candidates 0 _pc_file)
  get_filename_component(_pc_dir "${_pc_file}" DIRECTORY)
  message(STATUS "Found qeeg.pc: ${_pc_file}")

  find_program(_PKG_CONFIG_EXECUTABLE pkg-config)
  if(_PKG_CONFIG_EXECUTABLE)
    # Query cflags/libs separately.
    #
    # This makes it straightforward to compile+link a tiny consumer program with
    # correct argument ordering (libs typically must come *after* objects).
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_dir}" "${_PKG_CONFIG_EXECUTABLE}" --cflags qeeg
      RESULT_VARIABLE _pc_cflags_rc
      OUTPUT_VARIABLE _pc_cflags
      ERROR_VARIABLE _pc_cflags_stderr
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )

    if(NOT _pc_cflags_rc EQUAL 0)
      message(FATAL_ERROR "pkg-config --cflags failed. rc=${_pc_cflags_rc}\nstdout=${_pc_cflags}\nstderr=${_pc_cflags_stderr}\nPKG_CONFIG_PATH=${_pc_dir}")
    endif()

    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_dir}" "${_PKG_CONFIG_EXECUTABLE}" --libs qeeg
      RESULT_VARIABLE _pc_libs_rc
      OUTPUT_VARIABLE _pc_libs
      ERROR_VARIABLE _pc_libs_stderr
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )

    if(NOT _pc_libs_rc EQUAL 0)
      message(FATAL_ERROR "pkg-config --libs failed. rc=${_pc_libs_rc}\nstdout=${_pc_libs}\nstderr=${_pc_libs_stderr}\nPKG_CONFIG_PATH=${_pc_dir}")
    endif()

    separate_arguments(_pc_cflags_args NATIVE_COMMAND "${_pc_cflags}")
    separate_arguments(_pc_libs_args NATIVE_COMMAND "${_pc_libs}")

    # Validate that pkg-config produces *existing* include/lib directories.
    #
    # This catches common mistakes like hard-coding the configure-time
    # CMAKE_INSTALL_PREFIX into qeeg.pc, which breaks when users override
    # the install prefix at install time (cmake --install --prefix ...).
    set(_pc_includes "")
    foreach(_arg IN LISTS _pc_cflags_args)
      if(_arg MATCHES "^-I(.+)")
        list(APPEND _pc_includes "${CMAKE_MATCH_1}")
      endif()
    endforeach()

    set(_pc_libdirs "")
    foreach(_arg IN LISTS _pc_libs_args)
      if(_arg MATCHES "^-L(.+)")
        list(APPEND _pc_libdirs "${CMAKE_MATCH_1}")
      endif()
    endforeach()

    if(NOT _pc_includes)
      message(FATAL_ERROR "pkg-config did not report any -I include dirs. Output: ${_pc_cflags}")
    endif()
    if(NOT _pc_libdirs)
      message(FATAL_ERROR "pkg-config did not report any -L lib dirs. Output: ${_pc_libs}")
    endif()

    list(GET _pc_includes 0 _pc_inc0)
    if(NOT IS_DIRECTORY "${_pc_inc0}")
      message(FATAL_ERROR "pkg-config returned include dir that does not exist: ${_pc_inc0}\nOutput: ${_pc_cflags}")
    endif()
    if(NOT EXISTS "${_pc_inc0}/qeeg/version.hpp")
      message(FATAL_ERROR "pkg-config include dir exists but qeeg/version.hpp was not found: ${_pc_inc0}/qeeg/version.hpp")
    endif()

    list(GET _pc_libdirs 0 _pc_lib0)
    if(NOT IS_DIRECTORY "${_pc_lib0}")
      message(FATAL_ERROR "pkg-config returned lib dir that does not exist: ${_pc_lib0}\nOutput: ${_pc_libs}")
    endif()

    # Accept both static and shared builds (future-proofing):
    # require that some libqeeg.* exists in the reported libdir.
    file(GLOB _qeeg_lib_candidates "${_pc_lib0}/libqeeg.*")
    if(NOT _qeeg_lib_candidates)
      message(FATAL_ERROR "Expected installed qeeg library not found in: ${_pc_lib0} (looked for libqeeg.*)")
    endif()

    message(STATUS "pkg-config qeeg cflags: ${_pc_cflags}")
    message(STATUS "pkg-config qeeg libs: ${_pc_libs}")


    # Optional: compile+run a tiny program using the installed headers *and*
    # linking with the pkg-config-specified library flags.
    file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _cxx_cache_lines
         REGEX "^CMAKE_CXX_COMPILER:FILEPATH=")
    set(_cxx_compiler "")
    if(_cxx_cache_lines)
      list(GET _cxx_cache_lines 0 _cxx_line)
      string(REPLACE "CMAKE_CXX_COMPILER:FILEPATH=" "" _cxx_compiler "${_cxx_line}")
    endif()

    if(_cxx_compiler AND EXISTS "${_cxx_compiler}")
      set(_pc_consume_dir "${_tmp}/pkgconfig_consumer")
      file(MAKE_DIRECTORY "${_pc_consume_dir}")
      file(WRITE "${_pc_consume_dir}/main.cpp"
"#include <qeeg/utils.hpp>\n\n#include <qeeg/version.hpp>\n\n#include <iostream>\n#include <string>\n\nint main() {\n  // Ensure we actually link against libqeeg (trim() is defined in the library).\n  const std::string t = qeeg::trim(\"  hello  \");\n  if (t != \"hello\") {\n    std::cerr << \"trim() failed: got '\" << t << \"'\\n\";\n    return 2;\n  }\n\n  std::cout << qeeg::version_string() << \"\\n\";\n  return 0;\n}\n"
      )

      set(_pc_exe "qeeg_pkgconfig_consumer_test")

      execute_process(
        COMMAND "${_cxx_compiler}" -std=c++17
          ${_pc_cflags_args}
          "${_pc_consume_dir}/main.cpp"
          -o "${_pc_consume_dir}/${_pc_exe}"
          ${_pc_libs_args}
        RESULT_VARIABLE _pc_build_rc
        OUTPUT_VARIABLE _pc_build_stdout
        ERROR_VARIABLE _pc_build_stderr
      )

      if(NOT _pc_build_rc EQUAL 0)
        message(FATAL_ERROR "pkg-config-style compile+link failed. rc=${_pc_build_rc}\nstdout=${_pc_build_stdout}\nstderr=${_pc_build_stderr}\n\ncflags=${_pc_cflags}\nlibs=${_pc_libs}")
      endif()

      # If the library is shared, the runtime loader may need help finding it.
      set(_run_env_cmd "${CMAKE_COMMAND}" -E env)
      if(APPLE)
        list(APPEND _run_env_cmd "DYLD_LIBRARY_PATH=${_pc_lib0}")
      else()
        list(APPEND _run_env_cmd "LD_LIBRARY_PATH=${_pc_lib0}")
      endif()

      execute_process(
        COMMAND ${_run_env_cmd} "${_pc_consume_dir}/${_pc_exe}"
        RESULT_VARIABLE _pc_run_rc
        OUTPUT_VARIABLE _pc_run_stdout
        ERROR_VARIABLE _pc_run_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
      )

      if(NOT _pc_run_rc EQUAL 0)
        message(FATAL_ERROR "pkg-config-style run failed. rc=${_pc_run_rc}\nstdout=${_pc_run_stdout}\nstderr=${_pc_run_stderr}")
      endif()

      string(STRIP "${_pc_run_stdout}" _pc_reported_version)
      if(_pc_reported_version STREQUAL "0.0.0")
        message(FATAL_ERROR "pkg-config-style consumer still saw default version (0.0.0). Expected real project version. Output: ${_pc_run_stdout}")
      endif()

      message(STATUS "pkg-config-style consumer OK (version=${_pc_reported_version})")
    else()
      message(STATUS "CMAKE_CXX_COMPILER not found; skipping pkg-config compile+run check")
    endif()
  else()
    message(STATUS "pkg-config executable not found; skipping pkg-config query check (qeeg.pc presence verified)")
  endif()
endif()

# --- Create a tiny downstream consumer project ---
set(_consumer_src "${_tmp}/consumer_src")
set(_consumer_build "${_tmp}/consumer_build")
file(MAKE_DIRECTORY "${_consumer_src}")

file(WRITE "${_consumer_src}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.16)\n\nproject(qeeg_consumer_test LANGUAGES CXX)\n\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n\nfind_package(qeeg CONFIG REQUIRED)\n\nadd_executable(qeeg_consumer_test main.cpp)\ntarget_link_libraries(qeeg_consumer_test PRIVATE qeeg::qeeg)\n")

file(WRITE "${_consumer_src}/main.cpp" "#include <qeeg/version.hpp>\n\n#include <iostream>\n\nint main() {\n  std::cout << qeeg::version_string() << \\\"\\n\\\";\n  return 0;\n}\n")

# Configure the consumer using the installed prefix.
set(_cfg_args "")
if(NOT _consumer_build_type STREQUAL "")
  set(_cfg_args "-DCMAKE_BUILD_TYPE=${_consumer_build_type}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${_consumer_src}" -B "${_consumer_build}" "-DCMAKE_PREFIX_PATH=${_prefix}" ${_cfg_args}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Consumer configure failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()

# Build the consumer.
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}" --parallel 2 ${_config_args}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Consumer build failed. rc=${_rc}\nstdout=${_stdout}\nstderr=${_stderr}")
endif()

# Sanity check that the executable exists.
set(_exe "qeeg_consumer_test")
if(CMAKE_HOST_WIN32)
  set(_exe "${_exe}.exe")
endif()

if(DEFINED QEEG_CONFIG AND NOT QEEG_CONFIG STREQUAL "")
  set(_exe_path "${_consumer_build}/${QEEG_CONFIG}/${_exe}")
else()
  set(_exe_path "${_consumer_build}/${_exe}")
endif()

if(NOT EXISTS "${_exe_path}")
  message(FATAL_ERROR "Consumer executable was not produced at expected path: ${_exe_path}")
endif()

# Optional: run the consumer executable to validate runtime linkage.
#
# This is especially important for shared-library builds, where the executable
# must be able to locate libqeeg at runtime.
set(_is_shared FALSE)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt" _shared_lines
       REGEX "^BUILD_SHARED_LIBS:BOOL=")
  if(_shared_lines)
    list(GET _shared_lines 0 _shared_line)
    string(REPLACE "BUILD_SHARED_LIBS:BOOL=" "" _shared_val "${_shared_line}")
    if(_shared_val STREQUAL "ON" OR _shared_val STREQUAL "1" OR _shared_val STREQUAL "TRUE" OR _shared_val STREQUAL "YES")
      set(_is_shared TRUE)
    endif()
  endif()
endif()

if(_is_shared AND CMAKE_HOST_WIN32)
  message(STATUS "Shared Windows build detected; skipping runtime run check (PATH setup not handled in this test).")
else()
  set(_run_cmd "${CMAKE_COMMAND}" -E env)

  if(_is_shared)
    # Find the installed shared library directory (lib or lib64).
    if(APPLE)
      file(GLOB_RECURSE _installed_shlibs "${_prefix}/*/libqeeg*.dylib")
      if(NOT _installed_shlibs)
        file(GLOB_RECURSE _installed_shlibs "${_prefix}/*/libqeeg*.so*")
      endif()
    else()
      file(GLOB_RECURSE _installed_shlibs "${_prefix}/*/libqeeg*.so*")
    endif()

    if(_installed_shlibs)
      list(GET _installed_shlibs 0 _installed_shlib)
      get_filename_component(_installed_shlib_dir "${_installed_shlib}" DIRECTORY)
      if(APPLE)
        list(APPEND _run_cmd "DYLD_LIBRARY_PATH=${_installed_shlib_dir}")
      else()
        list(APPEND _run_cmd "LD_LIBRARY_PATH=${_installed_shlib_dir}")
      endif()
    else()
      message(WARNING "Shared build detected but no installed shared library was found under prefix: ${_prefix}")
    endif()
  endif()

  execute_process(
    COMMAND ${_run_cmd} "${_exe_path}"
    RESULT_VARIABLE _run_rc
    OUTPUT_VARIABLE _run_stdout
    ERROR_VARIABLE _run_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )

  if(NOT _run_rc EQUAL 0)
    message(FATAL_ERROR "Consumer executable failed at runtime. rc=${_run_rc}\nstdout=${_run_stdout}\nstderr=${_run_stderr}")
  endif()

  string(STRIP "${_run_stdout}" _run_version)
  if(_run_version STREQUAL "" OR _run_version STREQUAL "0.0.0")
    message(FATAL_ERROR "Consumer executable ran but reported unexpected version: '${_run_version}' (stdout='${_run_stdout}')")
  endif()

  message(STATUS "CMake consumer run OK (version=${_run_version})")
endif()



# --- Uninstall sanity check ---
#
# Many users expect an `uninstall` target in CMake-based projects. We provide one
# that removes files listed in install_manifest.txt (generated by `cmake --install`).
#
# Here we run it as a smoke test to ensure the generated uninstall script works
# in the same build tree we just installed from.
if(EXISTS "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target uninstall ${_config_args}
    RESULT_VARIABLE _un_rc
    OUTPUT_VARIABLE _un_stdout
    ERROR_VARIABLE _un_stderr
  )

  if(NOT _un_rc EQUAL 0)
    message(FATAL_ERROR "Uninstall target failed. rc=${_un_rc}\nstdout=${_un_stdout}\nstderr=${_un_stderr}")
  endif()

  # Verify that key installed files were removed.
  file(GLOB_RECURSE _remaining_cfg "${_prefix}/*/cmake/qeeg/qeegConfig.cmake")
  if(_remaining_cfg)
    message(FATAL_ERROR "Uninstall ran but qeegConfig.cmake still exists under prefix: ${_prefix}")
  endif()

  message(STATUS "Uninstall target OK")
else()
  message(STATUS "No uninstall script found in build tree; skipping uninstall smoke check")
endif()

message(STATUS "CMake install + find_package(qeeg) consumer build OK")
