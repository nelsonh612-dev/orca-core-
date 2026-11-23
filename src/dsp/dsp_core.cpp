// Orca Core — DSP interpreter. Every behaviour cites the GameCube DSP User's
// Manual v0.1.8; see the header for the list of host stops and model choices.
#include "orca/dsp/dsp_core.hpp"

#include <cstdio>

namespace orca::dsp {

namespace {
constexpr u64 kMask40 = 0xFFFFFFFFFFULL;
s64 sext40(s64 v) noexcept { return static_cast<s64>(static_cast<u64>(v) << 24) >> 24; }
s64 sext40u(u64 v) noexcept { return static_cast<s64>(v << 24) >> 24; }
u64 u40(s64 v) noexcept { return static_cast<u64>(v) & kMask40; }
bool neg40(s64 v) noexcept { return ((static_cast<u64>(v) >> 39) & 1U) != 0U; }
s64 sx16(u16 v) noexcept { return static_cast<s16>(v); }
s64 sx8(u16 v) noexcept { return static_cast<s8>(static_cast<u8>(v)); }
u32 bit(unsigned r) noexcept { return 1U << r; }
} // namespace

DspCore::DspCore(DspHost& host) : host_(host) { reset(0); }

void DspCore::reset(u16 pc) {
    s_ = DspState{};
    const u8 caps[4] = {8, 4, 4, 4}; // manual "Stacks": call 8, data 4, loop 4
    for (unsigned i = 0; i < 4; ++i) s_.st[i].cap = caps[i];
    s_.wr = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // model: no wrapping
    s_.config = 0xFF;                         // manual: "usually 0x00FF"
    s_.pc = pc;
    rep_count_ = 0;
    int_pending_ = false;
    exc_pending_ = 0;
    failed_ = false;
    error_.clear();
}

bool DspCore::load_irom(std::span<const u16> w) {
    if (w.size() != kIromWords) return false;
    irom_.assign(w.begin(), w.end());
    return true;
}
bool DspCore::load_coef(std::span<const u16> w) {
    if (w.size() != kCoefWords) return false;
    coef_.assign(w.begin(), w.end());
    return true;
}

bool DspCore::fail(std::string why) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "DSP pc=%04X: ", cur_pc_);
    error_ = buf + why;
    failed_ = true;
    return false;
}

// ------------------------------------------------------------------ values
s64 DspCore::acc(unsigned d) const noexcept {
    const u64 raw = (static_cast<u64>(s_.ac_h[d]) << 32) | (static_cast<u64>(s_.ac_m[d]) << 16) | s_.ac_l[d];
    return sext40u(raw);
}
s64 DspCore::prod() const noexcept {
    // manual: $prod = ($prod.h << 32) + (($prod.m1 + $prod.m2) << 16) + $prod.l
    const u64 sum = (static_cast<u64>(s_.prod_h & 0xFFU) << 32) +
                    ((static_cast<u64>(s_.prod_m1) + s_.prod_m2) << 16) + s_.prod_l;
    return sext40u(sum & kMask40);
}
bool DspCore::prod_internal_carry() const noexcept {
    const u64 sum = (static_cast<u64>(s_.prod_h & 0xFFU) << 32) +
                    ((static_cast<u64>(s_.prod_m1) + s_.prod_m2) << 16) + s_.prod_l;
    return (sum >> 40) != 0U;
}
s32 DspCore::ax(unsigned s) const noexcept {
    return static_cast<s32>((static_cast<u32>(s_.ax_h[s]) << 16) | s_.ax_l[s]);
}
void DspCore::set_acc(unsigned d, s64 v) {
    const u64 u = static_cast<u64>(v);
    s_.ac_h[d] = static_cast<u8>(u >> 32);
    s_.ac_m[d] = static_cast<u16>(u >> 16);
    s_.ac_l[d] = static_cast<u16>(u);
    written_ |= bit(16 + d) | bit(30 + d) | bit(28 + d);
}
void DspCore::write_m(unsigned d, u16 v) {
    s_.ac_m[d] = v;
    written_ |= bit(30 + d);
}
void DspCore::set_prod(s64 v) {
    const u64 u = static_cast<u64>(v) & kMask40;
    s_.prod_l = static_cast<u16>(u);
    s_.prod_m1 = static_cast<u16>(u >> 16);
    s_.prod_h = static_cast<u16>((u >> 32) & 0xFFU);
    s_.prod_m2 = 0;
    written_ |= bit(20) | bit(21) | bit(22) | bit(23);
}
u16 DspCore::acm_saturated(unsigned d) const noexcept {
    const s64 a = acc(d);
    if (a != static_cast<s64>(static_cast<s32>(a))) return a < 0 ? 0x8000U : 0x7FFFU;
    return s_.ac_m[d];
}
s64 DspCore::multiply(u16 a, u16 b) const noexcept {
    s64 p = (s_.sr & SR_SU) != 0U ? static_cast<s64>(static_cast<u64>(a) * b) : sx16(a) * sx16(b);
    if ((s_.sr & SR_AM) == 0U) p *= 2; // AM = 0: product doubled
    return sext40(p);
}
s64 DspCore::shift_by(s64 a, u16 amount, bool arithmetic) const noexcept {
    // bit 6 set: right shift by 64 - bits 0-5 (if non-zero); clear: left by bits 0-5
    if ((amount & 64U) != 0U) {
        const unsigned n = amount & 63U;
        if (n == 0U) return a;
        const unsigned sh = 64U - n;
        if (arithmetic) return a >> sh;
        return sext40u(u40(a) >> sh);
    }
    const unsigned n = amount & 63U;
    return sext40u((u40(a) << n) & kMask40);
}

// ------------------------------------------------------------------ flags
void DspCore::flags(const char* spec, s64 r, bool carry, bool overflow) {
    // spec order: OS LZ TB AS S Z O C  == $sr bits 7..0
    const bool tb = ((static_cast<u64>(r) >> 31) & 1U) == ((static_cast<u64>(r) >> 30) & 1U);
    const bool as = r != static_cast<s64>(static_cast<s32>(r));
    const bool val[8] = {overflow, false, tb, as, r < 0, r == 0, overflow, carry};
    u16 sr = s_.sr;
    for (unsigned i = 0; i < 8; ++i) {
        const u16 m = static_cast<u16>(1U << (7U - i));
        switch (spec[i]) {
        case '0': sr = static_cast<u16>(sr & ~m); break;
        case '1': sr = static_cast<u16>(sr | m); break;
        case 'X':
            if (i == 0) { if (val[0]) sr = static_cast<u16>(sr | m); } // OS is sticky
            else sr = val[i] ? static_cast<u16>(sr | m) : static_cast<u16>(sr & ~m);
            break;
        default: break;
        }
    }
    s_.sr = sr;
    written_ |= bit(19);
}
void DspCore::flags_add(const char* spec, s64 a, s64 b, s64 r) {
    // manual: C = A + B carries if A > C (unsigned); overflow on same-sign operands
    const bool c = u40(a) > u40(r);
    const bool o = neg40(a) == neg40(b) && neg40(r) != neg40(a);
    flags(spec, r, c, o);
}
void DspCore::flags_sub(const char* spec, s64 a, s64 b, s64 r) {
    // manual: C = A - B carries if A >= C (carry, not borrow)
    const bool c = u40(a) >= u40(r);
    const bool o = neg40(a) != neg40(b) && neg40(r) != neg40(a);
    flags(spec, r, c, o);
}

bool DspCore::cond(unsigned cc) const noexcept {
    const u16 sr = s_.sr;
    const bool O = (sr & SR_O) != 0U, S = (sr & SR_S) != 0U, Z = (sr & SR_Z) != 0U, C = (sr & SR_C) != 0U,
               AS = (sr & SR_AS) != 0U, TB = (sr & SR_TB) != 0U, LZ = (sr & SR_LZ) != 0U;
    switch (cc & 15U) {
    case 0x0: return O == S;
    case 0x1: return O != S;
    case 0x2: return O == S && !Z;
    case 0x3: return O != S || Z;
    case 0x4: return !Z;
    case 0x5: return Z;
    case 0x6: return !C;
    case 0x7: return C;
    case 0x8: return !AS;
    case 0x9: return AS;
    case 0xA: return (AS || TB) && !Z;
    case 0xB: return (!AS && !TB) || Z;
    case 0xC: return !LZ;
    case 0xD: return LZ;
    case 0xE: return O;
    default: return true;
    }
}

