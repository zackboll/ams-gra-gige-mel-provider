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

  add_executable(provider_tests tests/provider_tests.cpp tests/join_observer.cpp tests/clock_observer.cpp)
  target_link_libraries(provider_tests PRIVATE gige_mel_provider gige_mel_backend gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(provider_tests PRIVATE -Wl,--export-dynamic-symbol=pthread_join -Wl,--export-dynamic-symbol=clock_gettime)
  add_test(NAME provider_contract COMMAND provider_tests)

  add_executable(configuration_probe tests/configuration_probe.cpp)
  target_link_libraries(configuration_probe PRIVATE gige_mel_provider gige_mel_build_options)
  add_test(NAME configuration_public_api COMMAND configuration_probe)

  add_executable(loader_tests tests/loader_tests.cpp)
  target_link_libraries(loader_tests PRIVATE gige_mel_headers gige_mel_build_options Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(loader_tests gige_mel_provider)
  add_test(NAME native_loader COMMAND loader_tests $<TARGET_FILE:gige_mel_provider>)
  add_test(NAME upstream_integrity COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/vendor_upstream.py --verify)
  add_test(NAME json_integrity COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_json.py)
  set_tests_properties(backend_contract provider_contract configuration_public_api native_loader PROPERTIES TIMEOUT 30)
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
