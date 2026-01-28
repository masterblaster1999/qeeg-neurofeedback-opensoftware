cmake_minimum_required(VERSION 3.16)

project(qeeg_map
  VERSION 0.1.0
  DESCRIPTION "First-pass open-source qEEG mapping + topomap renderer in C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(QEEG_BUILD_TESTS "Build tests" ON)
option(QEEG_BUILD_CLI "Build CLI tools" ON)
option(QEEG_BUILD_DOCS "Build API documentation with Doxygen" OFF)
option(QEEG_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(QEEG_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (best-effort; non-MSVC)" OFF)


# Optional: run clang-tidy during compilation (developer convenience)
#
# This is OFF by default because it can significantly slow down builds.
option(QEEG_ENABLE_CLANG_TIDY "Run clang-tidy during compilation (if available)" OFF)

set(QEEG_CLANG_TIDY_EXECUTABLE "")
if (QEEG_ENABLE_CLANG_TIDY)
  find_program(QEEG_CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
  if (QEEG_CLANG_TIDY_EXECUTABLE)
    message(STATUS "clang-tidy enabled: ${QEEG_CLANG_TIDY_EXECUTABLE}")
  else()
    message(WARNING "QEEG_ENABLE_CLANG_TIDY=ON but clang-tidy was not found in PATH")
  endif()

endif()

# Optional: use ccache if available (developer convenience)
option(QEEG_ENABLE_CCACHE "Use ccache to speed up rebuilds (if available)" OFF)
if (QEEG_ENABLE_CCACHE)
  find_program(QEEG_CCACHE_PROGRAM ccache)
  if (QEEG_CCACHE_PROGRAM)
    if (NOT CMAKE_CXX_COMPILER_LAUNCHER)
      message(STATUS "Using ccache: ${QEEG_CCACHE_PROGRAM}")
      set(CMAKE_CXX_COMPILER_LAUNCHER "${QEEG_CCACHE_PROGRAM}")
    else()
      message(STATUS "CMAKE_CXX_COMPILER_LAUNCHER already set to '${CMAKE_CXX_COMPILER_LAUNCHER}'; not overriding with ccache")
    endif()
  else()
    message(WARNING "QEEG_ENABLE_CCACHE=ON but ccache was not found in PATH")
  endif()
endif()

# Optional: precompiled headers (best-effort)
#
# This is OFF by default because PCH behavior can be toolchain-specific.
# When enabled, it can significantly speed up local builds for this large
# translation-unit-heavy project.
option(QEEG_ENABLE_PCH "Enable precompiled headers to speed up builds (best-effort)" OFF)


# Position-independent code (PIC)
#
# When building a static libqeeg.a, downstream consumers may want to link it
# into their own shared libraries. Building qeeg with PIC makes that possible
# on platforms where it matters (e.g., ELF/Mach-O).
set(_QEEG_DEFAULT_PIC ON)
if (WIN32)
  set(_QEEG_DEFAULT_PIC OFF)
endif()
option(QEEG_ENABLE_PIC "Build qeeg with position-independent code (-fPIC where relevant)" ${_QEEG_DEFAULT_PIC})

# Interprocedural optimization (LTO / IPO)
#
# This is an opt-in, best-effort switch for Release-ish builds.
# We use CMake's CheckIPOSupported to avoid breaking toolchains that
# don't support LTO.
option(QEEG_ENABLE_IPO "Enable interprocedural optimization (LTO) for Release builds where supported" OFF)

if (QEEG_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _QEEG_IPO_SUPPORTED OUTPUT _QEEG_IPO_ERROR)
  if (_QEEG_IPO_SUPPORTED)
    # Enable IPO for optimized configurations. Debug remains unchanged.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  else()
    message(WARNING "QEEG_ENABLE_IPO was requested but is not supported: ${_QEEG_IPO_ERROR}")
  endif()
endif()

# Standard CMake option: allow users to build qeeg as a shared library by
# configuring with -DBUILD_SHARED_LIBS=ON.
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Install a pkg-config file (qeeg.pc) for non-CMake consumers.
#
# Default: ON for Unix-y platforms, OFF for Windows (where pkg-config is uncommon).
set(_QEEG_DEFAULT_PKGCONFIG ON)
if (WIN32)
  set(_QEEG_DEFAULT_PKGCONFIG OFF)
endif()
option(QEEG_INSTALL_PKGCONFIG "Install pkg-config metadata (qeeg.pc)" ${_QEEG_DEFAULT_PKGCONFIG})

include(GNUInstallDirs)

# Install common documentation files (LICENSE, README, etc.).
#
# Some packagers prefer to manage documentation separately; this can be
# disabled with -DQEEG_INSTALL_DOCS=OFF.
option(QEEG_INSTALL_DOCS "Install documentation files (LICENSE, README, etc.)" ON)


# Install shell completion scripts for select CLI tools (bash/zsh/fish).
#
# Default: ON for Unix-y platforms, OFF for Windows (where shell completion
# integration varies and is uncommon).
set(_QEEG_DEFAULT_INSTALL_COMPLETIONS ON)
if (WIN32)
  set(_QEEG_DEFAULT_INSTALL_COMPLETIONS OFF)
endif()
option(QEEG_INSTALL_COMPLETIONS "Install shell completion scripts (bash/zsh/fish where supported)" ${_QEEG_DEFAULT_INSTALL_COMPLETIONS})


# Install man pages for select CLI tools (Unix-y platforms).
#
# Default: ON for Unix-y platforms, OFF for Windows.
set(_QEEG_DEFAULT_INSTALL_MANPAGES ON)
if (WIN32)
  set(_QEEG_DEFAULT_INSTALL_MANPAGES OFF)
endif()
option(QEEG_INSTALL_MANPAGES "Install man pages for CLI tools (where supported)" ${_QEEG_DEFAULT_INSTALL_MANPAGES})


# Install RPATH (shared-library builds)
#
# If users build qeeg as a shared library and install the CLI tools, the
# resulting executables may not find libqeeg at runtime unless the user sets
# LD_LIBRARY_PATH (Linux) / DYLD_LIBRARY_PATH (macOS) or installs into a system
# library path.
#
# For local installs, it's convenient to bake a *relative* RPATH into installed
# executables so they can find libqeeg under the same prefix.
set(_QEEG_DEFAULT_INSTALL_RPATH OFF)
if (UNIX AND NOT WIN32 AND BUILD_SHARED_LIBS)
  set(_QEEG_DEFAULT_INSTALL_RPATH ON)
endif()
option(QEEG_INSTALL_RPATH "Set an install RPATH on installed executables for shared-library builds" ${_QEEG_DEFAULT_INSTALL_RPATH})

if (QEEG_INSTALL_RPATH AND UNIX AND NOT WIN32 AND BUILD_SHARED_LIBS)
  if (APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()


# Directory for small generated headers (e.g., version metadata) that should be
# visible to both in-tree builds and installed consumers.
set(QEEG_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Build qeeg as either a static or shared library.
#
# Default: static. Users can configure with -DBUILD_SHARED_LIBS=ON to build a
# shared library instead (CMake controls this when no library type is specified).
add_library(qeeg
  src/utils.cpp
  src/run_meta.cpp
  src/subprocess.cpp
  src/cli_input.cpp
  src/ui_dashboard.cpp
  src/csv_io.cpp
  src/line_noise.cpp
  src/resample.cpp
  src/segments.cpp
  src/channel_map.cpp
  src/reader.cpp
  src/recording_ops.cpp
  src/annotations.cpp
  src/edf_reader.cpp
  src/edf_writer.cpp
  src/brainvision_writer.cpp
  src/brainvision_reader.cpp
  src/triggers.cpp
  src/bdf_reader.cpp
  src/bdf_writer.cpp
  src/csv_reader.cpp
  src/fft.cpp
  src/signal.cpp
  src/spectrogram.cpp
  src/spectral_features.cpp
  src/biquad.cpp
  src/preprocess.cpp
  src/welch_psd.cpp
  src/bandpower.cpp
  src/iaf.cpp
  src/microstates.cpp
  src/coherence.cpp
  src/pac.cpp
  src/plv.cpp
  src/online_plv.cpp
  src/nf_metric.cpp
  src/nf_protocols.cpp
  src/artifacts.cpp
  src/online_artifacts.cpp
  src/online_bandpower.cpp
  src/online_coherence.cpp
  src/online_pac.cpp
  src/spherical_spline.cpp
  src/montage.cpp
  src/topomap.cpp
  src/channel_qc.cpp
  src/channel_qc_io.cpp
  src/interpolate.cpp
  src/bmp_writer.cpp
  src/wav_writer.cpp
  src/osc.cpp
  src/bids.cpp
)

add_library(qeeg::qeeg ALIAS qeeg)

# clang-tidy (optional)
#
# When enabled, we attach clang-tidy to the core qeeg library build.
# This can slow down builds, so it is opt-in.
if (QEEG_ENABLE_CLANG_TIDY AND QEEG_CLANG_TIDY_EXECUTABLE)
  set_target_properties(qeeg PROPERTIES CXX_CLANG_TIDY "${QEEG_CLANG_TIDY_EXECUTABLE}")
endif()

# Build static libraries with position-independent code if requested.
#
# This is mainly relevant when BUILD_SHARED_LIBS=OFF but downstream users
# want to link libqeeg.a into their own shared libraries.
if (QEEG_ENABLE_PIC)
  set_target_properties(qeeg PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Shared library versioning (Unix-like platforms)
#
# When building shared libraries, it is conventional to set VERSION/SOVERSION so
# the resulting SONAME encodes ABI compatibility.
if (BUILD_SHARED_LIBS)
  set_target_properties(qeeg PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
endif()


# Shared library builds on Windows usually require explicit symbol export
# annotations (e.g., __declspec(dllexport)). For this early-stage project, we
# default to exporting all symbols when BUILD_SHARED_LIBS=ON to keep shared builds
# usable without intrusive header changes.
if (WIN32 AND BUILD_SHARED_LIBS)
  set_target_properties(qeeg PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(qeeg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${QEEG_GENERATED_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Ensure downstream consumers (via find_package) build with at least C++17.
# This is exported as an INTERFACE requirement on qeeg::qeeg.
target_compile_features(qeeg PUBLIC cxx_std_17)

# Precompiled headers (optional)
#
# When enabled, this can significantly reduce compile times for local builds.
# It is a best-effort developer convenience: disable it if your toolchain
# has issues with PCH.
if (QEEG_ENABLE_PCH)
  target_precompile_headers(qeeg PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp>")
endif()

# Version / build metadata
#
# In addition to the project version, we expose a best-effort git describe string.
# This is helpful when users report issues from a binary build (it answers "which
# exact commit was this built from?") while remaining safe for non-git source
# distributions (where it falls back to "unknown").
set(QEEG_GIT_DESCRIBE "unknown")
find_package(Git QUIET)
if (GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE QEEG_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if (NOT QEEG_GIT_DESCRIBE)
  set(QEEG_GIT_DESCRIBE "unknown")
endif()
string(REPLACE "\"" "\\\"" QEEG_GIT_DESCRIBE_ESC "${QEEG_GIT_DESCRIBE}")

# Generate a tiny header that defines QEEG_VERSION_STRING and QEEG_GIT_DESCRIBE.
#
# Why: when consuming the library via non-CMake systems (e.g., pkg-config),
# consumers do not get CMake target compile definitions, so <qeeg/version.hpp>
# would otherwise fall back to its default values. Installing this header keeps
# version reporting correct across build systems.
file(MAKE_DIRECTORY "${QEEG_GENERATED_INCLUDE_DIR}/qeeg")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_config.hpp.in"
  "${QEEG_GENERATED_INCLUDE_DIR}/qeeg/version_config.hpp"
  @ONLY
)

target_compile_definitions(qeeg PUBLIC
  QEEG_VERSION_STRING=\"${PROJECT_VERSION}\"
  QEEG_GIT_DESCRIBE=\"${QEEG_GIT_DESCRIBE_ESC}\"
)

# Enable warnings (best-effort, portable)
if (MSVC)
  target_compile_options(qeeg PRIVATE /W4)
else()
  target_compile_options(qeeg PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (QEEG_WARNINGS_AS_ERRORS)
  if (MSVC)
    target_compile_options(qeeg PRIVATE /WX)
  else()
    target_compile_options(qeeg PRIVATE -Werror)
  endif()
endif()

if (QEEG_ENABLE_SANITIZERS AND NOT MSVC)
  # Enable ASan + UBSan for all targets that link against qeeg.
  target_compile_options(qeeg PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(qeeg PUBLIC -fsanitize=address,undefined)
endif()

# Sockets (OSC) on Windows
if (WIN32)
  target_link_libraries(qeeg PUBLIC ws2_32)
endif()

if (QEEG_BUILD_CLI)
  add_executable(qeeg_map_cli src/main.cpp)
  target_link_libraries(qeeg_map_cli PRIVATE qeeg)

  add_executable(qeeg_topomap_cli src/topomap_cli.cpp)
  target_link_libraries(qeeg_topomap_cli PRIVATE qeeg)

  add_executable(qeeg_region_summary_cli src/region_summary_cli.cpp)
  target_link_libraries(qeeg_region_summary_cli PRIVATE qeeg)

  add_executable(qeeg_connectivity_map_cli src/connectivity_map_cli.cpp)
  target_link_libraries(qeeg_connectivity_map_cli PRIVATE qeeg)

  add_executable(qeeg_bandpower_cli src/bandpower_cli.cpp)
  target_link_libraries(qeeg_bandpower_cli PRIVATE qeeg)

  add_executable(qeeg_bandratios_cli src/bandratios_cli.cpp)
  target_link_libraries(qeeg_bandratios_cli PRIVATE qeeg)

  # Neurofeedback CLI: build the heavy implementation once and reuse it for
  # both the standalone executable and the offline multicall toolbox.
  add_library(qeeg_cli_impl_nf OBJECT src/nf_cli.cpp)
  target_link_libraries(qeeg_cli_impl_nf PRIVATE qeeg)

  # nf_cli.cpp is mostly orchestration + UI/report generation. The hot DSP paths
  # live in libqeeg, so compiling this translation unit with full -O3 / /O2 in
  # Release isn't performance-critical but can noticeably slow down builds.
  #
  # We disable optimization for this TU *only* in Debug builds to keep Release
  # builds warning-free on MSVC (avoids D9025: overriding '/O2' with '/Od') and
  # to preserve expected performance characteristics in Release packages.
  if (MSVC)
    target_compile_options(qeeg_cli_impl_nf PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(qeeg_cli_impl_nf PRIVATE $<$<CONFIG:Debug>:-O0>)
  endif()

  add_executable(qeeg_nf_cli src/nf_cli_main.cpp $<TARGET_OBJECTS:qeeg_cli_impl_nf>)
  target_link_libraries(qeeg_nf_cli PRIVATE qeeg)

  add_executable(qeeg_coherence_cli src/coherence_cli.cpp)
  target_link_libraries(qeeg_coherence_cli PRIVATE qeeg)

  add_executable(qeeg_plv_cli src/plv_cli.cpp)
  target_link_libraries(qeeg_plv_cli PRIVATE qeeg)

  add_executable(qeeg_epoch_cli src/epoch_cli.cpp)
  target_link_libraries(qeeg_epoch_cli PRIVATE qeeg)

  add_executable(qeeg_spectrogram_cli src/spectrogram_cli.cpp)
  target_link_libraries(qeeg_spectrogram_cli PRIVATE qeeg)

  add_executable(qeeg_trace_plot_cli src/trace_plot_cli.cpp)
  target_link_libraries(qeeg_trace_plot_cli PRIVATE qeeg)

  add_executable(qeeg_spectral_features_cli src/spectral_features_cli.cpp)
  target_link_libraries(qeeg_spectral_features_cli PRIVATE qeeg)

  add_executable(qeeg_iaf_cli src/iaf_cli.cpp)
  target_link_libraries(qeeg_iaf_cli PRIVATE qeeg)

  add_executable(qeeg_microstates_cli src/microstates_cli.cpp)
  target_link_libraries(qeeg_microstates_cli PRIVATE qeeg)

  add_executable(qeeg_pac_cli src/pac_cli.cpp)
  target_link_libraries(qeeg_pac_cli PRIVATE qeeg)

  add_executable(qeeg_artifacts_cli src/artifacts_cli.cpp)
  target_link_libraries(qeeg_artifacts_cli PRIVATE qeeg)

  add_executable(qeeg_reference_cli src/reference_cli.cpp)
  target_link_libraries(qeeg_reference_cli PRIVATE qeeg)

  add_executable(qeeg_info_cli src/info_cli.cpp)
  target_link_libraries(qeeg_info_cli PRIVATE qeeg)

  add_executable(qeeg_version_cli src/version_cli.cpp)
  target_link_libraries(qeeg_version_cli PRIVATE qeeg)


  add_executable(qeeg_convert_cli src/convert_cli.cpp)
  target_link_libraries(qeeg_convert_cli PRIVATE qeeg)

  add_executable(qeeg_export_edf_cli src/export_edf_cli.cpp)
  target_link_libraries(qeeg_export_edf_cli PRIVATE qeeg)

  add_executable(qeeg_export_bdf_cli src/export_bdf_cli.cpp)
  target_link_libraries(qeeg_export_bdf_cli PRIVATE qeeg)

  add_executable(qeeg_export_brainvision_cli src/export_brainvision_cli.cpp)
  target_link_libraries(qeeg_export_brainvision_cli PRIVATE qeeg)

  add_executable(qeeg_export_bids_cli src/export_bids_cli.cpp)
  target_link_libraries(qeeg_export_bids_cli PRIVATE qeeg)

  add_executable(qeeg_bids_scan_cli src/bids_scan_cli.cpp)
  target_link_libraries(qeeg_bids_scan_cli PRIVATE qeeg)

  add_executable(qeeg_export_derivatives_cli src/export_derivatives_cli.cpp)
  target_link_libraries(qeeg_export_derivatives_cli PRIVATE qeeg)

  add_executable(qeeg_ui_cli src/ui_cli.cpp)
  target_link_libraries(qeeg_ui_cli PRIVATE qeeg)

  add_executable(qeeg_ui_server_cli src/ui_server_cli.cpp)
  target_link_libraries(qeeg_ui_server_cli PRIVATE qeeg)

  add_executable(qeeg_bundle_cli src/bundle_cli.cpp)
  target_link_libraries(qeeg_bundle_cli PRIVATE qeeg)

  add_executable(qeeg_pipeline_cli src/pipeline_cli.cpp)
  target_link_libraries(qeeg_pipeline_cli PRIVATE qeeg)

  add_executable(qeeg_clean_cli src/clean_cli.cpp)
  target_link_libraries(qeeg_clean_cli PRIVATE qeeg)

  add_executable(qeeg_quality_cli src/quality_cli.cpp)
  target_link_libraries(qeeg_quality_cli PRIVATE qeeg)

  add_executable(qeeg_preprocess_cli src/preprocess_cli.cpp)
  target_link_libraries(qeeg_preprocess_cli PRIVATE qeeg)

  add_executable(qeeg_channel_qc_cli src/channel_qc_cli.cpp)
  target_link_libraries(qeeg_channel_qc_cli PRIVATE qeeg)


  # Offline multicall app: build a single binary that can dispatch to any qeeg_*_cli tool.
  #
  # This is useful for offline distribution (one executable) and as a "toolbox" runner for
  # qeeg_ui_server_cli --toolbox, which lets the browser UI run tools even if individual
  # qeeg_*_cli executables aren't present in --bin-dir.
  function(qeeg_add_cli_entry obj_target src_file entry_symbol)
    add_library(${obj_target} OBJECT ${src_file})
    target_link_libraries(${obj_target} PRIVATE qeeg)
    target_compile_definitions(${obj_target} PRIVATE main=${entry_symbol})
  endfunction()

  qeeg_add_cli_entry(qeeg_cli_entry_map src/main.cpp qeeg_map_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_topomap src/topomap_cli.cpp qeeg_topomap_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_region_summary src/region_summary_cli.cpp qeeg_region_summary_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_connectivity_map src/connectivity_map_cli.cpp qeeg_connectivity_map_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_bandpower src/bandpower_cli.cpp qeeg_bandpower_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_bandratios src/bandratios_cli.cpp qeeg_bandratios_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_nf src/nf_cli_main.cpp qeeg_nf_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_coherence src/coherence_cli.cpp qeeg_coherence_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_plv src/plv_cli.cpp qeeg_plv_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_epoch src/epoch_cli.cpp qeeg_epoch_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_spectrogram src/spectrogram_cli.cpp qeeg_spectrogram_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_trace_plot src/trace_plot_cli.cpp qeeg_trace_plot_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_spectral_features src/spectral_features_cli.cpp qeeg_spectral_features_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_iaf src/iaf_cli.cpp qeeg_iaf_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_microstates src/microstates_cli.cpp qeeg_microstates_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_pac src/pac_cli.cpp qeeg_pac_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_artifacts src/artifacts_cli.cpp qeeg_artifacts_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_reference src/reference_cli.cpp qeeg_reference_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_info src/info_cli.cpp qeeg_info_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_version src/version_cli.cpp qeeg_version_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_convert src/convert_cli.cpp qeeg_convert_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_export_edf src/export_edf_cli.cpp qeeg_export_edf_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_export_bdf src/export_bdf_cli.cpp qeeg_export_bdf_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_export_brainvision src/export_brainvision_cli.cpp qeeg_export_brainvision_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_export_bids src/export_bids_cli.cpp qeeg_export_bids_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_bids_scan src/bids_scan_cli.cpp qeeg_bids_scan_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_export_derivatives src/export_derivatives_cli.cpp qeeg_export_derivatives_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_ui src/ui_cli.cpp qeeg_ui_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_ui_server src/ui_server_cli.cpp qeeg_ui_server_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_bundle src/bundle_cli.cpp qeeg_bundle_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_pipeline src/pipeline_cli.cpp qeeg_pipeline_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_clean src/clean_cli.cpp qeeg_clean_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_quality src/quality_cli.cpp qeeg_quality_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_preprocess src/preprocess_cli.cpp qeeg_preprocess_cli_entry)
  qeeg_add_cli_entry(qeeg_cli_entry_channel_qc src/channel_qc_cli.cpp qeeg_channel_qc_cli_entry)

  add_executable(qeeg_offline_app_cli
    src/offline_app_cli.cpp
    $<TARGET_OBJECTS:qeeg_cli_entry_map>
    $<TARGET_OBJECTS:qeeg_cli_entry_topomap>
    $<TARGET_OBJECTS:qeeg_cli_entry_region_summary>
    $<TARGET_OBJECTS:qeeg_cli_entry_connectivity_map>
    $<TARGET_OBJECTS:qeeg_cli_entry_bandpower>
    $<TARGET_OBJECTS:qeeg_cli_entry_bandratios>
    $<TARGET_OBJECTS:qeeg_cli_entry_nf>
    $<TARGET_OBJECTS:qeeg_cli_impl_nf>
    $<TARGET_OBJECTS:qeeg_cli_entry_coherence>
    $<TARGET_OBJECTS:qeeg_cli_entry_plv>
    $<TARGET_OBJECTS:qeeg_cli_entry_epoch>
    $<TARGET_OBJECTS:qeeg_cli_entry_spectrogram>
    $<TARGET_OBJECTS:qeeg_cli_entry_trace_plot>
    $<TARGET_OBJECTS:qeeg_cli_entry_spectral_features>
    $<TARGET_OBJECTS:qeeg_cli_entry_iaf>
    $<TARGET_OBJECTS:qeeg_cli_entry_microstates>
    $<TARGET_OBJECTS:qeeg_cli_entry_pac>
    $<TARGET_OBJECTS:qeeg_cli_entry_artifacts>
    $<TARGET_OBJECTS:qeeg_cli_entry_reference>
    $<TARGET_OBJECTS:qeeg_cli_entry_info>
    $<TARGET_OBJECTS:qeeg_cli_entry_version>
    $<TARGET_OBJECTS:qeeg_cli_entry_convert>
    $<TARGET_OBJECTS:qeeg_cli_entry_export_edf>
    $<TARGET_OBJECTS:qeeg_cli_entry_export_bdf>
    $<TARGET_OBJECTS:qeeg_cli_entry_export_brainvision>
    $<TARGET_OBJECTS:qeeg_cli_entry_export_bids>
    $<TARGET_OBJECTS:qeeg_cli_entry_bids_scan>
    $<TARGET_OBJECTS:qeeg_cli_entry_export_derivatives>
    $<TARGET_OBJECTS:qeeg_cli_entry_ui>
    $<TARGET_OBJECTS:qeeg_cli_entry_ui_server>
    $<TARGET_OBJECTS:qeeg_cli_entry_bundle>
    $<TARGET_OBJECTS:qeeg_cli_entry_pipeline>
    $<TARGET_OBJECTS:qeeg_cli_entry_clean>
    $<TARGET_OBJECTS:qeeg_cli_entry_quality>
    $<TARGET_OBJECTS:qeeg_cli_entry_preprocess>
    $<TARGET_OBJECTS:qeeg_cli_entry_channel_qc>
  )
  target_link_libraries(qeeg_offline_app_cli PRIVATE qeeg)
endif()


# Install + package config (so downstream projects can use find_package(qeeg))
install(TARGETS qeeg
  EXPORT qeegTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Optional: install top-level documentation files for users of binary packages.
if (QEEG_INSTALL_DOCS)
  install(FILES
    LICENSE
    README.md
    CONTRIBUTING.md
    SECURITY.md
    CODE_OF_CONDUCT.md
    CITATION.cff
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
  )

  # Install JSON Schemas for machine-readable CLI outputs (useful for GUI / scripting integrations).
  # Installed under <prefix>/${CMAKE_INSTALL_DOCDIR}/schemas when docs installation is enabled.
  install(DIRECTORY schemas/
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/schemas"
    FILES_MATCHING PATTERN "*.json"
  )
endif()


# Install the generated version metadata header.
install(FILES "${QEEG_GENERATED_INCLUDE_DIR}/qeeg/version_config.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qeeg"
)

if (QEEG_BUILD_CLI)
  # Install CLI tools
  install(TARGETS
    qeeg_map_cli
    qeeg_topomap_cli
    qeeg_region_summary_cli
    qeeg_connectivity_map_cli
    qeeg_bandpower_cli
    qeeg_bandratios_cli
    qeeg_nf_cli
    qeeg_coherence_cli
    qeeg_plv_cli
    qeeg_epoch_cli
    qeeg_spectrogram_cli
    qeeg_trace_plot_cli
    qeeg_spectral_features_cli
    qeeg_iaf_cli
    qeeg_microstates_cli
    qeeg_pac_cli
    qeeg_artifacts_cli
    qeeg_reference_cli
    qeeg_info_cli
    qeeg_version_cli
    qeeg_convert_cli
    qeeg_export_edf_cli
    qeeg_export_bdf_cli
    qeeg_export_brainvision_cli
    qeeg_export_bids_cli
    qeeg_bids_scan_cli
    qeeg_export_derivatives_cli
    qeeg_preprocess_cli
    qeeg_quality_cli
    qeeg_clean_cli
    qeeg_channel_qc_cli
    qeeg_ui_cli
    qeeg_ui_server_cli
    qeeg_bundle_cli
    qeeg_pipeline_cli
    qeeg_offline_app_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  # Optional: install shell completion scripts for CLI tools.
  #
  #  - bash-completion uses an on-demand loader which expects the installed file
  #    name to match the command name.
  #  - zsh functions are conventionally installed as files named "_<command>".
  #  - fish loads vendor completions from share/fish/vendor_completions.d.
  if (QEEG_INSTALL_COMPLETIONS AND UNIX AND NOT WIN32)
    # --- qeeg_offline_app_cli ---
    # Bash
    install(FILES completions/qeeg_offline_app_cli.bash
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
      RENAME qeeg_offline_app_cli
    )

    # Zsh
    install(FILES completions/_qeeg_offline_app_cli
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions"
    )

    # Fish
    install(FILES completions/qeeg_offline_app_cli.fish
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d"
    )

    # --- qeeg_version_cli ---
    # Bash
    install(FILES completions/qeeg_version_cli.bash
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
      RENAME qeeg_version_cli
    )

    # Zsh
    install(FILES completions/_qeeg_version_cli
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions"
    )

    # Fish
    install(FILES completions/qeeg_version_cli.fish
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d"
    )


    # --- qeeg_nf_cli ---
    # Bash
    install(FILES completions/qeeg_nf_cli.bash
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
      RENAME qeeg_nf_cli
    )

    # Zsh
    install(FILES completions/_qeeg_nf_cli
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions"
    )

    # Fish
    install(FILES completions/qeeg_nf_cli.fish
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d"
    )
  endif()

  # Optional: install man pages for select CLI tools.
  #
  # These are useful for system-wide installs on Unix-y platforms and allow:
  #   man qeeg_offline_app_cli
  #   man qeeg_version_cli
  #   man qeeg_nf_cli
  if (QEEG_INSTALL_MANPAGES AND UNIX AND NOT WIN32)
    install(FILES
      man/qeeg_offline_app_cli.1
      man/qeeg_version_cli.1
      man/qeeg_nf_cli.1
      DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
    )
  endif()
endif()

include(CMakePackageConfigHelpers)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qeegConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/qeegConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qeeg"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/qeegConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/qeegConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/qeegConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qeeg"
)

install(EXPORT qeegTargets
  FILE qeegTargets.cmake
  NAMESPACE qeeg::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qeeg"
)

# Optional: pkg-config metadata for non-CMake consumers.
#
# This installs a qeeg.pc file under <prefix>/${CMAKE_INSTALL_LIBDIR}/pkgconfig
# so users can do:
#   pkg-config --cflags --libs qeeg
#
# Package managers for Unix platforms often expect this even when a CMake config
# is provided.
if (QEEG_INSTALL_PKGCONFIG)
  # Generate qeeg.pc for pkg-config.
  #
  # We prefer a relocatable .pc file by defining "prefix" relative to the
  # installed location of qeeg.pc (pkg-config's ${pcfiledir}). This makes the
  # result robust to `cmake --install --prefix <path>`.
  #
  # Note: GNUInstallDirs variables like CMAKE_INSTALL_INCLUDEDIR can be either
  # relative or absolute depending on the toolchain/packaging system. To avoid
  # generating broken paths, we compute the final strings here and substitute
  # them into the template.
  set(QEEG_PKGCONFIG_PREFIX "\${pcfiledir}/../..")
  set(QEEG_PKGCONFIG_LIBDIR "\${pcfiledir}/..")
  if (IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(QEEG_PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(QEEG_PKGCONFIG_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qeeg.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qeeg.pc"
    @ONLY
  )
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qeeg.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
  )
endif()

# Convenience target: uninstall
#
# CMake does not ship an uninstall target by default, but many projects add
# one as a convenience for local installs. This uses install_manifest.txt
# generated by `cmake --install`.
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
  @ONLY
)

add_custom_target(uninstall
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
  COMMENT "Uninstall installed qeeg files (uses install_manifest.txt)"
)

# ------------------------------------------------------------------------------
# Documentation (Doxygen)
# ------------------------------------------------------------------------------
#
# Optional: generate API documentation.
#
# This is opt-in and does not add any build dependencies unless enabled.
if (QEEG_BUILD_DOCS)
  find_package(Doxygen QUIET)
  if (DOXYGEN_FOUND)
    # Configure common doxygen settings.
    set(DOXYGEN_PROJECT_NAME "qeeg")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_QUIET YES)

    # Make the top-level README the landing page (Doxygen supports Markdown).
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

    # Generate documentation for public headers and (optionally) implementation.
    doxygen_add_docs(qeeg_docs
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      COMMENT "Generate API documentation with Doxygen"
    )

    # Convenience alias (common target name)
    add_custom_target(docs DEPENDS qeeg_docs)
  else()
    message(WARNING "QEEG_BUILD_DOCS=ON but doxygen was not found")
  endif()
endif()

if(QEEG_BUILD_TESTS)
  enable_testing()

  # Guard against MSVC C2026 ("string too big, trailing characters truncated") by
  # ensuring we never let embedded UI assets grow past the compiler's per-literal
  # limit. This test scans the source tree for raw string literals that exceed a
  # conservative threshold.
  add_executable(qeeg_test_string_literal_limits tests/test_string_literal_limits.cpp)
  target_compile_definitions(qeeg_test_string_literal_limits PRIVATE QEEG_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\")
  add_test(NAME qeeg_test_string_literal_limits COMMAND qeeg_test_string_literal_limits)

  # Packaging smoke test: ensure install() exports a working CMake config that
  # downstream projects can consume via find_package(qeeg CONFIG REQUIRED).
  add_test(NAME qeeg_test_cmake_package_consume
           COMMAND ${CMAKE_COMMAND}
             -DQEEG_CONFIG=$<CONFIG>
             -DQEEG_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_cmake_package_consume.cmake)

  # Offline app/toolbox sanity checks (only when CLI tools are built).
  if (QEEG_BUILD_CLI)
    add_test(NAME qeeg_test_offline_app_list_tools COMMAND qeeg_offline_app_cli --list-tools)
    add_test(NAME qeeg_test_offline_app_list_tools_json COMMAND qeeg_offline_app_cli --list-tools --json)
    add_test(NAME qeeg_test_offline_app_dispatch_version_help COMMAND qeeg_offline_app_cli qeeg_version_cli --help)

    # qeeg_nf_cli machine-readable protocol listings should remain stable
    add_test(NAME qeeg_test_nf_cli_list_protocols_names COMMAND qeeg_nf_cli --list-protocols-names)
    add_test(NAME qeeg_test_nf_cli_list_protocols_json COMMAND qeeg_nf_cli --list-protocols-json)
    add_test(NAME qeeg_test_nf_cli_protocol_help_json COMMAND qeeg_nf_cli --protocol-help-json alpha_up_pz)
    add_test(NAME qeeg_test_nf_cli_list_bands COMMAND qeeg_nf_cli --list-bands)
    add_test(NAME qeeg_test_nf_cli_list_bands_json COMMAND qeeg_nf_cli --list-bands-json)
    add_test(NAME qeeg_test_nf_cli_list_metrics COMMAND qeeg_nf_cli --list-metrics)
    add_test(NAME qeeg_test_nf_cli_list_metrics_json COMMAND qeeg_nf_cli --list-metrics-json)
    add_test(NAME qeeg_test_nf_cli_list_channels_demo COMMAND qeeg_nf_cli --demo --fs 250 --seconds 1 --list-channels)
    add_test(NAME qeeg_test_nf_cli_list_channels_json_demo COMMAND qeeg_nf_cli --demo --fs 250 --seconds 1 --list-channels-json)
    add_test(NAME qeeg_test_nf_cli_print_config_json_demo COMMAND qeeg_nf_cli --demo --fs 250 --seconds 1 --print-config-json)
    add_test(NAME qeeg_test_nf_cli_dry_run_demo COMMAND qeeg_nf_cli --demo --fs 250 --seconds 1 --dry-run)
    add_test(NAME qeeg_test_nf_cli_version COMMAND qeeg_nf_cli --version)
    add_test(NAME qeeg_test_nf_cli_version_json COMMAND qeeg_nf_cli --version-json)

    # Validate machine-readable qeeg_nf_cli outputs against the JSON Schemas in schemas/.
    # Prefer full Draft 2020-12 validation via python-jsonschema when available, but fall back
    # to a minimal required-key check if the dependency isn't installed.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_FOUND)
      add_test(NAME qeeg_test_nf_cli_json_schema_validation_demo
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_nf_cli_json_outputs.py
                 --nf-cli $<TARGET_FILE:qeeg_nf_cli>
                 --schemas-dir ${CMAKE_CURRENT_SOURCE_DIR}/schemas
                 --fs 250
                 --seconds 1)

      # Avoid writing .pyc / __pycache__ during CI/CTest runs.
      set_tests_properties(qeeg_test_nf_cli_json_schema_validation_demo
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

      # Smoke test the dependency-free HTML report renderers.
      add_test(NAME qeeg_test_report_renderers_selftest
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftest_reports.py)

      set_tests_properties(qeeg_test_report_renderers_selftest
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")


      # Smoke test the dependency-free real-time qEEG dashboard server.
      add_test(NAME qeeg_test_rt_dashboard_selftest
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftest_rt_dashboard.py)

      set_tests_properties(qeeg_test_rt_dashboard_selftest
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                                      TIMEOUT 60)


      # Smoke test the dependency-free BioTrace container helper scripts.
      add_test(NAME qeeg_test_biotrace_extract_selftest
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftest_biotrace_extract.py)

      set_tests_properties(qeeg_test_biotrace_extract_selftest
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

      add_test(NAME qeeg_test_biotrace_run_nf_selftest
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftest_biotrace_run_nf.py)

      set_tests_properties(qeeg_test_biotrace_run_nf_selftest
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

      # Validate machine-readable BioTrace helper outputs against the JSON Schemas in schemas/.
      add_test(NAME qeeg_test_biotrace_json_schema_validation
               COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_biotrace_json_outputs.py
                 --schemas-dir ${CMAKE_CURRENT_SOURCE_DIR}/schemas)

      set_tests_properties(qeeg_test_biotrace_json_schema_validation
                           PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

      # Developer convenience: allow running the same JSON output validation from the
      # build system, e.g. `cmake --build build --target qeeg_validate_nf_cli_json`.
      add_custom_target(qeeg_validate_nf_cli_json
                        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                          ${Python3_EXECUTABLE} -B
                          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_nf_cli_json_outputs.py
                          --nf-cli $<TARGET_FILE:qeeg_nf_cli>
                          --schemas-dir ${CMAKE_CURRENT_SOURCE_DIR}/schemas
                          --fs 250
                          --seconds 1
                          --verbose
                        DEPENDS qeeg_nf_cli
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        COMMENT "Validate qeeg_nf_cli JSON outputs against schemas"
                        VERBATIM)

      # Developer convenience: validate BioTrace helper JSON outputs against their schemas.
      add_custom_target(qeeg_validate_biotrace_json
                        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                          ${Python3_EXECUTABLE} -B
                          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_biotrace_json_outputs.py
                          --schemas-dir ${CMAKE_CURRENT_SOURCE_DIR}/schemas
                          --verbose
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        COMMENT "Validate BioTrace helper JSON outputs against schemas"
                        VERBATIM)

      # Developer convenience: smoke test the real-time dashboard server.
      add_custom_target(qeeg_selftest_rt_dashboard
                        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                          ${Python3_EXECUTABLE} -B
                          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftest_rt_dashboard.py
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        COMMENT "Smoke test the real-time dashboard server"
                        VERBATIM)

    else()
      message(STATUS "Python3 interpreter not found; skipping qeeg_nf_cli JSON schema validation test")
    endif()
    add_test(NAME qeeg_test_offline_app_shims_install_uninstall
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_OFFLINE_APP=$<TARGET_FILE:qeeg_offline_app_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_offline_app_shims.cmake)
    add_test(NAME qeeg_test_offline_app_shims_dryrun
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_OFFLINE_APP=$<TARGET_FILE:qeeg_offline_app_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_offline_app_shims_dryrun.cmake)

    add_test(NAME qeeg_test_bandratios_input_resolution
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_BANDRATIOS=$<TARGET_FILE:qeeg_bandratios_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_bandratios_input_resolution.cmake)

    add_test(NAME qeeg_test_recording_input_resolution
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_BANDPOWER=$<TARGET_FILE:qeeg_bandpower_cli>
               -DQEEG_PREPROCESS=$<TARGET_FILE:qeeg_preprocess_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_recording_input_resolution.cmake)

    add_test(NAME qeeg_test_pipeline_basic
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_PIPELINE=$<TARGET_FILE:qeeg_pipeline_cli>
               -DQEEG_BIN_DIR=$<TARGET_FILE_DIR:qeeg_pipeline_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pipeline_basic.cmake)

    add_test(NAME qeeg_test_offline_app_pipeline_auto_toolbox
             COMMAND ${CMAKE_COMMAND}
               -DQEEG_OFFLINE_APP=$<TARGET_FILE:qeeg_offline_app_cli>
               -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_offline_app_pipeline_auto_toolbox.cmake)

  endif()

  add_executable(qeeg_test_fft tests/test_fft.cpp)
  target_link_libraries(qeeg_test_fft PRIVATE qeeg)
  add_test(NAME qeeg_test_fft COMMAND qeeg_test_fft)

  add_executable(qeeg_test_bdf_reader tests/test_bdf_reader.cpp)
  target_link_libraries(qeeg_test_bdf_reader PRIVATE qeeg)
  add_test(NAME qeeg_test_bdf_reader COMMAND qeeg_test_bdf_reader)

  add_executable(qeeg_test_edf_reader tests/test_edf_reader.cpp)
  target_link_libraries(qeeg_test_edf_reader PRIVATE qeeg)
  add_test(NAME qeeg_test_edf_reader COMMAND qeeg_test_edf_reader)

  add_executable(qeeg_test_edf_writer tests/test_edf_writer.cpp)
  target_link_libraries(qeeg_test_edf_writer PRIVATE qeeg)
  add_test(NAME qeeg_test_edf_writer COMMAND qeeg_test_edf_writer)

  add_executable(qeeg_test_bdf_writer tests/test_bdf_writer.cpp)
  target_link_libraries(qeeg_test_bdf_writer PRIVATE qeeg)
  add_test(NAME qeeg_test_bdf_writer COMMAND qeeg_test_bdf_writer)

  add_executable(qeeg_test_brainvision_writer tests/test_brainvision_writer.cpp)
  target_link_libraries(qeeg_test_brainvision_writer PRIVATE qeeg)
  add_test(NAME qeeg_test_brainvision_writer COMMAND qeeg_test_brainvision_writer)

  add_executable(qeeg_test_csv_reader tests/test_csv_reader.cpp)
  target_link_libraries(qeeg_test_csv_reader PRIVATE qeeg)
  add_test(NAME qeeg_test_csv_reader COMMAND qeeg_test_csv_reader)


  add_executable(qeeg_test_csv_to_tsv tests/test_csv_to_tsv.cpp)
  target_link_libraries(qeeg_test_csv_to_tsv PRIVATE qeeg)
  add_test(NAME qeeg_test_csv_to_tsv COMMAND qeeg_test_csv_to_tsv)

  add_executable(qeeg_test_cmdline_split tests/test_cmdline_split.cpp)
  target_link_libraries(qeeg_test_cmdline_split PRIVATE qeeg)
  add_test(NAME qeeg_test_cmdline_split COMMAND qeeg_test_cmdline_split)

  add_executable(qeeg_test_cli_input_selectors tests/test_cli_input_selectors.cpp)
  target_link_libraries(qeeg_test_cli_input_selectors PRIVATE qeeg)
  add_test(NAME qeeg_test_cli_input_selectors COMMAND qeeg_test_cli_input_selectors)

  add_executable(qeeg_test_cli_input_nested_run_meta tests/test_cli_input_nested_run_meta.cpp)
  target_link_libraries(qeeg_test_cli_input_nested_run_meta PRIVATE qeeg)
  add_test(NAME qeeg_test_cli_input_nested_run_meta COMMAND qeeg_test_cli_input_nested_run_meta)

  add_executable(qeeg_test_number_parse tests/test_number_parse.cpp)
  target_link_libraries(qeeg_test_number_parse PRIVATE qeeg)
  add_test(NAME qeeg_test_number_parse COMMAND qeeg_test_number_parse)

  add_executable(qeeg_test_http_range tests/test_http_range.cpp)
  target_link_libraries(qeeg_test_http_range PRIVATE qeeg)
  add_test(NAME qeeg_test_http_range COMMAND qeeg_test_http_range)

  add_executable(qeeg_test_random_token tests/test_random_token.cpp)
  target_link_libraries(qeeg_test_random_token PRIVATE qeeg)
  add_test(NAME qeeg_test_random_token COMMAND qeeg_test_random_token)

  add_executable(qeeg_test_nf_protocols tests/test_nf_protocols.cpp)
  target_link_libraries(qeeg_test_nf_protocols PRIVATE qeeg)
  add_test(NAME qeeg_test_nf_protocols COMMAND qeeg_test_nf_protocols)

  add_executable(qeeg_test_smoother tests/test_smoother.cpp)
  target_link_libraries(qeeg_test_smoother PRIVATE qeeg)
  add_test(NAME qeeg_test_smoother COMMAND qeeg_test_smoother)

  add_executable(qeeg_test_adaptive_threshold tests/test_adaptive_threshold.cpp)
  target_link_libraries(qeeg_test_adaptive_threshold PRIVATE qeeg)
  add_test(NAME qeeg_test_adaptive_threshold COMMAND qeeg_test_adaptive_threshold)

  add_executable(qeeg_test_reward_shaper tests/test_reward_shaper.cpp)
  target_link_libraries(qeeg_test_reward_shaper PRIVATE qeeg)
  add_test(NAME qeeg_test_reward_shaper COMMAND qeeg_test_reward_shaper)

  add_executable(qeeg_test_feedback_value tests/test_feedback_value.cpp)
  target_link_libraries(qeeg_test_feedback_value PRIVATE qeeg)
  add_test(NAME qeeg_test_feedback_value COMMAND qeeg_test_feedback_value)

  add_executable(qeeg_test_hysteresis_gate tests/test_hysteresis_gate.cpp)
  target_link_libraries(qeeg_test_hysteresis_gate PRIVATE qeeg)
  add_test(NAME qeeg_test_hysteresis_gate COMMAND qeeg_test_hysteresis_gate)



  add_executable(qeeg_test_channel_map tests/test_channel_map.cpp)
  target_link_libraries(qeeg_test_channel_map PRIVATE qeeg)
  add_test(NAME qeeg_test_channel_map COMMAND qeeg_test_channel_map)

  add_executable(qeeg_test_channel_qc_io tests/test_channel_qc_io.cpp)
  target_link_libraries(qeeg_test_channel_qc_io PRIVATE qeeg)
  add_test(NAME qeeg_test_channel_qc_io COMMAND qeeg_test_channel_qc_io)

  add_executable(qeeg_test_topomap_nan tests/test_topomap_nan.cpp)
  target_link_libraries(qeeg_test_topomap_nan PRIVATE qeeg)
  add_test(NAME qeeg_test_topomap_nan COMMAND qeeg_test_topomap_nan)
  add_executable(qeeg_test_run_meta tests/test_run_meta.cpp)
  target_link_libraries(qeeg_test_run_meta PRIVATE qeeg)
  add_test(NAME qeeg_test_run_meta COMMAND qeeg_test_run_meta)

  add_executable(qeeg_test_run_meta_write tests/test_run_meta_write.cpp)
  target_link_libraries(qeeg_test_run_meta_write PRIVATE qeeg)
  add_test(NAME qeeg_test_run_meta_write COMMAND qeeg_test_run_meta_write)


  add_executable(qeeg_test_ui_dashboard tests/test_ui_dashboard.cpp)
  target_link_libraries(qeeg_test_ui_dashboard PRIVATE qeeg)
  add_test(NAME qeeg_test_ui_dashboard COMMAND qeeg_test_ui_dashboard)

  add_executable(qeeg_test_ui_bin_scan tests/test_ui_bin_scan.cpp)
  target_link_libraries(qeeg_test_ui_bin_scan PRIVATE qeeg)
  add_test(NAME qeeg_test_ui_bin_scan COMMAND qeeg_test_ui_bin_scan)

  add_executable(qeeg_test_reader_extensions tests/test_reader_extensions.cpp)
  target_link_libraries(qeeg_test_reader_extensions PRIVATE qeeg)
  add_test(NAME qeeg_test_reader_extensions COMMAND qeeg_test_reader_extensions)

  add_executable(qeeg_test_reader_input_resolution tests/test_read_recording_auto_resolve.cpp)
  target_link_libraries(qeeg_test_reader_input_resolution PRIVATE qeeg)
  add_test(NAME qeeg_test_reader_input_resolution COMMAND qeeg_test_reader_input_resolution)

  add_executable(qeeg_test_triggers tests/test_triggers.cpp)
  target_link_libraries(qeeg_test_triggers PRIVATE qeeg)
  add_test(NAME qeeg_test_triggers COMMAND qeeg_test_triggers)

  add_executable(qeeg_test_debounce tests/test_debounce.cpp)
  target_link_libraries(qeeg_test_debounce PRIVATE qeeg)
  add_test(NAME qeeg_test_debounce COMMAND qeeg_test_debounce)

  add_executable(qeeg_test_bids tests/test_bids.cpp)
  target_link_libraries(qeeg_test_bids PRIVATE qeeg)
  add_test(NAME qeeg_test_bids COMMAND qeeg_test_bids)

  add_executable(qeeg_test_line_noise tests/test_line_noise.cpp)
  target_link_libraries(qeeg_test_line_noise PRIVATE qeeg)
  add_test(NAME qeeg_test_line_noise COMMAND qeeg_test_line_noise)

  add_executable(qeeg_test_segments tests/test_segments.cpp)
  target_link_libraries(qeeg_test_segments PRIVATE qeeg)
  add_test(NAME qeeg_test_segments COMMAND qeeg_test_segments)

  add_executable(qeeg_test_recording_ops tests/test_recording_ops.cpp)
  target_link_libraries(qeeg_test_recording_ops PRIVATE qeeg)
  add_test(NAME qeeg_test_recording_ops COMMAND qeeg_test_recording_ops)


  add_executable(qeeg_test_plv tests/test_plv.cpp)
  target_link_libraries(qeeg_test_plv PRIVATE qeeg)
  add_test(NAME qeeg_test_plv COMMAND qeeg_test_plv)

  add_executable(qeeg_test_wpli tests/test_wpli.cpp)
  target_link_libraries(qeeg_test_wpli PRIVATE qeeg)
  add_test(NAME qeeg_test_wpli COMMAND qeeg_test_wpli)

  add_executable(qeeg_test_wpli2_debiased tests/test_wpli2_debiased.cpp)
  target_link_libraries(qeeg_test_wpli2_debiased PRIVATE qeeg)
  add_test(NAME qeeg_test_wpli2_debiased COMMAND qeeg_test_wpli2_debiased)

  add_executable(qeeg_test_online_bandpower tests/test_online_bandpower.cpp)
  target_link_libraries(qeeg_test_online_bandpower PRIVATE qeeg)
  add_test(NAME qeeg_test_online_bandpower COMMAND qeeg_test_online_bandpower)

  add_executable(qeeg_test_spherical_spline tests/test_spherical_spline.cpp)
  target_link_libraries(qeeg_test_spherical_spline PRIVATE qeeg)
  add_test(NAME qeeg_test_spherical_spline COMMAND qeeg_test_spherical_spline)

  add_executable(qeeg_test_coherence tests/test_coherence.cpp)
  target_link_libraries(qeeg_test_coherence PRIVATE qeeg)
  add_test(NAME qeeg_test_coherence COMMAND qeeg_test_coherence)

  add_executable(qeeg_test_imag_coherence tests/test_imag_coherence.cpp)
  target_link_libraries(qeeg_test_imag_coherence PRIVATE qeeg)
  add_test(NAME qeeg_test_imag_coherence COMMAND qeeg_test_imag_coherence)

  add_executable(qeeg_test_online_coherence tests/test_online_coherence.cpp)
  target_link_libraries(qeeg_test_online_coherence PRIVATE qeeg)
  add_test(NAME qeeg_test_online_coherence COMMAND qeeg_test_online_coherence)

  add_executable(qeeg_test_online_plv tests/test_online_plv.cpp)
  target_link_libraries(qeeg_test_online_plv PRIVATE qeeg)
  add_test(NAME qeeg_test_online_plv COMMAND qeeg_test_online_plv)

  add_executable(qeeg_test_biquad tests/test_biquad.cpp)
  target_link_libraries(qeeg_test_biquad PRIVATE qeeg)
  add_test(NAME qeeg_test_biquad COMMAND qeeg_test_biquad)

  add_executable(qeeg_test_preprocess tests/test_preprocess.cpp)
  target_link_libraries(qeeg_test_preprocess PRIVATE qeeg)
  add_test(NAME qeeg_test_preprocess COMMAND qeeg_test_preprocess)

  add_executable(qeeg_test_annotations tests/test_annotations.cpp)
  target_link_libraries(qeeg_test_annotations PRIVATE qeeg)
  add_test(NAME qeeg_test_annotations COMMAND qeeg_test_annotations)

  add_executable(qeeg_test_spectrogram tests/test_spectrogram.cpp)
  target_link_libraries(qeeg_test_spectrogram PRIVATE qeeg)
  add_test(NAME qeeg_test_spectrogram COMMAND qeeg_test_spectrogram)

  add_executable(qeeg_test_spectral_features tests/test_spectral_features.cpp)
  target_link_libraries(qeeg_test_spectral_features PRIVATE qeeg)
  add_test(NAME qeeg_test_spectral_features COMMAND qeeg_test_spectral_features)

  add_executable(qeeg_test_iaf tests/test_iaf.cpp)
  target_link_libraries(qeeg_test_iaf PRIVATE qeeg)
  add_test(NAME qeeg_test_iaf COMMAND qeeg_test_iaf)

  add_executable(qeeg_test_microstates tests/test_microstates.cpp)
  target_link_libraries(qeeg_test_microstates PRIVATE qeeg)
  add_test(NAME qeeg_test_microstates COMMAND qeeg_test_microstates)

  add_executable(qeeg_test_microstate_segments tests/test_microstate_segments.cpp)
  target_link_libraries(qeeg_test_microstate_segments PRIVATE qeeg)
  add_test(NAME qeeg_test_microstate_segments COMMAND qeeg_test_microstate_segments)


  add_executable(qeeg_test_pac tests/test_pac.cpp)
  target_link_libraries(qeeg_test_pac PRIVATE qeeg)
  add_test(NAME qeeg_test_pac COMMAND qeeg_test_pac)

  add_executable(qeeg_test_online_pac tests/test_online_pac.cpp)
  target_link_libraries(qeeg_test_online_pac PRIVATE qeeg)
  add_test(NAME qeeg_test_online_pac COMMAND qeeg_test_online_pac)

  add_executable(qeeg_test_artifacts tests/test_artifacts.cpp)
  target_link_libraries(qeeg_test_artifacts PRIVATE qeeg)
  add_test(NAME qeeg_test_artifacts COMMAND qeeg_test_artifacts)

  add_executable(qeeg_test_online_artifacts tests/test_online_artifacts.cpp)
  target_link_libraries(qeeg_test_online_artifacts PRIVATE qeeg)
  add_test(NAME qeeg_test_online_artifacts COMMAND qeeg_test_online_artifacts)

  add_executable(qeeg_test_wav_writer tests/test_wav_writer.cpp)
  target_link_libraries(qeeg_test_wav_writer PRIVATE qeeg)
  add_test(NAME qeeg_test_wav_writer COMMAND qeeg_test_wav_writer)


  add_executable(qeeg_test_osc tests/test_osc.cpp)
  target_link_libraries(qeeg_test_osc PRIVATE qeeg)
  add_test(NAME qeeg_test_osc COMMAND qeeg_test_osc)

  add_executable(qeeg_test_running_stats tests/test_running_stats.cpp)
  target_link_libraries(qeeg_test_running_stats PRIVATE qeeg)
  add_test(NAME qeeg_test_running_stats COMMAND qeeg_test_running_stats)

  add_executable(qeeg_test_robust_stats tests/test_robust_stats.cpp)
  target_link_libraries(qeeg_test_robust_stats PRIVATE qeeg)
  add_test(NAME qeeg_test_robust_stats COMMAND qeeg_test_robust_stats)

  add_executable(qeeg_test_svg_utils tests/test_svg_utils.cpp)
  target_link_libraries(qeeg_test_svg_utils PRIVATE qeeg)
  add_test(NAME qeeg_test_svg_utils COMMAND qeeg_test_svg_utils)

  add_executable(qeeg_test_pattern_match tests/test_pattern_match.cpp)
  target_link_libraries(qeeg_test_pattern_match PRIVATE qeeg)
  add_test(NAME qeeg_test_pattern_match COMMAND qeeg_test_pattern_match)


  add_executable(qeeg_test_reference_csv tests/test_reference_csv.cpp)
  target_link_libraries(qeeg_test_reference_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_reference_csv COMMAND qeeg_test_reference_csv)

  add_executable(qeeg_test_events_table tests/test_events_table.cpp)
  target_link_libraries(qeeg_test_events_table PRIVATE qeeg)
  add_test(NAME qeeg_test_events_table COMMAND qeeg_test_events_table)

  add_executable(qeeg_test_event_ops tests/test_event_ops.cpp)
  target_link_libraries(qeeg_test_event_ops PRIVATE qeeg)
  add_test(NAME qeeg_test_event_ops COMMAND qeeg_test_event_ops)

  add_executable(qeeg_test_nf_session tests/test_nf_session.cpp)
  target_link_libraries(qeeg_test_nf_session PRIVATE qeeg)
  add_test(NAME qeeg_test_nf_session COMMAND qeeg_test_nf_session)

  add_executable(qeeg_test_relative_bandpower tests/test_relative_bandpower.cpp)
  target_link_libraries(qeeg_test_relative_bandpower PRIVATE qeeg)
  add_test(NAME qeeg_test_relative_bandpower COMMAND qeeg_test_relative_bandpower)

  add_executable(qeeg_test_montage_csv tests/test_montage_csv.cpp)
  target_link_libraries(qeeg_test_montage_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_csv COMMAND qeeg_test_montage_csv)

  add_executable(qeeg_test_montage_aliases tests/test_montage_aliases.cpp)
  target_link_libraries(qeeg_test_montage_aliases PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_aliases COMMAND qeeg_test_montage_aliases)

  add_executable(qeeg_test_montage_builtin_1010 tests/test_montage_builtin_1010.cpp)
  target_link_libraries(qeeg_test_montage_builtin_1010 PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_builtin_1010 COMMAND qeeg_test_montage_builtin_1010)


  add_executable(qeeg_test_nf_metric_spec tests/test_nf_metric_spec.cpp)
  target_link_libraries(qeeg_test_nf_metric_spec PRIVATE qeeg)
  add_test(NAME qeeg_test_nf_metric_spec COMMAND qeeg_test_nf_metric_spec)

  add_executable(qeeg_test_nf_threshold tests/test_nf_threshold.cpp)
  target_link_libraries(qeeg_test_nf_threshold PRIVATE qeeg)
  add_test(NAME qeeg_test_nf_threshold COMMAND qeeg_test_nf_threshold)

  add_executable(qeeg_test_nf_metric_eval tests/test_nf_metric_eval.cpp)
  target_link_libraries(qeeg_test_nf_metric_eval PRIVATE qeeg)
  add_test(NAME qeeg_test_nf_metric_eval COMMAND qeeg_test_nf_metric_eval)

  add_executable(qeeg_test_band_spec_file tests/test_band_spec_file.cpp)
  target_link_libraries(qeeg_test_band_spec_file PRIVATE qeeg)
  add_test(NAME qeeg_test_band_spec_file COMMAND qeeg_test_band_spec_file)

  add_executable(qeeg_test_band_spec_iaf tests/test_band_spec_iaf.cpp)
  target_link_libraries(qeeg_test_band_spec_iaf PRIVATE qeeg)
  add_test(NAME qeeg_test_band_spec_iaf COMMAND qeeg_test_band_spec_iaf)

  add_executable(qeeg_test_baseline_norm tests/test_baseline_norm.cpp)
  target_link_libraries(qeeg_test_baseline_norm PRIVATE qeeg)
  add_test(NAME qeeg_test_baseline_norm COMMAND qeeg_test_baseline_norm)

  add_executable(qeeg_test_spherical_spline_weights tests/test_spherical_spline_weights.cpp)
  target_link_libraries(qeeg_test_spherical_spline_weights PRIVATE qeeg)
  add_test(NAME qeeg_test_spherical_spline_weights COMMAND qeeg_test_spherical_spline_weights)

  add_executable(qeeg_test_channel_qc tests/test_channel_qc.cpp)
  target_link_libraries(qeeg_test_channel_qc PRIVATE qeeg)
  add_test(NAME qeeg_test_channel_qc COMMAND qeeg_test_channel_qc)

  add_executable(qeeg_test_interpolate tests/test_interpolate.cpp)
  target_link_libraries(qeeg_test_interpolate PRIVATE qeeg)
  add_test(NAME qeeg_test_interpolate COMMAND qeeg_test_interpolate)

  # Unit tests:
  #
  # Tests include tests/test_support.hpp which overrides assert() with an
  # always-on check, so tests remain meaningful even in Release builds without
  # forcing toolchain-level /UNDEBUG flags (avoids MSVC D9025 warnings).

endif()

# ------------------------------------------------------------------------------
# Packaging (CPack)
# ------------------------------------------------------------------------------
#
# This enables generating simple binary archive packages from the install() rules
# above:
#   cpack --config <build>/CPackConfig.cmake
#
# We keep the default generators conservative and cross-platform: ZIP everywhere,
# plus TGZ on Unix-like hosts.
set(CPACK_PACKAGE_NAME "qeeg")
set(CPACK_PACKAGE_VENDOR "qeeg-map contributors")
set(CPACK_PACKAGE_CONTACT "https://github.com/masterblaster1999/qeeg-neurofeedback-opensoftware/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# Give the archive a deterministic, informative name.
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

if (WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ;ZIP")
endif()

include(CPack)
