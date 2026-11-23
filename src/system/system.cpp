#include "orca/system/system.hpp"

#include "orca/memory/console_maps.hpp"

#include <array>
#include <cstdio>
#include <vector>

namespace orca::system {

System::System(SystemConfig config) : config_(config), clocks_(clocks_for(config.console)) {
    if (config_.cycles_per_instruction == 0U) config_.cycles_per_instruction = 1U;
    if (config_.console == Console::GameCube) mem::map_gamecube_ram(bus_);
    else mem::map_wii_ram(bus_);
    cpu_ = std::make_unique<ppc::Cpu>(bus_, ppc::CpuConfig{config_.pvr});
    pi_ = std::make_unique<hw::ProcessorInterface>(bus_, [this](bool level) { cpu_->set_external_interrupt(level); });
    gather_pipe_ = std::make_unique<hw::GatherPipe>(*pi_);
    bus_.map_mmio("PI", hw::pi::kBase, 0x1000, *pi_);
    bus_.map_mmio("GatherPipe", hw::pi::kGatherPipeBase, 0x1000, *gather_pipe_);
    di_ = std::make_unique<hw::DiscInterface>(bus_, scheduler_, *pi_);
    page_0c006_ = std::make_unique<hw::MmioSplitter>();
    vi_ = std::make_unique<hw::VideoInterface>(scheduler_, *pi_, clocks_.cpu_hz);
    si_ = std::make_unique<hw::SerialInterface>(scheduler_, *pi_, vi_.get());
    ai_ = std::make_unique<hw::AudioInterface>(scheduler_, *pi_, clocks_.cpu_hz);
    vi_->set_field_listener([this](bool bottom) {
        si_->on_video_field();
        if (field_listener_) field_listener_(bottom);
    });
    page_0c006_->attach(0x000, 0x40, *di_);
    page_0c006_->attach(hw::si::kOffsetInPage, 0x100, *si_);
    page_0c006_->attach(hw::ai::kOffsetInPage, 0x20, *ai_); // EXI (0x800) not implemented yet
    bus_.map_mmio("DI/SI/EXI/AI", hw::di::kBase, 0x1000, *page_0c006_);
    bus_.map_mmio("VI", hw::vi::kBase, 0x1000, *vi_);
}

System::~System() = default;

void System::hle_boot(u32 entry) {
    ppc::CpuState& s = cpu_->state();
    s.ibat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[1] = {0xC0001FFFU, 0x0000002AU}; // WIMG = 0101 (I, G), PP = 10
    s.msr = ppc::MSR_FP | ppc::MSR_IR | ppc::MSR_DR;
    s.pc = entry;
    (void)bus_.write32(0x20, 0x0D15EA5EU);
    (void)bus_.write32(0x28, mem::kMem1Size);
}

bool System::load_executable(std::span<const u8> file, std::string& error, bool boot) {
    const auto image = loader::parse_executable(file, error);
    if (!image) return false;
    if (!loader::load_into(bus_, *image, error)) return false;
    if (boot) hle_boot(image->entry);
    return true;
}

void System::advance(u64 cycles) {
    timebase_remainder_ += cycles;
    const u64 ticks = timebase_remainder_ / clocks_.cpu_cycles_per_timebase_tick;
    timebase_remainder_ %= clocks_.cpu_cycles_per_timebase_tick;
    if (ticks != 0U) cpu_->advance_timebase(ticks);
    scheduler_.advance_to(scheduler_.now() + cycles);
}

RunResult System::run_until(u32 stop_pc, u64 max_cycles) {
    stop_pc_ = stop_pc;
    const RunResult r = run(max_cycles);
    stop_pc_.reset();
    return r;
}

std::optional<u32> System::call_guest(u32 function, std::initializer_list<u32> args, u64 max_cycles, std::string& error) {
    ppc::CpuState& s = cpu_->state();
    unsigned reg = 3;
    for (u32 a : args) s.gpr[reg++] = a;
    s.lr = kGuestReturn;
    s.pc = function;
    const RunResult r = run_until(kGuestReturn, max_cycles);
    if (!r.reached_stop_pc) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "guest call to %08X did not return: %s at pc=%08X (%s)", function,
                      ppc::to_string(r.stop), s.pc, cpu_->last_stop().reason.c_str());
        error = buf;
        return std::nullopt;
    }
    return s.gpr[3];
}

