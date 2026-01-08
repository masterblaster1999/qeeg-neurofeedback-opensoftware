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
  src/annotations.cpp
  src/edf_reader.cpp
  src/bdf_reader.cpp
  src/csv_reader.cpp
  src/fft.cpp
  src/signal.cpp
  src/spectrogram.cpp
  src/biquad.cpp
  src/preprocess.cpp
  src/welch_psd.cpp
  src/bandpower.cpp
  src/iaf.cpp
  src/microstates.cpp
  src/coherence.cpp
  src/pac.cpp
  src/plv.cpp
  src/artifacts.cpp
  src/online_artifacts.cpp
  src/online_bandpower.cpp
  src/online_coherence.cpp
  src/online_pac.cpp
  src/spherical_spline.cpp
  src/montage.cpp
  src/topomap.cpp
  src/bmp_writer.cpp
  src/wav_writer.cpp
  src/osc.cpp
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

# Sockets (OSC) on Windows
if (WIN32)
  target_link_libraries(qeeg PUBLIC ws2_32)
endif()

add_executable(qeeg_map_cli src/main.cpp)
target_link_libraries(qeeg_map_cli PRIVATE qeeg)

add_executable(qeeg_nf_cli src/nf_cli.cpp)
target_link_libraries(qeeg_nf_cli PRIVATE qeeg)

add_executable(qeeg_coherence_cli src/coherence_cli.cpp)
target_link_libraries(qeeg_coherence_cli PRIVATE qeeg)

add_executable(qeeg_plv_cli src/plv_cli.cpp)
target_link_libraries(qeeg_plv_cli PRIVATE qeeg)

add_executable(qeeg_epoch_cli src/epoch_cli.cpp)
target_link_libraries(qeeg_epoch_cli PRIVATE qeeg)

add_executable(qeeg_spectrogram_cli src/spectrogram_cli.cpp)
target_link_libraries(qeeg_spectrogram_cli PRIVATE qeeg)

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

if(QEEG_BUILD_TESTS)
  enable_testing()

  add_executable(qeeg_test_fft tests/test_fft.cpp)
  target_link_libraries(qeeg_test_fft PRIVATE qeeg)
  add_test(NAME qeeg_test_fft COMMAND qeeg_test_fft)

  add_executable(qeeg_test_bdf_reader tests/test_bdf_reader.cpp)
  target_link_libraries(qeeg_test_bdf_reader PRIVATE qeeg)
  add_test(NAME qeeg_test_bdf_reader COMMAND qeeg_test_bdf_reader)
  add_executable(qeeg_test_csv_reader tests/test_csv_reader.cpp)
  target_link_libraries(qeeg_test_csv_reader PRIVATE qeeg)
  add_test(NAME qeeg_test_csv_reader COMMAND qeeg_test_csv_reader)

  add_executable(qeeg_test_plv tests/test_plv.cpp)
  target_link_libraries(qeeg_test_plv PRIVATE qeeg)
  add_test(NAME qeeg_test_plv COMMAND qeeg_test_plv)

  add_executable(qeeg_test_online_bandpower tests/test_online_bandpower.cpp)
  target_link_libraries(qeeg_test_online_bandpower PRIVATE qeeg)
  add_test(NAME qeeg_test_online_bandpower COMMAND qeeg_test_online_bandpower)

  add_executable(qeeg_test_spherical_spline tests/test_spherical_spline.cpp)
  target_link_libraries(qeeg_test_spherical_spline PRIVATE qeeg)
  add_test(NAME qeeg_test_spherical_spline COMMAND qeeg_test_spherical_spline)

  add_executable(qeeg_test_coherence tests/test_coherence.cpp)
  target_link_libraries(qeeg_test_coherence PRIVATE qeeg)
  add_test(NAME qeeg_test_coherence COMMAND qeeg_test_coherence)

  add_executable(qeeg_test_online_coherence tests/test_online_coherence.cpp)
  target_link_libraries(qeeg_test_online_coherence PRIVATE qeeg)
  add_test(NAME qeeg_test_online_coherence COMMAND qeeg_test_online_coherence)

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

  add_executable(qeeg_test_iaf tests/test_iaf.cpp)
  target_link_libraries(qeeg_test_iaf PRIVATE qeeg)
  add_test(NAME qeeg_test_iaf COMMAND qeeg_test_iaf)

  add_executable(qeeg_test_microstates tests/test_microstates.cpp)
  target_link_libraries(qeeg_test_microstates PRIVATE qeeg)
  add_test(NAME qeeg_test_microstates COMMAND qeeg_test_microstates)

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

  add_executable(qeeg_test_reference_csv tests/test_reference_csv.cpp)
  target_link_libraries(qeeg_test_reference_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_reference_csv COMMAND qeeg_test_reference_csv)

  add_executable(qeeg_test_montage_csv tests/test_montage_csv.cpp)
  target_link_libraries(qeeg_test_montage_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_csv COMMAND qeeg_test_montage_csv)

endif()
