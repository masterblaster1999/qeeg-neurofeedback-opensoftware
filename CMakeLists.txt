cmake_minimum_required(VERSION 3.16)

project(qeeg_map
  VERSION 0.1.0
  DESCRIPTION "First-pass open-source qEEG mapping + topomap renderer in C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(QEEG_BUILD_TESTS "Build tests" ON)

add_library(qeeg STATIC
  src/utils.cpp
  src/reader.cpp
  src/edf_reader.cpp
  src/csv_reader.cpp
  src/fft.cpp
  src/welch_psd.cpp
  src/bandpower.cpp
  src/montage.cpp
  src/topomap.cpp
  src/bmp_writer.cpp
)

target_include_directories(qeeg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Enable warnings (best-effort, portable)
if (MSVC)
  target_compile_options(qeeg PRIVATE /W4)
else()
  target_compile_options(qeeg PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(qeeg_map_cli src/main.cpp)
target_link_libraries(qeeg_map_cli PRIVATE qeeg)

if(QEEG_BUILD_TESTS)
  enable_testing()
  add_executable(qeeg_tests tests/test_fft.cpp)
  target_link_libraries(qeeg_tests PRIVATE qeeg)
  add_test(NAME qeeg_tests COMMAND qeeg_tests)
endif()
