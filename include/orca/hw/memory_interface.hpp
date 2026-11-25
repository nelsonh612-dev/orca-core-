// Orca Core — Memory Interface (MI), physical 0x0C004000 (window 0x80).
//
// Registers (YAGCD chapter 5, "Memory Interface"); 16-bit register file,
// 16- and 32-bit accesses:
//   0x00-0x0E  protected regions 0-3 (32 bits each: bits 16-31 page lo,
//              bits 0-15 page hi; page = physical address >> 10)
//   0x10       protection type: 2 bits per channel (0 = no access,
//              1 = read only, 2 = write only, 3 = read/write)
//   0x1C       interrupt mask (bits 0-3 MEM0-3, bit 4 all)
//   0x1E       interrupt cause (write 1 to clear)
//   0x20       status (bit 1 = an MI interrupt is asserted)
//   0x22/0x24  address of the failed access (read-only)
//   0x32-0x58  timer pairs: writing any value resets the timer
// (mask & cause) raises the PI MEM cause.
//
// Protection is NOT enforced. The register values are kept, and every write
// that leaves a channel with type != 3 adds a host diagnostic to warnings()
// (orca_run prints them), so the missing enforcement is never silent.
// The reset value of the type register is not documented; it starts at 0xFF
// (every channel read/write), which needs hardware verification.
// Device errors: reading the timers (what they count is not documented) and
// offsets with no documented register.
#pragma once
#include "orca/hw/processor_interface.hpp"

#include <array>
#include <string>
#include <vector>

namespace orca::hw {

namespace mi {
inline constexpr u32 kBase = 0x0C004000U;
inline constexpr u32 kSize = 0x80U;
inline constexpr u32 REGION0 = 0x00, TYPE = 0x10, IMASK = 0x1C, ICAUSE = 0x1E, STATUS = 0x20, ADDRLO = 0x22,
                     ADDRHI = 0x24, TIMER_FIRST = 0x32, TIMER_LAST = 0x58,
                     UNKNOWN2 = 0x5A;
} // namespace mi

class MemoryInterface final : public mem::MmioDevice {
public:
    explicit MemoryInterface(ProcessorInterface& pi) : pi_(pi) { regs_[mi::TYPE / 2U] = 0xFFU; }
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    u16 reg(u32 offset) const noexcept { return regs_[offset / 2U]; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "MI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    bool read16(u32 offset, u16& v);
    bool write16(u32 offset, u16 v);
    void note_protection();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    ProcessorInterface& pi_;
    std::array<u16, mi::kSize / 2U> regs_{};
    std::string last_error_;
    std::vector<std::string> warnings_;
};

} // namespace orca::hw
