// Orca Core — PowerPC to AArch64 JIT. See jit.hpp.
//
// Register use in translated code (AAPCS64):
//   x19 = CpuState*, x20 = JitExit*, x21 = Jit*   (callee-saved, set in the prologue)
//   x22-x28 = cache of guest GPRs (callee-saved, survive helper calls)
//   w8-w15 = temporaries, x0-x3 = helper arguments, x16 = helper address
// CR, XER, LR and CTR stay in CpuState and are read/written where used.
#include "orca/ppc/jit.hpp"

#include "cpu_internal.hpp"

#include "orca/ppc/mmu.hpp"
#include "orca/ppc/registers.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace orca::ppc {

using jit::Cond;
using jit::Emitter;
using jit::Reg;
using jit::Shift;
using jit::ZR;

namespace {

constexpr Reg kState = 19, kExit = 20, kSelf = 21;
constexpr Reg kCacheFirst = 22, kCacheCount = 7;
constexpr Reg T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12;

struct Offsets {
    u32 gpr, pc, lr, ctr, cr, xer;
};
const Offsets& offsets() {
    static const Offsets o = [] {
        CpuState s;
        auto off = [&](const void* p) { return static_cast<u32>(reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(&s)); };
        return Offsets{off(s.gpr.data()), off(&s.pc), off(&s.lr), off(&s.ctr), off(&s.cr), off(&s.xer)};
    }();
    return o;
}

constexpr u32 ppc_mask(unsigned mb, unsigned me) {
    const u32 a = 0xFFFFFFFFU >> mb, b = me == 31U ? 0xFFFFFFFFU : ~(0xFFFFFFFFU >> (me + 1U));
    return mb <= me ? (a & b) : (a | b);
}

} // namespace

// ---------------------------------------------------------------------------
// Translator
// ---------------------------------------------------------------------------
struct Jit::Compiler {
    Emitter e;
    const Offsets& o = offsets();
    std::vector<Emitter::Label> to_epilogue;

    // guest GPR cache: host register kCacheFirst + i holds guest reg slot[i]
    std::array<int, kCacheCount> slot{};
    std::array<bool, kCacheCount> dirty{};
    std::array<u32, kCacheCount> last_use{};
    u32 clock = 0;

    Compiler() { slot.fill(-1); }

    // --- state access -------------------------------------------------------
    void ld(Reg r, u32 off) { e.ldr(4, r, kState, off); }
    void st(Reg r, u32 off) { e.str(4, r, kState, off); }
    u32 gpr_off(unsigned g) const { return o.gpr + 4U * g; }

    int find(unsigned g) const {
        for (unsigned i = 0; i < kCacheCount; ++i) if (slot[i] == static_cast<int>(g)) return static_cast<int>(i);
        return -1;
    }
    unsigned victim(unsigned keep_a, unsigned keep_b) {
        for (unsigned i = 0; i < kCacheCount; ++i) if (slot[i] < 0) return i;
        unsigned best = kCacheCount;
        for (unsigned i = 0; i < kCacheCount; ++i) {
            if (slot[i] == static_cast<int>(keep_a) || slot[i] == static_cast<int>(keep_b)) continue;
            if (best == kCacheCount || last_use[i] < last_use[best]) best = i;
        }
        if (dirty[best]) st(static_cast<Reg>(kCacheFirst + best), gpr_off(static_cast<unsigned>(slot[best])));
        slot[best] = -1; dirty[best] = false;
        return best;
    }
    // host register with the value of guest GPR g (keep: guest regs that must stay cached)
    Reg get(unsigned g, unsigned keep_a = 99, unsigned keep_b = 99) {
        int i = find(g);
        if (i < 0) {
            i = static_cast<int>(victim(keep_a, keep_b));
            slot[static_cast<unsigned>(i)] = static_cast<int>(g);
            ld(static_cast<Reg>(kCacheFirst + i), gpr_off(g));
        }
        last_use[static_cast<unsigned>(i)] = ++clock;
        return static_cast<Reg>(kCacheFirst + i);
    }
    Reg get0(unsigned g, unsigned keep_a = 99, unsigned keep_b = 99) { return g == 0U ? ZR : get(g, keep_a, keep_b); }
    // host register to write guest GPR g (contents undefined until written)
    Reg def(unsigned g, unsigned keep_a = 99, unsigned keep_b = 99) {
        int i = find(g);
        if (i < 0) {
            i = static_cast<int>(victim(keep_a, keep_b));
            slot[static_cast<unsigned>(i)] = static_cast<int>(g);
        }
        dirty[static_cast<unsigned>(i)] = true;
        last_use[static_cast<unsigned>(i)] = ++clock;
        return static_cast<Reg>(kCacheFirst + i);
    }
    void flush_dirty() {
        for (unsigned i = 0; i < kCacheCount; ++i)
            if (slot[i] >= 0 && dirty[i]) st(static_cast<Reg>(kCacheFirst + i), gpr_off(static_cast<unsigned>(slot[i])));
    }
    void after_flush_clean() { dirty.fill(false); }

    // --- exits --------------------------------------------------------------
    // Leave the block: guest PC (in register or constant), steps completed, kind.
    void exit_const(u32 pc, u32 steps, u32 kind) {
        flush_dirty();
        e.mov_imm(false, T0, pc);
        st(T0, o.pc);
        e.mov_imm(false, T0, steps);
        e.str(4, T0, kExit, 0);
        e.mov_imm(false, 0, kind);
        to_epilogue.push_back(e.b());
    }
    void exit_reg(Reg pc, u32 steps, u32 kind) {
        flush_dirty();
        st(pc, o.pc);
        e.mov_imm(false, T0, steps);
        e.str(4, T0, kExit, 0);
        e.mov_imm(false, 0, kind);
        to_epilogue.push_back(e.b());
    }

