// Whole-program tests: C programs compiled by clang for the PowerPC 750 run on
// Orca (loaded as ELF and as DOL converted by orca_elf2dol). The integer and
// float programs are also compiled natively; both runs must emit identical
// value sequences. The compiler, not our test encoder, generated this code.
#include "orca/jit/code_buffer.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <chrono>
#include <fstream>
#include <iterator>
#include <vector>

using namespace orca;

extern "C" {
int integer_suite_main(void);
int float_suite_main(void);
static std::vector<u32>* g_host_log = nullptr;
void orca_emit(u32 v) { g_host_log->push_back(v); }
}

namespace {

using orca::test::TestPort;

struct ProgramRun {
    bool loaded = false;
    ppc::StepResult stop{};
    std::vector<u32> log;
    bool exited = false;
    u32 exit_code = 0;
    u64 instructions = 0;
    std::string error;
};

ProgramRun run_file(const char* path) {
    ProgramRun r;
    std::ifstream in(path, std::ios::binary);
    if (!in) { r.error = std::string("cannot open ") + path; return r; }
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    system::System sys({});
    TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", 0x0E000000U, 0x1000, port);
    if (!sys.load_executable(file, r.error)) return r;
    r.loaded = true;
    const auto res = sys.run(4000000000ULL);
    r.stop = res.stop;
    r.instructions = res.instructions;
    r.log = port.log;
    r.exited = port.exited;
    r.exit_code = port.exit_code;
    if (res.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    %s stopped: %s (%s) pc=%08X ins=%08X\n", path, ppc::to_string(res.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc, sys.cpu().last_stop().instruction);
    return r;
}

std::vector<u32> run_host(int (*fn)(void)) {
    std::vector<u32> log;
    g_host_log = &log;
    const int rc = fn();
    CHECK_EQ(rc, 0);
    g_host_log = nullptr;
    return log;
}

void compare(const char* name, const ProgramRun& target, const std::vector<u32>& host) {
    CHECK(target.loaded);
    CHECK(target.stop == ppc::StepResult::Halted);
    CHECK(target.exited);
    CHECK_EQ(target.exit_code, 0U);
    CHECK_EQ(target.log.size(), host.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < std::min(target.log.size(), host.size()); ++i) {
        if (target.log[i] != host[i]) {
            if (mismatches++ < 8) std::fprintf(stderr, "    %s value %zu: orca %08X host %08X\n", name, i, target.log[i], host[i]);
        }
    }
    CHECK_EQ(mismatches, 0U);
    std::printf("[programs] %-28s %3zu values, %9llu instructions on Orca, %s\n", name, target.log.size(),
                static_cast<unsigned long long>(target.instructions), mismatches == 0 && target.log.size() == host.size() ? "identical to host" : "MISMATCH");
}

} // namespace

#ifndef ORCA_PROGRAMS_DIR
int main() { std::puts("PowerPC cross toolchain (clang + ld.lld) not available: SKIPPED"); return 77; }
#else

TEST_CASE(integer_suite_elf_and_dol_match_host) {
    const auto host = run_host(integer_suite_main);
    CHECK(host.size() > 10U);
    CHECK_EQ(host[1], 0xCBF43926U); // the standard CRC-32 check value: the host side is sane
    compare("integer_suite.elf", run_file(ORCA_PROGRAMS_DIR "/integer_suite.elf"), host);
    compare("integer_suite.dol", run_file(ORCA_PROGRAMS_DIR "/integer_suite.dol"), host);
}

TEST_CASE(float_suite_elf_and_dol_match_host) {
    const auto host = run_host(float_suite_main);
    compare("float_suite.elf", run_file(ORCA_PROGRAMS_DIR "/float_suite.elf"), host);
    compare("float_suite.dol", run_file(ORCA_PROGRAMS_DIR "/float_suite.dol"), host);
}

TEST_CASE(hardware_suite_exceptions_timers_and_fifo) {
    // Expected values come from the program's own logic (no host equivalent).
    const std::vector<u32> expected = {
        3,                        // three system calls handled through vector 0xC00
        5, 1, 1, 1,               // five decrementer interrupts (one already pending at boot + four
                                  // full periods), timing within [4, 5) periods, rfi restored IR|DR
        0xF0000000U, 0xF000000FU, // gather pipe data in the FIFO
        0x00100000U | 0x20000000U,// write pointer wrapped to base, wrap flag set
        0x00000100U,              // INTMR round trip
    };
    for (const char* f : {ORCA_PROGRAMS_DIR "/hardware_suite.elf", ORCA_PROGRAMS_DIR "/hardware_suite.dol"}) {
        const auto r = run_file(f);
        CHECK(r.stop == ppc::StepResult::Halted);
        CHECK_EQ(r.exit_code, 0U);
        CHECK(r.log == expected);
        if (r.log != expected) for (u32 v : r.log) std::fprintf(stderr, "    got %08X\n", v);
        std::printf("[programs] %-28s %3zu values, %9llu instructions on Orca, %s\n", f + std::string(f).rfind('/') + 1,
                    r.log.size(), static_cast<unsigned long long>(r.instructions), r.log == expected ? "as expected" : "MISMATCH");
    }
}

// The same programs with the JIT (on AArch64 hosts): identical output, retired
// instructions, cycles, CPU state and RAM. hardware_suite checks interrupts and the
// decrementer, so the block budget of System::run is exercised too.
namespace {
struct FullRun {
    ppc::StepResult stop{};
    u64 instructions = 0, cycles = 0, ram_hash = 0;
    std::vector<u32> log;
    ppc::CpuState state;
    u64 block_steps = 0, blocks = 0, helper_steps = 0;
    double ms = 0;
};
FullRun run_full(const char* path, bool jit) {
    FullRun r;
    std::ifstream in(path, std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    system::SystemConfig cfg{};
    cfg.use_jit = jit;
    system::System sys(cfg);
    TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", 0x0E000000U, 0x1000, port);
    std::string err;
    if (!sys.load_executable(file, err)) return r;
    const auto t0 = std::chrono::steady_clock::now();
    const auto res = sys.run(4000000000ULL);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.stop = res.stop;
    r.instructions = res.instructions;
    r.cycles = res.cycles;
    r.log = port.log;
    r.state = sys.cpu().state();
    const u8* ram = sys.bus().ram_pointer(0, 24U << 20);
    u64 h = 0xCBF29CE484222325ULL;
    for (std::size_t i = 0; ram != nullptr && i < (24U << 20); ++i) { h ^= ram[i]; h *= 0x100000001B3ULL; }
    r.ram_hash = h;
    if (sys.jit() != nullptr) {
        r.block_steps = sys.jit()->stats().block_steps;
        r.blocks = sys.jit()->stats().blocks_compiled;
        r.helper_steps = sys.jit()->stats().helper_steps;
    }
    return r;
}
} // namespace

TEST_CASE(programs_identical_with_the_jit) {
    if (!jit::host_can_execute()) {
        std::printf("[programs] host is not AArch64: JIT runs are compared on the ARM64 CI\n");
        return;
    }
    for (const char* f : {ORCA_PROGRAMS_DIR "/integer_suite.elf", ORCA_PROGRAMS_DIR "/float_suite.elf",
                          ORCA_PROGRAMS_DIR "/hardware_suite.elf", ORCA_PROGRAMS_DIR "/integer_suite.dol"}) {
        const FullRun a = run_full(f, false), b = run_full(f, true);
        CHECK(a.stop == b.stop);
        CHECK_EQ(a.instructions, b.instructions);
        CHECK_EQ(a.cycles, b.cycles);
        CHECK(a.log == b.log);
        CHECK(a.state == b.state);
        CHECK_EQ(a.ram_hash, b.ram_hash);
        CHECK(b.block_steps > 0U);
        std::printf("[programs] %-24s JIT: %9llu of %9llu instructions in %llu translated blocks (%llu of them by the in-block interpreter call: FP/PS, lmw/stmw, ...), %s\n",
                    f + std::string(f).rfind('/') + 1, static_cast<unsigned long long>(b.block_steps),
                    static_cast<unsigned long long>(b.instructions), static_cast<unsigned long long>(b.blocks),
                    static_cast<unsigned long long>(b.helper_steps),
                    a.state == b.state && a.ram_hash == b.ram_hash && a.cycles == b.cycles ? "identical to the interpreter" : "DIFFERENT");
        std::printf("[programs] %-24s time (whole System, same machine): interpreter %.1f ms, JIT %.1f ms\n",
                    f + std::string(f).rfind('/') + 1, a.ms, b.ms);
    }
}

TEST_MAIN("programs")
#endif
