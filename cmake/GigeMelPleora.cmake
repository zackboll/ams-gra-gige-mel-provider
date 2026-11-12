# Included only for the explicit optional build. No download, system install,
# SDK driver setup, environment change or SDK files in the installed package.
set(GIGE_MEL_PLEORA_VERSION "6.5.3.7155")
set(GIGE_MEL_PLEORA_ROOT "" CACHE PATH "External eBUS 6.5.3.7155 directory containing include/ and lib/")
set(GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR "${GIGE_MEL_PLEORA_ROOT}/genicam-headers/library/CPP/include"
  CACHE PATH "External GenApi 3.4.1.1 headers (directory containing GenApi/IFloat.h)")

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "The initial Pleora build is qualified only against the Linux x86_64 eBUS SDK layout.")
endif()
if(NOT EXISTS "${GIGE_MEL_PLEORA_ROOT}/include/PvVersion.h")
  message(FATAL_ERROR "GIGE_MEL_BUILD_PLEORA=ON requires -DGIGE_MEL_PLEORA_ROOT=<external eBUS ${GIGE_MEL_PLEORA_VERSION} directory>. "
    "See docs/pleora-integration.md and the explicit scripts/provision_pleora.py helper. Configure never downloads an SDK.")
endif()
file(READ "${GIGE_MEL_PLEORA_ROOT}/include/PvVersion.h" _pleora_version_header)
foreach(_part IN ITEMS MAJOR MINOR SUB BUILD)
  string(REGEX MATCH "#define[ \t]+VERSION_${_part}[ \t]+([0-9]+)" _match "${_pleora_version_header}")
  set(_pleora_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(_pleora_found "${_pleora_MAJOR}.${_pleora_MINOR}.${_pleora_SUB}.${_pleora_BUILD}")
if(NOT _pleora_found STREQUAL GIGE_MEL_PLEORA_VERSION)
  message(FATAL_ERROR "Pleora adapter requires eBUS ${GIGE_MEL_PLEORA_VERSION}; found headers for '${_pleora_found}'.")
endif()
if(NOT EXISTS "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}/GenApi/IFloat.h" OR
   NOT EXISTS "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}/_GenICamVersion.h")
  message(FATAL_ERROR "Pleora float-increment validation requires the external GenApi 3.4.1.1 headers. "
    "Set GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR to their library/CPP/include directory. See docs/pleora-integration.md.")
endif()
file(READ "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}/_GenICamVersion.h" _genicam_version_header)
foreach(_part IN ITEMS MAJOR MINOR SUBMINOR)
  string(REGEX MATCH "#define[ \t]+GENICAM_VERSION_${_part}[ \t]+([0-9]+)" _match "${_genicam_version_header}")
  set(_genicam_${_part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT "${_genicam_MAJOR}.${_genicam_MINOR}.${_genicam_SUBMINOR}" STREQUAL "3.4.1")
  message(FATAL_ERROR "The Pleora build requires GenApi 3.4.1.1 headers (which declare version 3.4.1), matching the SDK's GenApi_3_4 namespace.")
endif()

set(_pleora_lib_dir "${GIGE_MEL_PLEORA_ROOT}/lib")
set(_pleora_genicam_lib_dir "${_pleora_lib_dir}/genicam/bin/Linux64_x64")
set(_pleora_libraries)
foreach(_library IN ITEMS PvDevice PvStream PvSystem PvBuffer PvGenICam PvBase)
  set(_path "${_pleora_lib_dir}/lib${_library}.so")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "Missing eBUS library ${_path}; use a complete external SDK with intact symlinks.")
  endif()
  list(APPEND _pleora_libraries "${_path}")
endforeach()
foreach(_library IN ITEMS GenApi GCBase)
  set(_path "${_pleora_genicam_lib_dir}/lib${_library}_gcc48_v3_4.so")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "Missing SDK GenApi runtime ${_path}. Keep the runtime supplied with eBUS; the separate GenApi download supplies headers only.")
  endif()
  list(APPEND _pleora_libraries "${_path}")
endforeach()

add_library(gige_mel_pleora_sdk INTERFACE)
target_include_directories(gige_mel_pleora_sdk SYSTEM INTERFACE
  "${GIGE_MEL_PLEORA_ROOT}/include" "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}")
