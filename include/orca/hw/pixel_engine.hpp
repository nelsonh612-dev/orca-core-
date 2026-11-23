// Orca Core — Pixel Engine CPU-side registers (physical 0x0C001000, 16-bit).
//
// Sources: YAGCD for the base address; Dolphin's VideoCommon/PixelEngine.h and
// PixelEngine.cpp (read as information only) for the register map and the
// token / finish behaviour:
//   0x00 ZCONF  0x02 ALPHACONF  0x04 DSTALPHACONF  0x06 ALPHAMODE  0x08 ALPHAREAD
//        stored as written (they configure CPU access to the EFB, not modelled yet)
//   0x0A CTRL   bit 0 token interrupt enable, bit 1 finish interrupt enable;
//               writing 1 to bit 2 / bit 3 acknowledges the token / finish
//               interrupt (those two bits always read 0)
//   0x0E TOKEN  last token that reached the PE (read only)
// The GPU sets a token (BP 0x47, 0x48 = with interrupt) or "draw done"
// (BP 0x45 with low byte 2). The value and the interrupts are applied
// max(500, GPU cycles so far) CPU cycles later, as Dolphin does in single-core
// mode; a second request before that only updates the pending value.
// PI causes: PE_TOKEN while signalled && enabled, PE_FINISH likewise.
// Bounding-box (0x10-0x16) and performance-counter (0x18-0x2E) reads, writes
// to read-only registers and accesses that are not 16/32 bits are errors.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/memory/bus.hpp"

#include <string>

namespace orca::hw {

namespace pe {
inline constexpr u32 kBase = 0x0C001000U;
inline constexpr u32 ZCONF = 0x00, ALPHACONF = 0x02, DSTALPHACONF = 0x04, ALPHAMODE = 0x06, ALPHAREAD = 0x08,
                     CTRL = 0x0A, TOKEN = 0x0E;
inline constexpr u16 CTRL_TOKEN_ENABLE = 0x1, CTRL_FINISH_ENABLE = 0x2, CTRL_TOKEN_ACK = 0x4, CTRL_FINISH_ACK = 0x8;
inline constexpr u64 kMinSignalDelay = 500;
} // namespace pe

class PixelEngine final : public mem::MmioDevice {
public:
    PixelEngine(core::Scheduler& scheduler, ProcessorInterface& pi) : sched_(scheduler), pi_(pi) {}
    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "PixelEngine"; }
    std::string mmio_error() const override { return error_; }

    // Called by the GPU while it processes BP 0x45/0x47/0x48.
    void set_token(u16 token, bool interrupt, u64 gpu_cycles);
    void set_finish(u64 gpu_cycles);

    u16 token() const noexcept { return token_; }
    u16 config(u32 offset) const noexcept { return offset <= pe::ALPHAREAD ? conf_[offset / 2U] : 0; }

private:
    bool read16(u32 offset, u16& v);
    bool write16(u32 offset, u16 v);
    void raise_event(u64 gpu_cycles);
    void update_interrupts();
    bool fail(std::string msg) { error_ = std::move(msg); return false; }

    core::Scheduler& sched_;
    ProcessorInterface& pi_;
    u16 conf_[5]{};
    u16 ctrl_{0};
    u16 token_{0}, token_pending_{0};
    bool token_irq_pending_{false}, finish_irq_pending_{false}, event_raised_{false};
    bool token_signal_{false}, finish_signal_{false};
    std::string error_;
};

} // namespace orca::hw
