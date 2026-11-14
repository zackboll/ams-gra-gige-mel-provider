cmake_minimum_required(VERSION 3.24)
project(ams_gra_gige_mel_provider VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "The bootstrap currently targets Linux. Other platforms need loader/export validation.")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

option(GIGE_MEL_BUILD_EXAMPLES "Build the native dynamically loaded capture example" ON)
option(GIGE_MEL_WARNINGS_AS_ERRORS "Treat warnings in project code as errors" ON)
option(GIGE_MEL_BUILD_ARAVIS "Build the optional, internal Aravis 0.8.36 camera-control adapter, its SDK tests and diagnostic tool" OFF)
option(GIGE_MEL_BUILD_PLEORA "Build the optional Pleora eBUS backend" OFF)
set(GIGE_MEL_PLEORA_SDK_ROOT "" CACHE PATH "External Pleora SDK installation")
set(GIGE_MEL_SANITIZERS "" CACHE STRING "Compiler sanitizers, e.g. address,undefined or thread")

add_library(gige_mel_build_options INTERFACE)
target_compile_features(gige_mel_build_options INTERFACE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gige_mel_build_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(GIGE_MEL_WARNINGS_AS_ERRORS)
    target_compile_options(gige_mel_build_options INTERFACE -Werror)
  endif()
  if(GIGE_MEL_SANITIZERS)
    target_compile_options(gige_mel_build_options INTERFACE -fsanitize=${GIGE_MEL_SANITIZERS} -fno-omit-frame-pointer)
    target_link_options(gige_mel_build_options INTERFACE -fsanitize=${GIGE_MEL_SANITIZERS})
  endif()
elseif(GIGE_MEL_SANITIZERS)
  message(FATAL_ERROR "Sanitizer options require GCC or Clang")
endif()

# Never fetch dependencies at configure/build time. The manifest is reviewed
# together with the code and covers the unmodified, minimal header closure.
add_custom_target(verify_upstream ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/vendor_upstream.py --verify
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_json.py
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Verify pinned MEL dependency bytes")

add_library(gige_mel_headers INTERFACE)
add_library(gige_mel::mel_headers ALIAS gige_mel_headers)
set_target_properties(gige_mel_headers PROPERTIES EXPORT_NAME mel_headers)
target_compile_features(gige_mel_headers INTERFACE cxx_std_20)
target_include_directories(gige_mel_headers SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/ir-mel/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/common-mel/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/ams-math/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(gige_mel_backend STATIC src/backends/synthetic.cpp)
add_library(gige_mel::backend ALIAS gige_mel_backend)
set_target_properties(gige_mel_backend PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME backend)
target_compile_features(gige_mel_backend PUBLIC cxx_std_20)
target_include_directories(gige_mel_backend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gige_mel_backend PUBLIC Threads::Threads PRIVATE $<BUILD_INTERFACE:gige_mel_build_options>)

# Internal camera-profile policy, compiled and tested without any camera SDK.
# No MEL declarations, SDK handles, installed headers or plugin ABI are added.
add_library(gige_mel_camera_profiles STATIC
  src/profiles/mono_free_running.cpp
  src/profiles/mono_free_running_device.cpp)
set_target_properties(gige_mel_camera_profiles PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(gige_mel_camera_profiles PUBLIC cxx_std_20)
target_include_directories(gige_mel_camera_profiles PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gige_mel_camera_profiles PRIVATE gige_mel_build_options)

add_library(gige_mel_provider SHARED src/provider.cpp src/factories.cpp src/config_parser.cpp)
target_include_directories(gige_mel_provider SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/include)
add_library(gige_mel::provider ALIAS gige_mel_provider)
set_target_properties(gige_mel_provider PROPERTIES
  OUTPUT_NAME gige_mel_provider VERSION ${PROJECT_VERSION} SOVERSION 0 EXPORT_NAME provider)
target_compile_features(gige_mel_provider PUBLIC cxx_std_20)
target_include_directories(gige_mel_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gige_mel_provider PUBLIC gige_mel_headers
  PRIVATE gige_mel_backend Threads::Threads gige_mel_build_options)
# Private provider software version (not the MEL API version or a dependency
# revision); reported as text and as the Health subsystem CSCI Version.
target_compile_definitions(gige_mel_provider PRIVATE
  GIGE_MEL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  GIGE_MEL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  GIGE_MEL_VERSION_PATCH=${PROJECT_VERSION_PATCH})
add_dependencies(gige_mel_provider verify_upstream)

if(GIGE_MEL_BUILD_EXAMPLES)
  add_executable(mel_capture examples/capture.cpp)
  target_link_libraries(mel_capture PRIVATE gige_mel_headers gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(mel_capture verify_upstream)
endif()

if(BUILD_TESTING)
  add_executable(backend_tests tests/backend_tests.cpp)
  target_link_libraries(backend_tests PRIVATE gige_mel_backend gige_mel_build_options)
  add_test(NAME backend_contract COMMAND backend_tests)

  add_executable(camera_profile_tests tests/camera_profile_tests.cpp)
  target_link_libraries(camera_profile_tests PRIVATE gige_mel_camera_profiles gige_mel_build_options)
  add_test(NAME camera_profile_contract COMMAND camera_profile_tests)

  add_executable(device_feature_tests tests/device_feature_tests.cpp)
  target_link_libraries(device_feature_tests PRIVATE gige_mel_camera_profiles gige_mel_build_options)
  add_test(NAME device_feature_contract COMMAND device_feature_tests)

  add_executable(provider_tests tests/provider_tests.cpp tests/join_observer.cpp tests/clock_observer.cpp)
  target_link_libraries(provider_tests PRIVATE gige_mel_provider gige_mel_backend gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(provider_tests PRIVATE -Wl,--export-dynamic-symbol=pthread_join -Wl,--export-dynamic-symbol=pthread_detach -Wl,--export-dynamic-symbol=clock_gettime)
  add_test(NAME provider_contract COMMAND provider_tests)

  # Backend portability: the same MEL-facing scenarios against the synthetic
  # backend and an independent test-only fixture backend. The fixture source is
  # compiled only into this test executable and is never installed.
  add_executable(provider_backend_contract_tests tests/provider_backend_contract_tests.cpp
    tests/support/contract_fixture_camera.cpp tests/join_observer.cpp)
  target_link_libraries(provider_backend_contract_tests PRIVATE gige_mel_provider gige_mel_backend
    gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(provider_backend_contract_tests PRIVATE -Wl,--export-dynamic-symbol=pthread_join)
  add_test(NAME provider_backend_portability COMMAND provider_backend_contract_tests)

  add_executable(configuration_probe tests/configuration_probe.cpp)
  target_link_libraries(configuration_probe PRIVATE gige_mel_provider gige_mel_build_options)
  add_test(NAME configuration_public_api COMMAND configuration_probe)

  add_executable(loader_tests tests/loader_tests.cpp)
  target_link_libraries(loader_tests PRIVATE gige_mel_headers gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(loader_tests gige_mel_provider)
  add_test(NAME native_loader COMMAND loader_tests $<TARGET_FILE:gige_mel_provider>)
  add_test(NAME upstream_integrity COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/vendor_upstream.py --verify)
  add_test(NAME json_integrity COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_json.py)
  # Fixture-based regressions for the package-isolation verifier (no SDK needed).
  add_test(NAME package_isolation_verifier
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_package_isolation_tests.py)
  set_tests_properties(package_isolation_verifier PROPERTIES TIMEOUT 120)
  add_test(NAME proprietary_guard_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/proprietary_guard_tests.py)
  set_tests_properties(proprietary_guard_contract PROPERTIES TIMEOUT 30)
  add_test(NAME pleora_tooling_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pleora_tooling_tests.py)
  set_tests_properties(pleora_tooling_contract PROPERTIES TIMEOUT 30)
  add_test(NAME pleora_tsan_exception_guard
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pleora_tsan_exception_tests.py)
  set_tests_properties(pleora_tsan_exception_guard PROPERTIES TIMEOUT 30)
  add_test(NAME aravis_evidence_offline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aravis_evidence_tests.py)
  set_tests_properties(aravis_evidence_offline PROPERTIES TIMEOUT 60)
  set_tests_properties(backend_contract camera_profile_contract device_feature_contract provider_contract provider_backend_portability configuration_public_api
    native_loader PROPERTIES TIMEOUT 30)
endif()

# Optional Aravis camera-control foundation (GIGE-MEL-008). OFF by default:
# when OFF nothing below searches for Aravis, pkg-config or GLib. Everything
# here is internal: never installed, exported or selectable by getAPI_Manager().
if(GIGE_MEL_BUILD_ARAVIS)
  include(cmake/GigeMelAravis.cmake)
endif()
if(GIGE_MEL_BUILD_PLEORA)
  include(cmake/GigeMelPleora.cmake)
endif()

# Internal selection is never linked into or installed with the MEL provider.
add_library(gige_mel_selection STATIC
  src/selection/backend_catalog.cpp src/selection/session_selection.cpp)
target_include_directories(gige_mel_selection PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gige_mel_selection PUBLIC gige_mel_backend gige_mel_headers
  PRIVATE gige_mel_build_options)
add_library(gige_mel_selection_builtins STATIC src/selection/builtin_backends.cpp)
target_link_libraries(gige_mel_selection_builtins PUBLIC gige_mel_selection PRIVATE gige_mel_build_options)
if(GIGE_MEL_BUILD_ARAVIS)
  target_compile_definitions(gige_mel_selection_builtins PRIVATE GIGE_MEL_SELECT_ARAVIS=1)
  target_link_libraries(gige_mel_selection_builtins PRIVATE gige_mel_aravis)
endif()
if(GIGE_MEL_BUILD_PLEORA)
  target_compile_definitions(gige_mel_selection_builtins PRIVATE GIGE_MEL_SELECT_PLEORA=1)
  target_link_libraries(gige_mel_selection_builtins PRIVATE gige_mel_pleora)
endif()
if(GIGE_MEL_BUILD_EXAMPLES)
  add_executable(gige_mel_select examples/select_session.cpp)
  target_link_libraries(gige_mel_select PRIVATE gige_mel_selection_builtins gige_mel_provider gige_mel_build_options)
endif()
if(BUILD_TESTING)
  add_executable(selection_tests tests/selection_tests.cpp)
  target_link_libraries(selection_tests PRIVATE gige_mel_selection gige_mel_provider gige_mel_build_options)
  add_test(NAME selection_contract COMMAND selection_tests)
  add_executable(builtin_selection_tests tests/builtin_selection_tests.cpp)
  target_compile_definitions(builtin_selection_tests PRIVATE
    GIGE_MEL_EXPECT_ARAVIS=$<BOOL:${GIGE_MEL_BUILD_ARAVIS}>
    GIGE_MEL_EXPECT_PLEORA=$<BOOL:${GIGE_MEL_BUILD_PLEORA}>)
  target_link_libraries(builtin_selection_tests PRIVATE gige_mel_selection_builtins gige_mel_build_options)
  add_test(NAME builtin_selection_contract COMMAND builtin_selection_tests)
  set_tests_properties(selection_contract builtin_selection_contract PROPERTIES TIMEOUT 30)
  if(GIGE_MEL_BUILD_EXAMPLES)
    add_test(NAME selection_example_list COMMAND gige_mel_select --list)
    add_test(NAME selection_example_bind COMMAND gige_mel_select synthetic synthetic-0 mono_free_running_v1)
    add_test(NAME selection_example_inspect COMMAND gige_mel_select synthetic synthetic-0 mono_free_running_v1 --inspect)
  endif()
  if(GIGE_MEL_BUILD_ARAVIS)
    target_link_libraries(aravis_adapter_tests PRIVATE gige_mel_selection)
  endif()
  if(GIGE_MEL_BUILD_PLEORA)
    target_link_libraries(pleora_contract_tests PRIVATE gige_mel_selection)
  endif()
endif()
if(GIGE_MEL_BUILD_PLEORA)
  foreach(target gige_mel_select builtin_selection_tests)
    if(TARGET ${target})
      target_link_options(${target} PRIVATE "LINKER:-rpath-link,${pleora_root}/lib"
        "LINKER:-rpath-link,${pleora_root}/lib/genicam/bin/Linux64_x64")
    endif()
  endforeach()
  if(BUILD_TESTING)
    set_tests_properties(builtin_selection_contract PROPERTIES ENVIRONMENT
      "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
    if(GIGE_MEL_BUILD_EXAMPLES)
      set_tests_properties(selection_example_list selection_example_bind selection_example_inspect PROPERTIES ENVIRONMENT
        "LD_LIBRARY_PATH=${pleora_runtime};GENICAM_ROOT_V3_4=${pleora_root}/lib/genicam;GENICAM_CACHE_V3_4=${CMAKE_CURRENT_BINARY_DIR}/genicam-cache")
    endif()
  endif()
endif()

configure_package_config_file(cmake/GigeMelProviderConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/GigeMelProviderConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GigeMelProvider)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/GigeMelProviderConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(TARGETS gige_mel_provider gige_mel_backend gige_mel_headers EXPORT GigeMelProviderTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ third_party/ir-mel/include/ third_party/common-mel/include/ third_party/ams-math/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT GigeMelProviderTargets NAMESPACE gige_mel:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GigeMelProvider)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GigeMelProviderConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/GigeMelProviderConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GigeMelProvider)
foreach(component ir-mel common-mel ams-math)
  install(FILES third_party/${component}/LICENSE third_party/${component}/INTENT.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider/third_party/${component})
endforeach()
install(FILES third_party/manifest.json third_party/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider/third_party)
install(FILES third_party/nlohmann-json/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider/third_party/nlohmann-json)
install(FILES third_party/json-manifest.json DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider/third_party)
install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ams-gra-gige-mel-provider/docs)
