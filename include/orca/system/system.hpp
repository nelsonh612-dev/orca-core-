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
#include "orca/gpu/efb_access.hpp"
#include "orca/ppc/jit.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/audio/mixer.hpp"
#include "orca/hw/audio_interface.hpp"
#include "orca/hw/disc_interface.hpp"
#include "orca/hw/serial_interface.hpp"
#include "orca/hw/video_interface.hpp"
#include "orca/hw/mmio_splitter.hpp"
#include "orca/hw/external_interface.hpp"
#include "orca/hw/exi_ipl.hpp"
#include "orca/hw/exi_memcard.hpp"

#include <array>
#include "orca/hw/dsp_interface.hpp"
#include "orca/gpu/gpu.hpp"
#include "orca/hw/command_processor.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/hw/memory_interface.hpp"
#include "orca/hw/hollywood_ipc.hpp"
#include "orca/ios/kernel.hpp"
#include "orca/ios/net_kd.hpp"
#include "orca/ios/net_kd_time.hpp"
#include "orca/ios/sdio.hpp"
#include "orca/ios/stm.hpp"
#include "orca/ios/es.hpp"
#include "orca/ios/fs_device.hpp"
#include "orca/ios/di_device.hpp"
#include "orca/ios/bluetooth.hpp"
#include "orca/ios/memory_values.hpp"
#include "orca/disc/wii_disc.hpp"
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
    // Run translated AArch64 code (ppc::Jit) where the host can execute it. The
    // interpreter remains the reference: results, timing and interrupts are the same
    // (see System::run and ppc/jit.hpp). Ignored on other hosts.
    bool use_jit{false};
    // DSP: high-level emulation of the known ucodes (orca/dsp/hle.hpp) instead of
    // running DSP code, which needs the user's DSP ROM dumps.
    bool dsp_hle{false};
    // Wii: host directory holding the NAND (orca/ios/nand_fs.hpp). Empty: no
    // NAND; any NAND access stops explicitly.
    std::string nand_root;
    // Wii: the common keys (supplied by the user) for decrypting disc partitions.
    disc::WiiKeys wii_keys;
    // Wii: when creating the system, OrcaIOSC fills the empty key slots from the
    // fixed places of ios::load_default_key_provision (plus key_dirs), so that no
    // key file has to be chosen per game.
    bool auto_keys{true};
    std::vector<std::string> key_dirs;
    // Skip exact repeats of busy-wait loops (see System::run). Results and timing
    // are those of executing every iteration.
    bool idle_skip{true};
};