// ------------------------------------------------------------------ registers
bool DspCore::reg_read(unsigned r, u16& v, bool saturate) {
    if (r < 4) { v = s_.ar[r]; return true; }
    if (r < 8) { v = s_.ix[r - 4]; return true; }
    if (r < 12) { v = s_.wr[r - 8]; return true; }
    if (r < 16) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "generic read of $st%u is not documented", r - 12);
        return fail(buf);
    }
    switch (r) {
    case 16: case 17: v = static_cast<u16>(sx8(s_.ac_h[r - 16])); return true; // 8-bit, sign-extended reads
    case 18: v = static_cast<u16>(s_.config & 0xFFU); return true;
    case 19: v = s_.sr; return true;
    case 20: v = s_.prod_l; return true;
    case 21: v = s_.prod_m1; return true;
    case 22: v = static_cast<u16>(s_.prod_h & 0xFFU); return true;
    case 23: v = s_.prod_m2; return true;
    case 24: case 25: v = s_.ax_l[r - 24]; return true;
    case 26: case 27: v = s_.ax_h[r - 26]; return true;
    case 28: case 29: v = s_.ac_l[r - 28]; return true;
    default: { // 30, 31: $ac0.m / $ac1.m
        const unsigned d = r - 30;
        v = (saturate && (s_.sr & SR_SXM) != 0U) ? acm_saturated(d) : s_.ac_m[d];
        return true;
    }
    }
}

bool DspCore::reg_write(unsigned r, u16 v, bool sign_extend) {
    if (r < 4) { s_.ar[r] = v; return true; }
    if (r < 8) { s_.ix[r - 4] = v; return true; }
    if (r < 12) { s_.wr[r - 8] = v; return true; }
    if (r < 16) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "generic write of $st%u is not documented", r - 12);
        return fail(buf);
    }
    written_ |= bit(r);
    switch (r) {
    case 16: case 17: s_.ac_h[r - 16] = static_cast<u8>(v); return true; // upper 8 bits ignored
    case 18: s_.config = static_cast<u16>(v & 0xFFU); return true;
    case 19: s_.sr = static_cast<u16>(v & ~0x0100U); return true;      // bit 8 always reads 0
    case 20: s_.prod_l = v; return true;
    case 21: s_.prod_m1 = v; return true;
    case 22: s_.prod_h = static_cast<u16>(v & 0xFFU); return true;
    case 23: s_.prod_m2 = v; return true;
    case 24: case 25: s_.ax_l[r - 24] = v; return true;
    case 26: case 27: s_.ax_h[r - 26] = v; return true;
    case 28: case 29: s_.ac_l[r - 28] = v; return true;
    default: {
        const unsigned d = r - 30;
        s_.ac_m[d] = v;
        if (sign_extend && (s_.sr & SR_SXM) != 0U) { // 40-bit mode: load to the whole accumulator
            s_.ac_l[d] = 0;
            s_.ac_h[d] = (v & 0x8000U) != 0U ? 0xFFU : 0x00U;
            written_ |= bit(16 + d) | bit(28 + d);
        }
        return true;
    }
    }
}

bool DspCore::ar_add(unsigned r, s32 delta) {
    if (s_.wr[r] != 0xFFFFU) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "address arithmetic with $wr%u = %04X (wrapping is not documented)", r, s_.wr[r]);
        return fail(buf);
    }
    s_.ar[r] = static_cast<u16>(static_cast<s32>(s_.ar[r]) + delta);
    return true;
}

bool DspCore::push(unsigned stack, u16 v) {
    DspStack& st = s_.st[stack];
    if (st.n >= st.cap) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "$st%u overflow (STOVF exception not modelled)", stack);
        return fail(buf);
    }
    st.v[st.n++] = v;
    return true;
}
bool DspCore::pop(unsigned stack, u16& v) {
    DspStack& st = s_.st[stack];
    if (st.n == 0U) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "$st%u underflow (STOVF exception not modelled)", stack);
        return fail(buf);
    }
    v = st.v[--st.n];
    return true;
}

// ------------------------------------------------------------------ memory
bool DspCore::imem_read(u16 a, u16& v) {
    if (a < kIramWords) { v = iram_[a]; return true; }
    if (a >= kIromBase && a < kIromBase + kIromWords) {
        if (irom_.empty()) return fail("IROM access, but no IROM dump is loaded");
        v = irom_[a - kIromBase];
        return true;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "instruction memory address %04X is not documented", a);
    return fail(buf);
}
bool DspCore::dmem_read(u16 a, u16& v) {
    if (a < kDramWords) { v = dram_[a]; return true; }
    if (a >= kCoefBase && a < kCoefBase + kCoefWords) {
        if (coef_.empty()) return fail("COEF access, but no COEF dump is loaded");
        v = coef_[a - kCoefBase];
        return true;
    }
    if (a >= 0xFF00U) return ifx_read(a, v);
    char buf[64];
    std::snprintf(buf, sizeof buf, "data memory address %04X is not documented", a);
    return fail(buf);
}
bool DspCore::dmem_write(u16 a, u16 v) {
    if (a < kDramWords) { dram_[a] = v; return true; }
    if (a >= 0xFF00U) return ifx_write(a, v);
    char buf[80];
    if (a >= kCoefBase && a < kCoefBase + kCoefWords) std::snprintf(buf, sizeof buf, "write to COEF (read-only) at %04X", a);
    else std::snprintf(buf, sizeof buf, "data memory address %04X is not documented", a);
    return fail(buf);
}

bool DspCore::ifx_read(u16 a, u16& v) {
    if (a >= 0xFFA0U && a <= 0xFFAFU) { v = adpcm_coef_[a - 0xFFA0U]; return true; }
    switch (a) {
    case ifx::DSCR: v = dscr_; return true; // t (bit 2) is never set: DMA completes at once
    case ifx::DSPA: v = dspa_; return true;
    case ifx::DSMAH: v = dsmah_; return true;
    case ifx::DSMAL: v = dsmal_; return true;
    case ifx::FORMAT: v = format_; return true;
    case ifx::ACUNK1: v = acunk1_; return true;
    case ifx::ACSAH: v = acsah_; return true;
    case ifx::ACSAL: v = acsal_; return true;
    case ifx::ACEAH: v = aceah_; return true;
    case ifx::ACEAL: v = aceal_; return true;
    case ifx::ACCAH: v = accah_; return true;
    case ifx::ACCAL: v = accal_; return true;
    case ifx::PRED_SCALE: v = pred_scale_; return true;
    case ifx::YN1: v = yn1_; return true;
    case ifx::YN2: v = yn2_; return true;
    case ifx::GAIN: v = gain_; return true;
    case ifx::ACIN: v = acin_; return true;
    case ifx::AMDM_A: v = amdm_a_; return true;
    case ifx::AMDM_B: v = amdm_b_; return true;
    case ifx::DMBH: v = static_cast<u16>(dmb_ >> 16); return true;
    case ifx::CMBH: v = static_cast<u16>(cmb_ >> 16); return true;
    case ifx::CMBL: v = static_cast<u16>(cmb_); cmb_ &= 0x7FFFFFFFU; return true; // clears CMBH.M
    case ifx::ACDSAMP: return acc_read_sample(v);
    case ifx::ACDRAW: return acc_read_raw(v);
    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "IFX register %04X cannot be read (undocumented)", a);
        return fail(buf);
    }
    }
}

bool DspCore::ifx_write(u16 a, u16 v) {
    if (a >= 0xFFA0U && a <= 0xFFAFU) { adpcm_coef_[a - 0xFFA0U] = v; return true; }
    switch (a) {
    case ifx::DSCR: dscr_ = static_cast<u16>(v & 3U); return true;
    case ifx::DSBL: return dsp_dma(v);
    case ifx::DSPA: dspa_ = v; return true;
    case ifx::DSMAH: dsmah_ = v; return true;
    case ifx::DSMAL: dsmal_ = v; return true;
    case ifx::FORMAT: format_ = v; return true;
    case ifx::ACUNK1: acunk1_ = v; return true;
    case ifx::ACSAH: acsah_ = v; return true;
    case ifx::ACSAL: acsal_ = v; return true;
    case ifx::ACEAH: aceah_ = v; return true;
    case ifx::ACEAL: aceal_ = v; return true;
    case ifx::ACCAH: accah_ = v; return true;
    case ifx::ACCAL: accal_ = v; return true;
    case ifx::PRED_SCALE: pred_scale_ = v; return true;
    case ifx::YN1: yn1_ = v; return true;
    case ifx::YN2: yn2_ = v; return true;
    case ifx::GAIN: gain_ = v; return true;
    case ifx::ACIN: acin_ = v; return true;
    case ifx::AMDM_A: amdm_a_ = v; return true;
    case ifx::AMDM_B: amdm_b_ = v; return true;
    case ifx::DIRQ:
        if ((v & 1U) != 0U) host_.dsp_interrupt_cpu();
        return true;
    case ifx::DMBH: dmb_ = (dmb_ & 0x8000FFFFU) | (static_cast<u32>(v & 0x7FFFU) << 16); return true;
    case ifx::DMBL: dmb_ = (dmb_ & 0xFFFF0000U) | v | 0x80000000U; return true; // sets DMBH.M
    case ifx::ACDRAW: return acc_write_raw(v);
    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "IFX register %04X cannot be written (undocumented)", a);
        return fail(buf);
    }
    }
}

