cmake_minimum_required(VERSION 3.20)
project(orca_core VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORCA_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORCA_WERROR "Treat compiler warnings as errors" OFF)
option(ORCA_BUILD_TESTS "Build the test suite" ON)

set(ORCA_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
if(ORCA_WERROR)
    list(APPEND ORCA_WARNINGS -Werror)
endif()

if(ORCA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(orca_core
    src/memory/bus.cpp
    src/memory/console_maps.cpp
    src/ppc/mmu.cpp
    src/ppc/cpu.cpp
)
target_include_directories(orca_core PUBLIC include)
target_compile_options(orca_core PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)

if(ORCA_BUILD_TESTS)
    enable_testing()
    foreach(suite memory cpu_integer cpu_system mmu reference)
        add_executable(orca_test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(orca_test_${suite} PRIVATE orca_core)
        target_include_directories(orca_test_${suite} PRIVATE tests)
        add_test(NAME ${suite} COMMAND orca_test_${suite})
    endforeach()

    find_program(ORCA_LLVM_MC NAMES llvm-mc llvm-mc-18 llvm-mc-17 llvm-mc-16)
    add_executable(orca_encoding_check tests/encoding_check.cpp)
    target_include_directories(orca_encoding_check PRIVATE tests)
    add_test(NAME encodings COMMAND orca_encoding_check "${ORCA_LLVM_MC}")
    set_tests_properties(encodings PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(orca_bench_interpreter tools/bench_interpreter.cpp)
target_link_libraries(orca_bench_interpreter PRIVATE orca_core)
