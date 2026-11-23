// Orca Core — Pixel Engine CPU-side registers. See pixel_engine.hpp.
#include "orca/hw/pixel_engine.hpp"

#include <algorithm>
#include <cstdio>

namespace orca::hw {

bool PixelEngine::read16(u32 offset, u16& v) {
    if (offset <= pe::ALPHAREAD && (offset & 1U) == 0U) { v = conf_[offset / 2U]; return true; }
    if (offset == pe::CTRL) { v = ctrl_; return true; }
    if (offset == pe::TOKEN) { v = token_; return true; }
    char b[112];
    if (offset >= 0x10U && offset <= 0x2AU && (offset & 1U) == 0U && !stats_.tracked)
        return fail("PE: bounding box / performance counters are not tracked by the active GPU backend");
    if (offset >= 0x10U && offset <= 0x2AU && (offset & 1U) == 0U && sync_) {
        std::string e;
        if (!sync_(offset <= 0x16U, e)) return fail("PE: " + e);
    }
    if (offset >= 0x10U && offset <= 0x16U && (offset & 1U) == 0U) { v = stats_.bbox[(offset - 0x10U) / 2U]; return true; }
    if (offset >= 0x18U && offset <= 0x2AU && (offset & 1U) == 0U) {
        // Order (Dolphin): ZCOMPLOC in, ZCOMPLOC out, ZCOMP in, ZCOMP out, blend in; each low then high.
        const u32 q = stats_.quads[(offset - 0x18U) / 4U];
        v = static_cast<u16>((offset & 2U) != 0U ? q >> 16 : q & 0xFFFFU);
        return true;
    }
    if (offset == 0x2CU || offset == 0x2EU)
        std::snprintf(b, sizeof b, "PE: EFB copy clock counter %02X has no timing model", offset);
    else
        std::snprintf(b, sizeof b, "PE: no register at offset %02X", offset);
    return fail(b);
}

bool PixelEngine::write16(u32 offset, u16 v) {
    if (offset <= pe::ALPHAREAD && (offset & 1U) == 0U) { conf_[offset / 2U] = v; return true; }
    if (offset == pe::CTRL) {
        if ((v & pe::CTRL_TOKEN_ACK) != 0U) token_signal_ = false;
        if ((v & pe::CTRL_FINISH_ACK) != 0U) finish_signal_ = false;
        ctrl_ = static_cast<u16>(v & (pe::CTRL_TOKEN_ENABLE | pe::CTRL_FINISH_ENABLE));
        update_interrupts();
        return true;
    }
    char b[80];
    std::snprintf(b, sizeof b, "PE: write to offset %02X (read-only or unimplemented)", offset);
    return fail(b);
}

bool PixelEngine::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size == 2U) { u16 v = 0; if (!read16(offset, v)) return false; value = v; return true; }
    if (size == 4U) {
        u16 h = 0, l = 0;
        if (!read16(offset, h) || !read16(offset + 2U, l)) return false;
        value = (static_cast<u32>(h) << 16) | l;
        return true;
    }
    return fail("PE: only 16- and 32-bit accesses are implemented");
}

bool PixelEngine::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U) return write16(offset, static_cast<u16>(value));
    if (size == 4U) return write16(offset, static_cast<u16>(value >> 16)) && write16(offset + 2U, static_cast<u16>(value));
    return fail("PE: only 16- and 32-bit accesses are implemented");
}

void PixelEngine::update_interrupts() {
    if (token_signal_ && (ctrl_ & pe::CTRL_TOKEN_ENABLE) != 0U) pi_.raise(pi::CAUSE_PE_TOKEN); else pi_.lower(pi::CAUSE_PE_TOKEN);
    if (finish_signal_ && (ctrl_ & pe::CTRL_FINISH_ENABLE) != 0U) pi_.raise(pi::CAUSE_PE_FINISH); else pi_.lower(pi::CAUSE_PE_FINISH);
}

void PixelEngine::raise_event(u64 gpu_cycles) {
    if (event_raised_) return;
    event_raised_ = true;
    sched_.schedule_in(std::max(pe::kMinSignalDelay, gpu_cycles), "PE token/finish", [this](u64, u64) {
        event_raised_ = false;
        token_ = token_pending_;
        if (token_irq_pending_) { token_irq_pending_ = false; token_signal_ = true; }
        if (finish_irq_pending_) { finish_irq_pending_ = false; finish_signal_ = true; }
        update_interrupts();
    });
}

void PixelEngine::set_token(u16 token, bool interrupt, u64 gpu_cycles) {
    token_pending_ = token;
    token_irq_pending_ = token_irq_pending_ || interrupt;
    raise_event(gpu_cycles);
}

void PixelEngine::set_finish(u64 gpu_cycles) {
    finish_irq_pending_ = true;
    raise_event(gpu_cycles);
}

} // namespace orca::hw