bool DspCore::dsp_dma(u16 bytes) {
    char buf[128];
    const bool to_cpu = (dscr_ & 1U) != 0U, imem = (dscr_ & 2U) != 0U;
    const u32 main = (static_cast<u32>(dsmah_) << 16) | dsmal_;
    if (bytes == 0U || (bytes & 1U) != 0U) {
        std::snprintf(buf, sizeof buf, "DSP DMA length %u (must be even and non-zero)", bytes);
        return fail(buf);
    }
    const u32 words = bytes / 2U;
    const u32 limit = imem ? kIramWords : kDramWords;
    if (static_cast<u32>(dspa_) + words > limit) {
        std::snprintf(buf, sizeof buf, "DSP DMA to %s %04X+%X leaves %s", imem ? "IMEM" : "DMEM", dspa_, words,
                      imem ? "IRAM" : "DRAM");
        return fail(buf);
    }
    u16* mem = imem ? iram_.data() : dram_.data();
    std::vector<u8> data(bytes);
    if (to_cpu) {
        for (u32 i = 0; i < words; ++i) {
            data[2 * i] = static_cast<u8>(mem[dspa_ + i] >> 8);
            data[2 * i + 1] = static_cast<u8>(mem[dspa_ + i]);
        }
        if (!host_.dsp_dma_write(main, data)) {
            std::snprintf(buf, sizeof buf, "DSP DMA write to main memory %08X+%X failed", main, bytes);
            return fail(buf);
        }
    } else {
        if (!host_.dsp_dma_read(main, data)) {
            std::snprintf(buf, sizeof buf, "DSP DMA read from main memory %08X+%X failed", main, bytes);
            return fail(buf);
        }
        for (u32 i = 0; i < words; ++i)
            mem[dspa_ + i] = static_cast<u16>((static_cast<u16>(data[2 * i]) << 8) | data[2 * i + 1]);
    }
    return true;
}

// ------------------------------------------------------------------ accelerator
// DSP manual "Accelerator": after a read ACCA is incremented; when it grows past
// ACEA it is reloaded from ACSA and ACRROV / ACRWOV / ACSOV is raised.
void DspCore::acc_advance(u32 amount, unsigned level) {
    const u32 ca = acc_ca() + amount;
    if (ca > acc_ea()) {
        acc_set_ca(acc_sa());
        exc_pending_ = static_cast<u8>(exc_pending_ | (1U << level));
    } else {
        acc_set_ca(ca);
    }
}

bool DspCore::acc_read_sample(u16& v) {
    // Implemented: FORMAT = ADPCM from ARAM, 4-bit (dd = 0, ss = 0). Addresses
    // are nibbles and include the frame headers (Nintendo DSPADPCM spec); each
    // 8-byte frame starts with a predictor/scale byte (DSP file format docs).
    const unsigned size = format_ & 3U, src = (format_ >> 2) & 3U;
    if (src != 0U || size != 0U) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "accelerator FORMAT %04X: only ADPCM (0x0000) is implemented (PCM formula undocumented)", format_);
        return fail(buf);
    }
    u8 byte = 0;
    u32 ca = acc_ca();
    if ((ca & 15U) == 0U) { // frame header: predictor (bits 6-4) and scale (bits 3-0)
        if (!host_.dsp_aram_read8(ca >> 1, byte)) return fail("accelerator ARAM read failed (no ARAM or out of range)");
        pred_scale_ = static_cast<u16>(byte & 0x7FU);
        ca += 2U;
        acc_set_ca(ca);
    }
    if (!host_.dsp_aram_read8(ca >> 1, byte)) return fail("accelerator ARAM read failed (no ARAM or out of range)");
    const unsigned raw = (ca & 1U) != 0U ? (byte & 0xFU) : (byte >> 4U); // high nibble first
    const s32 nibble = raw >= 8U ? static_cast<s32>(raw) - 16 : static_cast<s32>(raw);
    const s32 scale = 1 << (pred_scale_ & 0xFU);
    const unsigned idx = (pred_scale_ >> 4) & 7U;
    const s32 c1 = static_cast<s16>(adpcm_coef_[2U * idx]), c2 = static_cast<s16>(adpcm_coef_[2U * idx + 1U]);
    s32 val = ((nibble * scale) * 2048 + 1024 + c1 * static_cast<s16>(yn1_) + c2 * static_cast<s16>(yn2_)) >> 11;
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;
    yn2_ = yn1_;
    yn1_ = static_cast<u16>(val);
    v = static_cast<u16>(val);
    acc_advance(1, 5); // ACSOV
    return true;
}

bool DspCore::acc_read_raw(u16& v) {
    // "Reads respect the FORMAT size": implemented for 16-bit (word addresses).
    if ((format_ & 3U) != 2U) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "accelerator raw read with FORMAT size %u: only 16-bit is implemented", format_ & 3U);
        return fail(buf);
    }
    const u32 byte_addr = (acc_ca() & 0x7FFFFFFFU) << 1;
    u8 hi = 0, lo = 0;
    if (!host_.dsp_aram_read8(byte_addr, hi) || !host_.dsp_aram_read8(byte_addr + 1U, lo))
        return fail("accelerator ARAM read failed (no ARAM or out of range)");
    v = static_cast<u16>((static_cast<u16>(hi) << 8) | lo);
    acc_advance(1, 3); // ACRROV
    return true;
}

bool DspCore::acc_write_raw(u16 v) {
    // "Writes are always 16-bit ... require that the uppermost bit of the current address is set."
    const u32 ca = acc_ca();
    if ((ca & 0x80000000U) == 0U) return fail("accelerator raw write with ACCA bit 31 clear (behaviour undocumented)");
    if (!host_.dsp_aram_write16((ca & 0x7FFFFFFFU) << 1, v)) return fail("accelerator ARAM write failed (no ARAM or out of range)");
    acc_advance(1, 4); // ACRWOV
    return true;
}