struct RunResult {
    ppc::StepResult stop{ppc::StepResult::Executed};
    u64 cycles{0};
    u64 instructions{0};
    bool reached_stop_pc{false}; // run_until / call_guest only
    bool device_stop{false};     // a device (DSP core, GPU, IOS) hit a host stop
    std::string device_error;
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
    gpu::Gpu& gpu() noexcept { return *gpu_; }
    hw::CommandProcessor& command_processor() noexcept { return *cp_; }
    hw::PixelEngine& pixel_engine() noexcept { return *pe_; }
    hw::DiscInterface& di() noexcept { return *di_; }
    hw::VideoInterface& vi() noexcept { return *vi_; }
    hw::SerialInterface& si() noexcept { return *si_; }
    hw::AudioInterface& ai() noexcept { return *ai_; }
    audio::Mixer& mixer() noexcept { return *mixer_; }
    hw::ExternalInterface& exi() noexcept { return *exi_; }
    hw::MemoryInterface& mi() noexcept { return *mi_; }
    hw::DspInterface& dsp() noexcept { return *dsp_; }
    // Wii only (nullptr on GameCube): Hollywood registers and the IOS kernel (HLE).
    hw::HollywoodIpc* hollywood() noexcept { return hollywood_.get(); }
    ios::Kernel* ios() noexcept { return ios_.get(); }
    ios::Es* es() noexcept { return es_; }
    ios::NandFs* nand() noexcept { return nand_.get(); }
    // Error from opening the NAND (SystemConfig::nand_root), empty if none.
    const std::string& nand_error() const noexcept { return nand_error_; }
    // Wii: what the automatic key provision loaded (or why a key file was refused).
    const std::string& key_report() const noexcept { return key_report_; }
    const std::string& key_error() const noexcept { return key_error_; }
    // Wii reset (or power) button: answers the game's STM event hook. False on
    // GameCube or when the game has no hook installed (the press has no effect).
    bool press_console_button(bool power);
    // RTC/SRAM/IPL-ROM chip on EXI channel 0, CS1 (ROM image supplied by the user).
    hw::IplRtcSram& ipl() noexcept { return *ipl_; }
    // Inserts a memory card in slot 0 (A, EXI0) or 1 (B, EXI1); empty image = blank card.
    bool insert_memory_card(unsigned slot, u32 size_mbit, std::vector<u8> image, std::string& error);
    hw::MemoryCard* memory_card(unsigned slot) noexcept { return slot < 2U ? cards_[slot].get() : nullptr; }
    // Called at every VI field start (top = false / bottom = true), after the SI hook.
    void set_field_listener(std::function<void(bool)> f) { field_listener_ = std::move(f); }
    void insert_disc(std::unique_ptr<disc::DiscReader> d) { di_->insert(std::move(d)); }
    const SystemConfig& config() const noexcept { return config_; }
    const Clocks& clocks() const noexcept { return clocks_; }
    // The JIT, when SystemConfig::use_jit was set and the host can run it.
    const ppc::Jit* jit() const noexcept { return jit_.get(); }
    // Diagnostics: host time in scheduler events (device work), ns, and how many
    // times the run loop handed the CPU a slice (one JIT run or interpreter step).
    u64 events_host_ns() const noexcept { return scheduler_.host_ns(); }
    u64 run_slices() const noexcept { return run_slices_; }

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
    // Busy-wait loops: when the CPU runs a short loop (at most 16 instructions back
    // to its start) whose instructions only load from RAM, compare, and branch
    // (no stores, no SPR/time base reads, no MMIO, no system calls), one
    // iteration is run and compared: if it changed nothing (registers, CR, XER,
    // CTR, LR, MSR, no memory or device access other than RAM loads), every
    // following iteration until the next scheduler event, decrementer
    // underflow or the end of the run is the same, so the whole iterations
    // that fit are accounted (time, time base, retired instructions) without
    // being executed. The CPU then continues normally, at the same state and
    // time as if it had run them.
    RunResult run(u64 max_cycles);
    u64 idle_skips() const noexcept { return idle_skips_; }
    u64 idle_skipped_instructions() const noexcept { return idle_skipped_; }
    // PC of a busy loop proven endless: one whole iteration changed no register
    // and wrote nothing, the loop reads no memory (nothing it looks at can
    // change) and MSR[EE] = 0 (no interrupt can take the CPU out). 0: none.
    u32 endless_loop_pc() const noexcept { return endless_loop_pc_; }
    u32 endless_loop_length() const noexcept { return endless_loop_length_; } // instructions
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

    // Boots the inserted Wii disc the way the system menu and IOS leave things,
    // at a high level (see docs/ESTADO.md):
    //   * the game partition (the first of type 0) is opened as /dev/di does:
    //     ticket and TMD become ES's active title; Broadway's owner/group come
    //     from /sys/uid.sys and the TMD (needs SystemConfig::nand_root and wii_keys);
    //   * low memory: the values IOS writes for the IOS the TMD names
    //     (ios/memory_values.hpp), the disc ID (0x00), boot magic, board model,
    //     clocks, video mode (0xCC, from the region letter), the partition's
    //     game ID (0x3180), partition type/offset (0x3194/0x3198), the devkit
    //     boot program version (0x315E) and setting.txt from the NAND (0x3800);
    //   * the apploader is run from the partition (offsets are words);
    //   * the CPU is left at the entry point with the boot BATs plus MEM2
    //     (BAT4 0x90000000, DBAT5 0xD0000000 uncached) and HID4[SBE].
    bool boot_wii_disc(std::string& error);
    ios::DiDevice* di_device() noexcept { return di_dev_; }
    // Wii Bluetooth module and its remotes (one per active remote in the NAND's
    // SYSCONF); bluetooth_error() says why there are none.
    ios::BluetoothDevice* bluetooth() noexcept { return bluetooth_; }
    const std::string& bluetooth_error() const noexcept { return bt_error_; }

