# Local SDK only: no download, installation, export, or SDK artifact packaging.
if(NOT GIGE_MEL_PLEORA_SDK_ROOT)
  message(FATAL_ERROR "GIGE_MEL_PLEORA_SDK_ROOT must identify an external licensed SDK")
endif()
file(REAL_PATH "${GIGE_MEL_PLEORA_SDK_ROOT}" pleora_root)
file(REAL_PATH "${PROJECT_SOURCE_DIR}" project_root)
cmake_path(IS_PREFIX project_root "${pleora_root}" NORMALIZE inside_project)
if(inside_project)
  message(FATAL_ERROR "The proprietary SDK must be outside the project working tree")
endif()
unset(pleora_include CACHE)
find_path(pleora_include PvDevice.h PATHS "${pleora_root}/include" NO_DEFAULT_PATH NO_CACHE REQUIRED)
file(READ "${pleora_include}/PvVersion.h" pleora_version_header)
foreach(part MAJOR MINOR SUB BUILD)
  string(REGEX MATCH "#define[ \t]+VERSION_${part}[ \t]+([0-9]+)" match "${pleora_version_header}")
  set(pleora_version_${part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT "${pleora_version_MAJOR}.${pleora_version_MINOR}.${pleora_version_SUB}.${pleora_version_BUILD}" STREQUAL "6.5.3.7155")
  message(FATAL_ERROR "Pleora adapter requires the tested eBUS 6.5.3.7155 headers")
endif()
set(GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR "${pleora_root}/genicam-headers/library/CPP/include"
  CACHE PATH "External matching GenApi 3.4.1 headers (never vendored)")
if(NOT EXISTS "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}/GenApi/IFloat.h")
  message(FATAL_ERROR "Native float-grid validation requires external GenApi headers; see docs/pleora-integration.md")
endif()
file(READ "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}/_GenICamVersion.h" genapi_version_header)
foreach(part MAJOR MINOR SUBMINOR)
  string(REGEX MATCH "#define[ \t]+GENICAM_VERSION_${part}[ \t]+([0-9]+)" match "${genapi_version_header}")
  set(genapi_version_${part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT "${genapi_version_MAJOR}.${genapi_version_MINOR}.${genapi_version_SUBMINOR}" STREQUAL "3.4.1")
  message(FATAL_ERROR "External GenApi headers must match the SDK's 3.4.1 runtime")
endif()
set(pleora_libraries)
foreach(component PvDevice PvStream PvBuffer PvGenICam PvSystem PvBase)
  unset(pleora_${component} CACHE)
  find_library(pleora_${component} NAMES ${component}
    PATHS "${pleora_root}/lib" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  list(APPEND pleora_libraries "${pleora_${component}}")
endforeach()
foreach(component GenApi GCBase)
  find_library(pleora_${component} NAMES ${component}_gcc48_v3_4
    PATHS "${pleora_root}/lib/genicam/bin/Linux64_x64" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  list(APPEND pleora_libraries "${pleora_${component}}")
endforeach()
add_library(gige_mel_pleora STATIC
  src/backends/pleora/pleora_backend.cpp
  src/backends/pleora/pleora_stream.cpp
  src/backends/pleora/pleora_features.cpp)
target_include_directories(gige_mel_pleora PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(gige_mel_pleora SYSTEM PRIVATE "${pleora_include}" "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}")
target_link_libraries(gige_mel_pleora PRIVATE gige_mel_camera_profiles
  gige_mel_build_options ${pleora_libraries} PUBLIC gige_mel_backend)
# RUNPATH on project executables does not resolve indirect SDK dependencies.
# Use the vendor's external library directories in the launching environment.
set(pleora_runtime "${pleora_root}/lib:${pleora_root}/lib/genicam/bin/Linux64_x64")
add_executable(gige_mel_pleora_diagnostic tools/pleora_diagnostic.cpp)
target_include_directories(gige_mel_pleora_diagnostic PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(gige_mel_pleora_diagnostic PRIVATE gige_mel_pleora gige_mel_build_options)
target_link_options(gige_mel_pleora_diagnostic PRIVATE
  "LINKER:-rpath-link,${pleora_root}/lib"
  "LINKER:-rpath-link,${pleora_root}/lib/genicam/bin/Linux64_x64")
if(BUILD_TESTING)
  add_executable(pleora_adapter_tests tests/pleora_adapter_tests.cpp)
  target_include_directories(pleora_adapter_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_include_directories(pleora_adapter_tests SYSTEM PRIVATE "${pleora_include}" "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}")
  target_link_libraries(pleora_adapter_tests PRIVATE gige_mel_pleora gige_mel_build_options ${pleora_libraries})
  target_link_options(pleora_adapter_tests PRIVATE
    "LINKER:-rpath-link,${pleora_root}/lib"
    "LINKER:-rpath-link,${pleora_root}/lib/genicam/bin/Linux64_x64")
  add_test(NAME pleora_adapter_contract COMMAND pleora_adapter_tests)
  set_tests_properties(pleora_adapter_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
  add_executable(pleora_contract_tests tests/pleora_contract_tests.cpp tests/support/pleora_node_map.cpp)
  target_include_directories(pleora_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/backends/pleora" "${PROJECT_SOURCE_DIR}/tests/support")
  target_include_directories(pleora_contract_tests SYSTEM PRIVATE "${pleora_include}" "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}")
  target_link_libraries(pleora_contract_tests PRIVATE gige_mel_pleora gige_mel_camera_profiles
    gige_mel_build_options ${pleora_libraries})
  # An exact vendor report is audited unsuppressed before one test-specific exception.
  # All other tests retain their ordinary TSan settings.
  if(GIGE_MEL_SANITIZERS MATCHES "(^|,)thread(,|$)")
    add_test(NAME pleora_sdk_contract COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/scripts/pleora_tsan_exception.py"
      --runtime-dir "${pleora_root}/lib/genicam/bin/Linux64_x64"
      --suppression-file "${PROJECT_SOURCE_DIR}/tests/support/pleora_genapi_tsan.supp"
      --contract "$<TARGET_FILE:pleora_contract_tests>")
  else()
    add_test(NAME pleora_sdk_contract COMMAND pleora_contract_tests)
  endif()
  add_test(NAME pleora_diagnostic_help COMMAND gige_mel_pleora_diagnostic --help)
  add_test(NAME pleora_sdk_version COMMAND gige_mel_pleora_diagnostic --version)
  add_executable(pleora_mel_contract tests/pleora_hardware_contract.cpp tests/support/aravis_virtual_clock.cpp)
  target_compile_definitions(pleora_mel_contract PRIVATE GIGE_MEL_PLEORA_CONTROLLED_MEL=1)
  target_include_directories(pleora_mel_contract PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/backends/pleora" "${PROJECT_SOURCE_DIR}/tests/support")
  target_include_directories(pleora_mel_contract SYSTEM PRIVATE "${pleora_include}" "${GIGE_MEL_PLEORA_GENICAM_INCLUDE_DIR}")
  target_link_libraries(pleora_mel_contract PRIVATE gige_mel_pleora gige_mel_provider gige_mel_build_options ${pleora_libraries})
  add_test(NAME pleora_mel_contract COMMAND pleora_mel_contract)
  # Always build the physical contract, but never register/run it implicitly.
  add_executable(pleora_hardware_contract tests/pleora_hardware_contract.cpp tests/support/aravis_virtual_clock.cpp)
  target_include_directories(pleora_hardware_contract PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/backends/pleora" "${PROJECT_SOURCE_DIR}/tests/support")
  target_link_libraries(pleora_hardware_contract PRIVATE gige_mel_pleora gige_mel_provider gige_mel_build_options)
  option(GIGE_MEL_PLEORA_HARDWARE_TESTS "Register the explicitly selected licensed physical camera contract" OFF)
  set(GIGE_MEL_PLEORA_DEVICE_ID "" CACHE STRING "Exact discovered MAC; no mac: prefix")
  if(GIGE_MEL_PLEORA_HARDWARE_TESTS)
    if(NOT GIGE_MEL_PLEORA_DEVICE_ID MATCHES "^[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]$")
      message(FATAL_ERROR "Physical contract requires an exact discovered lower-case MAC")
    endif()
    add_test(NAME pleora_hardware_contract COMMAND pleora_hardware_contract "${GIGE_MEL_PLEORA_DEVICE_ID}")
    set_tests_properties(pleora_hardware_contract PROPERTIES TIMEOUT 180 LABELS hardware RESOURCE_LOCK gige_physical_camera
      ENVIRONMENT "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
  endif()
  foreach(target pleora_contract_tests pleora_hardware_contract pleora_mel_contract)
    target_link_options(${target} PRIVATE "LINKER:-rpath-link,${pleora_root}/lib"
      "LINKER:-rpath-link,${pleora_root}/lib/genicam/bin/Linux64_x64")
  endforeach()
  set_tests_properties(pleora_sdk_contract pleora_diagnostic_help pleora_sdk_version pleora_mel_contract PROPERTIES TIMEOUT 60
    ENVIRONMENT "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
  if(GIGE_MEL_SANITIZERS MATCHES "(^|,)thread(,|$)")
    set_tests_properties(pleora_sdk_contract PROPERTIES TIMEOUT 150)
  endif()
endif()
