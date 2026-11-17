cmake_minimum_required(VERSION 3.20)
project(orca_core VERSION 0.25.0 LANGUAGES C CXX)

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
    src/core/scheduler.cpp
    src/crypto/aes128.cpp
    src/crypto/sha1.cpp
    src/disc/disc_image.cpp
    src/disc/wii_partition.cpp
    src/disc/wii_partition_reader.cpp
    src/disc/wii_ticket.cpp
    src/disc/fst.cpp
    src/dsp/dsp_core.cpp
    src/hw/audio_interface.cpp
    src/hw/disc_interface.cpp
    src/hw/dsp_interface.cpp
    src/hw/memory_interface.cpp
    src/hw/exi_ipl.cpp
    src/hw/exi_memcard.cpp
    src/gpu/command_stream.cpp
    src/gpu/xf_memory.cpp
    src/gpu/cp_state.cpp
    src/hw/pixel_engine.cpp
    src/hw/command_processor.cpp
    src/gpu/gpu.cpp
    src/gpu/texture_decoder.cpp
    src/gpu/sw/vertex.cpp
    src/gpu/sw/renderer.cpp
    src/gpu/sw/efb_copy.cpp
    src/video/xfb.cpp
    src/hw/external_interface.cpp
    src/hw/serial_interface.cpp
    src/hw/wii_remote.cpp
    src/hw/video_interface.cpp
    src/hw/mmio_splitter.cpp
    src/hw/processor_interface.cpp
    src/loader/executable.cpp
    src/system/system.cpp
    src/memory/bus.cpp
    src/memory/console_maps.cpp
    src/ppc/mmu.cpp
    src/ppc/cpu.cpp
    src/ppc/cpu_fpu.cpp
    src/ppc/fpu.cpp
    src/ppc/jit/arm64.cpp
)
# The FP engine changes the host rounding mode at run time.
set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math")
target_include_directories(orca_core PUBLIC include)
target_compile_options(orca_core PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)