    static constexpr u32 kGuestReturn = 0x81300000U;   // never executed
    static constexpr u32 kReportStub = 0x81300004U;    // blr (apploader OSReport)
    static constexpr u32 kApploaderStack = 0x816FFFF0U;
    static constexpr u32 kBi2Address = 0x817FE000U;

private:
    void advance(u64 cycles);
    // Loads and drives the apploader from `d` (header at 0x2440); read offsets are
    // shifted left by `offset_shift` (2 on Wii). Returns the game's entry point.
    std::optional<u32> run_apploader(disc::DiscReader& d, unsigned offset_shift, std::string& error);
    void wii_boot_state();
    void write_default_exception_handlers();
    void watch_video_mode();
    bool sysconf_eurgb60();
    // Tries the busy-wait skip at the CPU's current PC; true if time moved on.
    bool try_idle_skip(u64 end);
    bool idle_candidate(u32 pc, unsigned& length);
    std::array<std::pair<u32, u32>, 64> idle_rejects_{}; // pc -> first instruction word of rejected loops
    u64 idle_skips_{0}, idle_skipped_{0};
    u32 endless_loop_pc_{0}, endless_loop_length_{0};

    SystemConfig config_;
    Clocks clocks_;
    mem::Bus bus_;
    core::Scheduler scheduler_;
    std::unique_ptr<ppc::Cpu> cpu_;
    std::unique_ptr<ppc::Jit> jit_;
    // Guest instructions that can run before anything outside the CPU could change
    // what the next instruction sees (scheduler event, decrementer underflow, end).
    u64 steps_until_next_event(u64 end) const;
    std::unique_ptr<hw::ProcessorInterface> pi_;
    std::unique_ptr<hw::GatherPipe> gather_pipe_;
    std::unique_ptr<hw::PixelEngine> pe_;
    std::unique_ptr<gpu::Gpu> gpu_;
    std::unique_ptr<gpu::EfbAccess> efb_access_;
    std::unique_ptr<hw::CommandProcessor> cp_;
    u64 run_slices_{0};
    std::unique_ptr<hw::MmioSplitter> page_0c006_;
    std::unique_ptr<hw::DiscInterface> di_;
    std::unique_ptr<hw::VideoInterface> vi_;
    std::unique_ptr<hw::SerialInterface> si_;
    std::unique_ptr<hw::AudioInterface> ai_;
    std::unique_ptr<audio::Mixer> mixer_;
    std::unique_ptr<hw::ExternalInterface> exi_;
    std::unique_ptr<hw::IplRtcSram> ipl_;
    std::array<std::unique_ptr<hw::MemoryCard>, 2> cards_;
    std::unique_ptr<hw::MemoryInterface> mi_;
    std::unique_ptr<hw::DspInterface> dsp_;
    std::unique_ptr<hw::HollywoodIpc> hollywood_;
    std::unique_ptr<ios::Kernel> ios_;
    ios::StmEventHook* stm_hook_{nullptr};
    ios::Es* es_{nullptr};
    std::unique_ptr<ios::NandFs> nand_;
    std::string nand_error_;
    std::string key_report_, key_error_;
    ios::DiDevice* di_dev_{nullptr};
    ios::BluetoothDevice* bluetooth_{nullptr};
    std::string bt_error_;
    std::function<void(bool)> field_listener_;
    std::optional<u32> stop_pc_;
    u64 timebase_remainder_{0};
};

} // namespace orca::system
