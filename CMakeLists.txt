cmake_minimum_required(VERSION 3.20)

project(soundbox-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(THIRD_PARTY_SOURCE_ROOT "${THIRD_PARTY_ROOT}/src")
set(THIRD_PARTY_BUILD_ROOT "${THIRD_PARTY_ROOT}/build")
set(THIRD_PARTY_INSTALL_ROOT "${THIRD_PARTY_ROOT}/install")
set(THIRD_PARTY_BUILD_SCRIPT "${THIRD_PARTY_ROOT}/build_webrtc_apm_1_3.sh")
set(THIRD_PARTY_WEBRTC_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/webrtc-audio-processing-1.3")
set(THIRD_PARTY_PKGCONFIG_DIR "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/pkgconfig")
set(THIRD_PARTY_APM_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-processing-1.a")
set(THIRD_PARTY_AC_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-coding-1.a")
set(THIRD_PARTY_APM_PC "${THIRD_PARTY_PKGCONFIG_DIR}/webrtc-audio-processing-1.pc")
set(THIRD_PARTY_AC_PC "${THIRD_PARTY_PKGCONFIG_DIR}/webrtc-audio-coding-1.pc")
set(THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/ixwebsocket")
set(THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/nlohmann_json")
set(THIRD_PARTY_SPDLOG_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/spdlog")
set(THIRD_PARTY_CMAKE_BUILD_TYPE "Release")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

include("${THIRD_PARTY_ROOT}/ensure_dependencies.cmake")

find_package(nlohmann_json CONFIG REQUIRED
  PATHS "${THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT}"
  NO_DEFAULT_PATH
)

find_package(spdlog CONFIG REQUIRED
  PATHS "${THIRD_PARTY_SPDLOG_INSTALL_ROOT}"
  NO_DEFAULT_PATH
)

find_package(ixwebsocket CONFIG REQUIRED
  PATHS "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}"
  NO_DEFAULT_PATH
)

target_include_directories(ixwebsocket::ixwebsocket
  INTERFACE
    "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}/include"
)

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

add_custom_target(ensure_third_party
  COMMAND "${CMAKE_COMMAND}"
    -DTHIRD_PARTY_CMAKE_BUILD_TYPE=${THIRD_PARTY_CMAKE_BUILD_TYPE}
    -P "${THIRD_PARTY_ROOT}/ensure_dependencies.cmake"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Ensuring vendored third-party dependencies are prepared"
)

add_library(common_module
  src/common/audio_frame.cpp
  src/common/rate_limited_logger.cpp
  src/common/unix_socket.cpp
  src/common/wav_writer.cpp
)

target_include_directories(common_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(common_module
  PUBLIC
    Threads::Threads
    spdlog::spdlog
)

target_compile_options(common_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_dependencies(common_module ensure_third_party)

add_library(soundbox_frontend_module
  src/frontend/soundbox/audio_pipe.cpp
  src/frontend/soundbox/audio_router.cpp
  src/frontend/soundbox/client.cpp
  src/frontend/soundbox/command_guard.cpp
  src/frontend/soundbox/event_dispatcher.cpp
  src/frontend/soundbox/mode_controller.cpp
  src/frontend/soundbox/packet.cpp
  src/frontend/soundbox/packet_parser.cpp
  src/frontend/soundbox/response_hub.cpp
  src/frontend/soundbox/soundbox.cpp
  src/frontend/soundbox_frontend.cpp
)

target_include_directories(soundbox_frontend_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(soundbox_frontend_module
  PUBLIC
    common_module
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

target_compile_options(soundbox_frontend_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_dependencies(soundbox_frontend_module ensure_third_party)

add_library(kws_module
  src/apm/kws/gate.cpp
  src/apm/kws/keywords.cpp
  src/apm/kws/kws_socket_server.cpp
  src/apm/kws/local_listener.cpp
  src/apm/kws/trigger.cpp
)

target_include_directories(kws_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(kws_module
  PUBLIC
    common_module
    nlohmann_json::nlohmann_json
)

target_compile_options(kws_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_dependencies(kws_module ensure_third_party)

add_library(aec_module
  src/apm/aec/aec_stream_processor.cpp
  src/apm/aec/webrtc_processor.cpp
)

target_include_directories(aec_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_include_directories(aec_module
  SYSTEM
  PUBLIC
    ${WEBRTC_APM_INCLUDE_DIRS}
    ${WEBRTC_APM_EXTRA_INCLUDE_DIRS}
)

target_compile_options(aec_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  PUBLIC
    ${WEBRTC_APM_CFLAGS_OTHER}
)

target_link_libraries(aec_module
  PUBLIC
    common_module
    PkgConfig::WEBRTC_APM
)

add_dependencies(aec_module ensure_third_party)

add_library(llm_file_module
  src/llm/file_recorder.cpp
)

target_include_directories(llm_file_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(llm_file_module
  PUBLIC
    common_module
)

target_compile_options(llm_file_module
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_dependencies(llm_file_module ensure_third_party)

add_library(audio_processing_module
  src/config/application.cpp
)

target_include_directories(audio_processing_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(audio_processing_module
  PUBLIC
    aec_module
    kws_module
    llm_file_module
    soundbox_frontend_module
)

add_dependencies(audio_processing_module ensure_third_party)

add_executable(soundbox-server
  src/main.cpp
)

target_link_libraries(soundbox-server
  PRIVATE
    audio_processing_module
)

add_dependencies(soundbox-server ensure_third_party)

enable_testing()

add_executable(audio_processing_module_tests
  tests/audio_processing_module_tests.cpp
  tests/aec/file_audio_stream_frontend.cpp
)

target_link_libraries(audio_processing_module_tests
  PRIVATE
    audio_processing_module
)

add_dependencies(audio_processing_module_tests ensure_third_party)

add_test(NAME audio_processing_module_tests COMMAND audio_processing_module_tests)

add_custom_target(distclean
  COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target clean
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${CMAKE_BINARY_DIR}/soundbox-server"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ixwebsocket"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/install"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tools"
  COMMENT "Removing generated CMake files and build artifacts"
)
