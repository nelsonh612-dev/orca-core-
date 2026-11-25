// Orca Core — Command Processor and GPU FIFO. See command_processor.hpp.
#include "orca/hw/command_processor.hpp"
#include "orca/core/host_timer.hpp"

#include <array>
#include <cstdio>

namespace orca::hw {

CommandProcessor::CommandProcessor(mem::Bus& bus, ProcessorInterface& pi, gpu::Gpu& gpu, bool wii)
    : bus_(bus), pi_(pi), gpu_(gpu), hi_mask_(wii ? 0x1FFFU : 0x03FFU) {}

u32* CommandProcessor::reg32(u32 offset) {
    switch (offset & ~3U) {
    case cp::FIFO_BASE: return &base_;
    case cp::FIFO_END: return &end_;
    case cp::FIFO_HI_WM: return &hi_wm_;
    case cp::FIFO_LO_WM: return &lo_wm_;
    case cp::FIFO_RW_DIST: return &dist_;
    case cp::FIFO_WP: return &wp_;
    case cp::FIFO_RP: return &rp_;
    case cp::FIFO_BP: return &bp_;
    default: return nullptr;
    }
}

void CommandProcessor::update_status_and_interrupt() {
    // Dolphin: SetCPStatusFromGPU (breakpoint) + SetCPStatusFromCPU (watermarks).
    bp_hit_ = at_breakpoint();
    overflow_ = dist_ > hi_wm_;
    underflow_ = dist_ < lo_wm_;
    const bool irq = ((bp_hit_ && (ctrl_ & cp::CTRL_BP_INT) != 0U) || (overflow_ && (ctrl_ & cp::CTRL_OVF_INT) != 0U) ||
                      (underflow_ && (ctrl_ & cp::CTRL_UNDF_INT) != 0U)) &&
                     (ctrl_ & cp::CTRL_READ) != 0U;
    if (irq) pi_.raise(pi::CAUSE_CP); else pi_.lower(pi::CAUSE_CP);
}

bool CommandProcessor::read16(u32 offset, u16& v) {
    if (u32* r = reg32(offset); r != nullptr && (offset & 1U) == 0U) {
        v = (offset & 2U) != 0U ? static_cast<u16>(*r >> 16) : static_cast<u16>(*r);
        return true;
    }
    switch (offset) {
    case cp::STATUS:
        update_status_and_interrupt();
        v = static_cast<u16>((overflow_ ? cp::ST_OVERFLOW : 0U) | (underflow_ ? cp::ST_UNDERFLOW : 0U) |
                             ((dist_ == 0U || rp_ == wp_) ? cp::ST_READ_IDLE : 0U) |
                             ((dist_ == 0U || at_breakpoint() || (ctrl_ & cp::CTRL_READ) == 0U) ? cp::ST_CMD_IDLE : 0U) |
                             (bp_hit_ ? cp::ST_BREAKPOINT : 0U));
        return true;
    case cp::CTRL: v = ctrl_; return true;
    case cp::CLEAR: v = 0; return true;
    case cp::PERF_SELECT: v = perf_select_; return true;
    case cp::UNK_0A: v = unk_0a_; return true;
    case 0x64: v = 4; return true;
    default:
        if (offset >= 0x40U && offset <= 0x5AU && (offset & 1U) == 0U) { v = 0; return true; }
        break;
    }
    char b[64];
    std::snprintf(b, sizeof b, "CP: no register at offset %02X", offset);
    return fail(b);
}

bool CommandProcessor::write16(u32 offset, u16 v) {
    if (u32* r = reg32(offset); r != nullptr && (offset & 1U) == 0U) {
        if ((offset & 2U) != 0U) *r = (*r & 0x0000FFFFU) | (static_cast<u32>(v & hi_mask_) << 16);
        else *r = (*r & 0xFFFF0000U) | (v & 0xFFE0U);
        if ((offset & ~3U) == cp::FIFO_RW_DIST && (offset & 2U) != 0U) run_gpu(); // Dolphin runs the GPU here
        return stopped() ? fail(stop_reason()) : true;
    }
    switch (offset) {
    case cp::CTRL:
        ctrl_ = static_cast<u16>(v & 0x3FU);
        run_gpu();
        return stopped() ? fail(stop_reason()) : true;
    case cp::CLEAR: run_gpu(); return stopped() ? fail(stop_reason()) : true;
    case cp::PERF_SELECT: perf_select_ = static_cast<u16>(v & 0x7U); return true;
    case cp::UNK_0A: unk_0a_ = static_cast<u16>(v & 0xFFU); return true;
    default: break;
    }
    char b[80];
    std::snprintf(b, sizeof b, "CP: write to offset %02X (read-only or unimplemented)", offset);
    return fail(b);
}

bool CommandProcessor::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size == 2U) { u16 v = 0; if (!read16(offset, v)) return false; value = v; return true; }
    if (size == 4U) {
        u16 h = 0, l = 0;
        if (!read16(offset, h) || !read16(offset + 2U, l)) return false;
        value = (static_cast<u32>(h) << 16) | l;
        return true;
    }
    return fail("CP: only 16- and 32-bit accesses are implemented");
}

bool CommandProcessor::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U) return write16(offset, static_cast<u16>(value));
    if (size == 4U) return write16(offset, static_cast<u16>(value >> 16)) && write16(offset + 2U, static_cast<u16>(value));
    return fail("CP: only 16- and 32-bit accesses are implemented");
}

void CommandProcessor::on_gather_burst() {
    update_status_and_interrupt();
    if ((ctrl_ & cp::CTRL_LINK) == 0U) { run_gpu(); return; }
    bool wrapped = false;
    if (wp_ == end_) { wp_ = base_; wrapped = true; } else { wp_ += 32U; }
    if ((ctrl_ & cp::CTRL_READ) != 0U) pi_.set_fifo_from_cp(wp_, base_, end_, wrapped);
    dist_ += 32U;
    run_gpu();
}

void CommandProcessor::run_gpu() {
    if (running_) return; // a GPU command can touch CP registers only through the CPU; guard anyway
    running_ = true;
    const core::HostTimer timer(host_ns_);
    while ((ctrl_ & cp::CTRL_READ) != 0U && dist_ != 0U && !at_breakpoint() && !stopped()) {
        std::array<u8, 32> chunk{};
        char b[96];
        if (dist_ < 32U) {
            std::snprintf(b, sizeof b, "CP: FIFO read-write distance %08X is not a multiple of 32", dist_);
            fifo_failed_ = true;
            fifo_error_ = b;
            break;
        }
        if (bus_.dma_read(rp_, chunk) != mem::BusStatus::Ok) {
            std::snprintf(b, sizeof b, "CP: FIFO read pointer %08X is outside RAM", rp_);
            fifo_failed_ = true;
            fifo_error_ = b;
            break;
        }
        if (rp_ == end_) rp_ = base_; else rp_ += 32U;
        dist_ -= 32U;
        gpu_.feed(chunk);
    }
    running_ = false;
    update_status_and_interrupt();
}

} // namespace orca::hw
