// Every hardware register that Dolphin handles can be read on Orca's Wii system,
// at every width Dolphin accepts, unless Orca stops on it on purpose (listed
// below with the reason). Catches a game reading a register Orca lacks before a
// real game finds it.
#include "orca/system/system.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <string>

using namespace orca;

namespace {
struct Reg { const char* dev; u32 addr; unsigned widths; };
const Reg kRegs[] = {
#include "mmio_registers.inc"
};
// Explicit stops kept on purpose
bool expected_stop(u32 addr) {
    return addr == 0x0C00102CU || addr == 0x0C00102EU; // PE EFB-copy clock counter: no timing model
}
} // namespace

TEST_CASE(every_dolphin_register_reads_on_the_wii_system) {
    system::SystemConfig cfg;
    cfg.console = system::Console::Wii;
    cfg.auto_keys = false;
    system::System sys(cfg);
    mem::Bus& bus = sys.bus();
    unsigned checked = 0, refused = 0;
    for (const Reg& r : kRegs) {
        for (const unsigned bytes : {1U, 2U, 4U}) {
            if ((r.widths & bytes) == 0U || r.addr % bytes != 0U) continue;
            mem::BusStatus st{};
            if (bytes == 1U) { u8 v = 0; st = bus.read8(r.addr, v); }
            else if (bytes == 2U) { u16 v = 0; st = bus.read16(r.addr, v); }
            else { u32 v = 0; st = bus.read32(r.addr, v); }
            ++checked;
            const bool ok = st == mem::BusStatus::Ok;
            if (!ok && !expected_stop(r.addr)) {
                const auto* d = bus.mmio_device_at(r.addr);
                std::printf("    %s %08X (%u-bit): %s\n", r.dev, r.addr, bytes * 8U,
                            d != nullptr ? d->mmio_error().c_str() : "no device mapped");
            }
            CHECK(ok || expected_stop(r.addr));
            if (!ok) ++refused;
        }
    }
    std::printf("[mmio_coverage] %u register reads, %u refused on purpose\n", checked, refused);
    CHECK(checked > 400U);
}

TEST_MAIN("mmio_coverage")
