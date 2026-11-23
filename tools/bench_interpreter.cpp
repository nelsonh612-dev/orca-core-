// Measures raw interpreter throughput on the host. This is a host-side
// baseline only: it says nothing about speed on Android/ARM64 hardware.
#include "orca/memory/console_maps.hpp"
#include "orca/ppc/cpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace orca;
using namespace orca::ppc;

int main(int argc, char** argv) {
    const u32 iterations = argc > 1 ? static_cast<u32>(std::strtoul(argv[1], nullptr, 0)) : 5000000U;
    mem::Bus bus;
    mem::map_gamecube_ram(bus);
    Cpu cpu(bus, CpuConfig{0});
    // loop: addi r3,r3,1 ; lwz r4,0(r5) ; stw r3,4(r5) ; rlwinm r6,r3,3,0,28 ; bdnz loop ; (stop)
    const u32 code[] = {0x38630001U, 0x80850000U, 0x90650004U, 0x54661838U, 0x4200FFF0U, 0x00000000U};
    for (u32 i = 0; i < 6; ++i) bus.write32(0x3000 + i * 4, code[i]);
    cpu.state().pc = 0x3000;
    cpu.state().ctr = iterations;
    cpu.state().gpr[5] = 0x8000;
    const auto t0 = std::chrono::steady_clock::now();
    const StepResult r = cpu.run(static_cast<u64>(iterations) * 5U + 10U);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const u64 n = cpu.instructions_retired();
    std::printf("stop=%s retired=%llu r3=%u time=%.3fs -> %.1f M instr/s (host interpreter, real-mode, no translation)\n",
                to_string(r), static_cast<unsigned long long>(n), cpu.state().gpr[3], secs, static_cast<double>(n) / secs / 1e6);
    return r == StepResult::UnsupportedInstruction && cpu.state().gpr[3] == iterations ? 0 : 1;
}
