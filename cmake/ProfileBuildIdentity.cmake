# Shared by the host and the independently configured module example. Hash the
# complete C++ interface closure; no MEL or SDK headers enter this contract.
function(gige_mel_profile_identity root output)
  set(closure "")
  # Module states may implement the optional navigation and Image FOV companions
  # that the host loader cross-casts to. Any byte change (including comments)
  # in these interfaces or their value/layout dependencies needs a rebuild.
  # The default state base and the native snapshot it owns also cross the
  # source-extension boundary as inheritance/layout dependencies.
  foreach(header include/gige_mel/backend.hpp include/gige_mel/navigation.hpp
      include/gige_mel/image_field_of_view.hpp src/profiles/image_field_of_view.hpp
      src/device_features.hpp src/profiles/frame_policy.hpp src/profiles/camera_profile.hpp
      src/profiles/mono_free_running_profile.hpp src/profiles/mono_free_running.hpp src/profiles/mono_free_running_device.hpp
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