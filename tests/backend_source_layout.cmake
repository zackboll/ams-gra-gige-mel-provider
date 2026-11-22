# This source-only check needs neither SDK headers nor a configured SDK target.
set(synthetic "src/backends/synthetic/synthetic_backend.cpp")
if(NOT EXISTS "${SOURCE_ROOT}/${synthetic}" OR EXISTS "${SOURCE_ROOT}/src/backends/synthetic.cpp")
  message(FATAL_ERROR "Synthetic must live only at ${synthetic}")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" build)
if(NOT build MATCHES "add_library\\([ \t\r\n]*gige_mel_backend[ \t\r\n]+STATIC[ \t\r\n]+\"?${synthetic}\"?([ \t\r\n]|\\))"
   OR build MATCHES "src/backends/synthetic\\.cpp")
  message(FATAL_ERROR "Register ${synthetic} on the SDK-free gige_mel_backend target")
endif()