bool System::boot_disc(std::string& error) {
    disc::DiscReader* d = di_->disc();
    if (d == nullptr) { error = "boot_disc: no disc inserted"; return false; }
    const auto header = disc::parse_header(*d, error);
    if (!header) return false;
    const auto app = disc::parse_apploader_header(*d, error);
    if (!app) return false;
    const u32 app_bytes = app->size + app->trailer_size;
    if (disc::kApploaderLoadAddress + app_bytes > kGuestReturn) { error = "boot_disc: apploader overlaps the HLE stub area"; return false; }

    // Low-memory globals
    std::array<u8, 0x20> id{};
    d->read(0, id);
    (void)bus_.dma_write(0, id);
    const bool gc = config_.console == Console::GameCube;
    (void)bus_.write32(0x20, 0x0D15EA5EU);
    (void)bus_.write32(0x28, mem::kMem1Size);
    (void)bus_.write32(0xF8, gc ? 162000000U : 243000000U);
    (void)bus_.write32(0xFC, static_cast<u32>(clocks_.cpu_hz));
    std::vector<u8> bi2(0x2000);
    if (!d->read(0x440, bi2)) { error = "boot_disc: cannot read bi2.bin"; return false; }
    (void)bus_.dma_write(loader::virtual_to_physical(kBi2Address), bi2);
    (void)bus_.write32(0xF4, kBi2Address);

    // Apploader image and HLE stubs
    std::vector<u8> code(app_bytes);
    if (!d->read(disc::kApploaderOffset + 0x20U, code)) { error = "boot_disc: cannot read the apploader"; return false; }
    (void)bus_.dma_write(loader::virtual_to_physical(disc::kApploaderLoadAddress), code);
    const u32 stub_pa = loader::virtual_to_physical(kGuestReturn);
    (void)bus_.write32(stub_pa, 0x4E800020U);      // blr (never executed: the run loop stops here)
    (void)bus_.write32(stub_pa + 4U, 0x4E800020U); // report stub: blr
    hle_boot(0);
    cpu_->state().gpr[1] = kApploaderStack;

    constexpr u64 kBudget = 2000000000ULL;
    const u32 slots = kGuestReturn + 0x10U, args = kGuestReturn + 0x20U;
    if (!call_guest(app->entry, {slots, slots + 4U, slots + 8U}, kBudget, error)) return false;
    u32 init = 0, main_fn = 0, close = 0;
    bus_.read32(loader::virtual_to_physical(slots), init);
    bus_.read32(loader::virtual_to_physical(slots + 4U), main_fn);
    bus_.read32(loader::virtual_to_physical(slots + 8U), close);
    if (!call_guest(init, {kReportStub}, kBudget, error)) return false;
    for (int i = 0;; ++i) {
        if (i > 100000) { error = "boot_disc: apploader main() never finished"; return false; }
        const auto more = call_guest(main_fn, {args, args + 4U, args + 8U}, kBudget, error);
        if (!more) return false;
        u32 dst = 0, size = 0, offset = 0;
        bus_.read32(loader::virtual_to_physical(args), dst);
        bus_.read32(loader::virtual_to_physical(args + 4U), size);
        bus_.read32(loader::virtual_to_physical(args + 8U), offset);
        if (size != 0U) {
            const u32 pa = loader::virtual_to_physical(dst);
            std::vector<u8> chunk(size);
            if (!bus_.is_ram(pa, size)) { error = "boot_disc: apploader requested a read outside RAM"; return false; }
            if (!d->read(offset, chunk)) { error = "boot_disc: apploader requested a read outside the disc"; return false; }
            (void)bus_.dma_write(pa, chunk);
            (void)bus_.write32(loader::virtual_to_physical(args + 4U), 0); // consumed
        }
        if (*more == 0U) break;
    }
    const auto entry = call_guest(close, {}, kBudget, error);
    if (!entry) return false;
    hle_boot(*entry);
    return true;
}

RunResult System::run(u64 max_cycles) {
    RunResult r;
    const u64 start = scheduler_.now();
    const u64 end = start + max_cycles;
    const u64 retired0 = cpu_->instructions_retired();
    const u64 cpi = config_.cycles_per_instruction;
    while (scheduler_.now() < end) {
        if (stop_pc_ && cpu_->state().pc == *stop_pc_) { r.reached_stop_pc = true; break; }
        const ppc::StepResult s = cpu_->step();
        if (s == ppc::StepResult::Executed || s == ppc::StepResult::ExceptionTaken) {
            advance(cpi);
            continue;
        }
        if (s == ppc::StepResult::Idle) {
            const auto next = scheduler_.next_event_time();
            if (!next) { r.stop = s; break; } // nothing can ever wake the CPU
            const u64 target = std::min(std::max(*next, scheduler_.now()), end);
            advance(target - scheduler_.now());
            if (target == end && *next > end) break;
            continue;
        }
        r.stop = s;
        break;
    }
    r.cycles = scheduler_.now() - start;
    r.instructions = cpu_->instructions_retired() - retired0;
    return r;
}

} // namespace orca::system
