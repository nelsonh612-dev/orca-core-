// Orca Core — PowerPC to AArch64 JIT. See jit.hpp.
//
// Register use in translated code (AAPCS64):
//   x19 = CpuState*, x20 = JitExit*, x21 = Jit*   (callee-saved, set in the prologue)
//   x22-x28 = cache of guest GPRs (callee-saved, survive helper calls)
//   w8-w15 = temporaries, x0-x3 = helper arguments, x16 = helper address
// CR, XER, LR and CTR stay in CpuState and are read/written where used.
#include "orca/ppc/jit.hpp"

#include "cpu_internal.hpp"

#include "orca/ppc/fpu.hpp"
#include "orca/ppc/mmu.hpp"
#include "orca/ppc/registers.hpp"

#include <algorithm>
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
    u32 gpr, pc, lr, ctr, cr, xer, fpr, fpscr, msr, hid2;
};
const Offsets& offsets() {
    static const Offsets o = [] {
        CpuState s;
        auto off = [&](const void* p) { return static_cast<u32>(reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(&s)); };
        return Offsets{off(s.gpr.data()), off(&s.pc), off(&s.lr), off(&s.ctr), off(&s.cr), off(&s.xer),
                       off(s.fpr.data()), off(&s.fpscr), off(&s.msr), off(&s.hid2)};
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
    // JitExit::steps accumulates over linked blocks
    void add_steps(u32 steps) {
        e.ldr(4, T0, kExit, 0);
        if (steps != 0U) e.add_imm(false, T0, T0, steps);
        e.str(4, T0, kExit, 0);
    }
    void exit_const(u32 pc, u32 steps, u32 kind) {
        flush_dirty();
        add_steps(steps);
        e.mov_imm(false, T0, pc);
        st(T0, o.pc);
        e.mov_imm(false, 0, kind);
        to_epilogue.push_back(e.b());
    }
    void exit_reg(Reg pc, u32 steps, u32 kind) {
        flush_dirty();
        st(pc, o.pc);
        add_steps(steps);
        e.mov_imm(false, 0, kind);
        to_epilogue.push_back(e.b());
    }
    // Normal exit to a constant guest address, linkable: the site
    //   movz w10, #context ; cmp w9 (current context), w10 ; b.ne stub ; b stub
    // gets the context id and the target block's linked entry patched in (Jit::link).
    // The stub returns to the dispatcher with the site's address in JitExit::site.
    std::vector<u32> sites; // word index of each site's MOVZ
    u32 link_ctx_off = 0;   // offset from x21 of Jit::link_context_
    void exit_link(u32 target, u32 steps) {
        flush_dirty();
        add_steps(steps);
        e.ldr(4, T1, kSelf, link_ctx_off);
        const auto site = e.here();
        e.movz(false, T2, 0xFFFFU);
        e.cmp(false, T1, T2);
        const auto ne = e.b_cond(Cond::NE);
        const auto linked = e.b();
        e.bind_here(ne);
        e.bind_here(linked);
        e.adr(T1, -static_cast<s32>((e.here() - site) * 4U));
        e.str(8, T1, kExit, 16);
        e.mov_imm(false, T0, target);
        st(T0, o.pc);
        e.movz(false, 0, 0);
        to_epilogue.push_back(e.b());
        sites.push_back(static_cast<u32>(site));
    }
    // Linked entry, right after the prologue (the normal entry jumps over it): the
    // block runs only if its length still fits in JitExit::limit.
    Emitter::Label body_jump = 0, refuse = 0;
    u32 linked_entry = 0, count_word = 0;
    void linked_entry_begin() {
        body_jump = e.b();
        linked_entry = static_cast<u32>(e.here());
        e.ldr(4, T0, kExit, 0);
        count_word = static_cast<u32>(e.here());
        e.add_imm(false, T0, T0, 0);       // + block length (patched at the end)
        e.ldr(4, T1, kExit, 8);
        e.cmp(false, T0, T1);
        refuse = e.b_cond(Cond::HI);
        e.bind_here(body_jump);
    }
    void linked_entry_finish(u32 block_pc, u32 count) {
        Emitter t;
        t.add_imm(false, T0, T0, count);
        e.words()[count_word] = t.words()[0];
        e.bind_here(refuse);
        e.mov_imm(false, T0, block_pc);
        st(T0, o.pc);
        e.movz(false, 0, 0);
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
    bool native_fp = false; // host FPCR is the default (round to nearest, no flush to zero, no default NaN)
    u32 tlb_off = 0, epoch_off = 0; // offsets from x21 (Jit*) of tlb_ and bus_write_epoch_
    // Translation-cache probe for the EA in w1: on a hit x14 = host base of the page
    // (and x15 = its write stamp pointer for stores); otherwise branch to `miss`.
    void tlb_probe(unsigned size, bool write, std::vector<Emitter::Label>& miss);
    bool insn(u32 ins, u32 pc, u32 i, bool& ends);
    bool load_store(u32 ins, u32 pc, u32 i, unsigned size, bool load, bool update, bool sign, bool reverse,
                    unsigned rt, unsigned ra, Reg ea_reg_or_zr, s32 disp, Reg rb_reg);
    bool op31(u32 ins, u32 pc, u32 i, bool& ends);

    // In-place execution by the interpreter's implementation (see helper_exec).
    // `clean`: the register cache is written back and marked clean (otherwise
    // dirty registers are stored but stay dirty, for code that merges with a
    // path that did not store them).
    void interp_call(u32 ins, u32 pc, u32 i, bool generic, bool update, unsigned ureg, bool clean);

    // --- native floating point (see fp_native) -------------------------------
    std::vector<Emitter::Label> to_slow; // branches to the instruction's interpreter call
    void slow_if(Cond c) { to_slow.push_back(e.b_cond(c)); }
    u32 fpr_off(unsigned f, unsigned ps) const { return o.fpr + 16U * f + 8U * ps; }
    void exp_range(Reg bits, unsigned lo, unsigned hi, bool allow_zero);
    void fp_slot(fpu::Op op, bool single, u8 va, u8 vb, u8 vc, u8 out);
    void fp_commit(bool rc);
    void cr1_from_fpscr();
    bool fp_native(u32 ins, u32 pc, u32 i);
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

// Floating-point and paired-single instructions: executed inside the block by the
// interpreter's own implementation (Jit::helper_exec), so FPSCR, NaN and rounding
// behaviour are the reference ones. dcbz_l (opcode 4) is not part of this class.
bool fp_class(u32 ins, bool& update, unsigned& update_reg) {
    const unsigned op = ins >> 26, ra = (ins >> 16) & 31U;
    update = false;
    update_reg = ra;
    if (op >= 48U && op <= 55U) { update = (op & 1U) != 0U; return true; }
    if (op == 56U || op == 57U || op == 60U || op == 61U) { update = op == 57U || op == 61U; return true; }
    if (op == 59U || op == 63U) return true;
    if (op == 4U) {
        const unsigned xo10 = (ins >> 1) & 0x3FFU, xo6 = (ins >> 1) & 0x3FU;
        if (xo10 == 1014U) return false;
        update = xo6 == 38U || xo6 == 39U;
        return true;
    }
    if (op == 31U) {
        const unsigned xo = (ins >> 1) & 0x3FFU;
        switch (xo) {
        case 535: case 599: case 663: case 727: case 983: return true;
        case 567: case 631: case 695: case 759: update = true; return true;
        default: return false;
        }
    }
    return false;
}

// Other instructions the block runs through the interpreter's implementation, in place:
// multiple/string loads and stores, reservations, cache-block operations, barriers,
// traps and mfmsr. None of them reads the time base or changes MSR or the MMU.
// They may write any GPR, so the register cache is dropped after them.
// mfspr of an SPR that holds plain state. Not the time base, DEC or the
// performance counters: the System advances them only between run() calls, so
// read inside run() they would lag behind the instructions run so far.
bool plain_spr_read(u32 ins) {
    const unsigned n = ((ins >> 16) & 0x1FU) | (((ins >> 11) & 0x1FU) << 5);
    switch (n) {
    case 18: case 19: case 25: case 26: case 27:            // DSISR DAR SDR1 SRR0 SRR1
    case 272: case 273: case 274: case 275:                 // SPRG0-3
    case 282: case 287:                                     // EAR PVR
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: // GQR0-7
    case 920: case 1008: case 1009: case 1011: case 1017:   // HID2 HID0 HID1 HID4 L2CR
        return true;
    default:
        return n >= 528U && n <= 543U;                      // IBAT/DBAT 0-3
    }
}

bool interp_class(u32 ins) {
    const unsigned op = ins >> 26;
    if (op == 46U || op == 47U || op == 3U) return true; // lmw, stmw, twi
    if (op == 19U && ((ins >> 1) & 0x3FFU) == 150U) return true; // isync
    if (op != 31U) return false;
    switch ((ins >> 1) & 0x3FFU) {
    case 20: case 150:                       // lwarx, stwcx.
    case 1014: case 86: case 54: case 470: case 982: case 278: case 246: // dcbz dcbf dcbst dcbi icbi dcbt dcbtst
    case 598: case 854:                      // sync, eieio
    case 4:                                  // tw
    case 83:                                 // mfmsr
    case 146:                                // mtmsr (helper_exec decides whether the block may go on)
    case 533: case 597: case 661: case 725:  // lswx lswi stswx stswi
        return true;
    case 339: return plain_spr_read(ins); // mfspr of an SPR that holds plain state
    default:
        return false;
    }
}

// Data bytes an interp_class instruction may touch (as the interpreter computes them).
bool interp_memory_operand(u32 ins, const CpuState& s, u32& ea, u32& size, bool& store) {
    const unsigned op = ins >> 26, rt = (ins >> 21) & 31U, ra = (ins >> 16) & 31U, rb = (ins >> 11) & 31U;
    const u32 base = ra == 0U ? 0U : s.gpr[ra];
    if (op == 46U || op == 47U) {
        ea = base + static_cast<u32>(static_cast<s32>(static_cast<s16>(ins & 0xFFFFU)));
        size = (32U - rt) * 4U;
        store = op == 47U;
        return true;
    }
    if (op != 31U) return false;
    const unsigned xo = (ins >> 1) & 0x3FFU;
    switch (xo) {
    case 20: case 150: ea = base + s.gpr[rb]; size = 4; store = xo == 150U; return true;
    case 1014: ea = (base + s.gpr[rb]) & ~31U; size = 32; store = true; return true;
    case 597: case 725: ea = base; size = rb == 0U ? 32U : rb; store = xo == 725U; return true;
    case 533: case 661:
        ea = base + s.gpr[rb]; size = s.xer & XER_BYTE_COUNT; store = xo == 661U;
        return size != 0U;
    default: return false;
    }
}

// For FP/paired-single memory instructions: effective address, a size covering every
// byte the access can touch, and the direction. False for non-memory instructions.
bool fp_memory_operand(u32 ins, const CpuState& s, u32& ea, u32& size, bool& store) {
    const unsigned op = ins >> 26, ra = (ins >> 16) & 31U, rb = (ins >> 11) & 31U;
    const u32 base = ra == 0U ? 0U : s.gpr[ra];
    if (op >= 48U && op <= 55U) {
        ea = base + static_cast<u32>(static_cast<s32>(static_cast<s16>(ins & 0xFFFFU)));
        size = (op == 50U || op == 51U || op == 54U || op == 55U) ? 8U : 4U;
        store = op >= 52U;
        return true;
    }
    auto psq_size = [&](unsigned i, bool st, unsigned w) {
        const u32 gqr = s.gqr[i];
        const unsigned type = st ? (gqr & 7U) : ((gqr >> 16) & 7U);
        const unsigned el = type == 0U ? 4U : (type == 4U || type == 6U) ? 1U : (type == 5U || type == 7U) ? 2U : 4U;
        return el * (w != 0U ? 1U : 2U);
    };
    if (op == 56U || op == 57U || op == 60U || op == 61U) {
        const u32 d = ins & 0xFFFU;
        ea = base + ((d & 0x800U) != 0U ? (d | 0xFFFFF000U) : d);
        store = op >= 60U;
        size = psq_size((ins >> 12) & 7U, store, (ins >> 15) & 1U);
        return true;
    }
    if (op == 4U) {
        const unsigned xo6 = (ins >> 1) & 0x3FU;
        if (xo6 != 6U && xo6 != 7U && xo6 != 38U && xo6 != 39U) return false;
        ea = base + s.gpr[rb];
        store = xo6 == 7U || xo6 == 39U;
        size = psq_size((ins >> 7) & 7U, store, (ins >> 10) & 1U);
        return true;
    }
    if (op == 31U) {
        const unsigned xo = (ins >> 1) & 0x3FFU;
        if (xo != 535U && xo != 567U && xo != 599U && xo != 631U && xo != 663U && xo != 695U && xo != 727U && xo != 759U && xo != 983U)
            return false;
        ea = base + s.gpr[rb];
        size = (xo == 599U || xo == 631U || xo == 727U || xo == 759U) ? 8U : 4U;
        store = xo >= 663U;
        return true;
    }
    return false;
}

// Kind codes returned in w0: 0 = continue at state.pc, 1 = interpret the instruction at state.pc,
// 2 = continue at state.pc after re-checking the run() entry conditions (MSR changed).
static constexpr u32 kExitNormal = 0, kExitFallback = 1, kExitRecheck = 2;

void Jit::Compiler::tlb_probe(unsigned size, bool write, std::vector<Emitter::Label>& miss) {
    e.ubfx(false, 13, 1, 12, 8);                    // entry index (kTlbEntries = 256)
    e.add(true, 14, kSelf, 13, Shift::LSL, 5);      // 32-byte entries
    e.ldr(4, 15, 14, tlb_off + (write ? 4U : 0U));  // tag
    e.and_imm(false, 13, 1, 0xFFFFF000ULL);
    e.orr_imm(false, 13, 13, 1);
    e.cmp(false, 15, 13);
    miss.push_back(e.b_cond(Cond::NE));
    if (size > 1U) {                                // the access stays inside the page
        e.and_imm(false, 13, 1, 0xFFFULL);
        e.cmp_imm(false, 13, 0x1000U - size);
        miss.push_back(e.b_cond(Cond::HI));
    }
    if (write) e.ldr(8, 15, 14, tlb_off + 16U);     // write stamp pointer
    e.ldr(8, 14, 14, tlb_off + 8U);                 // host base
}

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
    std::vector<Emitter::Label> miss;
    if (load) {
        // fast path: RAM through the translation cache (guest memory is big-endian)
        tlb_probe(size, false, miss);
        e.ldr_reg(size, T0, 14, 1);
        if (!reverse) {
            if (size == 4U) e.rev(false, T0, T0);
            else if (size == 2U) e.rev16(false, T0, T0);
            if (sign) e.sxth(false, T0, T0);
        }
        const auto join = e.b();
        for (auto l : miss) e.bind_here(l);
        e.movz(false, 2, static_cast<u16>(size));
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
        e.bind_here(join);
        if (update) { ea_into(T1); e.mov(false, def(ra, rt), T1); }
        e.mov(false, def(rt, ra), T0);
    } else {
        const Reg v = value_reg_;
        tlb_probe(size, true, miss);
        if (!reverse && size == 4U) e.rev(false, 3, v);
        else if (!reverse && size == 2U) e.rev16(false, 3, v);
        else e.mov(false, 3, v);
        e.str_reg(size, 3, 14, 1);
        // the bus's write stamps (texture cache): ++epoch; page stamp = epoch
        e.ldr(8, 13, kSelf, epoch_off);
        e.ldr(8, 14, 13, 0);
        e.add_imm(true, 14, 14, 1);
        e.str(8, 14, 13, 0);
        const auto untracked = e.cbz(true, 15);
        e.str(8, 14, 15, 0);
        e.bind_here(untracked);
        e.movz(false, 0, 1); // "stored, code untouched" (a write entry never maps a code page)
        const auto join = e.b();
        for (auto l : miss) e.bind_here(l);
        e.movz(false, 2, static_cast<u16>(size));
        if (reverse) {
            if (size == 4U) e.rev(false, 3, v);
            else { e.rev16(false, 3, v); e.uxth(3, 3); }
        } else {
            e.mov(false, 3, v);
        }
        e.mov_imm(false, 4, pc);
        call(reinterpret_cast<const void*>(&Jit::helper_store));
        const auto ok = e.cbnz(false, 0);
        exit_const(pc, i, kExitFallback);
        e.bind_here(ok);
        e.bind_here(join);
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
        exit_link(target, i + 1U);
        return true;
    }
    const unsigned bo = F_RT(ins), bi = F_RA(ins);
    if (kind == 2U && (bo & 4U) == 0U) return false; // invalid form: interpreter
    // target into T1 (captured before LR/CTR are written)
    if (kind != 0U) {
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
    if (kind == 0U) { // constant target: two linkable exits
        const s32 disp = F_SIMM(ins & 0xFFFCU);
        const auto not_taken = e.cbz(false, T2);
        exit_link((ins & 2U) != 0U ? static_cast<u32>(disp) : pc + static_cast<u32>(disp), i + 1U);
        e.bind_here(not_taken);
        exit_link(pc + 4U, i + 1U);
        return true;
    }
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

void Jit::Compiler::interp_call(u32 ins, u32 pc, u32 i, bool generic, bool update, unsigned ureg, bool clean) {
    flush_dirty();
    if (clean) after_flush_clean();
    e.mov_imm(false, 1, ins);
    e.mov_imm(false, 2, pc);
    call(reinterpret_cast<const void*>(&Jit::helper_exec));
    const auto ok = e.cbnz(false, 0);
    exit_const(pc, i, kExitFallback);
    e.bind_here(ok);
    if (generic) slot.fill(-1); // any GPR may have changed in memory
    else if (update) { const int k = find(ureg); if (k >= 0) slot[static_cast<unsigned>(k)] = -1; } // rA changed in memory
    // 1: go on; 2: code was invalidated, leave; 3: MSR changed (mtmsr), leave
    // so that run() re-checks its entry conditions
    e.cmp_imm(false, 0, 1);
    const auto same = e.b_cond(Cond::EQ);
    e.cmp_imm(false, 0, 3);
    const auto invalidated = e.b_cond(Cond::NE);
    exit_const(pc + 4U, i + 1U, kExitRecheck);
    e.bind_here(invalidated);
    exit_const(pc + 4U, i + 1U, kExitNormal);
    e.bind_here(same);
}

// ---------------------------------------------------------------------------
// Native floating point
//
// The common case runs on the host FPU and produces exactly what the
// interpreter (fpu.cpp) produces, FPSCR included; everything else goes to the
// interpreter's implementation of the same instruction (the slow path), before
// anything was written. The native path requires: MSR[FP] (and HID2[PSE] for
// paired singles), FPSCR[VE,OE,UE,ZE,XE] = 0 (no enabled exception can arise)
// and RN = 0 (round to nearest), and a result that is a normal number well
// inside the format (or an exact zero where it can only be exact). Then the only
// possible exception is "inexact" and the result is the host's correctly
// rounded one. The exact error of the rounding, needed for FPSCR[FI] and [FR],
// is computed with error-free transformations, exact in that range:
//   add/sub: TwoSum (Knuth); mul: fma(a, c, -s); div: a - q*b with fma
//   (sign of the error = sign(r) xor sign(b)); fmadd family: ErrFma
//   (Boldo & Muller, "Exact and approximated error of the FMA", IEEE TC 2011).
// Single-precision results: the double result s is rounded once more to single;
// that equals rounding the exact value directly unless s lies exactly halfway
// between two singles while the first rounding was inexact (slow path then).
// tests/test_jit.cpp checks the same algorithm against fpu::arith on any host,
// and the generated code against the interpreter on AArch64.
// ---------------------------------------------------------------------------
namespace {
constexpr u64 kSign = 0x8000000000000000ULL, kMag = ~kSign;
constexpr u32 kFpEnables = fpu::VE | fpu::OE | fpu::UE | fpu::ZE | fpu::XE;
} // namespace

// Slow path unless the biased exponent of `bits` is in [lo, hi] (a zero passes when allowed).
void Jit::Compiler::exp_range(Reg bits, unsigned lo, unsigned hi, bool allow_zero) {
    Emitter::Label skip = 0;
    if (allow_zero) { e.ands_imm(true, ZR, bits, kMag); skip = e.b_cond(Cond::EQ); }
    e.ubfx(true, T0, bits, 52, 11);
    e.cmp_imm(false, T0, lo);
    slow_if(Cond::LO);
    e.cmp_imm(false, T0, hi);
    slow_if(Cond::HI);
    if (allow_zero) e.bind_here(skip);
}

// One arithmetic result. Inputs in d<va>, d<vb>, d<vc> (frA, frB, frC); result
// (double format, single-rounded when `single`) in d<out>; FI in w9, FR in w10.
// Clobbers d3-d7, d16-d21, x8, x11-x15.
void Jit::Compiler::fp_slot(fpu::Op op, bool single, u8 va, u8 vb, u8 vc, u8 out) {
    using F = fpu::Op;
    const bool negb = op == F::Sub || op == F::MSub || op == F::NMSub;
    u8 b = vb;
    if (negb) { e.fneg(4, vb); b = 4; }
    bool allow_zero = true;
    constexpr Reg S = 13, E = 14, X15 = 15;
    auto two_sum = [&](u8 s, u8 err, u8 x, u8 y, u8 t1, u8 t2) { // s = x + y already computed
        e.fsub(t1, s, x);     // bb
        e.fsub(t2, s, t1);
        e.fsub(t2, x, t2);    // x - (s - bb)
        e.fsub(t1, y, t1);    // y - bb
        e.fadd(err, t2, t1);
    };
    switch (op) {
    case F::Add: case F::Sub:
        e.fadd(3, va, b);
        two_sum(3, 5, va, b, 6, 7);
        e.fmov_to_x(E, 5);
        break;
    case F::Mul:
        e.fmul(3, va, vc);
        e.fnmsub(5, va, vc, 3); // a*c - s
        e.fmov_to_x(E, 5);
        allow_zero = false;
        break;
    case F::Div:
        e.fmov_to_x(S, va);
        exp_range(S, 256, 2046, false); // |a| >= 2^-767: the remainder is exact
        e.fdiv(3, va, vb);
        e.fmsub(5, 3, vb, va);  // a - q*b (exact)
        e.fmov_to_x(E, 5);
        e.fmov_to_x(X15, vb);
        e.and_imm(true, X15, X15, kSign);
        e.eor(true, E, E, X15);  // error sign = sign(r) xor sign(b)
        allow_zero = false;
        break;
    case F::MAdd: case F::MSub: case F::NMAdd: case F::NMSub: {
        e.fmul(6, va, vc);       // u1
        e.fmov_to_x(S, 6);
        // u1 = 0 only for an exact zero product; otherwise u1 inside [2^-767, 2^1022]
        e.ands_imm(true, ZR, S, kMag);
        const auto nz = e.b_cond(Cond::NE);
        e.fmov_to_x(E, va);
        e.ands_imm(true, ZR, E, kMag);
        const auto zero_a = e.b_cond(Cond::EQ);
        e.fmov_to_x(E, vc);
        e.ands_imm(true, ZR, E, kMag);
        slow_if(Cond::NE);
        e.bind_here(zero_a);
        const auto checked = e.b();
        e.bind_here(nz);
        exp_range(S, 256, 2045, false);
        e.bind_here(checked);
        e.fmov_to_x(S, b);
        exp_range(S, 256, 2047, true); // addend zero or >= 2^-767 (Inf/NaN make s non-finite)
        e.fmadd(3, va, vc, b);   // s = RN(a*c + b)
        e.fnmsub(7, va, vc, 6);  // u2 = a*c - u1 (exact)
        e.fadd(16, b, 7);        // (alpha1, z) = TwoSum(b, u2)
        two_sum(16, 17, b, 7, 18, 19);
        e.fadd(18, 6, 16);       // (beta1, beta2) = TwoSum(u1, alpha1)
        two_sum(18, 19, 6, 16, 20, 21);
        e.fsub(20, 18, 3);       // gamma = (beta1 - s) + beta2
        e.fadd(20, 20, 19);
        e.fadd(5, 20, 17);       // (r2, r3) = Fast2Sum(gamma, z)
        e.fsub(21, 5, 20);
        e.fsub(21, 17, 21);
        e.fmov_to_x(E, 5);
        e.fmov_to_x(X15, 21);
        e.ands_imm(true, ZR, E, kMag);
        e.csel(true, E, E, X15, Cond::NE); // the error has the sign of r2, or of r3 when r2 = 0
        break;
    }
    case F::Round:
        e.fmov_d(3, vb);
        e.movz(true, E, 0);
        break;
    default:
        break;
    }
    e.fmov_to_x(S, 3);
    // FI and FR of the first rounding: error != 0, and the error's sign differs from the result's
    e.ands_imm(true, ZR, E, kMag);
    e.cset(false, T1, Cond::NE);
    e.eor(true, T2, E, S);
    e.lsr_imm(true, T2, T2, 63);
    e.and_(false, T2, T2, T1);
    if (!single) {
        exp_range(S, 256, 2045, allow_zero);
        e.fmov_d(out, 3);
    } else {
        if (!allow_zero) { e.ands_imm(true, ZR, S, kMag); slow_if(Cond::EQ); }
        // s exactly halfway between two singles after an inexact first rounding
        e.and_imm(true, T0, S, 0x1FFFFFFFULL);
        e.movz(false, T3, 0x1000, 16);
        e.cmp(true, T0, T3);
        const auto not_mid = e.b_cond(Cond::NE);
        to_slow.push_back(e.cbnz(false, T1));
        e.bind_here(not_mid);
        e.fcvt_s_from_d(out, 3);
        e.fcvt_d_from_s(out, out);
        e.fmov_to_x(X15, out);
        e.ands_imm(true, ZR, S, kMag);
        const auto s_zero = e.b_cond(Cond::EQ);
        exp_range(X15, 898, 1150, false); // 2^-125 <= |result| < 2^128: normal single, no underflow/overflow
        e.bind_here(s_zero);
        e.cmp(true, X15, S);
        e.cset(false, T3, Cond::NE);        // second rounding changed the value
        e.orr(false, T1, T1, T3);
        e.and_imm(true, T0, X15, kMag);
        e.and_imm(true, T4, S, kMag);
        e.cmp(true, T0, T4);
        e.cset(false, T4, Cond::HI);        // ... away from zero
        e.cmp_imm(false, T3, 0);
        e.csel(false, T2, T4, T2, Cond::NE);
    }
    if (op == F::NMAdd || op == F::NMSub) e.fneg(out, out);
}

// FPSCR after an arithmetic instruction: XX (and FX when XX was clear) from w11,
// FI from w9, FR from w10, FPRF from the result bits in x15; VX recomputed and FEX
// cleared (no exception is enabled on the native path). Rc: CR1.
void Jit::Compiler::fp_commit(bool rc) {
    ld(T0, o.fpscr);
    e.ubfx(false, T4, T0, 25, 1);
    e.bic(false, T4, T3, T4);
    e.orr(false, T0, T0, T4, Shift::LSL, 31);
    e.orr(false, T0, T0, T3, Shift::LSL, 25);
    e.and_imm(false, T0, T0, ~u64{fpu::FR | fpu::FI | fpu::FPRF_MASK} & 0xFFFFFFFFULL);
    e.orr(false, T0, T0, T2, Shift::LSL, 18);
    e.orr(false, T0, T0, T1, Shift::LSL, 17);
    // FPRF: normal 0x04 / 0x08, zero 0x02 / 0x12
    e.lsr_imm(true, 13, 15, 63);
    e.movz(false, T4, 4);
    e.lslv(false, T4, T4, 13);
    e.movz(false, 14, 2);
    e.orr(false, 14, 14, 13, Shift::LSL, 4);
    e.ands_imm(true, ZR, 15, kMag);
    e.csel(false, T4, T4, 14, Cond::NE);
    e.orr(false, T0, T0, T4, Shift::LSL, 12);
    e.and_imm(false, T0, T0, ~u64{fpu::VX | fpu::FEX} & 0xFFFFFFFFULL);
    e.mov_imm(false, T4, fpu::VX_ALL);
    e.tst(false, T0, T4);
    e.cset(false, T4, Cond::NE);
    e.orr(false, T0, T0, T4, Shift::LSL, 29);
    st(T0, o.fpscr);
    if (rc) { e.lsr_imm(false, 13, T0, 28); set_cr_field_reg(1, 13); }
}

void Jit::Compiler::cr1_from_fpscr() {
    ld(13, o.fpscr);
    e.lsr_imm(false, 13, 13, 28);
    set_cr_field_reg(1, 13);
}

bool Jit::Compiler::fp_native(u32 ins, u32 pc, u32 i) {
    using F = fpu::Op;
    const unsigned op = ins >> 26, fd = F_RT(ins), fa = F_RA(ins), fb = F_RB(ins), fc = (ins >> 6) & 31U;
    const unsigned xo5 = (ins >> 1) & 31U, xo10 = (ins >> 1) & 0x3FFU;
    const bool rc = F_RC(ins);
    enum class K { None, Arith, Pair, Sum, Scalar, Move, Sel, Cmp, Merge };
    K k = K::None;
    F aop = F::Add;
    bool single = false;
    auto arith5 = [&](unsigned x, F& r) {
        switch (x) {
        case 18: r = F::Div; return true;
        case 20: r = F::Sub; return true;
        case 21: r = F::Add; return true;
        case 25: r = F::Mul; return true;
        case 28: r = F::MSub; return true;
        case 29: r = F::MAdd; return true;
        case 30: r = F::NMSub; return true;
        case 31: r = F::NMAdd; return true;
        default: return false;
        }
    };
    if (op == 63U) {
        if (arith5(xo5, aop)) k = K::Arith;
        else if (xo5 == 23U) k = K::Sel;
        else if (xo10 == 12U) { k = K::Arith; aop = F::Round; single = true; }
        else if (xo10 == 0U || xo10 == 32U) k = K::Cmp;
        else if (xo10 == 40U || xo10 == 72U || xo10 == 136U || xo10 == 264U) k = K::Move;
    } else if (op == 59U) {
        if (arith5(xo5, aop)) { k = K::Arith; single = true; }
    } else if (op == 4U) {
        if (arith5(xo5, aop)) { k = K::Pair; single = true; }
        else if (xo5 == 10U || xo5 == 11U) { k = K::Sum; aop = F::Add; single = true; }
        else if (xo5 >= 12U && xo5 <= 15U) { k = K::Scalar; aop = xo5 <= 13U ? F::Mul : F::MAdd; single = true; }
        else if (xo5 == 23U) k = K::Sel;
        else if (xo10 == 0U || xo10 == 32U || xo10 == 64U || xo10 == 96U) k = K::Cmp;
        else if (xo10 == 40U || xo10 == 72U || xo10 == 136U || xo10 == 264U) k = K::Move;
        else if (xo10 == 528U || xo10 == 560U || xo10 == 592U || xo10 == 624U) k = K::Merge;
    }
    if (k == K::None) return false;
    const bool ps = op == 4U;
    to_slow.clear();

    // availability and FPSCR preconditions (otherwise: the interpreter's implementation)
    ld(T0, o.msr);
    to_slow.push_back(e.tbz(T0, 13)); // MSR[FP]
    if (ps) { ld(T0, o.hid2); to_slow.push_back(e.tbz(T0, 29)); } // HID2[PSE]
    if (k == K::Arith || k == K::Pair || k == K::Sum || k == K::Scalar || k == K::Cmp) {
        ld(T0, o.fpscr);
        e.mov_imm(false, T1, k == K::Cmp ? kFpEnables : (kFpEnables | fpu::RN_MASK));
        e.tst(false, T0, T1);
        slow_if(Cond::NE);
    }

    auto load3 = [&](unsigned slot_a, unsigned slot_b, unsigned slot_c) {
        e.ldr_d(0, kState, fpr_off(fa, slot_a));
        e.ldr_d(1, kState, fpr_off(fb, slot_b));
        e.ldr_d(2, kState, fpr_off(fc, slot_c));
    };
    switch (k) {
    case K::Arith:
        load3(0, 0, 0);
        fp_slot(aop, single, 0, 1, 2, 24);
        e.str_d(24, kState, fpr_off(fd, 0));
        if (single) e.str_d(24, kState, fpr_off(fd, 1));
        e.fmov_to_x(15, 24);
        e.mov(false, T3, T1);
        fp_commit(rc);
        break;
    case K::Pair: case K::Scalar: {
        // slot 0, then slot 1; FPSCR: exceptions of both, FI/FR/FPRF of slot 0
        const unsigned cslot0 = k == K::Scalar ? (xo5 & 1U) : 0U, cslot1 = k == K::Scalar ? (xo5 & 1U) : 1U;
        load3(0, 0, cslot0);
        fp_slot(aop, true, 0, 1, 2, 24);
        e.mov(false, 0, T1);
        e.mov(false, 1, T2);
        load3(1, 1, cslot1);
        fp_slot(aop, true, 0, 1, 2, 25);
        e.orr(false, T3, 0, T1);
        e.mov(false, T1, 0);
        e.mov(false, T2, 1);
        e.str_d(24, kState, fpr_off(fd, 0));
        e.str_d(25, kState, fpr_off(fd, 1));
        e.fmov_to_x(15, 24);
        fp_commit(rc);
        break;
    }
    case K::Sum: {
        // ps_sum0: ps0 = A0 + B1, ps1 = C1; ps_sum1: ps0 = C0, ps1 = A0 + B1
        e.ldr_d(0, kState, fpr_off(fa, 0));
        e.ldr_d(1, kState, fpr_off(fb, 1));
        e.ldr_d(25, kState, fpr_off(fc, xo5 == 10U ? 1U : 0U));
        fp_slot(F::Add, true, 0, 1, 2, 24);
        e.str_d(24, kState, fpr_off(fd, xo5 == 10U ? 0U : 1U));
        e.str_d(25, kState, fpr_off(fd, xo5 == 10U ? 1U : 0U));
        e.fmov_to_x(15, 24);
        e.mov(false, T3, T1);
        fp_commit(rc);
        break;
    }
    case K::Move: {
        const unsigned slots = ps ? 2U : 1U;
        for (unsigned s = 0; s < slots; ++s) {
            e.ldr(8, 13, kState, fpr_off(fb, s));
            if (xo10 == 40U) e.eor_imm(true, 13, 13, kSign);
            else if (xo10 == 136U) e.orr_imm(true, 13, 13, kSign);
            else if (xo10 == 264U) e.and_imm(true, 13, 13, kMag);
            e.str(8, 13, kState, fpr_off(fd, s));
        }
        if (rc) cr1_from_fpscr();
        break;
    }
    case K::Merge: {
        e.ldr(8, 13, kState, fpr_off(fa, xo10 == 592U || xo10 == 624U ? 1U : 0U));
        e.ldr(8, 14, kState, fpr_off(fb, xo10 == 560U || xo10 == 624U ? 1U : 0U));
        e.str(8, 13, kState, fpr_off(fd, 0));
        e.str(8, 14, kState, fpr_off(fd, 1));
        if (rc) cr1_from_fpscr();
        break;
    }
    case K::Sel: {
        // D = (A is not a NaN and A >= 0) ? C : B ("GE" is false for unordered)
        const unsigned slots = ps ? 2U : 1U;
        for (unsigned s = 0; s < slots; ++s) {
            e.ldr_d(0, kState, fpr_off(fa, s));
            e.ldr_d(1, kState, fpr_off(fb, s));
            e.ldr_d(2, kState, fpr_off(fc, s));
            e.fcmp_zero(0);
            e.fcsel(s == 0U ? 24 : 25, 2, 1, Cond::GE);
        }
        for (unsigned s = 0; s < slots; ++s) e.str_d(s == 0U ? 24 : 25, kState, fpr_off(fd, s));
        if (rc) cr1_from_fpscr();
        break;
    }
    case K::Cmp: {
        // ordered operands only (NaNs set VXSNAN/VXVC: slow path); Rc is reserved
        const unsigned which = ps && xo10 >= 64U ? 1U : 0U, crf = (ins >> 23) & 7U;
        e.ldr_d(0, kState, fpr_off(fa, which));
        e.ldr_d(1, kState, fpr_off(fb, which));
        e.fcmp(0, 1);
        slow_if(Cond::VS);
        e.movz(false, T3, 2);
        e.movz(false, T4, 8);
        e.csel(false, T3, T4, T3, Cond::MI);
        e.movz(false, T4, 4);
        e.csel(false, T3, T4, T3, Cond::GT);
        ld(T0, o.fpscr);
        e.bfi(false, T0, T3, 12, 4);
        e.and_imm(false, T0, T0, ~u64{fpu::VX | fpu::FEX} & 0xFFFFFFFFULL);
        e.mov_imm(false, T4, fpu::VX_ALL);
        e.tst(false, T0, T4);
        e.cset(false, T4, Cond::NE);
        e.orr(false, T0, T0, T4, Shift::LSL, 29);
        st(T0, o.fpscr);
        set_cr_field_reg(crf, T3);
        break;
    }
    case K::None:
        break;
    }

    const auto done = e.b();
    for (auto l : to_slow) e.bind_here(l);
    to_slow.clear();
    interp_call(ins, pc, i, false, false, 0, false);
    e.bind_here(done);
    return true;
}

bool Jit::Compiler::insn(u32 ins, u32 pc, u32 i, bool& ends) {
    ends = false;
    {
        bool update = false;
        unsigned ureg = 0;
        const bool generic = !fp_class(ins, update, ureg) && interp_class(ins);
        if (!generic && native_fp && fp_native(ins, pc, i)) return true;
        if (generic || fp_class(ins, update, ureg)) {
            interp_call(ins, pc, i, generic, update, ureg, true);
            return true;
        }
    }
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
    bus_write_epoch_ = bus_.write_epoch_pointer();
    if (jit::host_can_execute()) code_ = std::make_unique<jit::CodeBuffer>();
    bus_.set_code_invalidation_handler([this](u32 address, u32 size) { invalidate(address, size); });
}

Jit::~Jit() { bus_.set_code_invalidation_handler(nullptr); }

bool Jit::can_execute() const noexcept { return code_ != nullptr && code_->ok(); }

void Jit::clear() {
    tlb_flush();
    sites_.clear();
    pending_site_ = nullptr;
    for (auto& [page, list] : page_blocks_) bus_.unmark_code_page(page << 12);
    no_block_.clear();
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
            if (b == running_ || (current_pc_valid_ && current_pc_ - b->ea < b->count * 4U)) running_invalidated_ = true;
            unlink_incoming(b);
            auto bi = blocks_.find(b->ea);
            if (bi != blocks_.end() && bi->second.get() == b) {
                retired_blocks_.push_back(std::move(bi->second));
                blocks_.erase(bi);
            }
        }
        std::erase_if(it->second, [](Block* b) { return !b->valid; });
        std::erase_if(no_block_, [p](const auto& kv) { return (kv.second >> 12) == p; });
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
    self->tlb_fill(ea, false);
    return 1;
}

u32 Jit::helper_exec(Jit* self, u32 ins, u32 pc) {
    Cpu& c = self->cpu_;
    const CpuState& s = c.state_;
    bool update = false;
    unsigned ureg = 0;
    const bool fp = fp_class(ins, update, ureg);
    // FP unavailable or enabled FP exceptions: the interpreter handles it outside the block
    if (fp && ((s.msr & MSR_FP) == 0U || (s.msr & (MSR_FE0 | MSR_FE1)) != 0U)) return 0;
    u32 ea = 0, size = 0;
    bool store = false;
    if (fp ? fp_memory_operand(ins, s, ea, size, store) : interp_memory_operand(ins, s, ea, size, store)) {
        u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
        if (!c.translate_data(ea, size, store, lo, hi, split, wimg).ok() || split != size) return 0;
        if (!c.locked_cache_.empty())
            for (u32 line = lo & ~31U; line <= ((lo + size - 1U) & ~31U); line += 32U)
                if (c.locked_cache_.count(line) != 0U) return 0;
        if (self->bus_.ram_pointer(lo, size) == nullptr) return 0;
    }
    self->running_invalidated_ = false;
    self->current_pc_ = pc;
    self->current_pc_valid_ = true;
    const u32 msr_before = s.msr;
    const Cpu::Outcome o = c.execute_one_for_jit(ins, pc);
    self->current_pc_valid_ = false;
    if (!o.ok() || c.fp_program_pending_) return 0; // precise: nothing was changed
    ++self->stats_.helper_steps;
    if (self->running_invalidated_) return 2U;
    // mtmsr: the block goes on only when nothing but EE changed and no interrupt
    // became deliverable (the SDK's OSDisableInterrupts / OSRestoreInterrupts);
    // anything else leaves for run(), which re-checks its entry conditions
    if (s.msr != msr_before) {
        if (((s.msr ^ msr_before) & ~MSR_EE) != 0U) return 3U;
        if ((s.msr & MSR_EE) != 0U && (c.external_line_ || s.dec_pending)) return 3U;
    }
    return 1U;
}

u32 Jit::helper_store(Jit* self, u32 ea, u32 size, u32 value, u32 pc) {
    Cpu& c = self->cpu_;
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    if (!c.translate_data(ea, size, true, lo, hi, split, wimg).ok() || split != size) return 0;
    if (!c.locked_cache_.empty() && c.locked_cache_.count(lo & ~31U) != 0U) return 0;
    if (self->bus_.ram_pointer(lo, size) == nullptr) return 0;
    self->running_invalidated_ = false;
    self->current_pc_ = pc;
    self->current_pc_valid_ = true;
    const mem::BusStatus s = self->bus_.write(lo, size, value, mem::AccessSource::Cpu);
    self->current_pc_valid_ = false;
    if (s != mem::BusStatus::Ok) return 0; // cannot happen for RAM; keep the interpreter's handling
    if (self->running_invalidated_) return 2U;
    self->tlb_fill(ea, true);
    return 1U;
}

void Jit::tlb_fill(u32 ea, bool write) {
    const CpuState& s = cpu_.state_;
    const u32 page = ea & ~0xFFFU;
    Translation t;
    if (!translate_without_page_table(s, page, write ? AccessType::Write : AccessType::Read, cpu_.config_.extended_bats, t) ||
        t.status != TranslateStatus::Ok)
        return;
    const u32 pp = t.physical & ~0xFFFU;
    u8* host = bus_.ram_pointer(pp, 0x1000U);
    if (host == nullptr) return;
    for (const auto& line : cpu_.locked_cache_)
        if ((line.first & ~0xFFFU) == pp) return;
    if (write && (bus_.is_code_page(pp) || bus_.observes_cpu_writes() || bus_.is_watched_page(pp))) return;
    TlbEntry& en = tlb_[(ea >> 12) & (kTlbEntries - 1U)];
    const u32 tag = page | 1U;
    const u64 base = static_cast<u64>(reinterpret_cast<std::uintptr_t>(host)) - page;
    if (en.base != base || en.phys_page != pp) { en.read_tag = 0; en.write_tag = 0; } // another page: drop both kinds
    en.base = base;
    en.phys_page = pp;
    en.epoch = bus_.page_epoch_pointer(pp);
    (write ? en.write_tag : en.read_tag) = tag;
    ++stats_.tlb_fills;
}

void Jit::tlb_drop_writes_to(u32 phys_page) noexcept {
    for (auto& en : tlb_)
        if (en.write_tag != 0U && en.phys_page == phys_page) en.write_tag = 0;
}

void Jit::tlb_check_context() {
    const CpuState& s = cpu_.state_;
    TlbContext now;
    now.msr = s.msr & (MSR_DR | MSR_PR);
    now.hid4 = cpu_.config_.extended_bats ? (s.hid4 & HID4_SBE) : 0U;
    now.dbat = s.dbat;
    now.locked = cpu_.locked_cache_generation_;
    now.cpu_observers = bus_.observes_cpu_writes();
    if (!(now == tlb_context_)) { tlb_flush(); tlb_context_ = now; }
}

namespace {
// The native FP code assumes the host's default floating-point control: round to
// nearest, no flush-to-zero, no default-NaN mode, no trapping. Jit::run checks it.
bool host_fp_control_is_default() noexcept {
#if defined(__aarch64__)
    u64 fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr & 0x03C09F00ULL) == 0U; // DN, FZ, RMode, trap enables
#else
    return true;
#endif
}
} // namespace

bool Jit::compile(u32 pc, u32 pa, std::vector<u32>& words, u32& count, std::string& error, u32* linked_offset,
                  std::vector<u32>* sites) {
    Compiler c;
    c.link_ctx_off = static_cast<u32>(reinterpret_cast<const char*>(&link_context_) - reinterpret_cast<const char*>(this));
    c.native_fp = native_fp_;
    c.tlb_off = static_cast<u32>(reinterpret_cast<const char*>(tlb_.data()) - reinterpret_cast<const char*>(this));
    c.epoch_off = static_cast<u32>(reinterpret_cast<const char*>(&bus_write_epoch_) - reinterpret_cast<const char*>(this));
    // both must be reachable with scaled LDR offsets from x21 (they are the first members)
    if (c.tlb_off + 24U >= 16384U || c.epoch_off >= 32768U || (c.tlb_off & 7U) != 0U || (c.epoch_off & 7U) != 0U ||
        c.link_ctx_off >= 16384U || (c.link_ctx_off & 3U) != 0U) {
        error = "Jit layout: translation cache out of reach";
        return false;
    }
    c.prologue();
    c.linked_entry_begin();
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
        const auto saved_sites = c.sites.size();
        bool ends = false;
        if (!c.insn(ins, cur, count, ends)) {
            // not translated: undo anything emitted and stop before it
            c.e.words().resize(mark);
            c.slot = saved_slot; c.dirty = saved_dirty; c.last_use = saved_use;
            c.to_epilogue.resize(saved_exits);
            c.sites.resize(saved_sites);
            break;
        }
        ++count;
        cur += 4U;
        if (ends) { ended = true; break; }
    }
    if (count == 0U) { error = "first instruction not translated"; return false; }
    if (!ended) {
        if (count < kMaxBlock && (cur & 0xFFFU) != 0U) c.exit_const(cur, count, kExitFallback);
        else c.exit_link(cur, count);
    }
    c.linked_entry_finish(pc, count);
    c.epilogue();
    words = std::move(c.e.words());
    if (linked_offset != nullptr) *linked_offset = c.linked_entry;
    if (sites != nullptr) *sites = std::move(c.sites);
    return true;
}

bool Jit::translate_for_test(u32 pc, std::vector<u32>& words, u32& guest_instructions, std::string& error) {
    const Translation t = translate(cpu_.state_, bus_, pc, AccessType::Fetch, cpu_.config_.extended_bats);
    if (t.status != TranslateStatus::Ok) { error = "fetch translation failed"; return false; }
    return compile(pc, t.physical, words, guest_instructions, error);
}

Jit::Block* Jit::lookup_or_compile(u32 pc, u32 pa, std::string& error) {
    if (auto nb = no_block_.find(pc); nb != no_block_.end()) {
        if (nb->second == pa) return nullptr;
        no_block_.erase(nb);
    }
    auto it = blocks_.find(pc);
    if (it != blocks_.end()) {
        if (it->second->valid && it->second->pa == pa) return it->second.get();
        retired_blocks_.push_back(std::move(it->second));
        blocks_.erase(it);
    }
    std::vector<u32> words, site_words;
    u32 count = 0, linked_offset = 0;
    if (!compile(pc, pa, words, count, error, &linked_offset, &site_words)) {
        no_block_[pc] = pa;
        page_blocks_[pa >> 12];                 // keep the page tracked so a write clears the entry
        bus_.mark_code_page(pa & ~0xFFFU);
        tlb_drop_writes_to(pa & ~0xFFFU);
        return nullptr;
    }
    const u32* code = code_->append(words);
    if (code == nullptr) { // full: start over
        clear();
        code = code_->append(words);
        if (code == nullptr) { error = code_->error(); return nullptr; }
    }
    auto b = std::make_unique<Block>();
    b->ea = pc; b->pa = pa; b->count = count; b->code = code; b->linked_offset = linked_offset;
    Block* raw = b.get();
    for (const u32 w : site_words) sites_[code + w] = raw;
    blocks_[pc] = std::move(b);
    const u32 first = pa >> 12, last = (pa + count * 4U - 1U) >> 12;
    for (u32 p = first; p <= last; ++p) { page_blocks_[p].push_back(raw); bus_.mark_code_page(p << 12); tlb_drop_writes_to(p << 12); }
    ++stats_.blocks_compiled;
    return raw;
}

void Jit::link_check_context() {
    const CpuState& s = cpu_.state_;
    FetchContext now;
    now.msr = s.msr & (MSR_IR | MSR_PR);
    now.hid4 = cpu_.config_.extended_bats ? (s.hid4 & HID4_SBE) : 0U;
    now.ibat = s.ibat;
    for (std::size_t k = 0; k < contexts_.size(); ++k)
        if (contexts_[k] == now) { link_context_ = static_cast<u32>(k); return; }
    if (contexts_.size() >= kNoContext) { link_context_ = kNoContext; return; } // no more ids: no new links
    contexts_.push_back(now);
    link_context_ = static_cast<u32>(contexts_.size() - 1U);
}

// Patches `site` (in a valid block) to jump straight to `target` while the current
// translation context is active. Only for targets whose fetch translation does not
// use the page table (it then depends on nothing but that context).
void Jit::link(const u32* site, Block* target) {
    if (link_context_ == kNoContext || !target->valid) return;
    const auto it = sites_.find(site);
    if (it == sites_.end() || !it->second->valid) return;
    Translation t;
    if (!translate_without_page_table(cpu_.state_, target->ea, AccessType::Fetch, cpu_.config_.extended_bats, t) ||
        t.status != TranslateStatus::Ok || t.physical != target->pa)
        return;
    const u32* entry = target->code + target->linked_offset;
    const auto delta = static_cast<s64>(entry - (site + 3));
    if (delta < -(s64{1} << 25) || delta >= (s64{1} << 25)) return;
    if (!code_->patch(site, Emitter::movz_encoding(false, T2, static_cast<u16>(link_context_))) ||
        !code_->patch(site + 3, Emitter::b_encoding(delta)))
        return;
    target->incoming.push_back(site);
    ++stats_.links;
}

void Jit::unlink_incoming(Block* b) {
    for (const u32* site : b->incoming) {
        (void)code_->patch(site, Emitter::movz_encoding(false, T2, static_cast<u16>(kNoContext)));
        (void)code_->patch(site + 3, Emitter::b_encoding(1)); // back to the stub that follows
    }
    b->incoming.clear();
}

bool Jit::resumes_after_interpreting(u32 ins) noexcept {
    const u32 op = ins >> 26;
    if (op == 19U) return ((ins >> 1) & 0x3FFU) == 50U; // rfi
    if (op != 31U) return false;
    switch ((ins >> 1) & 0x3FFU) {
    case 83: case 146:              // mfmsr mtmsr
    case 595: case 659:             // mfsr mfsrin
        return true;
    case 339:                       // mfspr, not of the time base / DEC (see plain_spr_read)
        return plain_spr_read(ins);
    default:
        return false;
    }
}

Jit::Result Jit::interpret() {
    ++stats_.interpreter_steps;
    return {cpu_.step(), 1};
}

Jit::Result Jit::run(u64 max_steps) {
    if (!can_execute() || max_steps == 0U) return interpret();
    CpuState& s = cpu_.state_;
    u64 total = 0;
    // Inside the budget nothing outside the CPU changes (no scheduler event, no
    // decrementer underflow) and translated code never changes MSR, so the entry
    // conditions checked here stay true from one block to the next.
    if (s.halted) return interpret();
    if (!host_fp_control_is_default()) return interpret(); // the native FP path relies on it
    tlb_check_context();
    link_check_context();
    pending_site_ = nullptr;
    if ((s.msr & (MSR_SE | MSR_BE | MSR_LE)) != 0U) return interpret();
    if ((s.msr & MSR_EE) != 0U && (cpu_.external_line_ || s.dec_pending)) return interpret();
    if ((s.msr & MSR_POW) != 0U && (s.hid0 & 0x00E00000U) != 0U) return interpret();
    using Fn = u32 (*)(CpuState*, JitExit*, Jit*);
    while (total < max_steps) {
        const u32 pc = s.pc;
        if ((pc & 3U) != 0U) break;
        const Translation t = translate(s, bus_, pc, AccessType::Fetch, cpu_.config_.extended_bats);
        if (t.status != TranslateStatus::Ok) break;
        std::string error;
        Block* b = lookup_or_compile(pc, t.physical, error);
        if (b == nullptr) break;
        if (pending_site_ != nullptr) { link(pending_site_, b); pending_site_ = nullptr; }
        if (b->count > max_steps - total) break;
        if (!code_->make_executable()) break;
        running_ = b;
        running_invalidated_ = false;
        exit_ = {};
        exit_.limit = static_cast<u32>(std::min<u64>(max_steps - total, 0xFFFFFFFFULL));
        const u32 kind = reinterpret_cast<Fn>(reinterpret_cast<std::uintptr_t>(b->code))(&s, &exit_, this);
        running_ = nullptr;
        const u64 steps = exit_.steps;
        cpu_.retired_ += steps;
        ++stats_.block_runs;
        stats_.block_steps += steps;
        if (total != 0U) ++stats_.chained;
        total += steps;
        if (kind == kExitRecheck) {
            ++stats_.recheck_exits;
            if (s.halted || (s.msr & (MSR_SE | MSR_BE | MSR_LE)) != 0U) break;
            if ((s.msr & MSR_EE) != 0U && (cpu_.external_line_ || s.dec_pending)) break;
            if ((s.msr & MSR_POW) != 0U && (s.hid0 & 0x00E00000U) != 0U) break;
            tlb_check_context();
            link_check_context();
            pending_site_ = nullptr;
            continue;
        }
        if (kind == kExitFallback) {
            ++stats_.fallback_exits;
            // the instruction the block stopped at: interpret it here when that is
            // safe, instead of a round trip through the caller for every one
            if (total >= max_steps || (s.pc & 3U) != 0U) break;
            const Translation ft = translate(s, bus_, s.pc, AccessType::Fetch, cpu_.config_.extended_bats);
            if (ft.status != TranslateStatus::Ok) break;
            const u8* p = bus_.ram_pointer(ft.physical, 4);
            if (p == nullptr) break;
            const u32 ins = (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
            if (!resumes_after_interpreting(ins)) break;
            const StepResult r = cpu_.step();
            ++stats_.interpreter_steps;
            ++stats_.resumed;
            ++total;
            if (r != StepResult::Executed) return {r, total};
            // the step may have changed MSR (mtmsr, rfi): the same entry
            // conditions as above, else the caller takes over; the translation
            // and link contexts follow MSR
            if (s.halted || (s.msr & (MSR_SE | MSR_BE | MSR_LE)) != 0U) break;
            if ((s.msr & MSR_EE) != 0U && (cpu_.external_line_ || s.dec_pending)) break;
            if ((s.msr & MSR_POW) != 0U && (s.hid0 & 0x00E00000U) != 0U) break;
            tlb_check_context();
            link_check_context();
            pending_site_ = nullptr;
            continue;
        }
        if (steps == 0U) break;
        pending_site_ = reinterpret_cast<const u32*>(static_cast<std::uintptr_t>(exit_.site));
    }
    pending_site_ = nullptr;
    if (total == 0U) return interpret(); // nothing translated applies here: the interpreter takes one step
    return {StepResult::Executed, total};
}

} // namespace orca::ppc
