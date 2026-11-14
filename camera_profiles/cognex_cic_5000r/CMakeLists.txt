cmake_minimum_required(VERSION 3.24)
project(cognex_cic_5000r_profile LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
set(GIGE_MEL_SOURCE_ROOT "" CACHE PATH "Matching provider source/header revision")
set(GIGE_MEL_SANITIZERS "" CACHE STRING "Match host sanitizer configuration")
if(NOT EXISTS "${GIGE_MEL_SOURCE_ROOT}/src/profiles/profile_module.hpp")
  message(FATAL_ERROR "Set GIGE_MEL_SOURCE_ROOT to the matching provider checkout")
endif()
include("${GIGE_MEL_SOURCE_ROOT}/cmake/ProfileBuildIdentity.cmake")
gige_mel_profile_identity("${GIGE_MEL_SOURCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_library(cognex_cic_5000r MODULE module.cpp profile.cpp
  "${GIGE_MEL_SOURCE_ROOT}/src/profiles/frame_policy.cpp"
  "${GIGE_MEL_SOURCE_ROOT}/src/profiles/mono_free_running.cpp"
  "${GIGE_MEL_SOURCE_ROOT}/src/profiles/mono_free_running_device.cpp")
target_compile_features(cognex_cic_5000r PRIVATE cxx_std_20)
target_include_directories(cognex_cic_5000r PRIVATE "${GIGE_MEL_SOURCE_ROOT}/include"
  "${GIGE_MEL_SOURCE_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(cognex_cic_5000r PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -fvisibility=hidden)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(cognex_cic_5000r PRIVATE -fno-gnu-unique)
endif()
if(GIGE_MEL_SANITIZERS)
  target_compile_options(cognex_cic_5000r PRIVATE -fsanitize=${GIGE_MEL_SANITIZERS} -fno-omit-frame-pointer)
  target_link_options(cognex_cic_5000r PRIVATE -fsanitize=${GIGE_MEL_SANITIZERS})
endif()
target_link_options(cognex_cic_5000r PRIVATE "LINKER:-z,defs"
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")