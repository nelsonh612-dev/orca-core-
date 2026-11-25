#include "orca/hw/memory_interface.hpp"

#include <cstdio>

namespace orca::hw {

void MemoryInterface::update_interrupt() {
    const u16 pending = static_cast<u16>(regs_[mi::IMASK / 2U] & regs_[mi::ICAUSE / 2U] & 0x1FU);
    if (pending != 0U) pi_.raise(pi::CAUSE_MEM); else pi_.lower(pi::CAUSE_MEM);
}

void MemoryInterface::note_protection() {
    const u16 type = regs_[mi::TYPE / 2U];
    for (u32 ch = 0; ch < 4U; ++ch) {
        const u32 t = (type >> (2U * ch)) & 3U;
        if (t == 3U) continue;
        // YAGCD: bits 16-31 of the region register = page lo, bits 0-15 = page hi.
        const u16 page_lo = regs_[(mi::REGION0 + 4U * ch) / 2U];
        const u16 page_hi = regs_[(mi::REGION0 + 4U * ch + 2U) / 2U];
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "MI: channel %u protection type %u over pages 0x%04X-0x%04X is configured but NOT enforced", ch, t,
                      page_lo, page_hi);
        if (warnings_.size() < 64U) warnings_.emplace_back(buf); // bounded
    }
}

bool MemoryInterface::read16(u32 offset, u16& v) {
    // MI_TIMER0-9 (0x32-0x58) and 0x5A: memory access counters whose count source
    // is undocumented. They keep the value last written and do not count (the
    // model Dolphin uses); software that only clears/reads them sees 0.
    if ((offset >= mi::TIMER_FIRST && offset <= mi::TIMER_LAST + 1U) || offset == mi::UNKNOWN2) {
        v = regs_[offset / 2U];
        return true;
    }
    if (offset < mi::TYPE + 2U || offset == mi::IMASK || offset == mi::ICAUSE || offset == mi::STATUS ||
        offset == mi::ADDRLO || offset == mi::ADDRHI) {
        v = regs_[offset / 2U];
        return true;
    }
    return fail("MI: register not implemented");
}

bool MemoryInterface::write16(u32 offset, u16 v) {
    if ((offset >= mi::TIMER_FIRST && offset <= mi::TIMER_LAST + 1U) || offset == mi::UNKNOWN2) {
        regs_[offset / 2U] = v; // see read16: no counting is modelled
        return true;
    }
    if (offset < mi::TYPE + 2U) {
        regs_[offset / 2U] = offset == mi::TYPE ? static_cast<u16>(v & 0xFFU) : v;
        note_protection();
        return true;
    }
    switch (offset) {
    case mi::IMASK: regs_[offset / 2U] = static_cast<u16>(v & 0x1FU); update_interrupt(); return true;
    case mi::ICAUSE: regs_[offset / 2U] = static_cast<u16>(regs_[offset / 2U] & ~v); update_interrupt(); return true;
    case mi::STATUS: regs_[offset / 2U] = v; return true;
    case mi::ADDRLO: case mi::ADDRHI: return true; // read-only
    default: return fail("MI: register not implemented");
    }
}

bool MemoryInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size == 2U) { u16 v = 0; if (!read16(offset, v)) return false; value = v; return true; }
    if (size == 4U) {
        u16 h = 0, l = 0;
        if (!read16(offset, h)) return false;
        // a 32-bit read of PROT_TYPE (0x10) or PROT_ADDR_HI (0x24) also covers the
        // halfword after it, where there is no register: that half reads 0
        if (!read16(offset + 2U, l)) {
            if (offset + 2U != mi::TYPE + 2U && offset + 2U != mi::ADDRHI + 2U) return false;
            l = 0;
        }
        value = (static_cast<u32>(h) << 16) | l;
        return true;
    }
    return fail("MI: only 16- and 32-bit accesses are implemented");
}

bool MemoryInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U) return write16(offset, static_cast<u16>(value));
    if (size == 4U) {
        if (offset < mi::TYPE && offset % 4U == 0U) { // one diagnostic per 32-bit region write
            regs_[offset / 2U] = static_cast<u16>(value >> 16);
            regs_[offset / 2U + 1U] = static_cast<u16>(value);
            note_protection();
            return true;
        }
        if (!write16(offset, static_cast<u16>(value >> 16))) return false;
        // the halfword after PROT_TYPE / PROT_ADDR_HI holds no register (see mmio_read)
        if (offset + 2U == mi::TYPE + 2U || offset + 2U == mi::ADDRHI + 2U) return true;
        return write16(offset + 2U, static_cast<u16>(value));
    }
    return fail("MI: only 16- and 32-bit accesses are implemented");
}

} // namespace orca::hw