    // --- flags --------------------------------------------------------------
    // CR field f (0-7) = 4-bit value in register v
    void set_cr_field_reg(unsigned f, Reg v) {
        ld(T4, o.cr);
        e.bfi(false, T4, v, 28U - 4U * f, 4);
        st(T4, o.cr);
    }
    // After a CMP of (a, b): field from the condition flags (signed or unsigned) + SO
    void cr_from_flags(unsigned f, bool is_signed) {
        e.movz(false, T2, 4);
        e.movz(false, T3, 8);
        e.csel(false, T2, T3, T2, is_signed ? Cond::LT : Cond::LO);
        e.movz(false, T3, 2);
        e.csel(false, T2, T3, T2, Cond::EQ);
        ld(T3, o.xer);
        e.lsr_imm(false, T3, T3, 31);
        e.orr(false, T2, T2, T3);
        set_cr_field_reg(f, T2);
    }
    void cr0_of(Reg r) {
        e.cmp_imm(false, r, 0);
        cr_from_flags(0, true);
    }
    // XER[CA] = register c (0/1)
    void set_ca_reg(Reg c) {
        ld(T4, o.xer);
        e.bfi(false, T4, c, 29, 1);
        st(T4, o.xer);
    }
    // XER[OV] = c (0/1); SO |= c
    void set_ov_reg(Reg c) {
        ld(T4, o.xer);
        e.bfi(false, T4, c, 30, 1);
        e.orr(false, T4, T4, c, Shift::LSL, 31);
        st(T4, o.xer);
    }
    void load_ca(Reg r) { ld(r, o.xer); e.ubfx(false, r, r, 29, 1); }

    // value of a 32-bit constant in a temporary
    Reg imm(Reg t, u32 v) {
        if (v == 0U) return ZR;
        e.mov_imm(false, t, v);
        return t;
    }

    // --- helper call ---------------------------------------------------------
    void call(const void* fn) {
        e.mov(true, 0, kSelf);
        e.mov_imm(true, 16, reinterpret_cast<u64>(fn));
        e.blr(16);
    }

    // --- prologue / epilogue --------------------------------------------------
    void prologue() {
        e.stp_pre(29, 30, jit::SP, -96);
        e.stp(19, 20, jit::SP, 16);
        e.stp(21, 22, jit::SP, 32);
        e.stp(23, 24, jit::SP, 48);
        e.stp(25, 26, jit::SP, 64);
        e.stp(27, 28, jit::SP, 80);
        e.mov(true, kState, 0);
        e.mov(true, kExit, 1);
        e.mov(true, kSelf, 2);
    }
    void epilogue() {
        const auto at = e.here();
        for (auto l : to_epilogue) e.bind(l, at);
        e.ldp(19, 20, jit::SP, 16);
        e.ldp(21, 22, jit::SP, 32);
        e.ldp(23, 24, jit::SP, 48);
        e.ldp(25, 26, jit::SP, 64);
        e.ldp(27, 28, jit::SP, 80);
        e.ldp_post(29, 30, jit::SP, 96);
        e.ret();
    }

    // Translates instruction `ins` at `pc` (index `i` in the block). Returns false
    // when the instruction is not handled (nothing emitted). Sets `ends` when the
    // instruction leaves the block itself (branches).
    Reg value_reg_ = ZR; // store value, fetched by the caller of load_store
    bool insn(u32 ins, u32 pc, u32 i, bool& ends);
    bool load_store(u32 ins, u32 pc, u32 i, unsigned size, bool load, bool update, bool sign, bool reverse,
                    unsigned rt, unsigned ra, Reg ea_reg_or_zr, s32 disp, Reg rb_reg);
    bool op31(u32 ins, u32 pc, u32 i, bool& ends);
    bool branch(u32 ins, u32 pc, u32 i, unsigned kind, bool& ends);
};

namespace {
constexpr unsigned F_RT(u32 i) { return (i >> 21) & 31U; }
constexpr unsigned F_RA(u32 i) { return (i >> 16) & 31U; }
constexpr unsigned F_RB(u32 i) { return (i >> 11) & 31U; }
constexpr s32 F_SIMM(u32 i) { return static_cast<s32>(static_cast<s16>(i & 0xFFFFU)); }
constexpr u32 F_UIMM(u32 i) { return i & 0xFFFFU; }
constexpr bool F_RC(u32 i) { return (i & 1U) != 0U; }
} // namespace

// Kind codes returned in w0: 0 = continue at state.pc, 1 = interpret the instruction at state.pc.
static constexpr u32 kExitNormal = 0, kExitFallback = 1;

