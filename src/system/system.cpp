#include "orca/system/system.hpp"

#include "orca/memory/console_maps.hpp"

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

RunResult System::run(u64 max_cycles) {
    RunResult r;
    const u64 start = scheduler_.now();
    const u64 end = start + max_cycles;
    const u64 retired0 = cpu_->instructions_retired();
    const u64 cpi = config_.cycles_per_instruction;
    while (scheduler_.now() < end) {
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