target_link_libraries(gige_mel_pleora_sdk INTERFACE ${_pleora_libraries})
# eBUS's libraries do not carry their own RUNPATH. These BUILD-ONLY internal
# programs use a transitive RPATH to this explicitly selected SDK. Nothing
# propagates to the provider or to an installed/exported target.
target_link_options(gige_mel_pleora_sdk INTERFACE
  "LINKER:--disable-new-dtags"
  "LINKER:-rpath,${_pleora_lib_dir}"
  "LINKER:-rpath,${_pleora_genicam_lib_dir}")

add_library(gige_mel_pleora STATIC
  src/backends/pleora/pleora_backend.cpp
  src/backends/pleora/pleora_features.cpp)
set_target_properties(gige_mel_pleora PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gige_mel_pleora PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/pleora)
target_link_libraries(gige_mel_pleora PUBLIC gige_mel_backend gige_mel_camera_profiles
  PRIVATE gige_mel_pleora_sdk gige_mel_build_options)
add_executable(gige_mel_pleora_diagnostic tools/pleora_diagnostic.cpp)
target_link_libraries(gige_mel_pleora_diagnostic PRIVATE gige_mel_pleora gige_mel_build_options)

if(BUILD_TESTING)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
  set(_pleora_test_environment
    "PUREGEV_ROOT=${GIGE_MEL_PLEORA_ROOT}"
    "GENICAM_ROOT=${_pleora_lib_dir}/genicam"
    "GENICAM_ROOT_V3_4=${_pleora_lib_dir}/genicam"
    "GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
  add_executable(pleora_contract_tests tests/pleora_contract_tests.cpp
    tests/support/pleora_node_map.cpp)
  target_include_directories(pleora_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(pleora_contract_tests PRIVATE gige_mel_pleora gige_mel_pleora_sdk
    gige_mel_build_options)
  add_test(NAME pleora_sdk_contract COMMAND pleora_contract_tests)
  set_tests_properties(pleora_sdk_contract PROPERTIES TIMEOUT 60)
  add_test(NAME pleora_diagnostic_help COMMAND gige_mel_pleora_diagnostic --help)
  add_test(NAME pleora_sdk_version COMMAND gige_mel_pleora_diagnostic --version)
  set_tests_properties(pleora_diagnostic_help pleora_sdk_version PROPERTIES TIMEOUT 30)
  set_tests_properties(pleora_sdk_contract pleora_diagnostic_help pleora_sdk_version
    PROPERTIES ENVIRONMENT "${_pleora_test_environment}")

  option(GIGE_MEL_PLEORA_HARDWARE_TESTS "Build/register Pleora native-stream and MEL tests for an explicitly selected physical camera" OFF)
  if(GIGE_MEL_PLEORA_HARDWARE_TESTS)
    set(GIGE_MEL_PLEORA_DEVICE_ID "" CACHE STRING "Exact mac:aa:bb:cc:dd:ee:ff Pleora discovery ID for physical qualification")
    if(GIGE_MEL_PLEORA_DEVICE_ID STREQUAL "")
      message(FATAL_ERROR "GIGE_MEL_PLEORA_HARDWARE_TESTS=ON requires -DGIGE_MEL_PLEORA_DEVICE_ID=<exact discovery ID>.")
    endif()
    add_executable(pleora_hardware_contract tests/pleora_hardware_contract.cpp
      tests/support/aravis_virtual_clock.cpp)
    target_include_directories(pleora_hardware_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
    target_link_libraries(pleora_hardware_contract PRIVATE gige_mel_pleora gige_mel_provider
      gige_mel_build_options Threads::Threads)
    add_test(NAME pleora_hardware_contract COMMAND pleora_hardware_contract ${GIGE_MEL_PLEORA_DEVICE_ID})
    set_tests_properties(pleora_hardware_contract PROPERTIES TIMEOUT 180 LABELS hardware
      RESOURCE_LOCK gige_physical_camera ENVIRONMENT "${_pleora_test_environment}")
  endif()
endif()
message(STATUS "Pleora eBUS ${GIGE_MEL_PLEORA_VERSION}: external internal-only adapter at ${GIGE_MEL_PLEORA_ROOT}")
