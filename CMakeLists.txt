cmake_minimum_required(VERSION 3.20)
project(orca_core VERSION 0.19.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Deterministic floating point on every host. AArch64 compilers fuse a*b+c into one
# FMA by default (GCC -ffp-contract=fast, Clang "on"), which rounds differently and
# changed depth values on ARM64 (found by the ARM64 CI). Fused operations are written
# explicitly (std::fma) where the emulated hardware fuses.
add_compile_options(-ffp-contract=off)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORCA_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORCA_WERROR "Treat compiler warnings as errors" OFF)
option(ORCA_BUILD_TESTS "Build the test suite" ON)
# Per-test time limits (seconds). Sanitizer Debug builds run 10-30x slower (the ARM64 CI
# runner took 207 s for cpu_fpu alone), so they get a larger limit by default.
if(ORCA_SANITIZE)
    set(ORCA_TEST_TIMEOUT 3600 CACHE STRING "ctest time limit per suite (s)")
    set(ORCA_GLES_TIMEOUT 5400 CACHE STRING "ctest time limit for the GLES suite (s)")
else()
    set(ORCA_TEST_TIMEOUT 300 CACHE STRING "ctest time limit per suite (s)")
    set(ORCA_GLES_TIMEOUT 600 CACHE STRING "ctest time limit for the GLES suite (s)")
endif()

