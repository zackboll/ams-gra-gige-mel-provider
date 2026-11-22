# Shared by the host and the independently configured module example. Hash the
# complete C++ interface closure; no MEL or SDK headers enter this contract.
function(gige_mel_profile_identity root output)
  set(closure "")
  # GIGE-MEL-047 adds the optional navigation handler: module states may
  # implement it and the host loader cross-casts to it, so it is part of the
  # matched interface. Any byte change (including comments) needs a rebuild.
  # The default state base and the native snapshot it owns also cross the
  # source-extension boundary as inheritance/layout dependencies.
  foreach(header include/gige_mel/backend.hpp include/gige_mel/navigation.hpp
      src/device_features.hpp src/profiles/frame_policy.hpp src/profiles/camera_profile.hpp
      src/profiles/mono_free_running_profile.hpp src/profiles/mono_free_running.hpp
      src/profiles/profile_module.hpp src/profiles/navigation_handler.hpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${header}")
    file(SHA256 "${root}/${header}" digest)
    string(APPEND closure "${digest}")
  endforeach()
  string(SHA256 revision "${closure}")
  string(TOUPPER "${CMAKE_BUILD_TYPE}" configuration)
  string(SHA256 flags "${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${configuration}};${CMAKE_BUILD_TYPE}")
  file(MAKE_DIRECTORY "${output}")
  configure_file("${root}/cmake/profile_build_identity.hpp.in"
    "${output}/profile_build_identity.hpp" @ONLY)
endfunction()