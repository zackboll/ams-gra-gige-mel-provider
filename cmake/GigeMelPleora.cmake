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
set(pleora_libraries)
foreach(component PvDevice PvStream PvBuffer PvGenICam PvSystem PvBase)
  unset(pleora_${component} CACHE)
  find_library(pleora_${component} NAMES ${component}
    PATHS "${pleora_root}/lib" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  list(APPEND pleora_libraries "${pleora_${component}}")
endforeach()
add_library(gige_mel_pleora STATIC
  src/backends/pleora/pleora_backend.cpp
  src/backends/pleora/pleora_stream.cpp
  src/backends/pleora/pleora_features.cpp)
target_include_directories(gige_mel_pleora PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(gige_mel_pleora SYSTEM PRIVATE "${pleora_include}")
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
  target_include_directories(pleora_adapter_tests SYSTEM PRIVATE "${pleora_include}")
  target_link_libraries(pleora_adapter_tests PRIVATE gige_mel_pleora gige_mel_build_options ${pleora_libraries})
  target_link_options(pleora_adapter_tests PRIVATE
    "LINKER:-rpath-link,${pleora_root}/lib"
    "LINKER:-rpath-link,${pleora_root}/lib/genicam/bin/Linux64_x64")
  add_test(NAME pleora_adapter_contract COMMAND pleora_adapter_tests)
  set_tests_properties(pleora_adapter_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
endif()