// ------------------------------------------------------------------ decoding
const DspCore::Op* DspCore::decode(u16 op) {
    static const Op ops[] = {
        {0xFFFF, 0x0000, &DspCore::op_nop, 1, "NOP"},
        {0xFFFC, 0x0004, &DspCore::op_dar, 1, "DAR"},
        {0xFFFC, 0x0008, &DspCore::op_iar, 1, "IAR"},
        {0xFFFC, 0x000C, &DspCore::op_subarn, 1, "SUBARN"},
        {0xFFF0, 0x0010, &DspCore::op_addarn, 1, "ADDARN"},
        {0xFFFF, 0x0021, &DspCore::op_halt, 1, "HALT"},
        {0xFFE0, 0x0040, &DspCore::op_loop, 1, "LOOP"},
        {0xFFE0, 0x0060, &DspCore::op_bloop, 2, "BLOOP"},
        {0xFFE0, 0x0080, &DspCore::op_lri, 2, "LRI"},
        {0xFFE0, 0x00C0, &DspCore::op_lr, 2, "LR"},
        {0xFFE0, 0x00E0, &DspCore::op_sr, 2, "SR"},
        {0xFFF0, 0x0270, &DspCore::op_ifcc, 1, "IFcc"},
        {0xFFF0, 0x0290, &DspCore::op_jcc, 2, "Jcc"},
        {0xFFF0, 0x02B0, &DspCore::op_callcc, 2, "CALLcc"},
        {0xFFF0, 0x02D0, &DspCore::op_retcc, 1, "RETcc"},
        {0xFFF0, 0x02F0, &DspCore::op_rticc, 1, "RTIcc"},
        {0xFFFF, 0x02CA, &DspCore::op_lsrn, 1, "LSRN"},
        {0xFFFF, 0x02CB, &DspCore::op_asrn, 1, "ASRN"},
        {0xFEFF, 0x0200, &DspCore::op_addi, 2, "ADDI"},
        {0xFEFF, 0x0220, &DspCore::op_xori, 2, "XORI"},
        {0xFEFF, 0x0240, &DspCore::op_andi, 2, "ANDI"},
        {0xFEFF, 0x0260, &DspCore::op_ori, 2, "ORI"},
        {0xFEFF, 0x0280, &DspCore::op_cmpi, 2, "CMPI"},
        {0xFEFF, 0x02A0, &DspCore::op_andf, 2, "ANDF"},
        {0xFEFF, 0x02C0, &DspCore::op_andcf, 2, "ANDCF"},
        {0xFEF0, 0x0210, &DspCore::op_ilrr, 1, "ILRR"},
        {0xFE00, 0x0400, &DspCore::op_addis, 1, "ADDIS"},
        {0xFE00, 0x0600, &DspCore::op_cmpis, 1, "CMPIS"},
        {0xF800, 0x0800, &DspCore::op_lris, 1, "LRIS"},
        {0xFF00, 0x1000, &DspCore::op_loopi, 1, "LOOPI"},
        {0xFF00, 0x1100, &DspCore::op_bloopi, 2, "BLOOPI"},
        {0xFF00, 0x1200, &DspCore::op_sbclr, 1, "SBCLR"},
        {0xFF00, 0x1300, &DspCore::op_sbset, 1, "SBSET"},
        {0xFE00, 0x1400, &DspCore::op_shift_imm, 1, "LSL/LSR/ASL/ASR"},
        {0xFF00, 0x1600, &DspCore::op_si, 2, "SI"},
        {0xFF10, 0x1700, &DspCore::op_jrcc, 1, "JRcc"},
        {0xFF10, 0x1710, &DspCore::op_callrcc, 1, "CALLRcc"},
        {0xFE00, 0x1800, &DspCore::op_lrr, 1, "LRR"},
        {0xFE00, 0x1A00, &DspCore::op_srr, 1, "SRR"},
        {0xFC00, 0x1C00, &DspCore::op_mrr, 1, "MRR"},
        {0xF800, 0x2000, &DspCore::op_lrs, 1, "LRS"},
        {0xFE00, 0x2800, &DspCore::op_srsh, 1, "SRSH"},
        {0xFC00, 0x2C00, &DspCore::op_srs, 1, "SRS"},
        {0xFE80, 0x3080, &DspCore::op_xorc, 1, "XORC"},
        {0xFE80, 0x3280, &DspCore::op_notop, 1, "NOT"},
        {0xFC80, 0x3000, &DspCore::op_logic_ax, 1, "XORR"},
        {0xFC80, 0x3400, &DspCore::op_logic_ax, 1, "ANDR"},
        {0xFC80, 0x3800, &DspCore::op_logic_ax, 1, "ORR"},
        {0xFE80, 0x3C00, &DspCore::op_logic_ac, 1, "ANDC"},
        {0xFE80, 0x3E00, &DspCore::op_logic_ac, 1, "ORC"},
        {0xFC80, 0x3480, &DspCore::op_shiftn_ax, 1, "LSRNRX"},
        {0xFC80, 0x3880, &DspCore::op_shiftn_ax, 1, "ASRNRX"},
        {0xFE80, 0x3C80, &DspCore::op_shiftn_ac, 1, "LSRNR"},
        {0xFE80, 0x3E80, &DspCore::op_shiftn_ac, 1, "ASRNR"},
        {0xF800, 0x4000, &DspCore::op_addr, 1, "ADDR"},
        {0xFC00, 0x4800, &DspCore::op_addax, 1, "ADDAX"},
        {0xFE00, 0x4C00, &DspCore::op_add, 1, "ADD"},
        {0xFE00, 0x4E00, &DspCore::op_addp, 1, "ADDP"},
        {0xF800, 0x5000, &DspCore::op_subr, 1, "SUBR"},
        {0xFC00, 0x5800, &DspCore::op_subax, 1, "SUBAX"},
        {0xFE00, 0x5C00, &DspCore::op_sub, 1, "SUB"},
        {0xFE00, 0x5E00, &DspCore::op_subp, 1, "SUBP"},
        {0xF800, 0x6000, &DspCore::op_movr, 1, "MOVR"},
        {0xFC00, 0x6800, &DspCore::op_movax, 1, "MOVAX"},
        {0xFE00, 0x6C00, &DspCore::op_mov, 1, "MOV"},
        {0xFE00, 0x6E00, &DspCore::op_movp, 1, "MOVP"},
        {0xFC00, 0x7000, &DspCore::op_addaxl, 1, "ADDAXL"},
        {0xFE00, 0x7400, &DspCore::op_incm, 1, "INCM"},
        {0xFE00, 0x7600, &DspCore::op_inc, 1, "INC"},
        {0xFE00, 0x7800, &DspCore::op_decm, 1, "DECM"},
        {0xFE00, 0x7A00, &DspCore::op_dec, 1, "DEC"},
        {0xFE00, 0x7C00, &DspCore::op_neg, 1, "NEG"},
        {0xFE00, 0x7E00, &DspCore::op_movnp, 1, "MOVNP"},
        {0xF700, 0x8000, &DspCore::op_nx, 1, "NX"},
        {0xF700, 0x8100, &DspCore::op_clr, 1, "CLR"},
        {0xFF00, 0x8200, &DspCore::op_cmp, 1, "CMP"},
        {0xFF00, 0x8300, &DspCore::op_mulaxh, 1, "MULAXH"},
        {0xFF00, 0x8400, &DspCore::op_clrp, 1, "CLRP"},
        {0xFF00, 0x8500, &DspCore::op_tstprod, 1, "TSTPROD"},
        {0xFE00, 0x8600, &DspCore::op_tstaxh, 1, "TSTAXH"},
        {0xFE00, 0x8A00, &DspCore::op_srbits, 1, "M2/M0"},
        {0xFC00, 0x8C00, &DspCore::op_srbits, 1, "CLR15/SET15/SET16/SET40"},
        {0xF700, 0x9100, &DspCore::op_asr16, 1, "ASR16"},
        {0xF000, 0x9000, &DspCore::op_mul_family, 1, "MUL*"},
        {0xF700, 0xA100, &DspCore::op_abs, 1, "ABS"},
        {0xF700, 0xB100, &DspCore::op_tst, 1, "TST"},
        {0xE000, 0xA000, &DspCore::op_mulx_family, 1, "MULX*"},
        {0xE700, 0xC100, &DspCore::op_cmpaxh, 1, "CMPAXH"},
        {0xE000, 0xC000, &DspCore::op_mulc_family, 1, "MULC*"},
        {0xF800, 0xE000, &DspCore::op_maddx, 1, "MADDX/MSUBX"},
        {0xF800, 0xE800, &DspCore::op_maddc, 1, "MADDC/MSUBC"},
        {0xFE00, 0xF000, &DspCore::op_lsl16, 1, "LSL16"},
        {0xFE00, 0xF200, &DspCore::op_madd, 1, "MADD"},
        {0xFE00, 0xF400, &DspCore::op_lsr16, 1, "LSR16"},
        {0xFE00, 0xF600, &DspCore::op_madd, 1, "MSUB"},
        {0xFC00, 0xF800, &DspCore::op_addpaxz, 1, "ADDPAXZ"},
        {0xFE00, 0xFC00, &DspCore::op_clrl, 1, "CLRL"},
        {0xFE00, 0xFE00, &DspCore::op_movpz, 1, "MOVPZ"},
    };
    struct Table {
        std::array<const Op*, 0x10000> t{};
        Table() {
            for (u32 i = 0; i < 0x10000U; ++i)
                for (const Op& o : ops)
                    if ((i & o.mask) == o.value) { t[i] = &o; break; }
        }
    };
    static const Table table;
    return table.t[op];
}

bool DspCore::next_is_two_words(u16 at, bool& two) {
    u16 w = 0;
    if (!imem_read(at, w)) return false;
    const Op* o = decode(w);
    two = o != nullptr && o->words == 2U;
    return true;
}

