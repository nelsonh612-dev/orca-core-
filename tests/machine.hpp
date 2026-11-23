// Test harness: a CPU attached to a bus with GameCube MEM1 mapped.
#pragma once
#include "orca/memory/console_maps.hpp"
#include "orca/ppc/cpu.hpp"

#include <initializer_list>
#include <memory>

namespace orca::test {

struct Machine {
    mem::Bus bus;
    std::unique_ptr<ppc::Cpu> cpu;

    explicit Machine(u32 pc = 0x1000) {
        mem::map_gamecube_ram(bus);
        cpu = std::make_unique<ppc::Cpu>(bus, ppc::CpuConfig{0xABCD1234U}); // arbitrary test PVR
        cpu->state().pc = pc;
    }
    ppc::CpuState& s() { return cpu->state(); }

    void code(u32 address, std::initializer_list<u32> words) {
        for (u32 w : words) { bus.write32(address, w); address += 4; }
    }
    // Places `ins` at the current PC and executes exactly one step.
    ppc::StepResult exec(u32 ins) {
        bus.write32(cpu->state().pc, ins);
        return cpu->step();
    }
    u32 r32(u32 a) { u32 v = 0; bus.read32(a, v); return v; }
    u16 r16(u32 a) { u16 v = 0; bus.read16(a, v); return v; }
    u8 r8(u32 a) { u8 v = 0; bus.read8(a, v); return v; }
};

} // namespace orca::test
