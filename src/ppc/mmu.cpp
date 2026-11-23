#include "orca/ppc/mmu.hpp"

#include "orca/ppc/registers.hpp"

namespace orca::ppc {
namespace {

enum class Perm : u8 { None, ReadOnly, ReadWrite };

bool allowed(Perm p, AccessType type) noexcept {
    if (p == Perm::None) return false;
    if (type == AccessType::Write) return p == Perm::ReadWrite;
    return true; // Read and Fetch need read permission
}

// BAT PP field: 00 no access, x1 read-only, 10 read/write (PEM table 7-x).
Perm bat_perm(u32 pp) noexcept {
    if (pp == 0U) return Perm::None;
    if (pp == 2U) return Perm::ReadWrite;
    return Perm::ReadOnly;
}

// Page PP with key (PEM "Protection key / PP" table).
Perm page_perm(bool key, u32 pp) noexcept {
    if (!key) return pp == 3U ? Perm::ReadOnly : Perm::ReadWrite;
    switch (pp) {
    case 0: return Perm::None;
    case 2: return Perm::ReadWrite;
    default: return Perm::ReadOnly;
    }
}

bool try_bat(const std::array<BatPair, 8>& bats, unsigned count, bool problem_state, u32 ea,
             AccessType type, Translation& out) {
    for (unsigned i = 0; i < count; ++i) {
        const BatPair& bat = bats[i];
        const bool valid = problem_state ? (bat.upper & 1U) != 0U : (bat.upper & 2U) != 0U;
        if (!valid) continue;
        const u32 bl = (bat.upper >> 2U) & 0x7FFU;
        const u32 compare_mask = 0xFFFE0000U & ~(bl << 17U);
        if ((ea & compare_mask) != (bat.upper & compare_mask)) continue;
        // PA = BRPN | (EA[4:14] & BL) | EA[15:31]
        const u32 pa = (bat.lower & 0xFFFE0000U) | (ea & ((bl << 17U) | 0x1FFFFU));
        out.physical = pa;
        out.wimg = static_cast<u8>((bat.lower >> 3U) & 0xFU);
        out.bat = true;
        out.status = allowed(bat_perm(bat.lower & 3U), type) ? TranslateStatus::Ok : TranslateStatus::Protection;
        return true;
    }
    return false;
}

Translation page_table(CpuState& st, mem::Bus& bus, u32 ea, AccessType type) {
    Translation out;
    const u32 sr = st.sr[ea >> 28U];
    if ((sr & 0x80000000U) != 0U) { out.status = TranslateStatus::DirectStore; return out; }
    const bool problem = (st.msr & MSR_PR) != 0U;
    const bool key = problem ? (sr & 0x20000000U) != 0U : (sr & 0x40000000U) != 0U;
    const bool no_execute = (sr & 0x10000000U) != 0U;
    if (type == AccessType::Fetch && no_execute) { out.status = TranslateStatus::NoExecute; return out; }

    const u32 vsid = sr & 0x00FFFFFFU;
    const u32 page_index = (ea >> 12U) & 0xFFFFU;
    const u32 api = page_index >> 10U;
    const u32 hash1 = (vsid & 0x7FFFFU) ^ page_index;
    const u32 htaborg = st.sdr1 & 0xFFFF0000U;
    const u32 htabmask = st.sdr1 & 0x1FFU;

    for (u32 h = 0; h < 2U; ++h) {
        const u32 hash = h == 0U ? hash1 : (~hash1 & 0x7FFFFU);
        const u32 pteg = (htaborg & 0xFE000000U) |
                         ((((htaborg >> 16U) & 0x1FFU) | ((hash >> 10U) & htabmask)) << 16U) |
                         ((hash & 0x3FFU) << 6U);
        for (u32 i = 0; i < 8U; ++i) {
            const u32 pte_addr = pteg + i * 8U;
            u32 w0 = 0, w1 = 0;
            if (bus.read32(pte_addr, w0) != mem::BusStatus::Ok ||
                bus.read32(pte_addr + 4U, w1) != mem::BusStatus::Ok) {
                out.status = TranslateStatus::BusError;
                return out;
            }
            if ((w0 & 0x80000000U) == 0U) continue;
            if (((w0 >> 7U) & 0xFFFFFFU) != vsid) continue;
            if (((w0 >> 6U) & 1U) != h) continue;
            if ((w0 & 0x3FU) != api) continue;

            const u8 wimg = static_cast<u8>((w1 >> 3U) & 0xFU);
            if (!allowed(page_perm(key, w1 & 3U), type)) { out.status = TranslateStatus::Protection; return out; }
            if (type == AccessType::Fetch && (wimg & WIMG_G) != 0U) { out.status = TranslateStatus::NoExecute; return out; }

            u32 new_w1 = w1 | 0x100U;                           // R (referenced)
            if (type == AccessType::Write) new_w1 |= 0x80U;     // C (changed)
            if (new_w1 != w1 && bus.write32(pte_addr + 4U, new_w1) != mem::BusStatus::Ok) {
                out.status = TranslateStatus::BusError;
                return out;
            }
            out.physical = (w1 & 0xFFFFF000U) | (ea & 0xFFFU);
            out.wimg = wimg;
            out.status = TranslateStatus::Ok;
            return out;
        }
    }
    out.status = TranslateStatus::PageFault;
    return out;
}

} // namespace

Translation translate(CpuState& st, mem::Bus& bus, u32 ea, AccessType type, bool extended_bats) {
    const bool fetch = type == AccessType::Fetch;
    const bool enabled = fetch ? (st.msr & MSR_IR) != 0U : (st.msr & MSR_DR) != 0U;
    if (!enabled) {
        return Translation{TranslateStatus::Ok, ea, fetch ? kRealModeFetchWimg : kRealModeDataWimg};
    }
    Translation out;
    const bool problem = (st.msr & MSR_PR) != 0U;
    const unsigned count = extended_bats && (st.hid4 & HID4_SBE) != 0U ? 8U : 4U;
    if (try_bat(fetch ? st.ibat : st.dbat, count, problem, ea, type, out)) return out;
    return page_table(st, bus, ea, type);
}

bool translate_without_page_table(const CpuState& st, u32 ea, AccessType type, bool extended_bats, Translation& out) {
    const bool fetch = type == AccessType::Fetch;
    const bool enabled = fetch ? (st.msr & MSR_IR) != 0U : (st.msr & MSR_DR) != 0U;
    if (!enabled) {
        out = Translation{TranslateStatus::Ok, ea, fetch ? kRealModeFetchWimg : kRealModeDataWimg};
        return true;
    }
    const bool problem = (st.msr & MSR_PR) != 0U;
    const unsigned count = extended_bats && (st.hid4 & HID4_SBE) != 0U ? 8U : 4U;
    return try_bat(fetch ? st.ibat : st.dbat, count, problem, ea, type, out);
}

} // namespace orca::ppc
