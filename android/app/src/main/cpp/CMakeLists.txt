# Orca Core Android app: the core library (no tests) plus the JNI bridge.
cmake_minimum_required(VERSION 3.20)
project(orca_android LANGUAGES C CXX)

set(ORCA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ORCA_GLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. orca_core_build)

add_library(orca_android SHARED orca_jni.cpp)
target_link_libraries(orca_android PRIVATE orca_core android log aaudio)
target_compile_options(orca_android PRIVATE -Wall -Wextra)