// ------------------------------------------------------------------ extended opcodes
bool DspCore::plan_extension(u16 x, ExtPlan& p) {
    auto load = [&](unsigned reg, u16 addr, bool sx) -> bool {
        u16 v = 0;
        if (!dmem_read(addr, v)) return false;
        p.regs[p.nregs++] = {reg, v, sx};
        return true;
    };
    auto store = [&](u16 addr, unsigned reg) -> bool {
        u16 v = 0;
        if (!reg_read(reg, v, true)) return false;
        p.mem[p.nmem++] = {addr, v};
        return true;
    };
    auto step_ar = [&](unsigned r, bool use_ix) { p.ar[p.nar++] = {r, use_ix ? static_cast<s32>(static_cast<s16>(s_.ix[r])) : 1}; };
    if (x < 0x04U) return true; // 'NOP
    if (x < 0x10U) {            // 'DR / 'IR / 'NR
        const unsigned r = x & 3U, kind = (x >> 2) & 3U;
        if (kind == 1U) p.ar[p.nar++] = {r, -1};
        else if (kind == 2U) p.ar[p.nar++] = {r, 1};
        else step_ar(r, true);
        return true;
    }
    if (x < 0x20U) { // 'MV $(0x18+D), $(0x1c+S)
        u16 v = 0;
        if (!reg_read(0x1CU + (x & 3U), v, true)) return false;
        p.regs[p.nregs++] = {0x18U + ((x >> 2) & 3U), v, false};
        return true;
    }
    if (x < 0x40U) { // 'S / 'SN @$arD, $(0x1c+S)
        const unsigned d = x & 3U;
        if (!store(s_.ar[d], 0x1CU + ((x >> 3) & 3U))) return false;
        step_ar(d, (x & 4U) != 0U);
        return true;
    }
    if (x < 0x80U) { // 'L / 'LN $(0x18+D), @$arS
        const unsigned sreg = x & 3U;
        if (!load(0x18U + ((x >> 3) & 7U), s_.ar[sreg], true)) return false;
        step_ar(sreg, (x & 4U) != 0U);
        return true;
    }
    if (x < 0xC0U) { // 'LS / 'SL family
        const unsigned d = (x >> 4) & 3U, mode = (x >> 1) & 7U, sacc = x & 1U;
        const bool load_from_ar0 = (mode & 1U) == 0U; // LS*, else SL*
        const bool n = (mode & 2U) != 0U, m = (mode & 4U) != 0U;
        const u16 ar0 = s_.ar[0], ar3 = s_.ar[3];
        if (load_from_ar0) {
            if (!load(0x18U + d, ar0, false) || !store(ar3, 0x1EU + sacc)) return false;
        } else {
            if (!load(0x18U + d, ar3, false) || !store(ar0, 0x1EU + sacc)) return false;
        }
        step_ar(0, n);
        step_ar(3, m);
        return true;
    }
    // 'LD / 'LDAX family
    const unsigned ss = x & 3U, mode = (x >> 2) & 3U, b5 = (x >> 5) & 1U, b4 = (x >> 4) & 1U;
    const u16 ar3 = s_.ar[3];
    unsigned src;
    if (ss == 3U) { // 'LDAX*: $axR.h = MEM[$arS], $axR.l = MEM[$ar3]
        src = b5;
        if (!load(26U + b4, s_.ar[src], false) || !load(24U + b4, ar3, false)) return false;
    } else { // 'LD*: $ax0.D = MEM[$arS], $ax1.R = MEM[$ar3]
        src = ss;
        if (!load(0x18U + b5 * 2U, s_.ar[src], false) || !load(0x19U + b4 * 2U, ar3, false)) return false;
    }
    step_ar(src, (mode & 1U) != 0U);
    step_ar(3, (mode & 2U) != 0U);
    return true;
}

void DspCore::ext_write_raw(unsigned r, u16 v) {
    // Main and extension writes to the same register are OR'd (manual).
    u16 cur = 0;
    const bool both = (written_ & bit(r)) != 0U;
    switch (r) {
    case 16: case 17: cur = s_.ac_h[r - 16]; s_.ac_h[r - 16] = static_cast<u8>(both ? (cur | v) : v); return;
    case 24: case 25: cur = s_.ax_l[r - 24]; s_.ax_l[r - 24] = both ? static_cast<u16>(cur | v) : v; return;
    case 26: case 27: cur = s_.ax_h[r - 26]; s_.ax_h[r - 26] = both ? static_cast<u16>(cur | v) : v; return;
    case 28: case 29: cur = s_.ac_l[r - 28]; s_.ac_l[r - 28] = both ? static_cast<u16>(cur | v) : v; return;
    case 30: case 31: cur = s_.ac_m[r - 30]; s_.ac_m[r - 30] = both ? static_cast<u16>(cur | v) : v; return;
    default: return;
    }
}

bool DspCore::apply_extension(const ExtPlan& p) {
    for (unsigned i = 0; i < p.nmem; ++i)
        if (!dmem_write(p.mem[i].first, p.mem[i].second)) return false;
    for (unsigned i = 0; i < p.nar; ++i)
        if (!ar_add(p.ar[i].first, p.ar[i].second)) return false;
    for (unsigned i = 0; i < p.nregs; ++i) {
        const auto& w = p.regs[i];
        if ((w.r == 30U || w.r == 31U) && w.sign_extend && (s_.sr & SR_SXM) != 0U) {
            const unsigned d = w.r - 30U;
            ext_write_raw(30U + d, w.v);
            ext_write_raw(16U + d, (w.v & 0x8000U) != 0U ? 0xFFU : 0x00U);
            ext_write_raw(28U + d, 0);
        } else {
            ext_write_raw(w.r, w.v);
        }
    }
    return true;
}

// ------------------------------------------------------------------ execution
bool DspCore::step() {
    if (halted_ || failed_) return false;
    // Accelerator exceptions (levels 3-5, vectors 0x0006/0x0008/0x000A), lowest
    // level first, taken when $sr.IE is set (model; the manual does not say).
    if (exc_pending_ != 0U && rep_count_ == 0U && (s_.sr & SR_IE) != 0U) {
        for (unsigned level = 3; level <= 5; ++level) {
            if ((exc_pending_ & (1U << level)) == 0U) continue;
            cur_pc_ = s_.pc;
            if (!push(0, s_.pc) || !push(1, s_.sr)) return false;
            s_.pc = static_cast<u16>(level * 2U);
            exc_pending_ = static_cast<u8>(exc_pending_ & ~(1U << level));
            ++cycles_;
            return true;
        }
    }
    // External interrupt (level 7, vector 0x000E) when IE and EIE are set (model).
    if (int_pending_ && rep_count_ == 0U && (s_.sr & SR_IE) != 0U && (s_.sr & SR_EIE) != 0U) {
        cur_pc_ = s_.pc;
        if (!push(0, s_.pc) || !push(1, s_.sr)) return false;
        s_.pc = 0x000E;
        int_pending_ = false;
        ++cycles_;
        return true;
    }
    cur_pc_ = s_.pc;
    u16 op = 0;
    if (!fetch(cur_pc_, op)) return false;
    const Op* o = decode(op);
    if (o == nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "unknown opcode %04X", op);
        return fail(buf);
    }
    written_ = 0;
    ExtPlan plan;
    const bool has_ext = (op >> 12) >= 3U;
    if (has_ext && !plan_extension(static_cast<u16>((op >> 12) == 3U ? (op & 0x7FU) : (op & 0xFFU)), plan)) return false;
    s_.pc = static_cast<u16>(cur_pc_ + o->words);
    if (!(this->*(o->fn))(op)) return false;
    if (has_ext && !apply_extension(plan)) return false;
    ++cycles_;

    // LOOP / LOOPI: repeat the single instruction at rep_pc_.
    if (rep_count_ != 0U && cur_pc_ == rep_pc_) {
        if (s_.pc != static_cast<u16>(rep_pc_ + 1U))
            return fail("the instruction repeated by LOOP changed the program flow (undocumented)");
        if (--rep_count_ != 0U) s_.pc = rep_pc_;
    }
    // BLOOP / BLOOPI: end of block reached.
    if (!s_.st[2].empty() && cur_pc_ == s_.st[2].top()) {
        DspStack& counter = s_.st[3];
        if (counter.empty()) return fail("loop stack $st3 empty at the end of a BLOOP block");
        counter.v[counter.n - 1U] = static_cast<u16>(counter.v[counter.n - 1U] - 1U);
        if (counter.v[counter.n - 1U] != 0U) {
            s_.pc = s_.st[0].top();
        } else {
            u16 dummy = 0;
            if (!pop(0, dummy) || !pop(2, dummy) || !pop(3, dummy)) return false;
        }
    }
    return true;
}

u64 DspCore::run(u64 max) {
    u64 n = 0;
    while (n < max && step()) ++n;
    return n;
}

// ------------------------------------------------------------------ opcodes: address registers, control
bool DspCore::op_nop(u16) { return true; }
bool DspCore::op_dar(u16 op) { return ar_add(op & 3U, -1); }
bool DspCore::op_iar(u16 op) { return ar_add(op & 3U, 1); }
bool DspCore::op_subarn(u16 op) { const unsigned d = op & 3U; return ar_add(d, -static_cast<s32>(static_cast<s16>(s_.ix[d]))); }
bool DspCore::op_addarn(u16 op) { return ar_add(op & 3U, static_cast<s16>(s_.ix[(op >> 2) & 3U])); }
bool DspCore::op_halt(u16) {
    halted_ = true; // model: PC already points past HALT
    host_.dsp_halted();
    return true;
}

