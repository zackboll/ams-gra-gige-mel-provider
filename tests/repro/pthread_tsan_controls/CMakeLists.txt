cmake_minimum_required(VERSION 3.24)
project(pthread_tsan_controls LANGUAGES CXX)

# GIGE-MEL-040-R2. Standalone and SDK-free: no provider, adapter or vendor targets.
# Valid and deliberately invalid lifecycles are SEPARATE executables. Neither is
# registered in the ordinary project CTest suite; see
# docs/pleora-native-tsan-investigation.md for the explicit commands.
set(REPRO_SANITIZERS "thread" CACHE STRING "Sanitizers; empty for an uninstrumented build")
find_package(Threads REQUIRED)
foreach(target pthread_valid_controls pthread_negative_controls)
  add_executable(${target} pthread_controls.cpp)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_options(${target} PRIVATE -g -O0 -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(REPRO_SANITIZERS)
    target_compile_options(${target} PRIVATE "-fsanitize=${REPRO_SANITIZERS}")
    target_link_options(${target} PRIVATE "-fsanitize=${REPRO_SANITIZERS}")
  endif()
endforeach()
target_compile_definitions(pthread_negative_controls PRIVATE PTHREAD_CONTROLS_NEGATIVE=1)
