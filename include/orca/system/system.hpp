// Orca Core — a console instance: bus + CPU + scheduler + devices, and the
// main run loop that interleaves CPU execution with timed events.
//
// Clocks (public GameCube/Wii documentation): Gekko 486 MHz, Broadway
// 729 MHz; the time base ticks once every 12 CPU cycles on both (bus clock / 4).
//
// Timing is approximate: the interpreter has no pipeline/cache timing model,
// so every instruction costs `cycles_per_instruction` cycles (default 1).
// This keeps time monotonic and deterministic; it is not cycle accurate.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/loader/executable.hpp"
#include "orca/memory/bus.hpp"
#include "orca/ppc/cpu.hpp"

#include <memory>
#include <span>
#include <string>

namespace orca::system {

enum class Console : u8 { GameCube, Wii };

struct Clocks {
    u64 cpu_hz;
    u32 cpu_cycles_per_timebase_tick;
};

constexpr Clocks clocks_for(Console c) noexcept {
    return c == Console::GameCube ? Clocks{486000000ULL, 12U} : Clocks{729000000ULL, 12U};
}

struct SystemConfig {
    Console console{Console::GameCube};
    u32 pvr{0};                      // supplied by the caller; not hard-coded here
    u32 cycles_per_instruction{1};   // approximate cost model (see above)
};

struct RunResult {
    ppc::StepResult stop{ppc::StepResult::Executed};
    u64 cycles{0};
    u64 instructions{0};
};

class System {
public:
    explicit System(SystemConfig config);
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    mem::Bus& bus() noexcept { return bus_; }
    ppc::Cpu& cpu() noexcept { return *cpu_; }
    core::Scheduler& scheduler() noexcept { return scheduler_; }
    hw::ProcessorInterface& pi() noexcept { return *pi_; }
    hw::GatherPipe& gather_pipe() noexcept { return *gather_pipe_; }
    const SystemConfig& config() const noexcept { return config_; }
    const Clocks& clocks() const noexcept { return clocks_; }

    // Parses and loads a DOL or ELF; with `boot` the CPU is put in the HLE boot state.
    bool load_executable(std::span<const u8> file, std::string& error, bool boot = true);

    // Minimal high-level boot state for a loaded executable (what the IPL would
    // leave behind), limited to what is publicly documented:
    //   IBAT0/DBAT0: 0x80000000 -> 0x00000000, 256 MiB, cached, supervisor R/W
    //   DBAT1:       0xC0000000 -> 0x00000000, 256 MiB, cache-inhibited + guarded
    //   MSR = FP | IR | DR; PC = entry
    //   low memory: 0x80000020 = 0x0D15EA5E (boot magic), 0x80000028 = MEM1 size
    // Other OS globals, the apploader and IPL services are NOT emulated.
    void hle_boot(u32 entry);

    // Runs for at most `max_cycles` cycles. Stops early on any host stop
    // (UnsupportedInstruction, InvalidForm, MemoryFault, Halted, ...) or when the
    // CPU is idle (MSR[POW]) and no event can ever wake it.
    RunResult run(u64 max_cycles);

private:
    void advance(u64 cycles);

    SystemConfig config_;
    Clocks clocks_;
    mem::Bus bus_;
    core::Scheduler scheduler_;
    std::unique_ptr<ppc::Cpu> cpu_;
    std::unique_ptr<hw::ProcessorInterface> pi_;
    std::unique_ptr<hw::GatherPipe> gather_pipe_;
    u64 timebase_remainder_{0};
};

} // namespace orca::system
