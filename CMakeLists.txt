cmake_minimum_required(VERSION 3.16)

project(qeeg_map
  VERSION 0.1.0
  DESCRIPTION "First-pass open-source qEEG mapping + topomap renderer in C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(QEEG_BUILD_TESTS "Build tests" ON)

include(GNUInstallDirs)

add_library(qeeg STATIC
  src/utils.cpp
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
  src/biquad.cpp
  src/preprocess.cpp
  src/welch_psd.cpp
  src/bandpower.cpp
  src/iaf.cpp
  src/microstates.cpp
  src/coherence.cpp
  src/pac.cpp
  src/plv.cpp
  src/nf_metric.cpp
  src/artifacts.cpp
  src/online_artifacts.cpp
  src/online_bandpower.cpp
  src/online_coherence.cpp
  src/online_pac.cpp
  src/spherical_spline.cpp
  src/montage.cpp
  src/topomap.cpp
  src/channel_qc.cpp
  src/interpolate.cpp
  src/bmp_writer.cpp
  src/wav_writer.cpp
  src/osc.cpp
  src/bids.cpp
)

add_library(qeeg::qeeg ALIAS qeeg)

target_include_directories(qeeg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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

add_executable(qeeg_info_cli src/info_cli.cpp)
target_link_libraries(qeeg_info_cli PRIVATE qeeg)

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


add_executable(qeeg_clean_cli src/clean_cli.cpp)
target_link_libraries(qeeg_clean_cli PRIVATE qeeg)

add_executable(qeeg_quality_cli src/quality_cli.cpp)
target_link_libraries(qeeg_quality_cli PRIVATE qeeg)

add_executable(qeeg_preprocess_cli src/preprocess_cli.cpp)
target_link_libraries(qeeg_preprocess_cli PRIVATE qeeg)

add_executable(qeeg_channel_qc_cli src/channel_qc_cli.cpp)
target_link_libraries(qeeg_channel_qc_cli PRIVATE qeeg)


# Install + package config (so downstream projects can use find_package(qeeg))
install(TARGETS qeeg
  EXPORT qeegTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install CLI tools
install(TARGETS
  qeeg_map_cli
  qeeg_nf_cli
  qeeg_coherence_cli
  qeeg_plv_cli
  qeeg_epoch_cli
  qeeg_spectrogram_cli
  qeeg_iaf_cli
  qeeg_microstates_cli
  qeeg_pac_cli
  qeeg_artifacts_cli
  qeeg_reference_cli
  qeeg_info_cli
  qeeg_convert_cli
  qeeg_export_edf_cli
  qeeg_export_bdf_cli
  qeeg_export_brainvision_cli
  qeeg_export_bids_cli
  qeeg_preprocess_cli
  qeeg_quality_cli
  qeeg_clean_cli
  qeeg_channel_qc_cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

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

if(QEEG_BUILD_TESTS)
  enable_testing()

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

  add_executable(qeeg_test_channel_map tests/test_channel_map.cpp)
  target_link_libraries(qeeg_test_channel_map PRIVATE qeeg)
  add_test(NAME qeeg_test_channel_map COMMAND qeeg_test_channel_map)

  add_executable(qeeg_test_reader_extensions tests/test_reader_extensions.cpp)
  target_link_libraries(qeeg_test_reader_extensions PRIVATE qeeg)
  add_test(NAME qeeg_test_reader_extensions COMMAND qeeg_test_reader_extensions)

  add_executable(qeeg_test_triggers tests/test_triggers.cpp)
  target_link_libraries(qeeg_test_triggers PRIVATE qeeg)
  add_test(NAME qeeg_test_triggers COMMAND qeeg_test_triggers)

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

  add_executable(qeeg_test_pattern_match tests/test_pattern_match.cpp)
  target_link_libraries(qeeg_test_pattern_match PRIVATE qeeg)
  add_test(NAME qeeg_test_pattern_match COMMAND qeeg_test_pattern_match)


  add_executable(qeeg_test_reference_csv tests/test_reference_csv.cpp)
  target_link_libraries(qeeg_test_reference_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_reference_csv COMMAND qeeg_test_reference_csv)

  add_executable(qeeg_test_relative_bandpower tests/test_relative_bandpower.cpp)
  target_link_libraries(qeeg_test_relative_bandpower PRIVATE qeeg)
  add_test(NAME qeeg_test_relative_bandpower COMMAND qeeg_test_relative_bandpower)

  add_executable(qeeg_test_montage_csv tests/test_montage_csv.cpp)
  target_link_libraries(qeeg_test_montage_csv PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_csv COMMAND qeeg_test_montage_csv)

  add_executable(qeeg_test_montage_aliases tests/test_montage_aliases.cpp)
  target_link_libraries(qeeg_test_montage_aliases PRIVATE qeeg)
  add_test(NAME qeeg_test_montage_aliases COMMAND qeeg_test_montage_aliases)

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

endif()
