// Test-harness-only MMIO device (not part of any console map), physical 0x0E000000:
//   +0 EMIT: append a 32-bit value to the log    +4 EXIT: record the code and halt the CPU
#pragma once
#include "orca/memory/bus.hpp"
#include "orca/ppc/cpu.hpp"

#include <vector>

namespace orca::test {

struct TestPort final : mem::MmioDevice {
    static constexpr u32 kPhysical = 0x0E000000U;
    std::vector<u32> log;
    bool exited = false;
    u32 exit_code = 0;
    ppc::Cpu* cpu = nullptr;
    bool mmio_read(u32, unsigned, u64&) override { return false; }
    bool mmio_write(u32 off, unsigned size, u64 v) override {
        if (size != 4) return false;
        if (off == 0) { log.push_back(static_cast<u32>(v)); return true; }
        if (off == 4) { exited = true; exit_code = static_cast<u32>(v); cpu->state().halted = true; return true; }
        return false;
    }
    const char* mmio_name() const noexcept override { return "OrcaTestPort"; }
};

} // namespace orca::test
