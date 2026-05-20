cmake_minimum_required(VERSION 3.20)

project(AudioProcessingModule LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(THIRD_PARTY_BUILD_SCRIPT "${THIRD_PARTY_ROOT}/build_webrtc_apm_1_3.sh")
set(THIRD_PARTY_INSTALL_ROOT "${THIRD_PARTY_ROOT}/install/webrtc-audio-processing-1.3")
set(THIRD_PARTY_PKGCONFIG_DIR "${THIRD_PARTY_INSTALL_ROOT}/lib/pkgconfig")
set(THIRD_PARTY_APM_LIB "${THIRD_PARTY_INSTALL_ROOT}/lib/libwebrtc-audio-processing-1.a")
set(THIRD_PARTY_AC_LIB "${THIRD_PARTY_INSTALL_ROOT}/lib/libwebrtc-audio-coding-1.a")
set(THIRD_PARTY_APM_PC "${THIRD_PARTY_PKGCONFIG_DIR}/webrtc-audio-processing-1.pc")
set(THIRD_PARTY_AC_PC "${THIRD_PARTY_PKGCONFIG_DIR}/webrtc-audio-coding-1.pc")

if(NOT EXISTS "${THIRD_PARTY_APM_LIB}" OR NOT EXISTS "${THIRD_PARTY_AC_LIB}" OR
   NOT EXISTS "${THIRD_PARTY_APM_PC}" OR NOT EXISTS "${THIRD_PARTY_AC_PC}")
  message(STATUS "Building vendored third-party WebRTC APM sources")
  execute_process(
    COMMAND "${THIRD_PARTY_BUILD_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _third_party_build_result
  )
  if(NOT _third_party_build_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to build vendored WebRTC APM sources with ${THIRD_PARTY_BUILD_SCRIPT}")
  endif()
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
  set(ENV{PKG_CONFIG_PATH}
    "${THIRD_PARTY_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
else()
  set(ENV{PKG_CONFIG_PATH} "${THIRD_PARTY_PKGCONFIG_DIR}")
endif()

pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)

set(WEBRTC_APM_EXTRA_INCLUDE_DIRS "")
foreach(_webrtc_include_dir IN LISTS WEBRTC_APM_INCLUDE_DIRS)
  get_filename_component(_webrtc_include_parent "${_webrtc_include_dir}" DIRECTORY)
  list(APPEND WEBRTC_APM_EXTRA_INCLUDE_DIRS "${_webrtc_include_parent}")
endforeach()
list(REMOVE_DUPLICATES WEBRTC_APM_EXTRA_INCLUDE_DIRS)

add_custom_target(third_party_webrtc_apm
  COMMAND "${THIRD_PARTY_BUILD_SCRIPT}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Ensuring vendored WebRTC APM sources are built"
)

add_library(audio_processing_module
  src/application.cpp
  src/audio_frame.cpp
  src/modules.cpp
  src/unix_socket.cpp
  src/wav_writer.cpp
  src/webrtc_processor.cpp
)

target_include_directories(audio_processing_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(audio_processing_module
  SYSTEM
  PUBLIC
    ${WEBRTC_APM_INCLUDE_DIRS}
    ${WEBRTC_APM_EXTRA_INCLUDE_DIRS}
)

target_compile_options(audio_processing_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  PUBLIC
    ${WEBRTC_APM_CFLAGS_OTHER}
)

target_link_libraries(audio_processing_module
  PUBLIC
    Threads::Threads
    PkgConfig::WEBRTC_APM
)

add_dependencies(audio_processing_module third_party_webrtc_apm)

add_executable(AudioProcessingModule
  src/main.cpp
)

target_link_libraries(AudioProcessingModule
  PRIVATE
    audio_processing_module
)

add_dependencies(AudioProcessingModule third_party_webrtc_apm)

enable_testing()

add_executable(audio_processing_module_tests
  tests/audio_processing_module_tests.cpp
)

target_link_libraries(audio_processing_module_tests
  PRIVATE
    audio_processing_module
)

add_dependencies(audio_processing_module_tests third_party_webrtc_apm)

add_test(NAME audio_processing_module_tests COMMAND audio_processing_module_tests)

add_custom_target(distclean
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${CMAKE_BINARY_DIR}/AudioProcessingModule"
    "${CMAKE_BINARY_DIR}/audio_processing_module_tests"
    "${CMAKE_BINARY_DIR}/libaudio_processing_module.a"
    "${CMAKE_BINARY_DIR}/CMakeCache.txt"
    "${CMAKE_BINARY_DIR}/CMakeFiles"
    "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
    "${CMAKE_BINARY_DIR}/CPackSourceConfig.cmake"
    "${CMAKE_BINARY_DIR}/CTestTestfile.cmake"
    "${CMAKE_BINARY_DIR}/DartConfiguration.tcl"
    "${CMAKE_BINARY_DIR}/Testing"
    "${CMAKE_BINARY_DIR}/build.ninja"
    "${CMAKE_BINARY_DIR}/cmake_install.cmake"
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_BINARY_DIR}/install_manifest.txt"
    "${CMAKE_BINARY_DIR}/Makefile"
    "${CMAKE_BINARY_DIR}/.ninja_deps"
    "${CMAKE_BINARY_DIR}/.ninja_log"
    "${CMAKE_BINARY_DIR}/output"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/build"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/install"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tools"
  COMMENT "Removing generated CMake files and build artifacts"
)
