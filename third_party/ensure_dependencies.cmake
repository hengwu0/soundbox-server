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

set(THIRD_PARTY_APM_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-processing-1.a")
set(THIRD_PARTY_AC_LIB "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/libwebrtc-audio-coding-1.a")
set(THIRD_PARTY_APM_PC "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/pkgconfig/webrtc-audio-processing-1.pc")
set(THIRD_PARTY_AC_PC "${THIRD_PARTY_WEBRTC_INSTALL_ROOT}/lib/pkgconfig/webrtc-audio-coding-1.pc")

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

if(NOT EXISTS "${THIRD_PARTY_APM_LIB}" OR NOT EXISTS "${THIRD_PARTY_AC_LIB}" OR
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

extract_third_party_source(ixwebsocket ixwebsocket-v11.4.5.tar.gz 1)
extract_third_party_source(nlohmann_json nlohmann_json-v3.11.3.tar.xz 1)
extract_third_party_source(spdlog spdlog-v1.15.1.tar.gz 1)

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
