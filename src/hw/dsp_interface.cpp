#include "orca/hw/dsp_interface.hpp"

#include <cstdio>
#include <cstring>

namespace orca::hw {

DspInterface::DspInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi, bool has_aram)
    : bus_(bus), scheduler_(scheduler), pi_(pi) {
    if (has_aram) aram_.assign(dsp::kAramSize, 0U);
}

void DspInterface::update_interrupt() {
    const bool a = (csr_ & dsp::CSR_AIDINT) != 0U && (csr_ & dsp::CSR_AIDINTMASK) != 0U;
    const bool r = (csr_ & dsp::CSR_ARINT) != 0U && (csr_ & dsp::CSR_ARINTMASK) != 0U;
    const bool d = (csr_ & dsp::CSR_DSPINT) != 0U && (csr_ & dsp::CSR_DSPINTMASK) != 0U;
    if (a || r || d) pi_.raise(pi::CAUSE_DSP); else pi_.lower(pi::CAUSE_DSP);
}

void DspInterface::dsp_send_mail(u32 mail) { mail_to_cpu_ = mail | 0x80000000U; }

bool DspInterface::read16(u32 offset, u16& v) {
    switch (offset) {
    case dsp::DSPMBH: v = static_cast<u16>(mail_to_dsp_ >> 16); return true;
    case dsp::DSPMBL: v = static_cast<u16>(mail_to_dsp_); return true;
    case dsp::CPUMBH: v = static_cast<u16>(mail_to_cpu_ >> 16); return true;
    case dsp::CPUMBL:
        v = static_cast<u16>(mail_to_cpu_);
        mail_to_cpu_ &= 0x7FFFFFFFU; // reading the low half consumes the mail
        return true;
    case dsp::CSR: v = csr_; return true;
    case dsp::AR_SIZE: v = ar_size_; return true;
    case dsp::AR_MODE: v = ar_mode_; return true;
    case dsp::AR_REFRESH: v = ar_refresh_; return true;
    case dsp::MMADDR_H: v = static_cast<u16>(mmaddr_ >> 16); return true;
    case dsp::MMADDR_L: v = static_cast<u16>(mmaddr_); return true;
    case dsp::ARADDR_H: v = static_cast<u16>(araddr_ >> 16); return true;
    case dsp::ARADDR_L: v = static_cast<u16>(araddr_); return true;
    case dsp::CNT_H: v = static_cast<u16>(cnt_ >> 16); return true;
    case dsp::CNT_L: v = static_cast<u16>(cnt_); return true;
    case dsp::AIDMA_H: v = aidma_h_; return true;
    case dsp::AIDMA_L: v = aidma_l_; return true;
    case dsp::AIDMA_CTRL: v = aidma_ctrl_; return true;
    case dsp::AIDMA_LEFT: v = 0; return true; // playback never starts (see header)
    default: return fail("DSP: register not implemented");
    }
}

