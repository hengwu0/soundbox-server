cmake_minimum_required(VERSION 3.20)

project(AudioProcessingModule LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)

set(WEBRTC_APM_EXTRA_INCLUDE_DIRS "")
foreach(_webrtc_include_dir IN LISTS WEBRTC_APM_INCLUDE_DIRS)
  get_filename_component(_webrtc_include_parent "${_webrtc_include_dir}" DIRECTORY)
  list(APPEND WEBRTC_APM_EXTRA_INCLUDE_DIRS "${_webrtc_include_parent}")
endforeach()
list(REMOVE_DUPLICATES WEBRTC_APM_EXTRA_INCLUDE_DIRS)

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

add_executable(AudioProcessingModule
  src/main.cpp
)

target_link_libraries(AudioProcessingModule
  PRIVATE
    audio_processing_module
)

enable_testing()

add_executable(audio_processing_module_tests
  tests/audio_processing_module_tests.cpp
)

target_link_libraries(audio_processing_module_tests
  PRIVATE
    audio_processing_module
)

add_test(NAME audio_processing_module_tests COMMAND audio_processing_module_tests)
