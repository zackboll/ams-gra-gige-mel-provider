# Included only when GIGE_MEL_BUILD_ARAVIS=ON. Never downloads anything: the
# qualified SDK is provisioned explicitly (scripts/provision_aravis.sh) and
# located through pkg-config (PKG_CONFIG_PATH).
set(GIGE_MEL_QUALIFIED_ARAVIS_VERSION 0.8.36)

find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
  message(FATAL_ERROR "GIGE_MEL_BUILD_ARAVIS=ON requires pkg-config to locate aravis-0.8 ${GIGE_MEL_QUALIFIED_ARAVIS_VERSION}. "
    "Install pkg-config, provision the SDK with scripts/provision_aravis.sh PREFIX and set PKG_CONFIG_PATH, "
    "or configure with -DGIGE_MEL_BUILD_ARAVIS=OFF.")
endif()
pkg_check_modules(GIGE_MEL_ARAVIS QUIET IMPORTED_TARGET aravis-0.8=${GIGE_MEL_QUALIFIED_ARAVIS_VERSION})
if(NOT GIGE_MEL_ARAVIS_FOUND)
  pkg_check_modules(GIGE_MEL_ARAVIS_ANY QUIET aravis-0.8)
  if(GIGE_MEL_ARAVIS_ANY_FOUND)
    set(_found "found aravis-0.8 ${GIGE_MEL_ARAVIS_ANY_VERSION}, which is not qualified")
  else()
    set(_found "aravis-0.8 (or one of its GLib/libxml2 dependencies) was not found by pkg-config")
  endif()
  message(FATAL_ERROR "GIGE_MEL_BUILD_ARAVIS=ON requires exactly Aravis ${GIGE_MEL_QUALIFIED_ARAVIS_VERSION}; ${_found}. "
    "Provision it explicitly with scripts/provision_aravis.sh PREFIX, then set "
    "PKG_CONFIG_PATH=PREFIX/lib/pkgconfig. See docs/aravis-integration.md.")
endif()
message(STATUS "Aravis ${GIGE_MEL_QUALIFIED_ARAVIS_VERSION} (internal camera-control and acquisition adapter): ${GIGE_MEL_ARAVIS_LINK_LIBRARIES}")

# Internal adapter. Not installed or exported; consumers of the installed
# package never see Aravis, GLib or this target.
add_library(gige_mel_aravis STATIC
  src/backends/aravis/aravis_backend.cpp
  src/backends/aravis/aravis_capture.cpp
  src/backends/aravis/aravis_time_probe.cpp
  src/backends/aravis/camera_profile.cpp)
target_include_directories(gige_mel_aravis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/aravis)
target_link_libraries(gige_mel_aravis PUBLIC gige_mel_backend PkgConfig::GIGE_MEL_ARAVIS
  PRIVATE gige_mel_build_options)

# Uninstalled diagnostic tool. GigE interface only; no Fake fallback.
add_executable(gige_mel_aravis_diagnostic tools/aravis_diagnostic.cpp)
target_link_libraries(gige_mel_aravis_diagnostic PRIVATE gige_mel_aravis gige_mel_build_options)

if(BUILD_TESTING)
  # Private test support: selects Aravis's in-process Fake interface.
  add_library(gige_mel_aravis_test_support STATIC tests/support/aravis_fake_support.cpp)
  target_include_directories(gige_mel_aravis_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(gige_mel_aravis_test_support PUBLIC gige_mel_aravis ${CMAKE_DL_LIBS} PRIVATE gige_mel_build_options)

  add_executable(aravis_adapter_tests tests/aravis_adapter_tests.cpp)
  target_link_libraries(aravis_adapter_tests PRIVATE gige_mel_aravis_test_support gige_mel_build_options Threads::Threads)
  add_test(NAME aravis_adapter COMMAND aravis_adapter_tests)

  add_executable(aravis_embedding_probe tests/aravis_embedding_probe.cpp tests/support/tsan_uninstrumented_sdk.cpp)
  target_link_libraries(aravis_embedding_probe PRIVATE gige_mel_provider gige_mel_aravis_test_support
    gige_mel_build_options Threads::Threads)
  add_test(NAME aravis_mel_embedding COMMAND aravis_embedding_probe)

  # GIGE-MEL-009 acquisition contract: real SDK Fake streams, pool, cancel,
  # lifetime, restart, failure cleanup and the shared direct-capture routine.
  add_executable(aravis_stream_contract_tests tests/aravis_stream_contract.cpp
    tests/support/tsan_uninstrumented_sdk.cpp)
  target_link_libraries(aravis_stream_contract_tests PRIVATE gige_mel_aravis_test_support gige_mel_build_options
    Threads::Threads)
  add_test(NAME aravis_stream_contract COMMAND aravis_stream_contract_tests)

  # GIGE-MEL-010: native timestamp observation, the shared time-probe routine
  # against the private Fake, and positive/negative MEL publication through a
  # TEST-ONLY virtual integration-start clock (tests/support only; never
  # linked into the adapter library, provider or diagnostic).
  add_executable(aravis_timestamp_contract_tests tests/aravis_timestamp_contract.cpp
    tests/support/aravis_virtual_clock.cpp tests/support/tsan_uninstrumented_sdk.cpp)
  target_include_directories(aravis_timestamp_contract_tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/include)
  target_link_libraries(aravis_timestamp_contract_tests PRIVATE gige_mel_provider gige_mel_aravis_test_support
    gige_mel_build_options Threads::Threads)
  add_test(NAME aravis_timestamp_contract COMMAND aravis_timestamp_contract_tests)

  # GIGE-MEL-011: the UNCHANGED production GigE backend (aravis::make_backend)
  # against the pinned SDK's in-process network camera simulator
  # ArvGvFakeCamera on 127.0.0.1: real GigE discovery, GVCP control and GVSP
  # over loopback UDP. The simulator fixture is test-only (tests/support) and
  # so is the virtual clock. Needs IPv4 loopback with UDP 3956 free; never
  # skipped. Its TSan suppressions are separate (tsan_gige_loopback_sdk.cpp).
  add_executable(aravis_gige_loopback_contract_tests tests/aravis_gige_loopback_contract.cpp
    tests/support/aravis_gv_loopback.cpp tests/support/aravis_virtual_clock.cpp
    tests/support/tsan_gige_loopback_sdk.cpp)
  target_include_directories(aravis_gige_loopback_contract_tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/include)
  target_link_libraries(aravis_gige_loopback_contract_tests PRIVATE gige_mel_provider gige_mel_aravis_test_support
    gige_mel_build_options Threads::Threads)
  add_test(NAME aravis_gige_loopback_contract COMMAND aravis_gige_loopback_contract_tests)
  # Uses the fixed GVCP port 3956 on loopback: never run concurrently with
  # another process that binds it (ctest -j).
  set_tests_properties(aravis_gige_loopback_contract PROPERTIES TIMEOUT 600 RESOURCE_LOCK gige_gvcp_loopback_3956)

  # --help must succeed without device I/O (no network discovery is started).
  add_test(NAME aravis_diagnostic_help COMMAND gige_mel_aravis_diagnostic --help)
  set_tests_properties(aravis_adapter aravis_mel_embedding aravis_stream_contract aravis_diagnostic_help
    PROPERTIES TIMEOUT 120)
  set_tests_properties(aravis_timestamp_contract PROPERTIES TIMEOUT 600)
endif()