bool DspInterface::write16(u32 offset, u16 v) {
    switch (offset) {
    case dsp::DSPMBH: mail_to_dsp_ = (mail_to_dsp_ & 0x0000FFFFU) | (static_cast<u32>(v) << 16); return true;
    case dsp::DSPMBL: {
        mail_to_dsp_ = (mail_to_dsp_ & 0xFFFF0000U) | v;
        if ((csr_ & dsp::CSR_HALT) == 0U) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "DSP: mail 0x%08X sent to a running DSP, but the DSP core is not emulated",
                          mail_to_dsp_);
            return fail(buf);
        }
        return true; // a halted DSP leaves the mail pending
    }
    case dsp::CPUMBH: case dsp::CPUMBL: return fail("DSP: the CPU mailbox is read-only");
    case dsp::CSR: {
        if ((v & dsp::CSR_PIINT) != 0U) return fail("DSP: PIINT (interrupt to the DSP) needs the DSP core, not emulated");
        const u16 rw = dsp::CSR_HALT | dsp::CSR_AIDINTMASK | dsp::CSR_ARINTMASK | dsp::CSR_DSPINTMASK | dsp::CSR_BIT11;
        const u16 w1c = dsp::CSR_AIDINT | dsp::CSR_ARINT | dsp::CSR_DSPINT;
        csr_ = static_cast<u16>((csr_ & ~rw) | (v & rw));
        csr_ = static_cast<u16>(csr_ & ~(v & w1c));
        if ((v & dsp::CSR_RES) != 0U) { // reset completes at once (model)
            mail_to_dsp_ = 0;
            mail_to_cpu_ = 0;
        }
        update_interrupt();
        return true;
    }
    case dsp::AR_SIZE: ar_size_ = v; return true;
    case dsp::AR_MODE: ar_mode_ = v; return true;
    case dsp::AR_REFRESH: ar_refresh_ = v; return true;
    case dsp::MMADDR_H: case dsp::MMADDR_L: case dsp::ARADDR_H: case dsp::ARADDR_L: case dsp::CNT_H:
        if (aram_busy()) return fail("DSP: ARAM DMA registers written during a transfer");
        if (offset == dsp::MMADDR_H) mmaddr_ = (mmaddr_ & 0xFFFFU) | (static_cast<u32>(v) << 16);
        else if (offset == dsp::MMADDR_L) mmaddr_ = (mmaddr_ & 0xFFFF0000U) | (v & 0xFFE0U);
        else if (offset == dsp::ARADDR_H) araddr_ = (araddr_ & 0xFFFFU) | (static_cast<u32>(v) << 16);
        else if (offset == dsp::ARADDR_L) araddr_ = (araddr_ & 0xFFFF0000U) | (v & 0xFFE0U);
        else cnt_ = (cnt_ & 0xFFFFU) | (static_cast<u32>(v) << 16);
        return true;
    case dsp::CNT_L:
        if (aram_busy()) return fail("DSP: ARAM DMA registers written during a transfer");
        cnt_ = (cnt_ & 0xFFFF0000U) | (v & 0xFFE0U);
        return start_aram_dma();
    case dsp::AIDMA_H: aidma_h_ = v; return true;
    case dsp::AIDMA_L: aidma_l_ = static_cast<u16>(v & 0xFFE0U); return true;
    case dsp::AIDMA_CTRL:
        if ((v & 0x8000U) != 0U) return fail("DSP: AI DMA playback is not implemented (its timing is not confirmed)");
        aidma_ctrl_ = v;
        return true;
    default: return fail("DSP: register not implemented");
    }
}

bool DspInterface::start_aram_dma() {
    char buf[160];
    const u32 len = cnt_ & 0x7FFFFFE0U;
    if (aram_.empty()) return fail("DSP: ARAM DMA on a console without ARAM (Wii)");
    if (len == 0U) return fail("DSP: ARAM DMA with length 0 (behaviour undocumented)");
    if (static_cast<u64>(araddr_) + len > aram_.size()) {
        std::snprintf(buf, sizeof buf, "DSP: ARAM DMA range %08X+%X is outside the 16 MiB ARAM", araddr_, len);
        return fail(buf);
    }
    if (!bus_.is_ram(mmaddr_, len)) {
        std::snprintf(buf, sizeof buf, "DSP: ARAM DMA main-memory range %08X+%X is not RAM", mmaddr_, len);
        return fail(buf);
    }
    csr_ |= dsp::CSR_DMABUSY;
    scheduler_.schedule_in(aram_cycles_per_unit_ * (len / 32U), "ARAM DMA", [this](u64, u64) { complete_aram_dma(); });
    return true;
}

void DspInterface::complete_aram_dma() {
    const u32 len = cnt_ & 0x7FFFFFE0U;
    if ((cnt_ & dsp::kCntToMain) != 0U)
        (void)bus_.dma_write(mmaddr_, std::span<const u8>(aram_.data() + araddr_, len));
    else
        (void)bus_.dma_read(mmaddr_, std::span<u8>(aram_.data() + araddr_, len));
    mmaddr_ += len;
    araddr_ += len;
    cnt_ &= dsp::kCntToMain; // count reaches 0; the direction bit is kept (model)
    csr_ = static_cast<u16>((csr_ & ~dsp::CSR_DMABUSY) | dsp::CSR_ARINT);
    update_interrupt();
}

bool DspInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size == 2U) { u16 v = 0; if (!read16(offset, v)) return false; value = v; return true; }
    if (size == 4U) {
        u16 h = 0, l = 0;
        if (!read16(offset, h) || !read16(offset + 2U, l)) return false;
        value = (static_cast<u32>(h) << 16) | l;
        return true;
    }
    return fail("DSP: only 16- and 32-bit accesses are implemented");
}

bool DspInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U) return write16(offset, static_cast<u16>(value));
    if (size == 4U) return write16(offset, static_cast<u16>(value >> 16)) && write16(offset + 2U, static_cast<u16>(value));
    return fail("DSP: only 16- and 32-bit accesses are implemented");
}

} // namespace orca::hw
