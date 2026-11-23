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
#include "orca/disc/disc_image.hpp"
#include "orca/hw/audio_interface.hpp"
#include "orca/hw/disc_interface.hpp"
#include "orca/hw/serial_interface.hpp"
#include "orca/hw/video_interface.hpp"
#include "orca/hw/mmio_splitter.hpp"
#include "orca/hw/external_interface.hpp"
#include "orca/hw/exi_ipl.hpp"
#include "orca/hw/dsp_interface.hpp"
#include "orca/hw/memory_interface.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/loader/executable.hpp"
#include "orca/memory/bus.hpp"
#include "orca/ppc/cpu.hpp"

#include <initializer_list>
#include <memory>
#include <optional>
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
    bool reached_stop_pc{false}; // run_until / call_guest only
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
    hw::DiscInterface& di() noexcept { return *di_; }
    hw::VideoInterface& vi() noexcept { return *vi_; }
    hw::SerialInterface& si() noexcept { return *si_; }
    hw::AudioInterface& ai() noexcept { return *ai_; }
    hw::ExternalInterface& exi() noexcept { return *exi_; }
    hw::MemoryInterface& mi() noexcept { return *mi_; }
    hw::DspInterface& dsp() noexcept { return *dsp_; }
    // RTC/SRAM/IPL-ROM chip on EXI channel 0, CS1 (ROM image supplied by the user).
    hw::IplRtcSram& ipl() noexcept { return *ipl_; }
    // Called at every VI field start (top = false / bottom = true), after the SI hook.
    void set_field_listener(std::function<void(bool)> f) { field_listener_ = std::move(f); }
    void insert_disc(std::unique_ptr<disc::DiscReader> d) { di_->insert(std::move(d)); }
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
    // Like run(), but also stops (reached_stop_pc = true) when PC == stop_pc
    // before executing the instruction there.
    RunResult run_until(u32 stop_pc, u64 max_cycles);

    // Calls a guest function: r3.. = args, LR = a return address that is never
    // executed, runs until the function returns. Returns r3, or nullopt (with
    // `error`) if it stopped for any other reason or ran out of cycles.
    std::optional<u32> call_guest(u32 function, std::initializer_list<u32> args, u64 max_cycles, std::string& error);

    // Boots the inserted GameCube disc the way the IPL does, at a high level:
    //   * low-memory globals: disc header copy (0x80000000, 0x20 bytes),
    //     0x80000020 = 0x0D15EA5E, 0x80000028 = MEM1 size,
    //     0x800000F8 = bus clock, 0x800000FC = CPU clock,
    //     0x800000F4 = pointer to a copy of bi2.bin (disc 0x440, 0x2000 bytes);
    //   * the apploader (disc 0x2460) is loaded at 0x81200000 and driven through
    //     its entry/init/main/close protocol; every read it requests is
    //     performed from the disc; close() returns the game entry point;
    //   * the CPU is left at the entry point with the HLE boot BATs/MSR.
    // Other IPL work (fonts, SRAM, video setup, OS globals beyond the list
    // above) is not done. See docs/ESTADO.md.
    bool boot_disc(std::string& error);

    static constexpr u32 kGuestReturn = 0x81300000U;   // never executed
    static constexpr u32 kReportStub = 0x81300004U;    // blr (apploader OSReport)
    static constexpr u32 kApploaderStack = 0x816FFFF0U;
    static constexpr u32 kBi2Address = 0x817FE000U;

private:
    void advance(u64 cycles);

    SystemConfig config_;
    Clocks clocks_;
    mem::Bus bus_;
    core::Scheduler scheduler_;
    std::unique_ptr<ppc::Cpu> cpu_;
    std::unique_ptr<hw::ProcessorInterface> pi_;
    std::unique_ptr<hw::GatherPipe> gather_pipe_;
    std::unique_ptr<hw::MmioSplitter> page_0c006_;
    std::unique_ptr<hw::DiscInterface> di_;
    std::unique_ptr<hw::VideoInterface> vi_;
    std::unique_ptr<hw::SerialInterface> si_;
    std::unique_ptr<hw::AudioInterface> ai_;
    std::unique_ptr<hw::ExternalInterface> exi_;
    std::unique_ptr<hw::IplRtcSram> ipl_;
    std::unique_ptr<hw::MemoryInterface> mi_;
    std::unique_ptr<hw::DspInterface> dsp_;
    std::function<void(bool)> field_listener_;
    std::optional<u32> stop_pc_;
    u64 timebase_remainder_{0};
};

} // namespace orca::system
