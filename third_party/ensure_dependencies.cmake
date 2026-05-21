cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED THIRD_PARTY_CMAKE_BUILD_TYPE)
  set(THIRD_PARTY_CMAKE_BUILD_TYPE "Release")
endif()

if(NOT DEFINED THIRD_PARTY_BUILD_JOBS)
  cmake_host_system_information(RESULT THIRD_PARTY_CPU_COUNT QUERY NUMBER_OF_LOGICAL_CORES)
  if(NOT THIRD_PARTY_CPU_COUNT MATCHES "^[0-9]+$" OR THIRD_PARTY_CPU_COUNT LESS 1)
    set(THIRD_PARTY_CPU_COUNT 1)
  endif()
  math(EXPR THIRD_PARTY_BUILD_JOBS "${THIRD_PARTY_CPU_COUNT} / 2")
  if(THIRD_PARTY_BUILD_JOBS LESS 1)
    set(THIRD_PARTY_BUILD_JOBS 1)
  endif()
endif()

get_filename_component(THIRD_PARTY_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(THIRD_PARTY_SOURCE_ROOT "${THIRD_PARTY_ROOT}/src")
set(THIRD_PARTY_BUILD_ROOT "${THIRD_PARTY_ROOT}/build")
set(THIRD_PARTY_INSTALL_ROOT "${THIRD_PARTY_ROOT}/install")
set(THIRD_PARTY_BUILD_SCRIPT "${THIRD_PARTY_ROOT}/build_webrtc_apm_1_3.sh")

set(THIRD_PARTY_WEBRTC_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/webrtc-audio-processing-1.3")
set(THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/ixwebsocket")
set(THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/nlohmann_json")
set(THIRD_PARTY_SPDLOG_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/spdlog")
set(THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT "${THIRD_PARTY_INSTALL_ROOT}/sherpa-onnx-x64-shared")

set(THIRD_PARTY_APM_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-processing-1.a")
set(THIRD_PARTY_AC_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-coding-1.a")
set(THIRD_PARTY_ABSL_CRC_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libabsl_crc.a")
set(THIRD_PARTY_APM_PC "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/pkgconfig/webrtc-audio-processing-1.pc")
set(THIRD_PARTY_AC_PC "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/pkgconfig/webrtc-audio-coding-1.pc")
set(THIRD_PARTY_SHERPA_ONNX_CXX_API "${THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT}/lib/libsherpa-onnx-cxx-api.so")
set(THIRD_PARTY_SHERPA_ONNX_C_API "${THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT}/lib/libsherpa-onnx-c-api.so")
set(THIRD_PARTY_ONNXRUNTIME_LIB "${THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT}/lib/libonnxruntime.so")
set(THIRD_PARTY_SHERPA_ONNX_HEADER "${THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT}/include/sherpa-onnx/c-api/cxx-api.h")

function(read_download_entry section key out_var)
  set(_downloads_file "${THIRD_PARTY_ROOT}/downloads")
  if(NOT EXISTS "${_downloads_file}")
    message(FATAL_ERROR "missing third-party downloads manifest: ${_downloads_file}")
  endif()

  file(STRINGS "${_downloads_file}" _download_lines)
  set(_in_requested_section FALSE)
  foreach(_download_line IN LISTS _download_lines)
    string(STRIP "${_download_line}" _download_line)
    if(_download_line STREQUAL "" OR _download_line MATCHES "^#")
      continue()
    endif()
    if(_download_line MATCHES "^\\[([^]]+)\\]$")
      set(_in_requested_section FALSE)
      if(CMAKE_MATCH_1 STREQUAL "${section}")
        set(_in_requested_section TRUE)
      endif()
      continue()
    endif()
    if(_in_requested_section AND _download_line MATCHES "^${key}=(.*)$")
      set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  message(FATAL_ERROR "missing ${key}= entry in [${section}] of ${_downloads_file}")
endfunction()

function(require_archive_from_downloads section out_archive)
  read_download_entry("${section}" archive _archive)
  read_download_entry("${section}" sha256 _expected_sha256)

  set(_archive_path "${THIRD_PARTY_ROOT}/archives/${_archive}")
  if(NOT EXISTS "${_archive_path}")
    message(FATAL_ERROR "missing third-party archive: ${_archive_path}")
  endif()

  file(SHA256 "${_archive_path}" _actual_sha256)
  if(NOT _actual_sha256 STREQUAL _expected_sha256)
    message(FATAL_ERROR
      "sha256 mismatch for ${_archive}: expected ${_expected_sha256}, got ${_actual_sha256}"
    )
  endif()

  set(${out_archive} "${_archive}" PARENT_SCOPE)
endfunction()

function(extract_third_party_source name archive strip_components)
  set(source_dir "${THIRD_PARTY_SOURCE_ROOT}/${name}")
  set(archive_path "${THIRD_PARTY_ROOT}/archives/${archive}")
  if(EXISTS "${source_dir}/CMakeLists.txt")
    return()
  endif()
  if(NOT EXISTS "${archive_path}")
    message(FATAL_ERROR "missing third-party archive: ${archive_path}")
  endif()
  file(MAKE_DIRECTORY "${source_dir}")
  execute_process(
    COMMAND tar --no-same-owner -xf "${archive_path}" --strip-components=${strip_components} -C "${source_dir}"
    RESULT_VARIABLE _extract_result
  )
  if(NOT _extract_result EQUAL 0 OR NOT EXISTS "${source_dir}/CMakeLists.txt")
    message(FATAL_ERROR "failed to extract ${archive_path} into ${source_dir}")
  endif()
endfunction()

function(build_cmake_third_party name)
  set(one_value_args SOURCE_DIR BUILD_DIR INSTALL_DIR)
  set(multi_value_args CMAKE_ARGS REQUIRED_FILES)
  cmake_parse_arguments(THIRD_PARTY "" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT THIRD_PARTY_SOURCE_DIR OR NOT THIRD_PARTY_BUILD_DIR OR NOT THIRD_PARTY_INSTALL_DIR)
    message(FATAL_ERROR "missing source, build, or install directory for third-party dependency: ${name}")
  endif()

  set(_all_required_files_exist TRUE)
  foreach(_required_file IN LISTS THIRD_PARTY_REQUIRED_FILES)
    if(NOT EXISTS "${_required_file}")
      set(_all_required_files_exist FALSE)
    endif()
  endforeach()
  if(_all_required_files_exist)
    message(STATUS "Vendored ${name} already prepared")
    return()
  endif()

  if(EXISTS "${THIRD_PARTY_BUILD_DIR}/CMakeFiles" AND NOT EXISTS "${THIRD_PARTY_BUILD_DIR}/CMakeCache.txt")
    file(REMOVE_RECURSE "${THIRD_PARTY_BUILD_DIR}")
  endif()

  message(STATUS "Preparing vendored ${name}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${THIRD_PARTY_SOURCE_DIR}"
      -B "${THIRD_PARTY_BUILD_DIR}"
      -DCMAKE_BUILD_TYPE=${THIRD_PARTY_CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX=${THIRD_PARTY_INSTALL_DIR}
      -DCMAKE_INSTALL_LIBDIR=lib
      -DCMAKE_INSTALL_INCLUDEDIR=include
      -DCMAKE_INSTALL_DATADIR=share
      ${THIRD_PARTY_CMAKE_ARGS}
    WORKING_DIRECTORY "${THIRD_PARTY_ROOT}/.."
    RESULT_VARIABLE _third_party_configure_result
  )
  if(NOT _third_party_configure_result EQUAL 0)
    message(FATAL_ERROR "failed to configure vendored ${name}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      --build "${THIRD_PARTY_BUILD_DIR}"
      --target install
      --config "${THIRD_PARTY_CMAKE_BUILD_TYPE}"
      --parallel "${THIRD_PARTY_BUILD_JOBS}"
    WORKING_DIRECTORY "${THIRD_PARTY_ROOT}/.."
    RESULT_VARIABLE _third_party_build_result
  )
  if(NOT _third_party_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build and install vendored ${name}")
  endif()
endfunction()

set(THIRD_PARTY_ABSL_CRC_READY FALSE)
if(EXISTS "${THIRD_PARTY_ABSL_CRC_LIB}")
  execute_process(
    COMMAND head -c 8 "${THIRD_PARTY_ABSL_CRC_LIB}"
    OUTPUT_VARIABLE _absl_crc_archive_header
  )
  if(_absl_crc_archive_header STREQUAL "!<arch>\n")
    set(THIRD_PARTY_ABSL_CRC_READY TRUE)
  endif()
endif()

if(NOT EXISTS "${THIRD_PARTY_APM_LIB}" OR NOT EXISTS "${THIRD_PARTY_AC_LIB}" OR
   NOT THIRD_PARTY_ABSL_CRC_READY OR
   NOT EXISTS "${THIRD_PARTY_APM_PC}" OR NOT EXISTS "${THIRD_PARTY_AC_PC}")
  message(STATUS "Preparing vendored webrtc-audio-processing")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "THIRD_PARTY_BUILD_JOBS=${THIRD_PARTY_BUILD_JOBS}"
      "${THIRD_PARTY_BUILD_SCRIPT}"
    WORKING_DIRECTORY "${THIRD_PARTY_ROOT}/.."
    RESULT_VARIABLE _webrtc_build_result
  )
  if(NOT _webrtc_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build vendored webrtc-audio-processing")
  endif()
else()
  message(STATUS "Vendored webrtc-audio-processing already prepared")
endif()

require_archive_from_downloads(ixwebsocket THIRD_PARTY_IXWEBSOCKET_ARCHIVE)
require_archive_from_downloads(nlohmann_json THIRD_PARTY_NLOHMANN_JSON_ARCHIVE)
require_archive_from_downloads(spdlog THIRD_PARTY_SPDLOG_ARCHIVE)

extract_third_party_source(ixwebsocket "${THIRD_PARTY_IXWEBSOCKET_ARCHIVE}" 1)
extract_third_party_source(nlohmann_json "${THIRD_PARTY_NLOHMANN_JSON_ARCHIVE}" 1)
extract_third_party_source(spdlog "${THIRD_PARTY_SPDLOG_ARCHIVE}" 1)

build_cmake_third_party(nlohmann_json
  SOURCE_DIR "${THIRD_PARTY_SOURCE_ROOT}/nlohmann_json"
  BUILD_DIR "${THIRD_PARTY_BUILD_ROOT}/nlohmann_json"
  INSTALL_DIR "${THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT}"
  REQUIRED_FILES
    "${THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT}/include/nlohmann/json.hpp"
    "${THIRD_PARTY_NLOHMANN_JSON_INSTALL_ROOT}/share/cmake/nlohmann_json/nlohmann_jsonConfig.cmake"
  CMAKE_ARGS
    -DJSON_BuildTests=OFF
    -DJSON_Install=ON
)

build_cmake_third_party(spdlog
  SOURCE_DIR "${THIRD_PARTY_SOURCE_ROOT}/spdlog"
  BUILD_DIR "${THIRD_PARTY_BUILD_ROOT}/spdlog"
  INSTALL_DIR "${THIRD_PARTY_SPDLOG_INSTALL_ROOT}"
  REQUIRED_FILES
    "${THIRD_PARTY_SPDLOG_INSTALL_ROOT}/include/spdlog/spdlog.h"
    "${THIRD_PARTY_SPDLOG_INSTALL_ROOT}/lib/libspdlog.a"
    "${THIRD_PARTY_SPDLOG_INSTALL_ROOT}/lib/cmake/spdlog/spdlogConfig.cmake"
  CMAKE_ARGS
    -DSPDLOG_BUILD_EXAMPLE=OFF
    -DSPDLOG_BUILD_TESTS=OFF
    -DSPDLOG_BUILD_BENCH=OFF
    -DSPDLOG_INSTALL=ON
    -DSPDLOG_BUILD_SHARED=OFF
)

build_cmake_third_party(ixwebsocket
  SOURCE_DIR "${THIRD_PARTY_SOURCE_ROOT}/ixwebsocket"
  BUILD_DIR "${THIRD_PARTY_BUILD_ROOT}/ixwebsocket"
  INSTALL_DIR "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}"
  REQUIRED_FILES
    "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}/include/ixwebsocket/IXWebSocket.h"
    "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}/lib/libixwebsocket.a"
    "${THIRD_PARTY_IXWEBSOCKET_INSTALL_ROOT}/lib/cmake/ixwebsocket/ixwebsocket-config.cmake"
  CMAKE_ARGS
    -DBUILD_DEMO=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DIXWEBSOCKET_INSTALL=ON
    -DUSE_TLS=ON
    -DUSE_ZLIB=ON
)

require_archive_from_downloads(sherpa-onnx THIRD_PARTY_SHERPA_ONNX_ARCHIVE)
extract_third_party_source(sherpa-onnx "${THIRD_PARTY_SHERPA_ONNX_ARCHIVE}" 1)

if(NOT EXISTS "${THIRD_PARTY_SHERPA_ONNX_CXX_API}" OR
   NOT EXISTS "${THIRD_PARTY_SHERPA_ONNX_C_API}" OR
   NOT EXISTS "${THIRD_PARTY_ONNXRUNTIME_LIB}" OR
   NOT EXISTS "${THIRD_PARTY_SHERPA_ONNX_HEADER}")
  message(STATUS "Preparing vendored sherpa-onnx shared runtime")
  set(_sherpa_build_dir "${THIRD_PARTY_BUILD_ROOT}/sherpa-onnx-x64-shared")
  file(MAKE_DIRECTORY "${_sherpa_build_dir}")

  foreach(_sherpa_dependency_section
      sherpa-onnxruntime
      sherpa-kaldi-native-fbank
      sherpa-kissfft
      sherpa-kaldi-decoder
      sherpa-kaldifst
      sherpa-openfst
      sherpa-eigen
      sherpa-simple-sentencepiece
      sherpa-json)
    require_archive_from_downloads("${_sherpa_dependency_section}" _sherpa_archive)
    if(EXISTS "${_sherpa_build_dir}/${_sherpa_archive}" OR
       IS_SYMLINK "${_sherpa_build_dir}/${_sherpa_archive}")
      file(REMOVE "${_sherpa_build_dir}/${_sherpa_archive}")
    endif()
    file(CREATE_LINK
      "${THIRD_PARTY_ROOT}/archives/${_sherpa_archive}"
      "${_sherpa_build_dir}/${_sherpa_archive}"
      SYMBOLIC
    )
  endforeach()

  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${THIRD_PARTY_SOURCE_ROOT}/sherpa-onnx"
      -B "${_sherpa_build_dir}"
      -DCMAKE_BUILD_TYPE=${THIRD_PARTY_CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX=${THIRD_PARTY_SHERPA_ONNX_INSTALL_ROOT}
      -DBUILD_SHARED_LIBS=ON
      -DSHERPA_ONNX_ENABLE_C_API=ON
      -DSHERPA_ONNX_ENABLE_BINARY=OFF
      -DSHERPA_ONNX_ENABLE_TESTS=OFF
      -DSHERPA_ONNX_ENABLE_PYTHON=OFF
      -DSHERPA_ONNX_ENABLE_TTS=OFF
      -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF
      -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF
      -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF
    WORKING_DIRECTORY "${THIRD_PARTY_ROOT}/.."
    RESULT_VARIABLE _sherpa_configure_result
  )
  if(NOT _sherpa_configure_result EQUAL 0)
    message(FATAL_ERROR "failed to configure vendored sherpa-onnx")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      --build "${_sherpa_build_dir}"
      --target install
      --config "${THIRD_PARTY_CMAKE_BUILD_TYPE}"
      --parallel "${THIRD_PARTY_BUILD_JOBS}"
    WORKING_DIRECTORY "${THIRD_PARTY_ROOT}/.."
    RESULT_VARIABLE _sherpa_build_result
  )
  if(NOT _sherpa_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build and install vendored sherpa-onnx")
  endif()
else()
  message(STATUS "Vendored sherpa-onnx already prepared")
endif()