# --- OpenGL ES 3.2 backend (optional: needs EGL and GLESv2/GLESv3 libraries) ------------
option(ORCA_GLES "Build the OpenGL ES 3.2 backend" ON)
if(ORCA_GLES)
    find_library(ORCA_EGL_LIB NAMES EGL libEGL.so.1)
    find_library(ORCA_GLES_LIB NAMES GLESv3 GLESv2 libGLESv2.so.2)
    if(ORCA_EGL_LIB AND ORCA_GLES_LIB)
        add_library(orca_gles src/gpu/gles/backend.cpp src/gpu/gles/shaders.cpp)
        target_include_directories(orca_gles PUBLIC include)
        # Khronos headers (third_party/khronos, unmodified) as system headers: their own
        # code is not held to Orca's warning flags
        target_include_directories(orca_gles SYSTEM PRIVATE third_party/khronos)
        target_compile_definitions(orca_gles PRIVATE EGL_NO_X11)
        target_link_libraries(orca_gles PUBLIC orca_core PRIVATE ${ORCA_EGL_LIB} ${ORCA_GLES_LIB})
        target_compile_options(orca_gles PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
        message(STATUS "OpenGL ES backend: ${ORCA_EGL_LIB} ${ORCA_GLES_LIB}")
    else()
        message(STATUS "OpenGL ES backend: EGL/GLES libraries not found, not built")
        set(ORCA_GLES OFF)
    endif()
endif()

# Android JNI entry point. The Kotlin GLSurfaceView owns the EGL window/context;
# this library probes the real device and initializes the GLES backend on that thread.
if(ANDROID)
    if(NOT ORCA_GLES)
        message(FATAL_ERROR "Android build requires ORCA_GLES=ON and EGL/GLESv3 from the NDK")
    endif()
    add_library(orca_android SHARED android/app/src/main/cpp/orca_jni.cpp)
    target_include_directories(orca_android PRIVATE include)
    target_link_libraries(orca_android PRIVATE orca_gles log android)
    target_compile_options(orca_android PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
endif()

if(ORCA_BUILD_TESTS)
    enable_testing()
    add_executable(orca_test_sha1 tests/test_sha1.cpp)
    target_link_libraries(orca_test_sha1 PRIVATE orca_core)
    add_test(NAME sha1 COMMAND orca_test_sha1)

    add_executable(orca_test_aes128 tests/test_aes128.cpp)
    target_link_libraries(orca_test_aes128 PRIVATE orca_core)
    add_test(NAME aes128 COMMAND orca_test_aes128)

    add_executable(orca_test_jit_arm64 tests/test_jit_arm64.cpp)
    target_link_libraries(orca_test_jit_arm64 PRIVATE orca_core)
    target_include_directories(orca_test_jit_arm64 PRIVATE tests)
    add_test(NAME jit_arm64 COMMAND orca_test_jit_arm64)

    foreach(suite memory cpu_integer cpu_system mmu reference fpu_engine cpu_fpu system)
        add_executable(orca_test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(orca_test_${suite} PRIVATE orca_core)
        target_include_directories(orca_test_${suite} PRIVATE tests)
        add_test(NAME ${suite} COMMAND orca_test_${suite})
    endforeach()

    # --- Whole-program tests -------------------------------------------------
    # Host build of the portable programs (same C source, native compiler).
    add_library(orca_host_programs STATIC tests/programs/integer_suite.c tests/programs/float_suite.c)
    target_compile_definitions(orca_host_programs PRIVATE ORCA_HOST)
    set_source_files_properties(tests/programs/integer_suite.c PROPERTIES COMPILE_DEFINITIONS PROGRAM=integer_suite_main)
    set_source_files_properties(tests/programs/float_suite.c PROPERTIES COMPILE_DEFINITIONS PROGRAM=float_suite_main)
    target_compile_options(orca_host_programs PRIVATE -O2 -ffp-contract=off -fno-fast-math)
    add_executable(orca_test_programs tests/test_programs.cpp)
    target_link_libraries(orca_test_programs PRIVATE orca_core orca_host_programs)
    target_include_directories(orca_test_programs PRIVATE tests)
    add_test(NAME programs COMMAND orca_test_programs)
    add_executable(orca_test_av tests/test_av.cpp)
    target_link_libraries(orca_test_av PRIVATE orca_core)
    target_include_directories(orca_test_av PRIVATE tests)
    add_test(NAME av COMMAND orca_test_av)
    add_executable(orca_test_disc tests/test_disc.cpp)
    target_link_libraries(orca_test_disc PRIVATE orca_core orca_host_programs)
    target_include_directories(orca_test_disc PRIVATE tests)
    add_test(NAME disc COMMAND orca_test_disc WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    add_executable(orca_test_exi tests/test_exi.cpp)
    target_link_libraries(orca_test_exi PRIVATE orca_core)
    target_include_directories(orca_test_exi PRIVATE tests)
    add_test(NAME exi COMMAND orca_test_exi)
    add_executable(orca_test_gpu tests/test_gpu.cpp)
    target_link_libraries(orca_test_gpu PRIVATE orca_core)
    target_include_directories(orca_test_gpu PRIVATE tests)
    add_test(NAME gpu COMMAND orca_test_gpu)
    add_executable(orca_test_gpu_sw tests/test_gpu_sw.cpp)
    target_link_libraries(orca_test_gpu_sw PRIVATE orca_core)
    target_include_directories(orca_test_gpu_sw PRIVATE tests)
    add_test(NAME gpu_sw COMMAND orca_test_gpu_sw)
    add_executable(orca_test_efb_copy tests/test_efb_copy.cpp)
    target_link_libraries(orca_test_efb_copy PRIVATE orca_core)
    target_include_directories(orca_test_efb_copy PRIVATE tests)
    add_test(NAME efb_copy COMMAND orca_test_efb_copy)
    if(ORCA_GLES)
        add_executable(orca_test_gles tests/test_gles.cpp)
        target_link_libraries(orca_test_gles PRIVATE orca_gles)
        target_include_directories(orca_test_gles PRIVATE tests)
        add_test(NAME gles COMMAND orca_test_gles)
        # no GPU / no EGL display: the suite reports itself as skipped (exit code 77)
        set_tests_properties(gles PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
    add_executable(orca_test_dsp_mi tests/test_dsp_mi.cpp)
    target_link_libraries(orca_test_dsp_mi PRIVATE orca_core)
    target_include_directories(orca_test_dsp_mi PRIVATE tests)
    add_test(NAME dsp_mi COMMAND orca_test_dsp_mi)
    add_executable(orca_test_dsp_core tests/test_dsp_core.cpp)
    target_link_libraries(orca_test_dsp_core PRIVATE orca_core)
    target_include_directories(orca_test_dsp_core PRIVATE tests)
    add_test(NAME dsp_core COMMAND orca_test_dsp_core)
    set_tests_properties(programs PROPERTIES SKIP_RETURN_CODE 77)

    # PowerPC 750 build with clang + ld.lld, if available.
    find_program(ORCA_PPC_CLANG NAMES clang clang-18 clang-17)
    find_program(ORCA_PPC_LLD NAMES ld.lld ld.lld-18 ld.lld-17)
    find_program(ORCA_OBJCOPY NAMES llvm-objcopy llvm-objcopy-18 llvm-objcopy-17)
    set(ORCA_PPC_OK FALSE)
    if(ORCA_PPC_CLANG AND ORCA_PPC_LLD AND ORCA_OBJCOPY)
        execute_process(COMMAND ${ORCA_PPC_CLANG} --target=powerpc-unknown-eabi -mcpu=750 -ffreestanding -c -x c /dev/null
                                -o ${CMAKE_BINARY_DIR}/ppc_probe.o RESULT_VARIABLE ppc_rc OUTPUT_QUIET ERROR_QUIET)
        if(ppc_rc EQUAL 0)
            set(ORCA_PPC_OK TRUE)
        endif()
    endif()
    if(ORCA_PPC_OK)
        set(PDIR ${CMAKE_SOURCE_DIR}/tests/programs)
        set(ODIR ${CMAKE_BINARY_DIR}/programs)
        file(MAKE_DIRECTORY ${ODIR})
        set(PPC_FLAGS --target=powerpc-unknown-eabi -mcpu=750 -O2 -ffreestanding -fno-builtin -nostdlib
                      -ffp-contract=off -fno-pic -Wall -Wextra -Werror)
        add_custom_command(OUTPUT ${ODIR}/crt0.o COMMAND ${ORCA_PPC_CLANG} ${PPC_FLAGS} -c ${PDIR}/crt0.S -o ${ODIR}/crt0.o
                           DEPENDS ${PDIR}/crt0.S)
        add_custom_command(OUTPUT ${ODIR}/runtime.o COMMAND ${ORCA_PPC_CLANG} ${PPC_FLAGS} -c ${PDIR}/runtime.c -o ${ODIR}/runtime.o
                           DEPENDS ${PDIR}/runtime.c)
        set(PPC_OUTPUTS)
        foreach(prog integer_suite float_suite hardware_suite dvd_suite av_suite exi_suite aram_suite dsp_suite gpu_suite gpu_draw)
            add_custom_command(
                OUTPUT ${ODIR}/${prog}.elf ${ODIR}/${prog}.dol
                COMMAND ${ORCA_PPC_CLANG} ${PPC_FLAGS} -c ${PDIR}/${prog}.c -o ${ODIR}/${prog}.o
                COMMAND ${ORCA_PPC_LLD} -T ${PDIR}/link.ld ${ODIR}/crt0.o ${ODIR}/runtime.o ${ODIR}/${prog}.o -o ${ODIR}/${prog}.elf
                COMMAND orca_elf2dol ${ODIR}/${prog}.elf ${ODIR}/${prog}.dol
                DEPENDS ${PDIR}/${prog}.c ${PDIR}/common.h ${PDIR}/link.ld ${ODIR}/crt0.o ${ODIR}/runtime.o orca_elf2dol
                COMMENT "Building PowerPC test program ${prog}")
            list(APPEND PPC_OUTPUTS ${ODIR}/${prog}.elf ${ODIR}/${prog}.dol)
        endforeach()
        # Apploader: flat binary loaded at 0x81200000 by the HLE IPL.
        add_custom_command(
            OUTPUT ${ODIR}/apploader.bin
            COMMAND ${ORCA_PPC_CLANG} ${PPC_FLAGS} -c ${PDIR}/apploader.c -o ${ODIR}/apploader.o
            COMMAND ${ORCA_PPC_LLD} -T ${PDIR}/apploader.ld ${ODIR}/apploader.o -o ${ODIR}/apploader.elf
            COMMAND ${ORCA_OBJCOPY} -O binary ${ODIR}/apploader.elf ${ODIR}/apploader.bin
            DEPENDS ${PDIR}/apploader.c ${PDIR}/apploader.ld
            COMMENT "Building PowerPC test apploader")
        list(APPEND PPC_OUTPUTS ${ODIR}/apploader.bin)
        add_custom_target(orca_ppc_programs ALL DEPENDS ${PPC_OUTPUTS})
        add_dependencies(orca_test_programs orca_ppc_programs)
        target_compile_definitions(orca_test_programs PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        add_dependencies(orca_test_disc orca_ppc_programs)
        add_dependencies(orca_test_av orca_ppc_programs)
        add_dependencies(orca_test_exi orca_ppc_programs)
        add_dependencies(orca_test_dsp_mi orca_ppc_programs)
        add_dependencies(orca_test_dsp_core orca_ppc_programs)
        target_compile_definitions(orca_test_dsp_core PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        target_compile_definitions(orca_test_dsp_mi PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        target_compile_definitions(orca_test_exi PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        target_compile_definitions(orca_test_gpu PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        add_dependencies(orca_test_gpu orca_ppc_programs)
        target_compile_definitions(orca_test_av PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        if(ORCA_GLES)
            target_compile_definitions(orca_test_gles PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
            add_dependencies(orca_test_gles orca_ppc_programs)
        endif()
        target_compile_definitions(orca_test_disc PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
    else()
        message(STATUS "PowerPC cross toolchain not found: whole-program tests will be skipped")
    endif()

    # A hang (e.g. an emulated deadlock) must fail, not block the test run.
    set_tests_properties(sha1 aes128 jit_arm64 memory cpu_integer cpu_system mmu reference fpu_engine cpu_fpu system programs disc av exi dsp_mi dsp_core gpu gpu_sw efb_copy PROPERTIES TIMEOUT 300)

    find_program(ORCA_LLVM_MC NAMES llvm-mc llvm-mc-18 llvm-mc-17 llvm-mc-16)
    add_executable(orca_encoding_check tests/encoding_check.cpp)
    target_include_directories(orca_encoding_check PRIVATE tests)
    add_test(NAME encodings COMMAND orca_encoding_check "${ORCA_LLVM_MC}")
    set_tests_properties(encodings PROPERTIES SKIP_RETURN_CODE 77)
endif()

foreach(tool bench_interpreter elf2dol orca_run)
    string(REGEX REPLACE "^orca_" "" tool_name ${tool})
    add_executable(orca_${tool_name} tools/${tool}.cpp)
    target_link_libraries(orca_${tool_name} PRIVATE orca_core)
    target_compile_options(orca_${tool_name} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
endforeach()
