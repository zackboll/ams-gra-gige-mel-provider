cmake_minimum_required(VERSION 3.24)
project(gige_mel_consumer LANGUAGES CXX)

find_package(GigeMelProvider 0.1 CONFIG REQUIRED)
add_executable(capture_one main.cpp)
target_compile_features(capture_one PRIVATE cxx_std_20)
target_link_libraries(capture_one PRIVATE gige_mel::provider Threads::Threads)