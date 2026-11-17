// Reproducible interpreter/JIT comparison. Run this on the target Android
// ARM64 device to obtain real hardware numbers; QEMU/host results are not a
// substitute for device benchmarks.
#include "orca/memory/console_maps.hpp"
#include "orca/ppc/cpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace orca;
using namespace orca::ppc;

namespace {
struct BenchResult {
    StepResult result{StepResult::UnsupportedInstruction};
    u64 retired{};
    u32 r3{};
    u64 ram_fastpath_hits{};
    double seconds{};
};

BenchResult run_bench(u32 iterations, bool fp, bool jit) {
    mem::Bus bus;
    mem::map_gamecube_ram(bus);
    Cpu cpu(bus, CpuConfig{0});
    // Integer loop: addi, lwz, stw, rlwinm, bdnz.
    // FP loop intentionally measures the current software arithmetic fallback.
    const u32 int_code[] = {0x38630001U, 0x80850000U, 0x90650004U, 0x54661838U, 0x4200FFF0U, 0x00000000U};
    const u32 fp_code[] = {0xFC2208FAU, 0xEC84102AU, 0x10A5102AU, 0x38630001U, 0x4200FFF0U, 0x00000000U};
    const u32* code = fp ? fp_code : int_code;
    for (u32 i = 0; i < 6U; ++i) bus.write32(0x3000U + i * 4U, code[i]);
    bus.write32(0x8000U, 0x12345678U);
    cpu.state().pc = 0x3000U;
    cpu.state().ctr = iterations;
    cpu.state().gpr[5] = 0x8000U;
    cpu.state().msr = MSR_FP;
    cpu.state().hid2 = HID2_PSE;
    cpu.state().fpr[1].ps0 = cpu.state().fpr[1].ps1 = 0x3FF0000000000000ULL;
    cpu.state().fpr[2].ps0 = cpu.state().fpr[2].ps1 = 0x3FF0000000000000ULL;
    cpu.state().fpr[3].ps0 = cpu.state().fpr[3].ps1 = 0x3FE0000000000000ULL;

    const u64 budget = static_cast<u64>(iterations) * 5U + 10U;
    const auto t0 = std::chrono::steady_clock::now();
    StepResult result;
#if defined(__aarch64__)
    result = jit ? cpu.run_jit(budget) : cpu.run(budget);
#else
    (void)jit;
    result = cpu.run(budget);
#endif
    const auto t1 = std::chrono::steady_clock::now();
    BenchResult out;
    out.result = result;
    out.retired = cpu.instructions_retired();
    out.r3 = cpu.state().gpr[3];
    out.ram_fastpath_hits = cpu.jit_memory_fastpath_hits();
    out.seconds = std::chrono::duration<double>(t1 - t0).count();
    return out;
}

void print_result(const char* label, const BenchResult& r, const char* loop) {
    const double mips = r.seconds > 0.0 ? static_cast<double>(r.retired) / r.seconds / 1e6 : 0.0;
    std::printf("%s: stop=%s retired=%llu r3=%u time=%.3fs throughput=%.2f M instr/s (%s loop)\n",
                label, to_string(r.result), static_cast<unsigned long long>(r.retired),
                r.r3, r.seconds, mips, static_cast<unsigned long long>(r.ram_fastpath_hits), loop);
}
} // namespace

int main(int argc, char** argv) {
    const u32 iterations = argc > 1 ? static_cast<u32>(std::strtoul(argv[1], nullptr, 0)) : 5000000U;
    const bool fp = argc > 2 && argv[2][0] == 'f';
    const char* mode = argc > 3 ? argv[3] : "interpreter";
    const char* loop = fp ? "FP-fallback" : "integer-memory";
    if (iterations == 0U) {
        std::fprintf(stderr, "iterations must be greater than zero\n");
        return 2;
    }

    if (mode[0] == 'c') {
        const BenchResult interpreter = run_bench(iterations, fp, false);
        print_result("Interpreter", interpreter, loop);
#if defined(__aarch64__)
        const BenchResult jit = run_bench(iterations, fp, true);
        print_result("ARM64 JIT", jit, loop);
        const double interpreter_mips = static_cast<double>(interpreter.retired) / interpreter.seconds / 1e6;
        const double jit_mips = static_cast<double>(jit.retired) / jit.seconds / 1e6;
        std::printf("speedup=%.3fx (same guest instruction budget; includes first-block compilation)\n",
                    jit_mips > 0.0 ? jit_mips / interpreter_mips : 0.0);
        return interpreter.result == StepResult::UnsupportedInstruction &&
               jit.result == StepResult::UnsupportedInstruction &&
               interpreter.r3 == iterations && jit.r3 == iterations ? 0 : 1;
#else
        std::printf("JIT comparison unavailable: this binary is not running on AArch64.\n");
        return 2;
#endif
    }

    if (mode[0] == 'j') {
#if defined(__aarch64__)
        const BenchResult result = run_bench(iterations, fp, true);
        print_result("ARM64 JIT", result, loop);
        return result.result == StepResult::UnsupportedInstruction && result.r3 == iterations ? 0 : 1;
#else
        std::fprintf(stderr, "JIT mode requires an AArch64 build.\n");
        return 2;
#endif
    }

    const BenchResult result = run_bench(iterations, fp, false);
    print_result("Interpreter", result, loop);
    return result.result == StepResult::UnsupportedInstruction && result.r3 == iterations ? 0 : 1;
}