bool Jit::Compiler::load_store(u32 ins, u32 pc, u32 i, unsigned size, bool load, bool update, bool sign, bool reverse,
                               unsigned rt, unsigned ra, Reg base, s32 disp, Reg rb_reg) {
    (void)ins;
    // The caller fetched every guest operand (base, index and, for stores, the value
    // in `value_reg_`) so no cache eviction can happen between here and the end.
    auto ea_into = [&](Reg d) {
        if (rb_reg != 255U) e.add(false, d, base, rb_reg);          // shifted-register ADD: 31 = ZR
        else if (base == ZR) e.mov_imm(false, d, static_cast<u32>(disp)); // ADD (immediate) would read SP
        else if (disp >= 0 && disp < 4096) e.add_imm(false, d, base, static_cast<u32>(disp));
        else if (disp < 0 && disp > -4096) e.sub_imm(false, d, base, static_cast<u32>(-disp));
        // scratch T2: T0 holds the loaded value when the EA is recomputed for an update
        else { e.mov_imm(false, T2, static_cast<u32>(disp)); e.add(false, d, base, T2); }
    };
    ea_into(1);
    e.movz(false, 2, static_cast<u16>(size));
    if (load) {
        e.mov(true, 3, kExit);
        call(reinterpret_cast<const void*>(&Jit::helper_load));
        const auto ok = e.cbnz(false, 0);
        exit_const(pc, i, kExitFallback);
        e.bind_here(ok);
        e.ldr(4, T0, kExit, 4);
        if (reverse) {
            if (size == 4U) e.rev(false, T0, T0);
            else { e.rev16(false, T0, T0); e.uxth(T0, T0); }
        } else if (sign) {
            e.sxth(false, T0, T0);
        }
        if (update) { ea_into(T1); e.mov(false, def(ra, rt), T1); }
        e.mov(false, def(rt, ra), T0);
    } else {
        const Reg v = value_reg_;
        if (reverse) {
            if (size == 4U) e.rev(false, 3, v);
            else { e.rev16(false, 3, v); e.uxth(3, 3); }
        } else {
            e.mov(false, 3, v);
        }
        call(reinterpret_cast<const void*>(&Jit::helper_store));
        const auto ok = e.cbnz(false, 0);
        exit_const(pc, i, kExitFallback);
        e.bind_here(ok);
        if (update) { ea_into(T1); e.mov(false, def(ra), T1); }
        // w0 == 2: the store hit this block's own code -> stop right after it
        e.cmp_imm(false, 0, 2);
        const auto same = e.b_cond(Cond::NE);
        exit_const(pc + 4U, i + 1U, kExitNormal);
        e.bind_here(same);
    }
    return true;
}

bool Jit::Compiler::branch(u32 ins, u32 pc, u32 i, unsigned kind, bool& ends) {
    // kind: 0 = bc, 1 = bclr, 2 = bcctr, 3 = b
    ends = true;
    const bool lk = (ins & 1U) != 0U;
    if (kind == 3U) {
        const s32 li = static_cast<s32>((ins & 0x03FFFFFCU) << 6) >> 6;
        const u32 target = (ins & 2U) != 0U ? static_cast<u32>(li) : pc + static_cast<u32>(li);
        if (lk) { e.mov_imm(false, T0, pc + 4U); st(T0, o.lr); }
        exit_const(target, i + 1U, kExitNormal);
        return true;
    }
    const unsigned bo = F_RT(ins), bi = F_RA(ins);
    if (kind == 2U && (bo & 4U) == 0U) return false; // invalid form: interpreter
    // target into T1 (captured before LR/CTR are written)
    if (kind == 0U) {
        const s32 disp = F_SIMM(ins & 0xFFFCU);
        e.mov_imm(false, T1, (ins & 2U) != 0U ? static_cast<u32>(disp) : pc + static_cast<u32>(disp));
    } else {
        ld(T1, kind == 1U ? o.lr : o.ctr);
        e.and_imm(false, T1, T1, 0xFFFFFFFCU);
    }
    // condition into T2 (1 = taken)
    e.movz(false, T2, 1);
    if ((bo & 4U) == 0U) {
        ld(T3, o.ctr);
        e.sub_imm(false, T3, T3, 1);
        st(T3, o.ctr);
        e.cmp_imm(false, T3, 0);
        // ctr_ok = (ctr != 0) != BO[1]
        e.cset(false, T3, (bo & 2U) != 0U ? Cond::EQ : Cond::NE);
        e.and_(false, T2, T2, T3);
    }
    if ((bo & 16U) == 0U) {
        ld(T3, o.cr);
        e.ubfx(false, T3, T3, 31U - bi, 1);
        if ((bo & 8U) == 0U) e.eor_imm(false, T3, T3, 1);
        e.and_(false, T2, T2, T3);
    }
    if (lk) { e.mov_imm(false, T3, pc + 4U); st(T3, o.lr); }
    e.mov_imm(false, T3, pc + 4U);
    e.cmp_imm(false, T2, 0);
    e.csel(false, T1, T1, T3, Cond::NE);
    exit_reg(T1, i + 1U, kExitNormal);
    return true;
}