bool DspCore::op_loop(u16 op) {
    u16 n = 0;
    if (!reg_read(op & 0x1FU, n, true)) return false;
    bool two = false;
    if (!next_is_two_words(static_cast<u16>(cur_pc_ + 1U), two)) return false;
    if (two) return fail("LOOP over a 2-word instruction (undocumented)");
    if (n == 0U) { s_.pc = static_cast<u16>(cur_pc_ + 2U); return true; }
    rep_count_ = n;
    rep_pc_ = static_cast<u16>(cur_pc_ + 1U);
    return true;
}
bool DspCore::op_loopi(u16 op) {
    const u16 n = static_cast<u16>(op & 0xFFU);
    bool two = false;
    if (!next_is_two_words(static_cast<u16>(cur_pc_ + 1U), two)) return false;
    if (two) return fail("LOOPI over a 2-word instruction (undocumented)");
    if (n == 0U) { s_.pc = static_cast<u16>(cur_pc_ + 2U); return true; }
    rep_count_ = n;
    rep_pc_ = static_cast<u16>(cur_pc_ + 1U);
    return true;
}
bool DspCore::op_bloop(u16 op) {
    u16 n = 0, end = 0;
    if (!reg_read(op & 0x1FU, n, true) || !imem_read(static_cast<u16>(cur_pc_ + 1U), end)) return false;
    if (n == 0U) { s_.pc = static_cast<u16>(end + 1U); return true; }
    return push(0, static_cast<u16>(cur_pc_ + 2U)) && push(2, end) && push(3, n);
}
bool DspCore::op_bloopi(u16 op) {
    u16 end = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), end)) return false;
    const u16 n = static_cast<u16>(op & 0xFFU);
    if (n == 0U) { s_.pc = static_cast<u16>(end + 1U); return true; }
    return push(0, static_cast<u16>(cur_pc_ + 2U)) && push(2, end) && push(3, n);
}

bool DspCore::op_ifcc(u16 op) {
    if (cond(op & 15U)) return true; // next instruction executes normally
    bool two = false;
    if (!next_is_two_words(static_cast<u16>(cur_pc_ + 1U), two)) return false;
    if (two) return fail("IFcc (false) followed by a 2-word instruction (undocumented)");
    s_.pc = static_cast<u16>(cur_pc_ + 2U);
    return true;
}
bool DspCore::op_jcc(u16 op) {
    u16 a = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), a)) return false;
    if (cond(op & 15U)) s_.pc = a;
    return true;
}
bool DspCore::op_callcc(u16 op) {
    u16 a = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), a)) return false;
    if (!cond(op & 15U)) return true;
    if (!push(0, static_cast<u16>(cur_pc_ + 2U))) return false;
    s_.pc = a;
    return true;
}
bool DspCore::op_retcc(u16 op) {
    if (!cond(op & 15U)) return true;
    u16 a = 0;
    if (!pop(0, a)) return false;
    s_.pc = a;
    return true;
}
bool DspCore::op_rticc(u16 op) {
    if (!cond(op & 15U)) return true;
    u16 sr = 0, pc = 0;
    if (!pop(1, sr) || !pop(0, pc)) return false;
    s_.sr = static_cast<u16>(sr & ~0x0100U);
    s_.pc = pc;
    return true;
}
bool DspCore::op_jrcc(u16 op) {
    u16 a = 0;
    if (!reg_read((op >> 5) & 7U, a, false)) return false;
    if (cond(op & 15U)) s_.pc = a;
    return true;
}
bool DspCore::op_callrcc(u16 op) {
    u16 a = 0;
    if (!reg_read((op >> 5) & 7U, a, false)) return false;
    if (!cond(op & 15U)) return true;
    if (!push(0, static_cast<u16>(cur_pc_ + 1U))) return false;
    s_.pc = a;
    return true;
}
bool DspCore::op_sbclr(u16 op) { s_.sr = static_cast<u16>(s_.sr & ~(1U << ((op & 7U) + 6U))); return true; }
bool DspCore::op_sbset(u16 op) { s_.sr = static_cast<u16>((s_.sr | (1U << ((op & 7U) + 6U))) & ~0x0100U); return true; }
bool DspCore::op_srbits(u16 op) {
    switch ((op >> 8) & 0xFU) {
    case 0xA: s_.sr = static_cast<u16>(s_.sr & ~SR_AM); break;  // M2
    case 0xB: s_.sr = static_cast<u16>(s_.sr | SR_AM); break;   // M0
    case 0xC: s_.sr = static_cast<u16>(s_.sr & ~SR_SU); break;  // CLR15
    case 0xD: s_.sr = static_cast<u16>(s_.sr | SR_SU); break;   // SET15
    case 0xE: s_.sr = static_cast<u16>(s_.sr & ~SR_SXM); break; // SET16
    default: s_.sr = static_cast<u16>(s_.sr | SR_SXM); break;   // SET40
    }
    return true;
}

// ------------------------------------------------------------------ loads / stores
bool DspCore::op_lri(u16 op) {
    u16 i = 0;
    return imem_read(static_cast<u16>(cur_pc_ + 1U), i) && reg_write(op & 0x1FU, i, true);
}
bool DspCore::op_lr(u16 op) {
    u16 m = 0, v = 0;
    return imem_read(static_cast<u16>(cur_pc_ + 1U), m) && dmem_read(m, v) && reg_write(op & 0x1FU, v, true);
}
bool DspCore::op_sr(u16 op) {
    u16 m = 0, v = 0;
    return imem_read(static_cast<u16>(cur_pc_ + 1U), m) && reg_read(op & 0x1FU, v, true) && dmem_write(m, v);
}
bool DspCore::op_si(u16 op) {
    u16 i = 0;
    return imem_read(static_cast<u16>(cur_pc_ + 1U), i) && dmem_write(static_cast<u16>(0xFF00U | (op & 0xFFU)), i);
}
bool DspCore::op_lris(u16 op) {
    return reg_write(0x18U + ((op >> 8) & 7U), static_cast<u16>(sx8(op)), true);
}
bool DspCore::op_ilrr(u16 op) {
    const unsigned d = (op >> 8) & 1U, s = op & 3U, mode = (op >> 2) & 3U;
    u16 v = 0;
    if (!imem_read(s_.ar[s], v)) return false;
    if (!reg_write(30U + d, v, true)) return false;
    if (mode == 1U) return ar_add(s, -1);
    if (mode == 2U) return ar_add(s, 1);
    if (mode == 3U) return ar_add(s, static_cast<s16>(s_.ix[s]));
    return true;
}
bool DspCore::op_lrr(u16 op) {
    const unsigned mode = (((op >> 8) & 1U) << 1) | ((op >> 7) & 1U), s = (op >> 5) & 3U, d = op & 0x1FU;
    u16 v = 0;
    if (!dmem_read(s_.ar[s], v) || !reg_write(d, v, true)) return false;
    if (mode == 1U) return ar_add(s, -1);
    if (mode == 2U) return ar_add(s, 1);
    if (mode == 3U) return ar_add(s, static_cast<s16>(s_.ix[s]));
    return true;
}
bool DspCore::op_srr(u16 op) {
    const unsigned mode = (((op >> 8) & 1U) << 1) | ((op >> 7) & 1U), d = (op >> 5) & 3U, s = op & 0x1FU;
    u16 v = 0;
    if (!reg_read(s, v, true) || !dmem_write(s_.ar[d], v)) return false;
    if (mode == 1U) return ar_add(d, -1);
    if (mode == 2U) return ar_add(d, 1);
    if (mode == 3U) return ar_add(d, static_cast<s16>(s_.ix[d]));
    return true;
}
bool DspCore::op_mrr(u16 op) {
    u16 v = 0;
    return reg_read(op & 0x1FU, v, true) && reg_write((op >> 5) & 0x1FU, v, true);
}
bool DspCore::op_lrs(u16 op) {
    u16 v = 0;
    const u16 a = static_cast<u16>(((s_.config & 0xFFU) << 8) | (op & 0xFFU));
    return dmem_read(a, v) && reg_write(0x18U + ((op >> 8) & 7U), v, true);
}
bool DspCore::op_srsh(u16 op) {
    u16 v = 0;
    const u16 a = static_cast<u16>(((s_.config & 0xFFU) << 8) | (op & 0xFFU));
    return reg_read(16U + ((op >> 8) & 1U), v, false) && dmem_write(a, v);
}
bool DspCore::op_srs(u16 op) {
    u16 v = 0;
    const u16 a = static_cast<u16>(((s_.config & 0xFFU) << 8) | (op & 0xFFU));
    return reg_read(0x1CU + ((op >> 8) & 3U), v, true) && dmem_write(a, v);
}

