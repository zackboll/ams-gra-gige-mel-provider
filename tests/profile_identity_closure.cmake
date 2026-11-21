# GIGE-MEL-047: the matched-C++ module identity must change when any byte of
# the optional navigation handler header changes, including a comment. Runs on
# a private copy of the closure; never edits the checkout or a generated ID.
# Usage: cmake -DSOURCE_ROOT=<checkout> -DWORK=<scratch dir> -P profile_identity_closure.cmake
cmake_minimum_required(VERSION 3.24)
include("${SOURCE_ROOT}/cmake/ProfileBuildIdentity.cmake")
set(headers include/gige_mel/backend.hpp include/gige_mel/navigation.hpp src/device_features.hpp
  src/profiles/frame_policy.hpp src/profiles/camera_profile.hpp src/profiles/profile_module.hpp
  src/profiles/navigation_handler.hpp)
file(REMOVE_RECURSE "${WORK}")
foreach(path ${headers} cmake/ProfileBuildIdentity.cmake cmake/profile_build_identity.hpp.in)
  get_filename_component(directory "${WORK}/tree/${path}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  file(COPY_FILE "${SOURCE_ROOT}/${path}" "${WORK}/tree/${path}")
endforeach()
# Extract the interface-closure revision hash from the generated identity.
function(identity output)
  gige_mel_profile_identity("${WORK}/tree" "${WORK}/${output}")
  file(READ "${WORK}/${output}/profile_build_identity.hpp" text)
  if(NOT text MATCHES "GIGE_MEL_PROFILE_BUILD_ID \"v1;([0-9a-f][0-9a-f]+);")
    message(FATAL_ERROR "unexpected generated identity form in ${output}")
  endif()
  set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
identity(original)
identity(repeat)
if(NOT original STREQUAL repeat)
  message(FATAL_ERROR "profile identity is not deterministic")
endif()
file(APPEND "${WORK}/tree/src/profiles/navigation_handler.hpp" "\n// comment-only change\n")
identity(changed)
if(original STREQUAL changed)
  message(FATAL_ERROR "navigation_handler.hpp is not part of the profile identity closure")
endif()
message(STATUS "Profile identity covers navigation_handler.hpp (comment-only change detected)")
