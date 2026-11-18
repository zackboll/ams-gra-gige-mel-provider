# GIGE-MEL-GENTL-002 internal GenTL camera backend. Included only when
# GIGE_MEL_BUILD_GENTL_BACKEND=ON. Never downloads anything, never installs or
# exports, and never links gige_mel_provider, the selection catalog or the
# public MEL factories. Requires the verified EMVA GenTL 1.6 header (through
# the GENTL-001 consumer) and exactly Aravis 0.8.36 for GenApi only.
if(NOT GIGE_MEL_BUILD_GENTL_PROBE)
  message(FATAL_ERROR "GIGE_MEL_BUILD_GENTL_BACKEND=ON requires GIGE_MEL_BUILD_GENTL_PROBE=ON "
    "(the verified GenTL consumer foundation and fixture producer).")
endif()
set(GIGE_MEL_GENTL_ARAVIS_VERSION 0.8.36)
find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
  message(FATAL_ERROR "GIGE_MEL_BUILD_GENTL_BACKEND=ON requires pkg-config to locate aravis-0.8 "
    "${GIGE_MEL_GENTL_ARAVIS_VERSION}; provision it with scripts/provision_aravis.sh PREFIX.")
endif()
if(NOT TARGET PkgConfig::GIGE_MEL_ARAVIS)
  pkg_check_modules(GIGE_MEL_ARAVIS QUIET IMPORTED_TARGET aravis-0.8=${GIGE_MEL_GENTL_ARAVIS_VERSION})
endif()
if(NOT TARGET PkgConfig::GIGE_MEL_ARAVIS)
  message(FATAL_ERROR "GIGE_MEL_BUILD_GENTL_BACKEND=ON requires exactly Aravis ${GIGE_MEL_GENTL_ARAVIS_VERSION} "
    "(GenApi only; no Aravis GenTL consumer, 0.9.x or 0.10). Provision it explicitly with "
    "scripts/provision_aravis.sh PREFIX and set PKG_CONFIG_PATH=PREFIX/lib/pkgconfig.")
endif()
# libxml2 is Aravis's own pinned parser dependency (Requires.private); used
# directly only for an independent well-formedness check (see design).
pkg_check_modules(GIGE_MEL_GENTL_LIBXML2 REQUIRED QUIET IMPORTED_TARGET libxml-2.0)
message(STATUS "GenTL backend: Aravis ${GIGE_MEL_GENTL_ARAVIS_VERSION} GenApi over the GenTL consumer (internal, uninstalled)")

add_library(gige_mel_gentl_backend STATIC
  src/backends/gentl/gentl_genapi.cpp
  src/backends/gentl/gentl_backend.cpp)
target_include_directories(gige_mel_gentl_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/gentl)
target_link_libraries(gige_mel_gentl_backend
  PUBLIC gige_mel_gentl_consumer gige_mel_camera_profiles gige_mel_backend
  PRIVATE PkgConfig::GIGE_MEL_ARAVIS PkgConfig::GIGE_MEL_GENTL_LIBXML2 gige_mel_build_options Threads::Threads)

if(BUILD_TESTING)
  add_executable(gentl_backend_tests tests/gentl_backend_tests.cpp)
  target_link_libraries(gentl_backend_tests PRIVATE gige_mel_gentl_backend gige_mel_selection PkgConfig::GIGE_MEL_ARAVIS
    gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(gentl_backend_tests gentl_mock_producer gentl_mock_minimal)
  target_compile_definitions(gentl_backend_tests PRIVATE
    GENTL_MOCK="$<TARGET_FILE:gentl_mock_producer>"
    GENTL_MOCK_MINIMAL_CTI="$<TARGET_FILE:gentl_mock_minimal>")
  add_test(NAME gentl_backend_contract COMMAND gentl_backend_tests)
  set_tests_properties(gentl_backend_contract PROPERTIES TIMEOUT 120 LABELS gentl)
  # Each failure-injection case runs in its own process: a producer that is
  # tainted by unresolved native ownership is retained for the process lifetime.
  # The R1 cases cover enumeration cleanup, the persistent ownership
  # registry and allocation failure.
  foreach(case xml_read_failure genapi_creation_failure profile_admission_failure
          devclose_failure ifclose_failure tlclose_failure gcclose_failure
          enum_ifclose_failure enum_tlclose_failure enum_discovery_ifclose_failure
          enum_discovery_tlclose_failure enum_system_discovery_tlclose_failure
          destructor_close_failure_after_backend partial_open_cleanup_failure
          consumer_open_cleanup_failure allocation_cleanup_failure)
    add_test(NAME gentl_backend_failure_${case} COMMAND gentl_backend_tests ${case})
    set_tests_properties(gentl_backend_failure_${case} PROPERTIES TIMEOUT 60 LABELS gentl)
  endforeach()

  if(GIGE_MEL_BUILD_EXAMPLES)
    add_executable(gentl_backend_example examples/gentl_backend_fixture.cpp)
    target_link_libraries(gentl_backend_example PRIVATE gige_mel_gentl_backend gige_mel_build_options)
    add_test(NAME gentl_backend_example_fixture
      COMMAND gentl_backend_example $<TARGET_FILE:gentl_mock_producer>)
    # The default fixture XML lacks the profile's features: the unchanged
    # profile must refuse it (exit 3), never fall back to fabricated features.
    add_test(NAME gentl_backend_example_profile_refusal
      COMMAND gentl_backend_example $<TARGET_FILE:gentl_mock_producer> "gentl1:9:mock-if-0mock-dev-A")
    set_tests_properties(gentl_backend_example_profile_refusal PROPERTIES TIMEOUT 60 LABELS gentl
      PASS_REGULAR_EXPRESSION "open refused by profile/backend \\(no fallback\\): .*SensorWidth")
    set_tests_properties(gentl_backend_example_fixture PROPERTIES TIMEOUT 60 LABELS gentl
      PASS_REGULAR_EXPRESSION "device gentl1:9:mock-if-0mock-dev-A")
  endif()
endif()
