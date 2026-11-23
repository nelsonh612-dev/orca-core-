// Whole-program tests: C programs compiled by clang for the PowerPC 750 run on
// Orca (loaded as ELF and as DOL converted by orca_elf2dol). The integer and
// float programs are also compiled natively; both runs must emit identical
// value sequences. The compiler, not our test encoder, generated this code.
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
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

TEST_MAIN("programs")
#endif