// ------------------------------------------------------------------ logic on $acD.m
bool DspCore::op_xori(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    write_m(d, static_cast<u16>(s_.ac_m[d] ^ i));
    flags("--XXXX00", acc(d), false, false);
    return true;
}
bool DspCore::op_andi(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    write_m(d, static_cast<u16>(s_.ac_m[d] & i));
    flags("--XXXX00", acc(d), false, false);
    return true;
}
bool DspCore::op_ori(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    write_m(d, static_cast<u16>(s_.ac_m[d] | i));
    flags("--XXXX00", acc(d), false, false); // manual lists C as X without a definition: cleared (model)
    return true;
}
bool DspCore::op_andf(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    s_.sr = (s_.ac_m[d] & i) == 0U ? static_cast<u16>(s_.sr | SR_LZ) : static_cast<u16>(s_.sr & ~SR_LZ);
    return true;
}
bool DspCore::op_andcf(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    s_.sr = (s_.ac_m[d] & i) == i ? static_cast<u16>(s_.sr | SR_LZ) : static_cast<u16>(s_.sr & ~SR_LZ);
    return true;
}
bool DspCore::op_logic_ax(u16 op) { // XORR / ANDR / ORR $acD.m, $axS.h
    const unsigned kind = (op >> 10) & 3U, s = (op >> 9) & 1U, d = (op >> 8) & 1U;
    const u16 x = s_.ax_h[s];
    const u16 m = s_.ac_m[d];
    write_m(d, static_cast<u16>(kind == 0U ? (m ^ x) : kind == 1U ? (m & x) : (m | x)));
    flags("--XXXX00", acc(d), false, false); // ORR: C listed as X, cleared (model)
    return true;
}
bool DspCore::op_logic_ac(u16 op) { // ANDC / ORC $acD.m, $ac(1-D).m
    const unsigned d = (op >> 8) & 1U;
    const bool is_or = ((op >> 9) & 1U) != 0U;
    const u16 x = s_.ac_m[1U - d];
    write_m(d, static_cast<u16>(is_or ? (s_.ac_m[d] | x) : (s_.ac_m[d] & x)));
    if (!is_or) flags("--XXXX00", acc(d), false, false); // ORC: manual lists no flag changes
    return true;
}
bool DspCore::op_xorc(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    write_m(d, static_cast<u16>(s_.ac_m[d] ^ s_.ac_m[1U - d]));
    flags("--XXXX00", acc(d), false, false);
    return true;
}
bool DspCore::op_notop(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    write_m(d, static_cast<u16>(~s_.ac_m[d]));
    flags("--XXXX00", acc(d), false, false);
    return true;
}

