# GIGE-MEL-GENTL-001 feasibility build. Included only when
# GIGE_MEL_BUILD_GENTL_PROBE=ON. Never downloads anything, never installs,
# exports or links into gige_mel_provider, the selection catalog or factories.
# Requires the unmodified official EMVA GenTL 1.6 header, provisioned with
# scripts/provision_gentl_header.sh; its SHA-256 is verified here.
set(GIGE_MEL_GENTL_HEADER_SHA256 "5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e")
set(GIGE_MEL_GENTL_INCLUDE_DIR "" CACHE PATH "Directory containing the provisioned, unmodified EMVA GenTL.h (1.6)")

if(NOT GIGE_MEL_GENTL_INCLUDE_DIR OR NOT EXISTS "${GIGE_MEL_GENTL_INCLUDE_DIR}/GenTL.h")
  message(FATAL_ERROR "GIGE_MEL_BUILD_GENTL_PROBE=ON requires the official EMVA GenTL 1.6 header. "
    "Provision it with scripts/provision_gentl_header.sh PREFIX and configure with "
    "-DGIGE_MEL_GENTL_INCLUDE_DIR=PREFIX/include. No substitute declarations are used.")
endif()
file(SHA256 "${GIGE_MEL_GENTL_INCLUDE_DIR}/GenTL.h" _gige_mel_gentl_sha)
if(NOT _gige_mel_gentl_sha STREQUAL GIGE_MEL_GENTL_HEADER_SHA256)
  message(FATAL_ERROR "${GIGE_MEL_GENTL_INCLUDE_DIR}/GenTL.h has SHA-256 ${_gige_mel_gentl_sha}; "
    "expected the verified EMVA GenTL 1.6 header ${GIGE_MEL_GENTL_HEADER_SHA256}")
endif()
message(STATUS "GenTL feasibility probe: verified EMVA GenTL 1.6 header in ${GIGE_MEL_GENTL_INCLUDE_DIR}")

# The header is third-party and unmodifiable: include it as a system header so
# project warning flags (-Werror) do not apply to its contents.
add_library(gige_mel_gentl_header INTERFACE)
target_include_directories(gige_mel_gentl_header SYSTEM INTERFACE "${GIGE_MEL_GENTL_INCLUDE_DIR}")

add_library(gige_mel_gentl_consumer STATIC src/gentl/gentl_consumer.cpp)
target_include_directories(gige_mel_gentl_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gige_mel_gentl_consumer PUBLIC gige_mel_backend
  PRIVATE gige_mel_gentl_header gige_mel_build_options ${CMAKE_DL_LIBS})

# Uninstalled diagnostic tool; every hardware-facing step is a CLI request.
add_executable(gige_mel_gentl_probe tools/gentl_probe.cpp)
target_link_libraries(gige_mel_gentl_probe PRIVATE gige_mel_gentl_consumer gige_mel_build_options Threads::Threads)

if(BUILD_TESTING)
  target_compile_definitions(gige_mel_gentl_consumer PUBLIC GIGE_MEL_GENTL_TESTING=1)
  # Controlled fixture producers, built against the same verified header.
  function(gige_mel_gentl_mock target)
    add_library(${target} MODULE tests/support/gentl_mock_producer.cpp)
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".cti")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE gige_mel_gentl_header Threads::Threads)
    if(GIGE_MEL_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=${GIGE_MEL_SANITIZERS} -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=${GIGE_MEL_SANITIZERS})
    endif()
    target_compile_definitions(${target} PRIVATE ${ARGN})
  endfunction()
  gige_mel_gentl_mock(gentl_mock_producer)
  gige_mel_gentl_mock(gentl_mock_missing_symbol GENTL_MOCK_OMIT_TLOPEN=1)
  gige_mel_gentl_mock(gentl_mock_minimal GENTL_MOCK_MINIMAL=1)

  add_executable(gentl_consumer_tests tests/gentl_consumer_tests.cpp)
  target_link_libraries(gentl_consumer_tests PRIVATE gige_mel_gentl_consumer gige_mel_build_options Threads::Threads)
  add_dependencies(gentl_consumer_tests gentl_mock_producer gentl_mock_missing_symbol gentl_mock_minimal)
  target_compile_definitions(gentl_consumer_tests PRIVATE
    GENTL_MOCK="$<TARGET_FILE:gentl_mock_producer>"
    GENTL_MOCK_MISSING="$<TARGET_FILE:gentl_mock_missing_symbol>"
    GENTL_MOCK_MINIMAL_CTI="$<TARGET_FILE:gentl_mock_minimal>")
  add_test(NAME gentl_consumer_contract COMMAND gentl_consumer_tests)
  set_tests_properties(gentl_consumer_contract PROPERTIES TIMEOUT 60 LABELS gentl)
  foreach(path system interface device stream)
    foreach(allocation 0 1)
      foreach(cleanup success failure)
        set(name gentl_r1_allocation_${path}_${allocation}_${cleanup})
        add_test(NAME ${name} COMMAND gentl_consumer_tests allocation ${path} ${allocation} ${cleanup})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS gentl)
      endforeach()
    endforeach()
  endforeach()
  foreach(operation DSStopAcquisition DSFlushQueue DSRevokeBuffer GCUnregisterEvent DSClose)
    foreach(phase running partial announced)
      if(phase STREQUAL "announced" AND
          (operation STREQUAL "DSStopAcquisition" OR operation STREQUAL "GCUnregisterEvent"))
        continue()
      endif()
      foreach(close unresolved conclusive)
        if(operation STREQUAL "DSClose" AND close STREQUAL "conclusive")
          continue()
        endif()
        set(name gentl_r1_teardown_${operation}_${phase}_${close})
        add_test(NAME ${name} COMMAND gentl_consumer_tests teardown ${operation} ${phase} ${close})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS gentl)
      endforeach()
    endforeach()
  endforeach()

  # The real probe executable against the fixture: offline only.
  add_test(NAME gentl_probe_fixture
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gentl_probe_tests.py
      $<TARGET_FILE:gige_mel_gentl_probe> $<TARGET_FILE:gentl_mock_producer>
      $<TARGET_FILE:gentl_mock_missing_symbol>)
  set_tests_properties(gentl_probe_fixture PROPERTIES TIMEOUT 60 LABELS gentl)
endif()