set(ORCA_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
if(ORCA_WERROR)
    list(APPEND ORCA_WARNINGS -Werror)
endif()

if(ORCA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_compile_definitions(ORCA_SANITIZED=1) # long randomised tests run a quarter of their cases
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(orca_core
    src/audio/mixer.cpp
    src/core/scheduler.cpp
    src/crypto/aes.cpp
    src/crypto/sha1.cpp
    src/crypto/rsa.cpp
    src/disc/blob.cpp
    src/disc/bootmii_keys.cpp
    src/disc/disc_image.cpp
    src/disc/fst.cpp
    src/disc/wii_disc.cpp
    src/disc/wii_meta.cpp
    src/dsp/dsp_core.cpp
    src/dsp/hle.cpp
    src/dsp/hle_ax.cpp
    src/dsp/hle_ax_common.cpp
    src/dsp/hle_axwii.cpp
    src/hw/audio_interface.cpp
    src/hw/disc_interface.cpp
    src/hw/dsp_interface.cpp
    src/hw/memory_interface.cpp
    src/hw/hollywood_ipc.cpp
    src/hw/av_encoder.cpp
    src/ios/iosc.cpp
    src/ios/kernel.cpp
    src/ios/stm.cpp
    src/ios/sdio.cpp
    src/ios/net_kd.cpp
    src/ios/net_kd_time.cpp
    src/ios/es.cpp
    src/ios/nand_fs.cpp
    src/ios/fs_device.cpp
    src/ios/di_device.cpp
    src/ios/memory_values.cpp
    src/ios/bluetooth.cpp
    src/ios/nand_setup.cpp
    src/frontend/emulator.cpp
    src/wiimote/wiimote.cpp
    src/hw/exi_ipl.cpp
    src/hw/exi_memcard.cpp
    src/gpu/command_stream.cpp
    src/gpu/xf_memory.cpp
    src/gpu/cp_state.cpp
    src/hw/pixel_engine.cpp
    src/hw/command_processor.cpp
    src/gpu/gpu.cpp
    src/gpu/efb_access.cpp
    src/gpu/texture_decoder.cpp
    src/gpu/sw/vertex.cpp
    src/gpu/sw/renderer.cpp
    src/gpu/sw/efb_copy.cpp
    src/video/xfb.cpp
    src/jit/a64_emitter.cpp
    src/jit/code_buffer.cpp
    src/ppc/jit.cpp
    src/hw/external_interface.cpp
    src/hw/serial_interface.cpp
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
)
# The FP engine changes the host rounding mode at run time.
set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math")
target_include_directories(orca_core PUBLIC include)
# ARMv8 Cryptography Extension for AES (used at run time only when the CPU has it).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    target_sources(orca_core PRIVATE src/crypto/aes_arm64.cpp)
    set_source_files_properties(src/crypto/aes_arm64.cpp PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crypto")
    message(STATUS "Orca: AES with the ARMv8 Cryptography Extension when available")
endif()
# Build-time key bundle (include/orca/ios/key_bundle.hpp): default values for
# OrcaIOSC, compiled in like Dolphin's default entries. The repository holds none;
# whoever builds and is entitled to the values passes a file.
set(ORCA_DEFAULT_KEYS_FILE "$ENV{ORCA_DEFAULT_KEYS_FILE}" CACHE FILEPATH "Key file whose values become OrcaIOSC's built-in defaults (empty: none)")
if(ORCA_DEFAULT_KEYS_FILE)
    if(NOT EXISTS "${ORCA_DEFAULT_KEYS_FILE}")
        message(FATAL_ERROR "ORCA_DEFAULT_KEYS_FILE ${ORCA_DEFAULT_KEYS_FILE} does not exist")
    endif()
    file(READ "${ORCA_DEFAULT_KEYS_FILE}" ORCA_KEY_BUNDLE_HEX HEX)
    string(LENGTH "${ORCA_KEY_BUNDLE_HEX}" ORCA_KEY_BUNDLE_HEXLEN)
    math(EXPR ORCA_KEY_BUNDLE_SIZE "${ORCA_KEY_BUNDLE_HEXLEN} / 2")
    if(ORCA_KEY_BUNDLE_SIZE EQUAL 0 OR ORCA_KEY_BUNDLE_SIZE GREATER 65536)
        message(FATAL_ERROR "ORCA_DEFAULT_KEYS_FILE must hold between 1 byte and 64 KiB")
    endif()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ORCA_KEY_BUNDLE_BYTES "${ORCA_KEY_BUNDLE_HEX}")
    get_filename_component(ORCA_KEY_BUNDLE_ORIGIN "${ORCA_DEFAULT_KEYS_FILE}" NAME)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORCA_DEFAULT_KEYS_FILE}")
    message(STATUS "Orca: OrcaIOSC default values from ${ORCA_KEY_BUNDLE_ORIGIN} (${ORCA_KEY_BUNDLE_SIZE} bytes)")
else()
    set(ORCA_KEY_BUNDLE_BYTES "0")
    set(ORCA_KEY_BUNDLE_SIZE 0)
    set(ORCA_KEY_BUNDLE_ORIGIN "")
    message(STATUS "Orca: no OrcaIOSC default values (ORCA_DEFAULT_KEYS_FILE not set)")
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/key_bundle.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/orca_key_bundle.cpp @ONLY)
# A library of its own: a program that defines the bundle itself (the key_bundle
# test) takes precedence over this one when linking.
add_library(orca_key_bundle STATIC ${CMAKE_CURRENT_BINARY_DIR}/generated/orca_key_bundle.cpp)
target_include_directories(orca_key_bundle PRIVATE include)
target_link_libraries(orca_core PUBLIC orca_key_bundle)
# GCZ disc images need zlib (present on Linux and in the Android NDK); without it
# GCZ images are refused with the reason.
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(orca_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(orca_core PUBLIC ORCA_HAVE_ZLIB=1)
    message(STATUS "Orca: zlib found, GCZ images supported")
else()
    message(STATUS "Orca: zlib not found, GCZ images are refused")
endif()
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

if(ORCA_BUILD_TESTS)
    enable_testing()
    foreach(suite memory cpu_integer cpu_system mmu reference fpu_engine cpu_fpu system crypto wii_disc filesystem iosc bootmii_keys mmio_coverage opcode_coverage audio dsp_hle ios wiimote frontend)
        add_executable(orca_test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(orca_test_${suite} PRIVATE orca_core)
        target_include_directories(orca_test_${suite} PRIVATE tests)
        add_test(NAME ${suite} COMMAND orca_test_${suite})
    endforeach()
    # Real disc images: only with ORCA_GC_IMAGE / ORCA_WII_IMAGE set (see the
    # source); otherwise ctest reports the test as skipped, never as passed.
    add_executable(orca_test_real_images tests/test_real_images.cpp)
    target_link_libraries(orca_test_real_images PRIVATE orca_core)
    target_compile_options(orca_test_real_images PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
    add_test(NAME real_images COMMAND orca_test_real_images)
    set_tests_properties(real_images PROPERTIES SKIP_RETURN_CODE 77)
    # The build-time key bundle path, with test-only values (tests/test_key_bundle.txt)
    # compiled in exactly as -DORCA_DEFAULT_KEYS_FILE would.
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_key_bundle.txt ORCA_TEST_BUNDLE_HEX HEX)
    string(LENGTH "${ORCA_TEST_BUNDLE_HEX}" ORCA_TEST_BUNDLE_HEXLEN)
    math(EXPR ORCA_TEST_BUNDLE_SIZE "${ORCA_TEST_BUNDLE_HEXLEN} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ORCA_TEST_BUNDLE_BYTES "${ORCA_TEST_BUNDLE_HEX}")
    set(ORCA_KEY_BUNDLE_BYTES_SAVED "${ORCA_KEY_BUNDLE_BYTES}")
    set(ORCA_KEY_BUNDLE_SIZE_SAVED "${ORCA_KEY_BUNDLE_SIZE}")
    set(ORCA_KEY_BUNDLE_ORIGIN_SAVED "${ORCA_KEY_BUNDLE_ORIGIN}")
    set(ORCA_KEY_BUNDLE_BYTES "${ORCA_TEST_BUNDLE_BYTES}")
    set(ORCA_KEY_BUNDLE_SIZE ${ORCA_TEST_BUNDLE_SIZE})
    set(ORCA_KEY_BUNDLE_ORIGIN "test_key_bundle.txt")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/key_bundle.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/test_key_bundle.cpp @ONLY)
    set(ORCA_KEY_BUNDLE_BYTES "${ORCA_KEY_BUNDLE_BYTES_SAVED}")
    set(ORCA_KEY_BUNDLE_SIZE ${ORCA_KEY_BUNDLE_SIZE_SAVED})
    set(ORCA_KEY_BUNDLE_ORIGIN "${ORCA_KEY_BUNDLE_ORIGIN_SAVED}")
    add_executable(orca_test_key_bundle tests/test_key_bundle.cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/test_key_bundle.cpp)
    target_link_libraries(orca_test_key_bundle PRIVATE orca_core)
    target_include_directories(orca_test_key_bundle PRIVATE tests)
    target_compile_options(orca_test_key_bundle PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
    add_test(NAME key_bundle COMMAND orca_test_key_bundle)
    # Wii reading compared with an independent implementation of Dolphin's procedure
    # (tests/dolphin_wii_reference.py: OpenSSL + hashlib); skipped without python3/openssl.
    add_executable(orca_wii_reference_fixture tests/wii_reference_fixture.cpp)
    target_link_libraries(orca_wii_reference_fixture PRIVATE orca_core)
    target_include_directories(orca_wii_reference_fixture PRIVATE tests)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME wii_dolphin_reference
                 COMMAND sh -c "rm -rf wii_reference && mkdir wii_reference && $<TARGET_FILE:orca_wii_reference_fixture> wii_reference && ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/dolphin_wii_reference.py wii_reference")
        set_tests_properties(wii_dolphin_reference PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT ${ORCA_TEST_TIMEOUT})
    endif()

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
        set_tests_properties(gles PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT ${ORCA_GLES_TIMEOUT})
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
        foreach(prog integer_suite float_suite hardware_suite dvd_suite wii_dvd_suite av_suite exi_suite aram_suite dsp_suite gpu_suite gpu_draw)
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
        add_custom_command(
            OUTPUT ${ODIR}/apploader_wii.bin
            COMMAND ${ORCA_PPC_CLANG} ${PPC_FLAGS} -DORCA_WII -c ${PDIR}/apploader.c -o ${ODIR}/apploader_wii.o
            COMMAND ${ORCA_PPC_LLD} -T ${PDIR}/apploader.ld ${ODIR}/apploader_wii.o -o ${ODIR}/apploader_wii.elf
            COMMAND ${ORCA_OBJCOPY} -O binary ${ODIR}/apploader_wii.elf ${ODIR}/apploader_wii.bin
            DEPENDS ${PDIR}/apploader.c ${PDIR}/apploader.ld
            COMMENT "Building PowerPC test apploader (Wii offsets)")
        list(APPEND PPC_OUTPUTS ${ODIR}/apploader.bin ${ODIR}/apploader_wii.bin)
        add_custom_target(orca_ppc_programs ALL DEPENDS ${PPC_OUTPUTS})
        add_dependencies(orca_test_programs orca_ppc_programs)
        target_compile_definitions(orca_test_programs PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        add_dependencies(orca_test_disc orca_ppc_programs)
        add_dependencies(orca_test_ios orca_ppc_programs)
        add_dependencies(orca_test_frontend orca_ppc_programs)
        target_compile_definitions(orca_test_frontend PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        target_compile_definitions(orca_test_ios PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
        target_compile_definitions(orca_test_key_bundle PRIVATE ORCA_PROGRAMS_DIR="${ODIR}")
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
    set_tests_properties(memory cpu_integer cpu_system mmu reference fpu_engine cpu_fpu system programs disc av exi dsp_mi dsp_core gpu gpu_sw efb_copy PROPERTIES TIMEOUT ${ORCA_TEST_TIMEOUT})

    find_program(ORCA_LLVM_MC NAMES llvm-mc llvm-mc-18 llvm-mc-17 llvm-mc-16)
    add_executable(orca_encoding_check tests/encoding_check.cpp)
    target_include_directories(orca_encoding_check PRIVATE tests)
    add_test(NAME encodings COMMAND orca_encoding_check "${ORCA_LLVM_MC}")
    set_tests_properties(encodings PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(orca_test_jit_emitter tests/test_jit_emitter.cpp)
    target_link_libraries(orca_test_jit_emitter PRIVATE orca_core)
    target_include_directories(orca_test_jit_emitter PRIVATE tests)
    target_compile_definitions(orca_test_jit_emitter PRIVATE ORCA_LLVM_MC="${ORCA_LLVM_MC}")
    add_test(NAME jit_emitter COMMAND orca_test_jit_emitter)
    set_tests_properties(jit_emitter PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT ${ORCA_TEST_TIMEOUT})

    add_executable(orca_test_jit tests/test_jit.cpp)
    target_link_libraries(orca_test_jit PRIVATE orca_core)
    target_include_directories(orca_test_jit PRIVATE tests)
    if(ORCA_LLVM_MC)
        target_compile_definitions(orca_test_jit PRIVATE ORCA_LLVM_MC="${ORCA_LLVM_MC}")
    endif()
    add_test(NAME jit COMMAND orca_test_jit)
    set_tests_properties(jit PROPERTIES TIMEOUT ${ORCA_TEST_TIMEOUT})
endif()

foreach(tool bench_interpreter elf2dol orca_run orca_disc)
    string(REGEX REPLACE "^orca_" "" tool_name ${tool})
    add_executable(orca_${tool_name} tools/${tool}.cpp)
    target_link_libraries(orca_${tool_name} PRIVATE orca_core)
    target_compile_options(orca_${tool_name} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:${ORCA_WARNINGS}>)
endforeach()