// ------------------------------------------------------------------ shifts
bool DspCore::op_shift_imm(u16 op) {
    const unsigned r = (op >> 8) & 1U, kind = (op >> 6) & 3U, i = op & 0x3FU;
    const s64 a = acc(r);
    s64 res = a;
    switch (kind) {
    case 0: res = sext40u((u40(a) << i) & kMask40); break;            // LSL
    case 1: if (i != 0U) res = sext40u(u40(a) >> (64U - i)); break;   // LSR
    case 2: res = sext40u((u40(a) << i) & kMask40); break;            // ASL
    default: if (i != 0U) res = a >> (64U - i); break;                // ASR
    }
    set_acc(r, res);
    flags("--XXXX00", res, false, false);
    return true;
}
bool DspCore::op_lsrn(u16) {
    const s64 r = shift_by(acc(0), s_.ac_m[1], false);
    set_acc(0, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_asrn(u16) {
    const s64 r = shift_by(acc(0), s_.ac_m[1], true);
    set_acc(0, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_shiftn_ax(u16 op) { // LSRNRX (0x34..) / ASRNRX (0x38..)
    const bool arith = ((op >> 11) & 1U) != 0U;
    const unsigned s = (op >> 9) & 1U, d = (op >> 8) & 1U;
    const s64 r = shift_by(acc(d), s_.ax_h[s], arith);
    set_acc(d, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_shiftn_ac(u16 op) { // LSRNR (0x3C8x) / ASRNR (0x3E8x)
    const bool arith = ((op >> 9) & 1U) != 0U;
    const unsigned d = (op >> 8) & 1U;
    const s64 r = shift_by(acc(d), s_.ac_m[1U - d], arith);
    set_acc(d, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_asr16(u16 op) {
    const unsigned r = (op >> 11) & 1U;
    const s64 res = acc(r) >> 16;
    set_acc(r, res);
    flags("--XXXX00", res, false, false);
    return true;
}
bool DspCore::op_lsl16(u16 op) {
    const unsigned r = (op >> 8) & 1U;
    const s64 res = sext40u((u40(acc(r)) << 16) & kMask40);
    set_acc(r, res);
    flags("--XXXX00", res, false, false);
    return true;
}
bool DspCore::op_lsr16(u16 op) {
    const unsigned r = (op >> 8) & 1U;
    const s64 res = sext40u(u40(acc(r)) >> 16);
    set_acc(r, res);
    flags("--XXXX00", res, false, false);
    return true;
}

// ------------------------------------------------------------------ arithmetic
#define ORCA_ADD(dst, b, spec) do { const s64 a_ = acc(dst); const s64 b_ = (b); const s64 r_ = sext40(a_ + b_); \
    set_acc(dst, r_); flags_add(spec, a_, b_, r_); } while (0)
#define ORCA_SUB(dst, b, spec) do { const s64 a_ = acc(dst); const s64 b_ = (b); const s64 r_ = sext40(a_ - b_); \
    set_acc(dst, r_); flags_sub(spec, a_, b_, r_); } while (0)

bool DspCore::op_addi(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    ORCA_ADD((op >> 8) & 1U, sx16(i) * 65536, "X-XXXXXX");
    return true;
}
bool DspCore::op_addis(u16 op) { ORCA_ADD((op >> 8) & 1U, sx8(op) * 65536, "X-XXXXXX"); return true; }
bool DspCore::op_cmpi(u16 op) {
    u16 i = 0;
    if (!imem_read(static_cast<u16>(cur_pc_ + 1U), i)) return false;
    const unsigned d = (op >> 8) & 1U;
    const s64 a = acc(d), b = sx16(i) * 65536;
    flags_sub("X-XXXXXX", a, b, sext40(a - b));
    return true;
}
bool DspCore::op_cmpis(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const s64 a = acc(d), b = sx8(op) * 65536;
    flags_sub("X-XXXXXX", a, b, sext40(a - b));
    return true;
}
bool DspCore::op_addr(u16 op) {
    u16 v = 0;
    if (!reg_read(0x18U + ((op >> 9) & 3U), v, false)) return false;
    ORCA_ADD((op >> 8) & 1U, sx16(v) * 65536, "X-XXXXXX");
    return true;
}
bool DspCore::op_addax(u16 op) { ORCA_ADD((op >> 8) & 1U, ax((op >> 9) & 1U), "X-XXXXXX"); return true; }
bool DspCore::op_add(u16 op) { const unsigned d = (op >> 8) & 1U; ORCA_ADD(d, acc(1U - d), "X-XXXXXX"); return true; }
bool DspCore::op_addp(u16 op) { ORCA_ADD((op >> 8) & 1U, prod(), "X-XXXXXX"); return true; }
bool DspCore::op_addaxl(u16 op) { ORCA_ADD((op >> 8) & 1U, static_cast<s64>(s_.ax_l[(op >> 9) & 1U]), "X-XXXXXX"); return true; }
bool DspCore::op_subr(u16 op) {
    u16 v = 0;
    if (!reg_read(0x18U + ((op >> 9) & 3U), v, false)) return false;
    ORCA_SUB((op >> 8) & 1U, sx16(v) * 65536, "X-XXXXXX");
    return true;
}
bool DspCore::op_subax(u16 op) { ORCA_SUB((op >> 8) & 1U, ax((op >> 9) & 1U), "X-XXXXXX"); return true; }
bool DspCore::op_sub(u16 op) { const unsigned d = (op >> 8) & 1U; ORCA_SUB(d, acc(1U - d), "X-XXXXXX"); return true; }
bool DspCore::op_subp(u16 op) { ORCA_SUB((op >> 8) & 1U, prod(), "X-XXXXXX"); return true; }
bool DspCore::op_incm(u16 op) { ORCA_ADD((op >> 8) & 1U, 0x10000, "X-XXXXXX"); return true; }
bool DspCore::op_inc(u16 op) { ORCA_ADD((op >> 8) & 1U, 1, "X-XXXXXX"); return true; }
bool DspCore::op_decm(u16 op) { ORCA_SUB((op >> 8) & 1U, 0x10000, "X-XXXXXX"); return true; }
bool DspCore::op_dec(u16 op) { ORCA_SUB((op >> 8) & 1U, 1, "X-XXXXXX"); return true; }
bool DspCore::op_neg(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const s64 a = acc(d), r = sext40(0 - a);
    set_acc(d, r);
    flags_sub("X-XXXXXX", 0, a, r);
    return true;
}
bool DspCore::op_cmp(u16) {
    const s64 a = acc(0), b = acc(1);
    flags_sub("X-XXXXXX", a, b, sext40(a - b));
    return true;
}
bool DspCore::op_cmpaxh(u16 op) { // 110r s001: CMPAXH $acS, $axR.h
    const unsigned r = (op >> 12) & 1U, s = (op >> 11) & 1U;
    const s64 a = acc(s), b = sx16(s_.ax_h[r]) * 65536;
    flags_sub("X-XXXXXX", a, b, sext40(a - b));
    return true;
}
bool DspCore::op_abs(u16 op) {
    const unsigned d = (op >> 11) & 1U;
    const s64 a = acc(d);
    const s64 r = a < 0 ? sext40(0 - a) : a;
    set_acc(d, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_tst(u16 op) { flags("--XXXX00", acc((op >> 11) & 1U), false, false); return true; }
bool DspCore::op_tstaxh(u16 op) {
    // FLAGS($axR.h): evaluated as the value in bits 31-16 (model: TB = bits 15/14 of $axR.h)
    flags("--X0XX00", sx16(s_.ax_h[(op >> 8) & 1U]) * 65536, false, false);
    return true;
}
bool DspCore::op_tstprod(u16) { flags("--X0XX0X", prod(), prod_internal_carry(), false); return true; }
bool DspCore::op_clr(u16 op) {
    const unsigned r = (op >> 11) & 1U;
    set_acc(r, 0);
    flags("--100100", 0, false, false);
    return true;
}
bool DspCore::op_clrl(u16 op) {
    const unsigned r = (op >> 8) & 1U;
    const s64 a = acc(r);
    const s64 res = sext40u(((u40(a) + ((u40(a) & 0x10000U) != 0U ? 0x8000U : 0x7FFFU)) & ~0xFFFFULL) & kMask40);
    set_acc(r, res);
    flags("--XXXX00", res, false, false);
    return true;
}
bool DspCore::op_nx(u16) { return true; }

// ------------------------------------------------------------------ moves
bool DspCore::op_movr(u16 op) {
    u16 v = 0;
    if (!reg_read(0x18U + ((op >> 9) & 3U), v, false)) return false;
    const unsigned d = (op >> 8) & 1U;
    const s64 r = sx16(v) * 65536;
    set_acc(d, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_movax(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const s64 r = ax((op >> 9) & 1U);
    set_acc(d, r);
    flags("--XXXX00", r, false, false);
    return true;
}
bool DspCore::op_mov(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const s64 r = acc(1U - d);
    set_acc(d, r);
    flags("--X0XX00", r, false, false);
    return true;
}
bool DspCore::op_movp(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const bool c = prod_internal_carry();
    const s64 r = prod();
    set_acc(d, r);
    flags("--XXXX0X", r, c, false);
    return true;
}
bool DspCore::op_movnp(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const s64 p = prod(), r = sext40(0 - p);
    set_acc(d, r);
    flags_sub("--XXXX0X", 0, p, r);
    return true;
}
bool DspCore::op_movpz(u16 op) {
    const unsigned d = (op >> 8) & 1U;
    const bool c = prod_internal_carry();
    const s64 r = sext40u(u40(prod()) & ~0xFFFFULL);
    set_acc(d, r);
    flags("--XXXX0X", r, c, false);
    return true;
}
bool DspCore::op_addpaxz(u16 op) {
    const unsigned s = (op >> 9) & 1U, d = (op >> 8) & 1U;
    const s64 a = sext40u(u40(prod()) & ~0xFFFFULL), b = sx16(s_.ax_h[s]) * 65536;
    const s64 r = sext40(a + b);
    set_acc(d, r);
    flags_add("--XXXX0X", a, b, r);
    return true;
}

// ------------------------------------------------------------------ multiplier
bool DspCore::mul_into(s64 product, unsigned mode, unsigned r) {
    switch (mode) {
    case 0: break; // plain multiply
    case 1: {      // ..MVZ: $acR.hm = $prod.hm, $acR.l = 0
        const bool c = prod_internal_carry();
        const s64 v = sext40u(u40(prod()) & ~0xFFFFULL);
        set_acc(r, v);
        flags("--XXXX0X", v, c, false);
        break;
    }
    case 2: { // ..AC: $acR += $prod
        const s64 a = acc(r), p = prod(), v = sext40(a + p);
        set_acc(r, v);
        flags_add("--XXXX0X", a, p, v);
        break;
    }
    default: { // ..MV: $acR = $prod
        const bool c = prod_internal_carry();
        const s64 v = prod();
        set_acc(r, v);
        flags("--XXXX0X", v, c, false);
        break;
    }
    }
    set_prod(product);
    return true;
}
bool DspCore::op_mul_family(u16 op) { // MUL / MULMVZ / MULAC / MULMV: $axS.l * $axS.h
    const unsigned s = (op >> 11) & 1U;
    return mul_into(multiply(s_.ax_l[s], s_.ax_h[s]), (op >> 9) & 3U, (op >> 8) & 1U);
}
bool DspCore::op_mulx_family(u16 op) { // MULX*: $ax0.S * $ax1.T
    const unsigned s = (op >> 12) & 1U, t = (op >> 11) & 1U;
    const u16 a = s != 0U ? s_.ax_h[0] : s_.ax_l[0], b = t != 0U ? s_.ax_h[1] : s_.ax_l[1];
    return mul_into(multiply(a, b), (op >> 9) & 3U, (op >> 8) & 1U);
}
bool DspCore::op_mulc_family(u16 op) { // MULC*: $acS.m * $axT.h
    const unsigned s = (op >> 12) & 1U, t = (op >> 11) & 1U;
    return mul_into(multiply(s_.ac_m[s], s_.ax_h[t]), (op >> 9) & 3U, (op >> 8) & 1U);
}
bool DspCore::op_mulaxh(u16) { set_prod(multiply(s_.ax_h[0], s_.ax_h[0])); return true; }
bool DspCore::op_clrp(u16) {
    // manual: cleared by setting l = 0, m1 = 0xfff0, h = 0x00ff, m2 = 0x0010
    s_.prod_l = 0x0000; s_.prod_m1 = 0xFFF0; s_.prod_h = 0x00FF; s_.prod_m2 = 0x0010;
    written_ |= bit(20) | bit(21) | bit(22) | bit(23);
    return true;
}
bool DspCore::op_maddx(u16 op) { // MADDX / MSUBX: $(0x18+S*2) * $(0x19+T*2)
    const unsigned s = (op >> 9) & 1U, t = (op >> 8) & 1U;
    const s64 p = multiply(s != 0U ? s_.ax_h[0] : s_.ax_l[0], t != 0U ? s_.ax_h[1] : s_.ax_l[1]);
    set_prod(((op >> 10) & 1U) != 0U ? prod() - p : prod() + p);
    return true;
}
bool DspCore::op_maddc(u16 op) { // MADDC / MSUBC: $acS.m * $axT.h
    const unsigned s = (op >> 9) & 1U, t = (op >> 8) & 1U;
    const s64 p = multiply(s_.ac_m[s], s_.ax_h[t]);
    set_prod(((op >> 10) & 1U) != 0U ? prod() - p : prod() + p);
    return true;
}
bool DspCore::op_madd(u16 op) { // MADD (F2/F3) / MSUB (F6/F7): $axS.l * $axS.h
    const unsigned s = (op >> 8) & 1U;
    const s64 p = multiply(s_.ax_l[s], s_.ax_h[s]);
    set_prod(((op >> 10) & 1U) != 0U ? prod() - p : prod() + p);
    return true;
}

#undef ORCA_ADD
#undef ORCA_SUB

} // namespace orca::dsp