bool Jit::Compiler::op31(u32 ins, u32 pc, u32 i, bool& ends) {
    (void)ends;
    const unsigned rt = F_RT(ins), ra = F_RA(ins), rb = F_RB(ins);
    const unsigned xo10 = (ins >> 1) & 0x3FFU, xo9 = (ins >> 1) & 0x1FFU;
    const bool rc = F_RC(ins), oe = (ins & 0x400U) != 0U;
    auto ls = [&](unsigned size, bool load, bool update, bool sign, bool reverse) {
        if (update && ra == 0U) return false;
        if (update && load && ra == rt) return false;
        const Reg base = get0(ra, rb, rt);
        const Reg rbr = get(rb, ra, rt);
        if (!load) value_reg_ = get(rt, ra, rb); // stores read r0 itself, never zero
        return load_store(ins, pc, i, size, load, update, sign, reverse, rt, ra, base, 0, rbr);
    };
    switch (xo10) {
    case 23: return ls(4, true, false, false, false);   // lwzx
    case 55: return ls(4, true, true, false, false);    // lwzux
    case 87: return ls(1, true, false, false, false);   // lbzx
    case 119: return ls(1, true, true, false, false);   // lbzux
    case 279: return ls(2, true, false, false, false);  // lhzx
    case 311: return ls(2, true, true, false, false);   // lhzux
    case 343: return ls(2, true, false, true, false);   // lhax
    case 375: return ls(2, true, true, true, false);    // lhaux
    case 151: return ls(4, false, false, false, false); // stwx
    case 183: return ls(4, false, true, false, false);  // stwux
    case 215: return ls(1, false, false, false, false); // stbx
    case 247: return ls(1, false, true, false, false);  // stbux
    case 407: return ls(2, false, false, false, false); // sthx
    case 439: return ls(2, false, true, false, false);  // sthux
    case 534: return ls(4, true, false, false, true);   // lwbrx
    case 790: return ls(2, true, false, false, true);   // lhbrx
    case 662: return ls(4, false, false, false, true);  // stwbrx
    case 918: return ls(2, false, false, false, true);  // sthbrx
    case 0: case 32: { // cmp / cmpl
        if ((ins & (1U << 21)) != 0U) return false; // L = 1
        const Reg a = get(ra, rb), b = get(rb, ra);
        e.cmp(false, a, b);
        cr_from_flags(rt >> 2, xo10 == 0U);
        return true;
    }
    case 19: { // mfcr
        ld(T0, o.cr);
        e.mov(false, def(rt), T0);
        return true;
    }
    case 144: { // mtcrf
        const u32 fxm = (ins >> 12) & 0xFFU;
        u32 mask = 0;
        for (unsigned k = 0; k < 8U; ++k) if ((fxm & (0x80U >> k)) != 0U) mask |= 0xF0000000U >> (k * 4U);
        const Reg s = get(rt);
        ld(T0, o.cr);
        if (mask == 0xFFFFFFFFU) { e.mov(false, T0, s); }
        else if (mask != 0U) {
            e.mov_imm(false, T1, mask);
            e.bic(false, T0, T0, T1);
            e.and_(false, T2, s, T1);
            e.orr(false, T0, T0, T2);
        }
        st(T0, o.cr);
        return true;
    }
    case 339: case 467: { // mfspr / mtspr: only XER, LR, CTR
        const unsigned n = ((ins >> 16) & 0x1FU) | (((ins >> 11) & 0x1FU) << 5);
        u32 off;
        if (n == 1U) off = o.xer; else if (n == 8U) off = o.lr; else if (n == 9U) off = o.ctr; else return false;
        if (xo10 == 339U) { ld(T0, off); e.mov(false, def(rt), T0); }
        else {
            const Reg s = get(rt);
            if (n == 1U) { e.mov_imm(false, T1, XER_SO | XER_OV | XER_CA | XER_BYTE_COUNT); e.and_(false, T0, s, T1); st(T0, off); }
            else st(s, off);
        }
        return true;
    }
    default: break;
    }
    // logical and shifts (X-form, Rc)
    {
        const unsigned rs = rt;
        bool handled = true;
        auto logical2 = [&](auto op) {
            const Reg a = get(rs, rb), b = get(rb, rs);
            op(T0, a, b);
            e.mov(false, def(ra, rs, rb), T0);
            if (rc) cr0_of(T0);
        };
        switch (xo10) {
        case 28: logical2([&](Reg d, Reg a, Reg b) { e.and_(false, d, a, b); }); break;
        case 60: logical2([&](Reg d, Reg a, Reg b) { e.bic(false, d, a, b); }); break;
        case 124: logical2([&](Reg d, Reg a, Reg b) { e.orr(false, d, a, b); e.mvn(false, d, d); }); break;
        case 284: logical2([&](Reg d, Reg a, Reg b) { e.eon(false, d, a, b); }); break;
        case 316: logical2([&](Reg d, Reg a, Reg b) { e.eor(false, d, a, b); }); break;
        case 412: logical2([&](Reg d, Reg a, Reg b) { e.orn(false, d, a, b); }); break;
        case 444: logical2([&](Reg d, Reg a, Reg b) { e.orr(false, d, a, b); }); break;
        case 476: logical2([&](Reg d, Reg a, Reg b) { e.and_(false, d, a, b); e.mvn(false, d, d); }); break;
        case 922: case 954: case 26: { // extsh / extsb / cntlzw
            const Reg s = get(rs);
            if (xo10 == 922U) e.sxth(false, T0, s); else if (xo10 == 954U) e.sxtb(false, T0, s); else e.clz(false, T0, s);
            e.mov(false, def(ra, rs), T0);
            if (rc) cr0_of(T0);
            break;
        }
        case 24: case 536: { // slw / srw: 6-bit amount, 64-bit shift gives 0 for 32..63
            const Reg s = get(rs, rb), b = get(rb, rs);
            e.and_imm(false, T1, b, 0x3F);
            e.mov(false, T0, s); // zero-extends into x8
            if (xo10 == 24U) e.lslv(true, T0, T0, T1); else e.lsrv(true, T0, T0, T1);
            e.mov(false, def(ra, rs, rb), T0);
            if (rc) cr0_of(T0);
            break;
        }
        case 792: { // sraw
            const Reg s = get(rs, rb), b = get(rb, rs);
            e.and_imm(false, T1, b, 0x3F);
            e.sxtw(T0, s);
            e.asrv(true, T2, T0, T1);            // result (sign fill for >= 32)
            // CA = (rs < 0) && (rs & ((1 << n) - 1)) != 0, all 32 bits for n >= 32
            e.movz(true, T3, 1);
            e.lslv(true, T3, T3, T1);
            e.sub_imm(true, T3, T3, 1);
            e.mov(false, T4, s);                 // zero-extended rs
            e.and_(true, T3, T3, T4);
            e.cmp_imm(true, T3, 0);
            e.cset(false, T3, Cond::NE);
            e.and_(false, T3, T3, T4, Shift::LSR, 31);
            set_ca_reg(T3);
            e.mov(false, def(ra, rs, rb), T2);
            if (rc) cr0_of(T2);
            break;
        }
        case 824: { // srawi
            const unsigned sh = rb;
            const Reg s = get(rs);
            e.asr_imm(false, T2, s, sh);
            if (sh == 0U) {
                e.mov(false, T3, ZR);
            } else {
                e.mov_imm(false, T3, (1U << sh) - 1U);
                e.tst(false, s, T3);
                e.cset(false, T3, Cond::NE);
                e.and_(false, T3, T3, s, Shift::LSR, 31);
            }
            set_ca_reg(T3);
            e.mov(false, def(ra, rs), T2);
            if (rc) cr0_of(T2);
            break;
        }
        default: handled = false; break;
        }
        if (handled) return true;
    }
    // XO-form arithmetic
    const Reg zr = ZR;
    auto arith_tail = [&](Reg value, bool ov_valid) {
        e.mov(false, def(rt, ra, rb), value);
        if (oe && ov_valid) set_ov_reg(T3);
        if (rc) cr0_of(value);
    };
    switch (xo9) {
    case 266: case 10: case 138: case 202: case 234: case 40: case 8: case 136: case 200: case 232: case 104: {
        // a + b + c with a = RA or ~RA, b = RB / 0 / -1, c = 0 / 1 / CA
        const bool neg_a = xo9 == 40U || xo9 == 8U || xo9 == 136U || xo9 == 200U || xo9 == 232U || xo9 == 104U;
        const bool b_rb = xo9 == 266U || xo9 == 10U || xo9 == 138U || xo9 == 40U || xo9 == 8U || xo9 == 136U;
        const bool b_ones = xo9 == 234U || xo9 == 232U;
        const bool c_ca = xo9 == 138U || xo9 == 202U || xo9 == 234U || xo9 == 136U || xo9 == 200U || xo9 == 232U;
        const bool c_one = xo9 == 40U || xo9 == 8U || xo9 == 104U;
        const bool sets_ca = !(xo9 == 266U || xo9 == 40U || xo9 == 104U);
        const Reg a = get(ra, rb);
        if (neg_a) e.mvn(false, T0, a); else e.mov(false, T0, a);
        Reg b = zr;
        if (b_rb) b = get(rb, ra);
        else if (b_ones) { e.movn(false, T1, 0); b = T1; }
        // carry-in into the C flag: CMP wzr? Use ADDS of carry-in with 0xFFFFFFFF (sets C iff c == 1)
        if (c_ca) {
            load_ca(T2);
            e.movn(false, T3, 0);
            e.adds(false, ZR, T2, T3);  // C = (ca + 0xFFFFFFFF) carries = (ca == 1)
        } else if (c_one) {
            e.cmp(false, ZR, ZR);       // 0 - 0: C = 1 (no borrow)
        } else {
            e.adds(false, ZR, ZR, ZR); // 0 + 0: C = 0
        }
        e.adcs(false, T0, T0, b);
        e.cset(false, T2, Cond::HS);
        e.cset(false, T3, Cond::VS);
        if (sets_ca) set_ca_reg(T2);
        arith_tail(T0, true);
        return true;
    }
    case 235: { // mullw
        const Reg a = get(ra, rb), b = get(rb, ra);
        e.smull(T1, a, b);
        if (oe) { e.sxtw(T2, T1); e.cmp(true, T1, T2); e.cset(false, T3, Cond::NE); } // product does not fit in 32 bits
        e.mov(false, T0, T1);
        arith_tail(T0, true);
        return true;
    }
    case 75: case 11: { // mulhw / mulhwu
        if (oe) return false;
        const Reg a = get(ra, rb), b = get(rb, ra);
        if (xo9 == 75U) e.smull(T1, a, b); else e.umull(T1, a, b);
        e.lsr_imm(true, T0, T1, 32);
        arith_tail(T0, false);
        return true;
    }
    case 491: { // divw: b == 0 or (INT_MIN / -1) -> a < 0 ? -1 : 0, OV = 1
        const Reg a = get(ra, rb), b = get(rb, ra);
        e.asr_imm(false, T2, a, 31);           // special result
        e.sdiv(false, T0, a, b);
        // special if b == 0 or (a == 0x80000000 and b == -1)
        e.cmp_imm(false, b, 0);
        e.cset(false, T3, Cond::EQ);
        e.mov_imm(false, T1, 0x80000000U);
        e.cmp(false, a, T1);
        e.cset(false, T4, Cond::EQ);
        e.adds_imm(false, ZR, b, 1);           // Z = (b == -1)
        e.cset(false, T1, Cond::EQ);
        e.and_(false, T4, T4, T1);
        e.orr(false, T3, T3, T4);
        e.cmp_imm(false, T3, 0);
        e.csel(false, T0, T2, T0, Cond::NE);
        arith_tail(T0, true);
        return true;
    }
    case 459: { // divwu: b == 0 -> 0, OV = 1
        const Reg a = get(ra, rb), b = get(rb, ra);
        e.udiv(false, T0, a, b);               // AArch64 gives 0 for /0
        e.cmp_imm(false, b, 0);
        e.cset(false, T3, Cond::EQ);
        arith_tail(T0, true);
        return true;
    }
    default: return false;
    }
}

