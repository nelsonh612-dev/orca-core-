#include "orca/hw/disc_interface.hpp"

#include <cstdio>
#include <vector>

namespace orca::hw {

DiscInterface::DiscInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi)
    : bus_(bus), scheduler_(scheduler), pi_(pi) {}

void DiscInterface::update_interrupt() {
    const bool tc = (disr_ & di::DISR_TCINT) != 0U && (disr_ & di::DISR_TCINTMASK) != 0U;
    const bool de = (disr_ & di::DISR_DEINT) != 0U && (disr_ & di::DISR_DEINTMASK) != 0U;
    const bool br = (disr_ & di::DISR_BRKINT) != 0U && (disr_ & di::DISR_BRKINTMASK) != 0U;
    const bool cv = (dicvr_ & di::DICVR_CVRINT) != 0U && (dicvr_ & di::DICVR_CVRINTMASK) != 0U;
    if (tc || de || br || cv) pi_.raise(pi::CAUSE_DI); else pi_.lower(pi::CAUSE_DI);
}

bool DiscInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("DI: only 32-bit accesses are implemented");
    switch (offset) {
    case 0x00: value = disr_; return true;
    case 0x04: value = dicvr_; return true; // cover closed
    case 0x08: case 0x0C: case 0x10: value = cmd_[(offset - 0x08U) / 4U]; return true;
    case 0x14: value = dimar_; return true;
    case 0x18: value = dilength_; return true;
    case 0x1C: value = dicr_; return true;
    case 0x20: value = diimmbuf_; return true;
    default: return fail("DI: register not implemented");
    }
}

bool DiscInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return fail("DI: only 32-bit accesses are implemented");
    const auto v = static_cast<u32>(value);
    switch (offset) {
    case 0x00: {
        if ((v & di::DISR_BRK) != 0U) return fail("DI: break requests are not implemented");
        const u32 masks = di::DISR_DEINTMASK | di::DISR_TCINTMASK | di::DISR_BRKINTMASK;
        const u32 ints = di::DISR_DEINT | di::DISR_TCINT | di::DISR_BRKINT;
        disr_ = (disr_ & ~masks) | (v & masks);
        disr_ &= ~(v & ints); // write 1 to clear
        update_interrupt();
        return true;
    }
    case 0x04:
        dicvr_ = (dicvr_ & ~di::DICVR_CVRINTMASK) | (v & di::DICVR_CVRINTMASK);
        dicvr_ &= ~(v & di::DICVR_CVRINT);
        update_interrupt();
        return true;
    case 0x08: case 0x0C: case 0x10: cmd_[(offset - 0x08U) / 4U] = v; return true;
    case 0x14: dimar_ = v & di::kAddressMask; return true;
    case 0x18: dilength_ = v & ~0x1FU; return true;
    case 0x1C:
        if (busy()) return fail("DI: DICR written while a transfer is in progress");
        dicr_ = v & (di::DICR_TSTART | di::DICR_DMA | di::DICR_RW);
        if ((dicr_ & di::DICR_TSTART) != 0U && !start_command()) { dicr_ = 0; return false; }
        return true;
    case 0x20: diimmbuf_ = v; return true;
    default: return fail("DI: register not implemented");
    }
}

bool DiscInterface::start_command() {
    const u32 command = cmd_[0] >> 24U;
    char buf[96];
    if (command != di::kCommandRead) {
        std::snprintf(buf, sizeof buf, "DI: command 0x%02X not implemented (CMDBUF0=%08X)", command, cmd_[0]);
        return fail(buf);
    }
    if ((dicr_ & di::DICR_DMA) == 0U) return fail("DI: immediate-mode read not implemented");
    if ((dicr_ & di::DICR_RW) != 0U) return fail("DI: writes to the drive are not implemented");
    if (!disc_) return fail("DI: read with no disc inserted");
    if (dilength_ != 0 && !bus_.is_ram(dimar_, dilength_)) return fail("DI: DMA destination is not RAM");
    scheduler_.schedule_in(latency_, "DI transfer", [this](u64, u64) { complete(); });
    return true;
}

void DiscInterface::complete() {
    const u64 offset = static_cast<u64>(cmd_[1]) << 2U;
    std::vector<u8> data(dilength_);
    if (!disc_->read(offset, data)) {
        disr_ |= di::DISR_DEINT; // drive error: read outside the image
    } else {
        (void)bus_.dma_write(dimar_, data);
        dimar_ += dilength_;
        dilength_ = 0;
        disr_ |= di::DISR_TCINT;
    }
    dicr_ &= ~di::DICR_TSTART;
    update_interrupt();
}

} // namespace orca::hw