bool Jit::Compiler::insn(u32 ins, u32 pc, u32 i, bool& ends) {
    ends = false;
    const unsigned op = ins >> 26;
    const unsigned rt = F_RT(ins), ra = F_RA(ins);
    const s32 simm = F_SIMM(ins);
    const u32 uimm = F_UIMM(ins);
    auto add_const = [&](Reg d, Reg s, u32 v) {
        if (v == 0U) { e.mov(false, d, s); return; }
        if (v < 4096U) { e.add_imm(false, d, s, v); return; }
        if (-v < 4096U) { e.sub_imm(false, d, s, -v); return; }
        if ((v & 0xFFFU) == 0U && v < (4096U << 12)) { e.add_imm(false, d, s, v >> 12, true); return; }
        e.mov_imm(false, T1, v);
        e.add(false, d, s, T1);
    };
    switch (op) {
    case 14: case 15: { // addi / addis
        const u32 v = op == 14U ? static_cast<u32>(simm) : static_cast<u32>(simm) << 16;
        if (ra == 0U) { e.mov_imm(false, T0, v); e.mov(false, def(rt), T0); }
        else { const Reg s = get(ra); add_const(T0, s, v); e.mov(false, def(rt, ra), T0); }
        return true;
    }
    case 12: case 13: { // addic / addic.
        const Reg s = get(ra);
        e.mov_imm(false, T1, static_cast<u32>(simm));
        e.adds(false, T0, s, T1);
        e.cset(false, T2, Cond::HS);
        set_ca_reg(T2);
        e.mov(false, def(rt, ra), T0);
        if (op == 13U) cr0_of(T0);
        return true;
    }
    case 8: { // subfic: simm - ra, CA = no borrow
        const Reg s = get(ra);
        e.mov_imm(false, T1, static_cast<u32>(simm));
        e.subs(false, T0, T1, s);
        e.cset(false, T2, Cond::HS);
        set_ca_reg(T2);
        e.mov(false, def(rt, ra), T0);
        return true;
    }
    case 7: { // mulli
        const Reg s = get(ra);
        e.mov_imm(false, T1, static_cast<u32>(simm));
        e.mul(false, T0, s, T1);
        e.mov(false, def(rt, ra), T0);
        return true;
    }
    case 10: case 11: { // cmpli / cmpi
        if ((ins & (1U << 21)) != 0U) return false;
        const Reg s = get(ra);
        e.mov_imm(false, T1, op == 10U ? uimm : static_cast<u32>(simm));
        e.cmp(false, s, T1);
        cr_from_flags(rt >> 2, op == 11U);
        return true;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: { // ori oris xori xoris andi. andis.
        const unsigned rs = rt;
        const u32 v = (op & 1U) != 0U ? uimm << 16 : uimm;
        const Reg s = get(rs);
        if (op <= 25U) { if (v == 0U) e.mov(false, T0, s); else if (!e.orr_imm(false, T0, s, v)) { e.mov_imm(false, T1, v); e.orr(false, T0, s, T1); } }
        else if (op <= 27U) { if (v == 0U) e.mov(false, T0, s); else if (!e.eor_imm(false, T0, s, v)) { e.mov_imm(false, T1, v); e.eor(false, T0, s, T1); } }
        else { if (v == 0U) e.mov(false, T0, ZR); else if (!e.and_imm(false, T0, s, v)) { e.mov_imm(false, T1, v); e.and_(false, T0, s, T1); } }
        e.mov(false, def(ra, rs), T0);
        if (op >= 28U) cr0_of(T0);
        return true;
    }
    case 21: case 23: case 20: { // rlwinm / rlwnm / rlwimi
        const unsigned rs = rt, sh = F_RB(ins), mb = (ins >> 6) & 31U, me = (ins >> 1) & 31U;
        const u32 m = ppc_mask(mb, me);
        if (op == 23U) {
            const Reg s = get(rs, sh), b = get(sh, rs);
            e.neg(false, T1, b);
            e.rorv(false, T0, s, T1);
        } else {
            const Reg s = get(rs, ra);
            if (sh == 0U) e.mov(false, T0, s); else e.ror_imm(false, T0, s, 32U - sh);
        }
        if (m != 0xFFFFFFFFU) e.and_imm(false, T0, T0, m);
        if (op == 20U) {
            const Reg old = get(ra, rs);
            if (m != 0xFFFFFFFFU) {
                if (m == 0U) e.mov(false, T0, old);
                else { e.and_imm(false, T1, old, ~m); e.orr(false, T0, T0, T1); }
            }
        }
        e.mov(false, def(ra, rs), T0);
        if (F_RC(ins)) cr0_of(T0);
        return true;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43:
    case 36: case 37: case 38: case 39: case 44: case 45: { // D-form integer loads/stores
        const bool update = (op & 1U) != 0U;
        const bool load = op <= 35U || (op >= 40U && op <= 43U);
        const unsigned size = (op <= 33U || op == 36U || op == 37U) ? 4U : ((op >= 40U) ? 2U : 1U);
        if (update && ra == 0U) return false;
        if (update && load && ra == rt) return false;
        const Reg base = get0(ra, rt);
        if (!load) value_reg_ = get(rt, ra);
        return load_store(ins, pc, i, size, load, update, op == 42U || op == 43U, false, rt, ra, base, simm, 255U);
    }
    case 18: return branch(ins, pc, i, 3, ends);
    case 16: return branch(ins, pc, i, 0, ends);
    case 19: {
        const unsigned xo = (ins >> 1) & 0x3FFU;
        if (xo == 16U) return branch(ins, pc, i, 1, ends);
        if (xo == 528U) return branch(ins, pc, i, 2, ends);
        if (xo == 0U) { // mcrf
            const unsigned bf = (ins >> 23) & 7U, bfa = (ins >> 18) & 7U;
            ld(T0, o.cr);
            e.ubfx(false, T1, T0, 28U - 4U * bfa, 4);
            e.bfi(false, T0, T1, 28U - 4U * bf, 4);
            st(T0, o.cr);
            return true;
        }
        const unsigned bt = rt, ba = ra, bb = F_RB(ins);
        auto crop = [&](auto f) {
            ld(T0, o.cr);
            e.ubfx(false, T1, T0, 31U - ba, 1);
            e.ubfx(false, T2, T0, 31U - bb, 1);
            f(T1, T2);
            e.bfi(false, T0, T1, 31U - bt, 1);
            st(T0, o.cr);
            return true;
        };
        switch (xo) {
        case 257: return crop([&](Reg a, Reg b) { e.and_(false, a, a, b); });                       // crand
        case 129: return crop([&](Reg a, Reg b) { e.bic(false, a, a, b); });                        // crandc
        case 289: return crop([&](Reg a, Reg b) { e.eor(false, a, a, b); e.eor_imm(false, a, a, 1); }); // creqv
        case 225: return crop([&](Reg a, Reg b) { e.and_(false, a, a, b); e.eor_imm(false, a, a, 1); }); // crnand
        case 33: return crop([&](Reg a, Reg b) { e.orr(false, a, a, b); e.eor_imm(false, a, a, 1); });  // crnor
        case 449: return crop([&](Reg a, Reg b) { e.orr(false, a, a, b); });                        // cror
        case 417: return crop([&](Reg a, Reg b) { e.eor_imm(false, b, b, 1); e.orr(false, a, a, b); }); // crorc
        case 193: return crop([&](Reg a, Reg b) { e.eor(false, a, a, b); });                        // crxor
        default: return false;
        }
    }
    case 31: return op31(ins, pc, i, ends);
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
Jit::Jit(Cpu& cpu, mem::Bus& bus) : cpu_(cpu), bus_(bus) {
    if (jit::host_can_execute()) code_ = std::make_unique<jit::CodeBuffer>();
    bus_.set_code_invalidation_handler([this](u32 address, u32 size) { invalidate(address, size); });
}

Jit::~Jit() { bus_.set_code_invalidation_handler(nullptr); }

bool Jit::can_execute() const noexcept { return code_ != nullptr && code_->ok(); }

void Jit::clear() {
    for (auto& [page, list] : page_blocks_) bus_.unmark_code_page(page << 12);
    blocks_.clear();
    page_blocks_.clear();
    retired_blocks_.clear();
    if (code_) code_->reset();
}

void Jit::invalidate(u32 address, u32 size) {
    const u32 first = address >> 12, last = static_cast<u32>((static_cast<u64>(address) + (size == 0U ? 1U : size) - 1U) >> 12);
    for (u32 p = first; p <= last; ++p) {
        auto it = page_blocks_.find(p);
        if (it == page_blocks_.end()) continue;
        for (Block* b : it->second) {
            if (!b->valid) continue;
            const u32 bend = b->pa + b->count * 4U;
            if (static_cast<u64>(address) + size <= b->pa || address >= bend) continue;
            b->valid = false;
            ++stats_.invalidations;
            if (b == running_) running_invalidated_ = true;
            auto bi = blocks_.find(b->ea);
            if (bi != blocks_.end() && bi->second.get() == b) {
                retired_blocks_.push_back(std::move(bi->second));
                blocks_.erase(bi);
            }
        }
        std::erase_if(it->second, [](Block* b) { return !b->valid; });
        if (it->second.empty()) { bus_.unmark_code_page(p << 12); page_blocks_.erase(it); }
        if (p == 0xFFFFFU) break;
    }
}

u32 Jit::helper_load(Jit* self, u32 ea, u32 size, JitExit* out) {
    Cpu& c = self->cpu_;
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    if (!c.translate_data(ea, size, false, lo, hi, split, wimg).ok() || split != size) return 0;
    if (!c.locked_cache_.empty() && c.locked_cache_.count(lo & ~31U) != 0U) return 0;
    const u8* p = self->bus_.ram_pointer(lo, size);
    if (p == nullptr) return 0;
    u32 v = 0;
    for (u32 k = 0; k < size; ++k) v = (v << 8) | p[k];
    out->value = v;
    return 1;
}

u32 Jit::helper_store(Jit* self, u32 ea, u32 size, u32 value) {
    Cpu& c = self->cpu_;
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    if (!c.translate_data(ea, size, true, lo, hi, split, wimg).ok() || split != size) return 0;
    if (!c.locked_cache_.empty() && c.locked_cache_.count(lo & ~31U) != 0U) return 0;
    if (self->bus_.ram_pointer(lo, size) == nullptr) return 0;
    self->running_invalidated_ = false;
    const mem::BusStatus s = self->bus_.write(lo, size, value, mem::AccessSource::Cpu);
    if (s != mem::BusStatus::Ok) return 0; // cannot happen for RAM; keep the interpreter's handling
    return self->running_invalidated_ ? 2U : 1U;
}

bool Jit::compile(u32 pc, u32 pa, std::vector<u32>& words, u32& count, std::string& error) {
    Compiler c;
    c.prologue();
    count = 0;
    bool ended = false;
    u32 cur = pc;
    while (count < kMaxBlock) {
        if (count > 0U && (cur & 0xFFFU) == 0U) break; // stay inside one page (one fetch translation)
        const u8* p = bus_.ram_pointer(pa + (cur - pc), 4);
        if (p == nullptr) break;
        const u32 ins = (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
        const std::size_t mark = c.e.size();
        const auto saved_slot = c.slot; const auto saved_dirty = c.dirty; const auto saved_use = c.last_use;
        const auto saved_exits = c.to_epilogue.size();
        bool ends = false;
        if (!c.insn(ins, cur, count, ends)) {
            // not translated: undo anything emitted and stop before it
            c.e.words().resize(mark);
            c.slot = saved_slot; c.dirty = saved_dirty; c.last_use = saved_use;
            c.to_epilogue.resize(saved_exits);
            break;
        }
        ++count;
        cur += 4U;
        if (ends) { ended = true; break; }
    }
    if (count == 0U) { error = "first instruction not translated"; return false; }
    if (!ended) c.exit_const(cur, count, count < kMaxBlock && (cur & 0xFFFU) != 0U ? kExitFallback : kExitNormal);
    c.epilogue();
    words = std::move(c.e.words());
    return true;
}

bool Jit::translate_for_test(u32 pc, std::vector<u32>& words, u32& guest_instructions, std::string& error) {
    const Translation t = translate(cpu_.state_, bus_, pc, AccessType::Fetch, cpu_.config_.extended_bats);
    if (t.status != TranslateStatus::Ok) { error = "fetch translation failed"; return false; }
    return compile(pc, t.physical, words, guest_instructions, error);
}

Jit::Block* Jit::lookup_or_compile(u32 pc, u32 pa, std::string& error) {
    auto it = blocks_.find(pc);
    if (it != blocks_.end()) {
        if (it->second->valid && it->second->pa == pa) return it->second.get();
        retired_blocks_.push_back(std::move(it->second));
        blocks_.erase(it);
    }
    std::vector<u32> words;
    u32 count = 0;
    if (!compile(pc, pa, words, count, error)) return nullptr;
    const u32* code = code_->append(words);
    if (code == nullptr) { // full: start over
        clear();
        code = code_->append(words);
        if (code == nullptr) { error = code_->error(); return nullptr; }
    }
    auto b = std::make_unique<Block>();
    b->ea = pc; b->pa = pa; b->count = count; b->code = code;
    Block* raw = b.get();
    blocks_[pc] = std::move(b);
    const u32 first = pa >> 12, last = (pa + count * 4U - 1U) >> 12;
    for (u32 p = first; p <= last; ++p) { page_blocks_[p].push_back(raw); bus_.mark_code_page(p << 12); }
    ++stats_.blocks_compiled;
    return raw;
}

Jit::Result Jit::interpret() {
    ++stats_.interpreter_steps;
    return {cpu_.step(), 1};
}

Jit::Result Jit::run(u64 max_steps) {
    if (!can_execute() || max_steps == 0U) return interpret();
    CpuState& s = cpu_.state_;
    if (s.halted) return interpret();
    if ((s.msr & (MSR_SE | MSR_BE | MSR_LE)) != 0U) return interpret();
    if ((s.msr & MSR_EE) != 0U && (cpu_.external_line_ || s.dec_pending)) return interpret();
    if ((s.msr & MSR_POW) != 0U && (s.hid0 & 0x00E00000U) != 0U) return interpret();
    const u32 pc = s.pc;
    if ((pc & 3U) != 0U) return interpret();
    const Translation t = translate(s, bus_, pc, AccessType::Fetch, cpu_.config_.extended_bats);
    if (t.status != TranslateStatus::Ok) return interpret();
    std::string error;
    Block* b = lookup_or_compile(pc, t.physical, error);
    if (b == nullptr || b->count > max_steps) return interpret();
    if (!code_->make_executable()) return interpret();
    using Fn = u32 (*)(CpuState*, JitExit*, Jit*);
    running_ = b;
    running_invalidated_ = false;
    exit_ = {};
    const u32 kind = reinterpret_cast<Fn>(reinterpret_cast<std::uintptr_t>(b->code))(&s, &exit_, this);
    running_ = nullptr;
    const u64 steps = exit_.steps;
    cpu_.retired_ += steps;
    ++stats_.block_runs;
    stats_.block_steps += steps;
    if (kind == kExitFallback) ++stats_.fallback_exits;
    if (steps == 0U) return interpret(); // left before its first instruction: let the interpreter do it
    return {StepResult::Executed, steps};
}

} // namespace orca::ppc
