// DSP core: every encoding below is taken from the bit fields of the
// GameCube DSP User's Manual v0.1.8; expected values are computed by hand from
// the operations and flag rules written in that manual.
#include "orca/dsp/dsp_core.hpp"
#include "orca/hw/dsp_interface.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <vector>

using namespace orca;
using namespace orca::dsp;

namespace {
struct Host final : DspHost {
    std::vector<u8> ram = std::vector<u8>(0x10000, 0);
    int irqs = 0, halts = 0;
    bool dsp_dma_read(u32 pa, std::span<u8> out) override {
        if (pa + out.size() > ram.size()) return false;
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = ram[pa + i];
        return true;
    }
    bool dsp_dma_write(u32 pa, std::span<const u8> in) override {
        if (pa + in.size() > ram.size()) return false;
        for (std::size_t i = 0; i < in.size(); ++i) ram[pa + i] = in[i];
        return true;
    }
    void dsp_interrupt_cpu() override { ++irqs; }
    void dsp_halted() override { ++halts; }
    std::vector<u8> aram = std::vector<u8>(0x1000, 0);
    bool dsp_aram_read8(u32 a, u8& v) override { if (a >= aram.size()) return false; v = aram[a]; return true; }
    bool dsp_aram_write16(u32 a, u16 v) override {
        if (a + 2U > aram.size()) return false;
        aram[a] = static_cast<u8>(v >> 8); aram[a + 1] = static_cast<u8>(v);
        return true;
    }
};

struct Rig {
    Host host;
    DspCore dsp{host};
    void load(std::initializer_list<u16> words, u16 at = 0) {
        u16 a = at;
        for (u16 w : words) dsp.iram()[a++] = w;
    }
    // Runs until HALT (or failure / budget); returns true if it halted cleanly.
    bool run(std::initializer_list<u16> words, u64 budget = 10000) {
        load(words);
        dsp.reset(0);
        dsp.set_halted(false);
        dsp.run(budget);
        if (dsp.failed()) std::fprintf(stderr, "    DSP failed: %s\n", dsp.error().c_str());
        return dsp.halted() && !dsp.failed();
    }
    DspState& s() { return dsp.state(); }
};

constexpr u16 HALT = 0x0021, NOP = 0x0000;
constexpr u16 LRI(unsigned r) { return static_cast<u16>(0x0080U | r); }
constexpr u16 LR(unsigned r) { return static_cast<u16>(0x00C0U | r); }
constexpr u16 SR(unsigned r) { return static_cast<u16>(0x00E0U | r); }
constexpr u16 MRR(unsigned d, unsigned s) { return static_cast<u16>(0x1C00U | (d << 5) | s); }
constexpr u16 SI(u16 m) { return static_cast<u16>(0x1600U | (m & 0xFFU)); }
constexpr unsigned AR0 = 0, AR3 = 3, IX0 = 4, WR0 = 8, ST0 = 12, AC0H = 16, AC1H = 17, CONFIG = 18, SR_ = 19,
                   AX0L = 24, AX1L = 25, AX0H = 26, AX1H = 27, AC0L = 28, AC1L = 29, AC0M = 30, AC1M = 31;
constexpr u16 SET16 = 0x8E00, SET40 = 0x8F00, SET15 = 0x8D00, CLR15 = 0x8C00, M0 = 0x8B00, M2 = 0x8A00;
} // namespace

// ------------------------------------------------------------------ registers / memory
TEST_CASE(dsp_lri_lr_sr_mrr_and_register_widths) {
    Rig t;
    CHECK(t.run({LRI(AR0), 0x0010, LRI(AC0M), 0x1234, SR(AC0M), 0x0020, LR(AX1H), 0x0020, LRI(AC0H), 0x0181,
                 LRI(CONFIG), 0xAB12, LRI(SR_), 0xFFFF, MRR(AX0L, AC0H), HALT}));
    CHECK_EQ(t.s().ar[0], 0x0010U);
    CHECK_EQ(t.dsp.dram()[0x20], 0x1234U);
    CHECK_EQ(t.s().ax_h[1], 0x1234U);
    CHECK_EQ(t.s().ac_h[0], 0x81U);       // upper 8 bits ignored
    CHECK_EQ(t.s().ax_l[0], 0xFF81U);     // $ac0.h reads sign-extended
    CHECK_EQ(t.s().config, 0x12U);        // 8-bit register
    CHECK_EQ(t.s().sr, 0xFEFFU);          // bit 8 always 0
    CHECK_EQ(t.host.halts, 1);
}

TEST_CASE(dsp_40bit_mode_sign_extension_and_saturation) {
    Rig t;
    // SET40: a load to $ac0.m sets $ac0.l = 0 and sign-extends into $ac0.h
    CHECK(t.run({LRI(AC0L), 0x5555, SET40, LRI(AC0M), 0x8000, SET16, LRI(AC1L), 0x7777, LRI(AC1M), 0x8000, HALT}));
    CHECK_EQ(t.dsp.acc(0), -0x80000000LL);
    CHECK_EQ(t.s().ac_l[1], 0x7777U); // 16-bit mode: only .m changes
    CHECK_EQ(t.s().ac_h[1], 0x00U);
    // saturation of reads from $acX.m in 40-bit mode
    Rig u;
    CHECK(u.run({SET40, LRI(AC0M), 0x1234, LRI(AC0H), 0x0001, MRR(AX0L, AC0M), LRI(AC1M), 0x1234, LRI(AC1H), 0x00FF,
                 MRR(AX0H, AC1M), SET16, MRR(AX1L, AC0M), HALT}));
    CHECK_EQ(u.dsp.acc(0), 0x0112340000LL);
    CHECK_EQ(u.s().ax_l[0], 0x7FFFU); // positive, does not fit in s32 -> 0x7fff
    CHECK_EQ(u.s().ax_h[0], 0x8000U); // 0xFF12340000 is negative and out of s32 range -> 0x8000
    CHECK_EQ(u.s().ax_l[1], 0x1234U); // 16-bit mode: no saturation
}

// ------------------------------------------------------------------ arithmetic and flags
TEST_CASE(dsp_add_sub_flags_follow_the_manual_rules) {
    Rig t;
    // ac0 = 0x7FFFFFFFFF, ac1 = 1, ADD $ac0, $ac1 -> 0x8000000000
    CHECK(t.run({LRI(AC0H), 0x007F, LRI(AC0M), 0xFFFF, LRI(AC0L), 0xFFFF, LRI(AC1L), 0x0001, 0x4C00, HALT}));
    CHECK_EQ(t.dsp.acc(0), -0x8000000000LL);
    CHECK_EQ(t.s().sr & 0xFFU, static_cast<unsigned>(SR_OS | SR_TB | SR_AS | SR_S | SR_O));
    // 5 - 3: carry (A >= C), no borrow semantics
    Rig u;
    CHECK(u.run({LRI(AC0L), 5, LRI(AC1L), 3, 0x5C00, HALT}));
    CHECK_EQ(u.dsp.acc(0), 2);
    CHECK_EQ(u.s().sr & 0xFFU, static_cast<unsigned>(SR_TB | SR_C));
    // 3 - 5 = -2: no carry, sign
    Rig v;
    CHECK(v.run({LRI(AC0L), 3, LRI(AC1L), 5, 0x5C00, HALT}));
    CHECK_EQ(v.dsp.acc(0), -2);
    CHECK_EQ(v.s().sr & 0xFFU, static_cast<unsigned>(SR_TB | SR_S));
    // OS is sticky: a later CLR does not clear it; CLR sets TB and Z
    Rig w;
    CHECK(w.run({LRI(AC0H), 0x007F, LRI(AC0M), 0xFFFF, LRI(AC0L), 0xFFFF, LRI(AC1L), 1, 0x4C00, 0x8100, HALT}));
    CHECK_EQ(w.s().sr & 0xFFU, static_cast<unsigned>(SR_OS | SR_TB | SR_Z));
}

TEST_CASE(dsp_immediate_inc_dec_neg_abs_cmp) {
    Rig t;
    CHECK(t.run({0x0200, 0x8000,   // ADDI $ac0, #0x8000 (sign-extended, << 16)
                 0x0501,           // ADDIS $ac1, #1
                 0x7600,           // INC $ac0
                 0x7B00,           // DEC $ac1
                 HALT}));
    CHECK_EQ(t.dsp.acc(0), -0x80000000LL + 1);
    CHECK_EQ(t.dsp.acc(1), 0x10000 - 1);
    Rig n;
    CHECK(n.run({0x7C00, HALT})); // NEG 0: carry set only for 0 (manual note)
    CHECK_EQ(n.dsp.acc(0), 0);
    CHECK((n.s().sr & SR_C) != 0U);
    CHECK((n.s().sr & SR_Z) != 0U);
    Rig a;
    CHECK(a.run({LRI(AC0M), 0xFFFE, LRI(AC0H), 0xFF, 0xA100, HALT})); // ABS -131072
    CHECK_EQ(a.dsp.acc(0), 0x20000);
    Rig c; // CMPIS $ac0, #-1 with ac0 = 0 -> 0 - (-1<<16): positive, no carry
    CHECK(c.run({0x06FF, HALT}));
    CHECK_EQ(c.dsp.acc(0), 0); // compare does not write the accumulator
    CHECK_EQ(c.s().sr & (SR_Z | SR_S | SR_C), 0U);
}

TEST_CASE(dsp_logic_ops_touch_only_mid) {
    Rig t;
    CHECK(t.run({LRI(AC0L), 0x1111, LRI(AC0M), 0xF0F0, 0x0240, 0x0FF0, // ANDI -> 0x00F0
                 0x0260, 0x0001,                                       // ORI  -> 0x00F1
                 0x0220, 0x00FF,                                       // XORI -> 0x000E
                 0x3280,                                               // NOT  -> 0xFFF1
                 HALT}));
    CHECK_EQ(t.s().ac_m[0], 0xFFF1U);
    CHECK_EQ(t.s().ac_l[0], 0x1111U);
    // ANDF / ANDCF set only LZ, then the LZ conditions
    Rig u;
    CHECK(u.run({LRI(AC0M), 0x0C00, 0x02A0, 0x0300, // ANDF: (0x0C00 & 0x0300) == 0 -> LZ = 1
                 LRI(AX0L), 0x0001, 0x02C0, 0x0C00, // ANDCF: == I -> LZ = 1
                 HALT}));
    CHECK((u.s().sr & SR_LZ) != 0U);
    Rig v;
    CHECK(v.run({LRI(AC0M), 0x0C00, 0x02C0, 0x0E00, HALT})); // ANDCF: 0x0C00 != 0x0E00 -> LZ = 0
    CHECK_EQ(v.s().sr & SR_LZ, 0U);
}

TEST_CASE(dsp_shifts) {
    Rig t;
    CHECK(t.run({LRI(AC0L), 0x0001, 0x1404, // LSL $ac0, #4 -> 0x10
                 LRI(AC1M), 0x8000, LRI(AC1H), 0xFF, 0x15FC, // ASR $ac1, 64-60=4 -> 0xFFF8000000
                 HALT}));
    CHECK_EQ(t.dsp.acc(0), 0x10);
    CHECK_EQ(t.dsp.acc(1), -0x8000000LL);
    Rig u; // LSR on a negative value is logical (40-bit)
    CHECK(u.run({LRI(AC0H), 0x80, 0x147C, HALT})); // LSR $ac0, 64-60=4
    CHECK_EQ(u.dsp.acc(0), 0x0800000000LL);
    Rig v; // ASRN: $ac1.m bit 6 set -> right by 64 - (m & 63); clear -> left by m & 63
    CHECK(v.run({LRI(AC0L), 0x0100, LRI(AC1M), 0x007C, 0x02CB, HALT})); // right by 4
    CHECK_EQ(v.dsp.acc(0), 0x10);
    Rig w;
    CHECK(w.run({LRI(AC0L), 0x0001, LRI(AC1M), 0x0008, 0x02CA, HALT})); // LSRN left by 8
    CHECK_EQ(w.dsp.acc(0), 0x100);
    Rig x; // ASR16 / LSL16 / LSR16
    CHECK(x.run({LRI(AC0H), 0xFF, LRI(AC0M), 0x8000, 0x9100, LRI(AC1L), 0x1234, 0xF100, HALT}));
    CHECK_EQ(x.dsp.acc(0), -0x8000LL);
    CHECK_EQ(x.dsp.acc(1), 0x12340000LL);
}

TEST_CASE(dsp_clrl_rounds_to_even) {
    Rig t; // m odd (0x0001), l = 0x8000 -> rounds up
    CHECK(t.run({LRI(AC0M), 0x0001, LRI(AC0L), 0x8000, 0xFC00, LRI(AC1M), 0x0002, LRI(AC1L), 0x8000, 0xFD00, HALT}));
    CHECK_EQ(t.dsp.acc(0), 0x20000);
    CHECK_EQ(t.dsp.acc(1), 0x20000); // even m, l = 0x8000 (exactly half): stays (operation: + 0x7fff)
    Rig u;
    CHECK(u.run({LRI(AC1M), 0x0002, LRI(AC1L), 0x8001, 0xFD00, LRI(AC0M), 0x0002, LRI(AC0L), 0x7FFF, 0xFC00, HALT}));
    CHECK_EQ(u.dsp.acc(1), 0x30000); // above half: rounds up
    CHECK_EQ(u.dsp.acc(0), 0x20000);
}

// ------------------------------------------------------------------ multiplier
TEST_CASE(dsp_multiplier_signed_unsigned_and_am) {
    Rig t;
    CHECK(t.run({LRI(AX0L), 3, LRI(AX0H), 4, 0x9000, 0x6E00, // MUL; MOVP $ac0 (AM = 0: doubled)
                 M0, 0x9000, 0x6F00,                         // M0: no doubling; MOVP $ac1
                 HALT}));
    CHECK_EQ(t.dsp.acc(0), 24);
    CHECK_EQ(t.dsp.acc(1), 12);
    Rig u;
    CHECK(u.run({LRI(AX0L), 0xFFFF, LRI(AX0H), 2, 0x9000, 0x6E00, SET15, 0x9000, 0x6F00, HALT}));
    CHECK_EQ(u.dsp.acc(0), -4);
    CHECK_EQ(u.dsp.acc(1), 0xFFFFLL * 2 * 2);
    Rig v; // CLRP leaves l=0, m1=fff0, h=ff, m2=0010, which sums to 0
    CHECK(v.run({LRI(AX0L), 5, LRI(AX0H), 5, M0, 0x9000, 0x8400, 0x6E00, HALT}));
    CHECK_EQ(v.s().prod_m1, 0xFFF0U);
    CHECK_EQ(v.s().prod_m2, 0x0010U);
    CHECK_EQ(v.dsp.prod(), 0);
    CHECK_EQ(v.dsp.acc(0), 0);
    Rig w; // MADD then MSUB, MULAC accumulates the previous product
    CHECK(w.run({M0, LRI(AX1L), 10, LRI(AX1H), 3, 0x9800, 0xF300, 0xF700, 0xF300, // MUL ax1; MADD ax1; MSUB; MADD
                 0x9C00, 0x9C00,                                                  // MULAC $ac0 twice
                 HALT}));
    CHECK_EQ(w.dsp.acc(0), 60 + 30);
    CHECK_EQ(w.dsp.prod(), 30);
    Rig x; // MULX $ax0.h * $ax1.l ; MULC $ac1.m * $ax0.h
    CHECK(x.run({M0, LRI(AX0H), 7, LRI(AX1L), 6, 0xB000, 0x6E00, LRI(AC1M), 0xFFFE, 0xD000, 0x6E00, HALT}));
    CHECK_EQ(x.dsp.acc(0), -14); // MULC $ac1.m (-2) * $ax0.h (7)
}

// ------------------------------------------------------------------ control flow
TEST_CASE(dsp_loops_calls_and_conditions) {
    Rig t;
    CHECK(t.run({0x8100,             // 0 CLR $ac0
                 0x1105, 0x0003,     // 1 BLOOPI 5, end = 3
                 0x7600,             // 3 INC $ac0 (block end, inclusive)
                 0x1003,             // 4 LOOPI 3
                 0x7700,             // 5 INC $ac1
                 0x1100, 0x0008,     // 6 BLOOPI 0 -> skip to 9
                 0x7600,             // 8 (skipped)
                 HALT}));            // 9
    CHECK_EQ(t.dsp.acc(0), 5);
    CHECK_EQ(t.dsp.acc(1), 3);
    CHECK(t.s().st[0].empty());
    CHECK(t.s().st[2].empty());
    Rig c;
    CHECK(c.run({0x02BF, 0x0003, HALT, LRI(AC0M), 7, 0x02DF})); // CALL 3; HALT; sub: LRI; RET
    CHECK_EQ(c.s().ac_m[0], 7U);
    Rig j; // JZ taken after CLR, JNZ not taken
    CHECK(j.run({0x8100, 0x0294, 0x0006, 0x0295, 0x0007, HALT, HALT, LRI(AC1M), 9, HALT}));
    CHECK_EQ(j.s().ac_m[1], 9U);
    CHECK_EQ(j.s().pc, 0x000AU);
    Rig i; // IFcc false skips one word, true executes it
    CHECK(i.run({0x8100, 0x0274, 0x7600, 0x0275, 0x7700, HALT})); // IFNZ (false) INC ac0; IFZ (true) INC ac1
    CHECK_EQ(i.dsp.acc(0), 0);
    CHECK_EQ(i.dsp.acc(1), 1);
    Rig r; // JMPR / CALLR via $ar0
    CHECK(r.run({LRI(AR0), 0x0005, 0x171F, HALT, HALT, LRI(AC0M), 1, 0x02DF})); // CALLR $ar0 -> 5, RET -> 3
    CHECK_EQ(r.s().ac_m[0], 1U);
    CHECK_EQ(r.s().pc, 0x0004U);
}

TEST_CASE(dsp_condition_table) {
    // Each condition evaluated against a hand-set $sr (via LRI $sr) and JMPcc.
    struct Case { u16 sr; unsigned cc; bool taken; };
    const Case cases[] = {
        {0, 0x0, true}, {SR_S, 0x0, false}, {SR_S | SR_O, 0x0, true}, {SR_S, 0x1, true},
        {0, 0x2, true}, {SR_Z, 0x2, false}, {SR_Z, 0x3, true}, {SR_S, 0x3, true}, {0, 0x3, false},
        {0, 0x4, true}, {SR_Z, 0x5, true}, {SR_C, 0x6, false}, {SR_C, 0x7, true},
        {0, 0x8, true}, {SR_AS, 0x9, true}, {SR_TB, 0xA, true}, {SR_TB | SR_Z, 0xA, false},
        {0, 0xB, true}, {SR_AS, 0xB, false}, {SR_LZ, 0xC, false}, {SR_LZ, 0xD, true}, {SR_O, 0xE, true},
        {0, 0xE, false}, {0, 0xF, true},
    };
    for (const Case& c : cases) {
        Rig t;
        CHECK(t.run({LRI(SR_), c.sr, static_cast<u16>(0x0290U | c.cc), 0x0005, HALT, HALT}));
        CHECK_EQ(t.s().pc, c.taken ? 0x0006U : 0x0005U);
    }
}

// ------------------------------------------------------------------ extended opcodes
TEST_CASE(dsp_extended_opcodes_run_in_parallel) {
    // Manual example: MOVR'MV $ac1, $ax0.l : $ax0.l, $ac1.m (0x6113) swaps the values.
    Rig t;
    CHECK(t.run({LRI(AX0L), 0x1111, LRI(AC1M), 0x2222, 0x6113, HALT}));
    CHECK_EQ(t.s().ac_m[1], 0x1111U);
    CHECK_EQ(t.s().ac_l[1], 0x0000U);
    CHECK_EQ(t.s().ax_l[0], 0x2222U);
    // Manual example: INC'L $ac0 : $ac0.l, @$ar0 (0x7660) ORs both writes to $ac0.l.
    Rig u;
    u.dsp.dram()[0x40] = 0x0010;
    CHECK(u.run({LRI(AR0), 0x0040, LRI(AC0L), 0x0001, 0x7660, HALT}));
    CHECK_EQ(u.s().ac_l[0], 0x0012U); // (1 + 1) | 0x10
    CHECK_EQ(u.s().ar[0], 0x0041U);
    // NX'LS $ax0.l, $ac0.m: load from @$ar0, store $ac0.m at @$ar3, both ++
    Rig v;
    v.dsp.dram()[0x10] = 0xABCD;
    CHECK(v.run({LRI(AR0), 0x0010, LRI(AR3), 0x0020, LRI(AC0M), 0x5A5A, 0x8080, HALT}));
    CHECK_EQ(v.s().ax_l[0], 0xABCDU);
    CHECK_EQ(v.dsp.dram()[0x20], 0x5A5AU);
    CHECK_EQ(v.s().ar[0], 0x11U);
    CHECK_EQ(v.s().ar[3], 0x21U);
    // NX'LDNM $ax0.h, $ax1.l, @$ar1 (11dr 11ss: d=1 -> ax0.h, r=0 -> ax1.l, ss=1): both += ix
    Rig w;
    w.dsp.dram()[0x30] = 0x1111;
    w.dsp.dram()[0x50] = 0x2222;
    CHECK(w.run({LRI(1), 0x0030, LRI(AR3), 0x0050, LRI(5), 0x0004, LRI(7), 0xFFFE, 0x80ED, HALT}));
    CHECK_EQ(w.s().ax_h[0], 0x1111U);
    CHECK_EQ(w.s().ax_l[1], 0x2222U);
    CHECK_EQ(w.s().ar[1], 0x0034U);
    CHECK_EQ(w.s().ar[3], 0x004EU);
    // NX'LDAX $ax1, @$ar0 (11sr 0011, s=0, r=1): $ax1.h = MEM[$ar0], $ax1.l = MEM[$ar3]
    Rig x;
    x.dsp.dram()[0x01] = 0x7777;
    x.dsp.dram()[0x02] = 0x8888;
    CHECK(x.run({LRI(AR0), 0x0001, LRI(AR3), 0x0002, 0x80D3, HALT}));
    CHECK_EQ(x.s().ax_h[1], 0x7777U);
    CHECK_EQ(x.s().ax_l[1], 0x8888U);
    // Nybble-3 opcodes use a 7-bit extension: ANDR'IR $ar1 (0x3409)
    Rig y;
    CHECK(y.run({LRI(AC0M), 0x00FF, LRI(AX0H), 0x0F0F, 0x3409, HALT}));
    CHECK_EQ(y.s().ac_m[0], 0x000FU);
    CHECK_EQ(y.s().ar[1], 0x0001U);
}



TEST_CASE(dsp_os_is_sticky_and_orc_keeps_flags) {
    // overflow, then an ADD without overflow: O clears, OS stays (manual "Flags")
    Rig t;
    CHECK(t.run({LRI(AC0H), 0x007F, LRI(AC0M), 0xFFFF, LRI(AC0L), 0xFFFF, LRI(AC1L), 1, 0x4C00, // overflow
                 0x8100, LRI(AC0L), 2, 0x4C00, HALT}));                                      // CLR; 2 + 1
    CHECK_EQ(t.dsp.acc(0), 3);
    CHECK((t.s().sr & SR_OS) != 0U);
    CHECK_EQ(t.s().sr & SR_O, 0U);
    // ORC: manual lists no flag changes
    Rig u;
    CHECK(u.run({0x8100, LRI(AC1M), 0x00F0, 0x3E00, HALT})); // CLR sets Z; ORC $ac0.m |= $ac1.m -> 0x00F0
    CHECK_EQ(u.s().ac_m[0], 0x00F0U);
    CHECK((u.s().sr & SR_Z) != 0U); // still the Z left by CLR
}

// ------------------------------------------------------------------ broader coverage (values computed by hand)
TEST_CASE(dsp_register_operand_arithmetic_and_compare_flags) {
    Rig t;
    CHECK(t.run({LRI(AR0), 0x0100, LRI(AX0L), 3, LRI(AX0H), 2, LRI(AX1L), 0xFFFF, LRI(AX1H), 0xFFFF,
                 0x4800, 0x1B1E, 0x1B1C,         // ADDAX $ac0,$ax0        -> 0x20003
                 0x4B00, 0x7100, 0x1B1F, 0x1B1D, // ADDAX $ac1,$ax1 ; ADDAXL $ac1,$ax0.l -> 2
                 0x4400, 0x1B1E, 0x1B1C,         // ADDR $ac0,$ax0.h      -> 0x40003
                 0x5000, 0x1B1E, 0x1B1C,         // SUBR $ac0,$ax0.l      -> 0x10003
                 0x5800, 0x1B1E, 0x1B1C,         // SUBAX $ac0,$ax0       -> -0x10000
                 0x6D00, 0x1B1F, 0x1B1D,         // MOV $ac1,$ac0
                 0x7500, 0x1B1F, 0x1B1D,         // INCM $ac1             -> 0
                 0x7800, 0x1B1E, 0x1B1C,         // DECM $ac0             -> -0x20000
                 0x6900, 0x1B1F, 0x1B1D,         // MOVAX $ac1,$ax0       -> 0x20003
                 0x6600, 0x1B1E, 0x1B1C,         // MOVR $ac0,$ax1.h      -> -0x10000
                 0x8200, 0x1B13,                 // CMP: -0x10000 - 0x20003
                 0x8700, 0x1B13,                 // TSTAXH $ax1.h
                 0xC900, 0x1B13,                 // CMPAXH $ac1,$ax0.h: 0x20003 - 0x20000
                 0x8500, 0x1B13,                 // TSTPROD (0)
                 HALT}));
    const u16 expect[] = {0x0002, 0x0003, 0x0000, 0x0002, 0x0004, 0x0003, 0x0001, 0x0003, 0xFFFF, 0x0000,
                          0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFE, 0x0000, 0x0002, 0x0003, 0xFFFF, 0x0000,
                          static_cast<u16>(SR_TB | SR_S | SR_C), static_cast<u16>(SR_TB | SR_S),
                          static_cast<u16>(SR_TB | SR_C), static_cast<u16>(SR_TB | SR_Z)};
    for (unsigned i = 0; i < std::size(expect); ++i) CHECK_EQ(t.dsp.dram()[0x100 + i], expect[i]);
    CHECK_EQ(t.s().ar[0], 0x0118U);
}

TEST_CASE(dsp_multiply_families_and_product_moves) {
    Rig t;
    CHECK(t.run({M0, LRI(AX0L), 3, LRI(AX0H), 5, LRI(AX1L), 7, LRI(AX1H), 11, LRI(AR0), 0x0100,
                 0x9000,         // MUL $ax0                  prod 15
                 0xAE00,         // MULXMV $ax0.l,$ax1.h,$ac0 ac0 15, prod 33
                 0xB400,         // MULXAC $ax0.h,$ax1.l,$ac0 ac0 48, prod 35
                 0xBB00,         // MULXMVZ $ax0.h,$ax1.h,$ac1 ac1 0, prod 55
                 0x1B1C,         // log ac0.l = 48
                 0x9A00,         // MULMVZ $ax1,$ac0          ac0 0, prod 77
                 0x9700,         // MULMV $ax0,$ac1           ac1 77, prod 15
                 0x9D00,         // MULAC $ax1,$ac1           ac1 92, prod 77
                 0x1B1D,         // log ac1.l = 92
                 LRI(AC0M), 2,
                 0xC800,         // MULC $ac0.m,$ax1.h         prod 22
                 0xC700,         // MULCMV $ac0.m,$ax0.h,$ac1  ac1 22, prod 10
                 0xCD00,         // MULCAC $ac0.m,$ax1.h,$ac1  ac1 32, prod 22
                 0x1B1D,         // log ac1.l = 32
                 0xC200,         // MULCMVZ $ac0.m,$ax0.h,$ac0 ac0 0, prod 10
                 0xE000,         // MADDX $ax0.l,$ax1.l       prod 31
                 0xE700,         // MSUBX $ax0.h,$ax1.h       prod -24
                 0x1B14,         // log prod.l = 0xFFE8
                 LRI(AC0M), 3,
                 0xE900,         // MADDC $ac0.m,$ax1.h       prod 9
                 0xEC00,         // MSUBC $ac0.m,$ax0.h       prod -6
                 0x7E00,         // MOVNP $ac0                ac0 6
                 0x8300,         // MULAXH                    prod 25
                 0xFF00,         // MOVPZ $ac1                ac1 0
                 0xF900,         // ADDPAXZ $ac1,$ax0         ac1 0x50000
                 0x4E00,         // ADDP $ac0                 ac0 31
                 0x5E00,         // SUBP $ac0                 ac0 6
                 HALT}));
    CHECK_EQ(t.dsp.dram()[0x100], 48U);
    CHECK_EQ(t.dsp.dram()[0x101], 92U);
    CHECK_EQ(t.dsp.dram()[0x102], 32U);
    CHECK_EQ(t.dsp.dram()[0x103], 0xFFE8U);
    CHECK_EQ(t.dsp.acc(0), 6);
    CHECK_EQ(t.dsp.acc(1), 0x50000);
    CHECK_EQ(t.dsp.prod(), 25);
}

TEST_CASE(dsp_register_logic_and_register_shifts) {
    Rig t;
    CHECK(t.run({LRI(AR0), 0x0100, LRI(AC0M), 0x00F0, LRI(AC1M), 0x0F00, LRI(AX0H), 0x0FF0,
                 0x3000, 0x1B1E,              // XORR $ac0.m,$ax0.h -> 0x0F00
                 0x3900, 0x1B1F,              // ORR  $ac1.m,$ax0.h -> 0x0FF0
                 0x3080, 0x1B1E,              // XORC $ac0.m,$ac1.m -> 0x00F0
                 0x3D00, 0x1B1F,              // ANDC $ac1.m,$ac0.m -> 0x00F0
                 LRI(AC0M), 0x0001, 0x3E00, 0x1B1E, // ORC $ac0.m,$ac1.m -> 0x00F1
                 LRI(AC1M), 0x0004, 0x3C80, 0x1B1E, // LSRNR $ac0 (left 4) -> m 0x0F10
                 LRI(AC1M), 0x007C, 0x3E80, 0x1B1E, // ASRNR $ac0 (right 4) -> m 0x00F1
                 LRI(AX0H), 0x0078, 0x3480, 0x1B1C, // LSRNRX $ac0,$ax0.h (right 8) -> l 0xF100
                 LRI(AC1M), 0x8000, LRI(AC1H), 0x00FF, LRI(AX1H), 0x0004, 0x3B80, 0x1B11, // ASRNRX $ac1,$ax1.h (left 4)
                 HALT}));
    const u16 expect[] = {0x0F00, 0x0FF0, 0x00F0, 0x00F0, 0x00F1, 0x0F10, 0x00F1, 0xF100, 0xFFF8};
    for (unsigned i = 0; i < std::size(expect); ++i) CHECK_EQ(t.dsp.dram()[0x100 + i], expect[i]);
    CHECK_EQ(t.dsp.acc(1), -0x800000000LL);
}

TEST_CASE(dsp_addressing_modes) {
    Rig t; // ILRR family reads instruction memory
    t.dsp.iram()[0x80] = 0xBEEF; t.dsp.iram()[0x81] = 0x1234; t.dsp.iram()[0x82] = 0x5555;
    t.load({LRI(1), 0x0080, LRI(5), 2, 0x0219, 0x0315, SR(AC1M), 0x0010, 0x021D, 0x0311, HALT});
    t.dsp.reset(0);
    t.dsp.set_halted(false);
    t.dsp.run(100);
    CHECK(t.dsp.halted() && !t.dsp.failed());
    CHECK_EQ(t.dsp.dram()[0x10], 0x1234U); // ILRRD
    CHECK_EQ(t.s().ac_m[0], 0xBEEFU);     // ILRRN
    CHECK_EQ(t.s().ac_m[1], 0x5555U);     // ILRR
    CHECK_EQ(t.s().ar[1], 0x0082U);
    Rig u; // LRR / SRR families
    u.dsp.dram()[0x300] = 0x1111; u.dsp.dram()[0x2FF] = 0x2222; u.dsp.dram()[0x310] = 0x3333;
    CHECK(u.run({LRI(2), 0x0300, LRI(6), 0x0010, 0x18D8, 0x195A, 0x19D9, 0x185B,
                 LRI(AR3), 0x0400, LRI(7), 4, 0x1AFB, 0x1BFA, 0x1A78, HALT}));
    CHECK_EQ(u.s().ax_l[0], 0x1111U);
    CHECK_EQ(u.s().ax_h[0], 0x2222U);
    CHECK_EQ(u.s().ax_l[1], 0x1111U);
    CHECK_EQ(u.s().ax_h[1], 0x3333U);
    CHECK_EQ(u.s().ar[2], 0x0310U);
    CHECK_EQ(u.dsp.dram()[0x400], 0x3333U);
    CHECK_EQ(u.dsp.dram()[0x3FF], 0x2222U);
    CHECK_EQ(u.dsp.dram()[0x403], 0x1111U);
    CHECK_EQ(u.s().ar[3], 0x0403U);
    Rig v; // $config-based LRS/SRS/SRSH, LRIS, SBSET/SBCLR, ADDARN/SUBARN/DAR/IAR
    CHECK(v.run({LRI(CONFIG), 0x0005, LRI(AC0M), 0x7777, 0x2E10, 0x2110, LRI(AC1H), 0x0080, 0x2911, 0x08FE,
                 0x1307, 0x1300, 0x1200, LRI(AR0), 0x0010, LRI(5), 3, 0x0014, LRI(IX0), 1, 0x000C, 0x0004, 0x0009,
                 HALT}));
    CHECK_EQ(v.dsp.dram()[0x510], 0x7777U);
    CHECK_EQ(v.s().ax_l[1], 0x7777U);
    CHECK_EQ(v.dsp.dram()[0x511], 0xFF80U);
    CHECK_EQ(v.s().ax_l[0], 0xFFFEU);
    CHECK((v.s().sr & SR_AM) != 0U);
    CHECK_EQ(v.s().sr & SR_LZ, 0U);
    CHECK_EQ(v.s().ar[0], 0x0011U);
    CHECK_EQ(v.s().ar[1], 0x0001U);
    Rig w; // LOOP/BLOOP with a register count, RETcc/RTIcc not taken, JMPR
    CHECK(w.run({LRI(5), 4, 0x8100, 0x0045, 0x7600, 0x0065, 0x0007, 0x7700, 0x02D5, 0x02F5, LRI(1), 0x000E, 0x172F,
                 HALT, HALT}));
    CHECK_EQ(w.dsp.acc(0), 4);
    CHECK_EQ(w.dsp.acc(1), 4);
    CHECK_EQ(w.s().pc, 0x000FU);
}

TEST_CASE(dsp_every_extended_opcode) {
    struct Exp { char what; unsigned idx; u16 val; };
    struct Case { u16 ext; std::vector<Exp> exp; };
    const std::vector<Case> cases = {
        {0x05, {{'a', 1, 0x1F}}},                                                     // 'DR $ar1
        {0x0A, {{'a', 2, 0x31}}},                                                     // 'IR $ar2
        {0x0F, {{'a', 3, 0x45}}},                                                     // 'NR $ar3
        {0x1C, {{'H', 1, 0x0B0B}}},                                                   // 'MV $ax1.h,$ac0.l
        {0x20, {{'d', 0x10, 0x0B0B}, {'a', 0, 0x11}}},                                // 'S @$ar0,$ac0.l
        {0x3D, {{'d', 0x20, 0x1A1A}, {'a', 1, 0x23}}},                                // 'SN @$ar1,$ac1.m
        {0x5E, {{'H', 1, 0x1030}, {'a', 2, 0x34}}},                                   // 'LN $ax1.h,@$ar2
        {0x7B, {{'M', 1, 0x1040}, {'a', 3, 0x41}}},                                   // 'L $ac1.m,@$ar3
        {0xA1, {{'H', 0, 0x1010}, {'d', 0x40, 0x1A1A}, {'a', 0, 0x11}, {'a', 3, 0x41}}}, // 'LS
        {0xA3, {{'d', 0x10, 0x1A1A}, {'H', 0, 0x1040}, {'a', 0, 0x11}, {'a', 3, 0x41}}}, // 'SL
        {0x94, {{'L', 1, 0x1010}, {'d', 0x40, 0x0A0A}, {'a', 0, 0x12}, {'a', 3, 0x41}}}, // 'LSN
        {0x96, {{'d', 0x10, 0x0A0A}, {'L', 1, 0x1040}, {'a', 0, 0x12}, {'a', 3, 0x41}}}, // 'SLN
        {0x89, {{'L', 0, 0x1010}, {'d', 0x40, 0x1A1A}, {'a', 0, 0x11}, {'a', 3, 0x45}}}, // 'LSM
        {0x8A, {{'d', 0x10, 0x0A0A}, {'L', 0, 0x1040}, {'a', 0, 0x11}, {'a', 3, 0x45}}}, // 'SLM
        {0xBC, {{'H', 1, 0x1010}, {'d', 0x40, 0x0A0A}, {'a', 0, 0x12}, {'a', 3, 0x45}}}, // 'LSNM
        {0xBF, {{'d', 0x10, 0x1A1A}, {'H', 1, 0x1040}, {'a', 0, 0x12}, {'a', 3, 0x45}}}, // 'SLNM
        {0xD2, {{'L', 0, 0x1030}, {'H', 1, 0x1040}, {'a', 2, 0x31}, {'a', 3, 0x41}}},    // 'LD
        {0xE4, {{'H', 0, 0x1010}, {'L', 1, 0x1040}, {'a', 0, 0x12}, {'a', 3, 0x41}}},    // 'LDN
        {0xC9, {{'L', 0, 0x1020}, {'L', 1, 0x1040}, {'a', 1, 0x21}, {'a', 3, 0x45}}},    // 'LDM
        {0xE7, {{'H', 0, 0x1020}, {'L', 0, 0x1040}, {'a', 1, 0x23}, {'a', 3, 0x41}}},    // 'LDAXN
        {0xDB, {{'H', 1, 0x1010}, {'L', 1, 0x1040}, {'a', 0, 0x11}, {'a', 3, 0x45}}},    // 'LDAXM
        {0xFF, {{'H', 1, 0x1020}, {'L', 1, 0x1040}, {'a', 1, 0x23}, {'a', 3, 0x45}}},    // 'LDAXNM
    };
    for (const Case& c : cases) {
        Rig t;
        for (u16 a = 0; a < 0x80; ++a) t.dsp.dram()[a] = static_cast<u16>(0x1000U + a);
        t.load({static_cast<u16>(0x8000U | c.ext), HALT});
        t.dsp.reset(0);
        DspState& st = t.s();
        st.ar = {0x10, 0x20, 0x30, 0x40};
        st.ix = {2, 3, 4, 5};
        st.ac_m = {0x0A0A, 0x1A1A}; st.ac_l = {0x0B0B, 0x1B1B};
        st.ax_l = {0x2020, 0x3030}; st.ax_h = {0x2121, 0x3131};
        t.dsp.set_halted(false);
        t.dsp.run(10);
        CHECK(t.dsp.halted() && !t.dsp.failed());
        for (const Exp& e : c.exp) {
            u16 got = 0;
            switch (e.what) {
            case 'a': got = st.ar[e.idx]; break;
            case 'd': got = t.dsp.dram()[e.idx]; break;
            case 'L': got = st.ax_l[e.idx]; break;
            case 'H': got = st.ax_h[e.idx]; break;
            default: got = st.ac_m[e.idx]; break;
            }
            if (got != e.val) std::fprintf(stderr, "    ext %02X: %c%u = %04X, expected %04X\n", c.ext, e.what, e.idx, got, e.val);
            CHECK_EQ(got, e.val);
        }
    }
}


// ------------------------------------------------------------------ accelerator (M011)
namespace {
// Independent reference written from the DSP-ADPCM file-format descriptions:
// 8-byte frames, header = predictor (bits 6-4) / scale (bits 3-0), 14 nibbles,
// high nibble first, y = clamp(((n * 2^s) << 11) + 1024 + c1*y1 + c2*y2) >> 11).
std::vector<s16> reference_adpcm(const std::vector<u8>& data, const std::array<s16, 16>& coef, s16 y1, s16 y2) {
    std::vector<s16> out;
    for (std::size_t f = 0; f + 8 <= data.size(); f += 8) {
        const int scale = 1 << (data[f] & 0xF);
        const int c1 = coef[((data[f] >> 4) & 7) * 2], c2 = coef[((data[f] >> 4) & 7) * 2 + 1];
        for (int i = 0; i < 14; ++i) {
            const u8 b = data[f + 1 + static_cast<std::size_t>(i / 2)];
            int n = (i % 2 == 0) ? (b >> 4) : (b & 0xF);
            if (n >= 8) n -= 16;
            int v = ((n * scale) * 2048 + 1024 + c1 * y1 + c2 * y2) >> 11;
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            y2 = y1;
            y1 = static_cast<s16>(v);
            out.push_back(y1);
        }
    }
    return out;
}
constexpr u16 LRR_AX0L_AR0 = 0x1818, SRRI_AR1_AX0L = 0x1B38;
// Main code at 0x0060 behind a reset jump, so the exception vectors stay free.
bool run_main(Rig& t, std::initializer_list<u16> main_code, u64 budget = 10000) {
    t.load({0x029F, 0x0060}, 0x0000);
    for (u16 a = 0x0002; a < 0x0010; ++a)
        if (t.dsp.iram()[a] == 0U) t.dsp.iram()[a] = HALT; // a wrong vector stops at once
    t.load(main_code, 0x0060);
    t.dsp.reset(0);
    t.dsp.set_halted(false);
    t.dsp.run(budget);
    if (t.dsp.failed()) std::fprintf(stderr, "    DSP failed: %s\n", t.dsp.error().c_str());
    return t.dsp.halted() && !t.dsp.failed();
}
} // namespace

TEST_CASE(dsp_accelerator_adpcm_hand_values_loop_and_acsov) {
    Rig t;
    // frame 0: header 0x00 (pair 0, scale 1), nibbles 1, 2, 3, -1, ...
    t.host.aram[0] = 0x00; t.host.aram[1] = 0x12; t.host.aram[2] = 0x3F;
    t.load({0x029F, 0x0040}, 0x000A);                    // ACSOV vector -> 0x40
    t.load({LRI(AC1M), 0x00AA, 0x02FF}, 0x0040);         // handler: mark, RTI
    // main at 0x60: 14 two-word setup instructions (0x60-0x7B), BLOOPI at 0x7C, body 0x7E-0x7F
    CHECK(run_main(t, {LRI(SR_), SR_IE, SI(0xFFA0), 0x0800, SI(0xFFA1), 0x0000, SI(ifx::FORMAT), 0x0000,
                       SI(ifx::ACSAH), 0, SI(ifx::ACSAL), 2, SI(ifx::ACEAH), 0, SI(ifx::ACEAL), 5,
                       SI(ifx::ACCAH), 0, SI(ifx::ACCAL), 0, SI(ifx::YN1), 0, SI(ifx::YN2), 0,
                       LRI(AR0), ifx::ACDSAMP, LRI(1), 0x0100,
                       0x1105, 0x007F, LRR_AX0L_AR0, SRRI_AR1_AX0L, // BLOOPI 5 (end = 0x7F)
                       HALT}));
    // y = ((n << 11) + 1024 + 2048*y1) >> 11: 1, 3, 6, 5; then the loop restarts at nibble 2 -> 6
    const u16 expect[] = {1, 3, 6, 5, 6};
    for (unsigned i = 0; i < 5; ++i) CHECK_EQ(t.dsp.dram()[0x100 + i], expect[i]);
    CHECK_EQ(t.s().ac_m[1], 0x00AAU); // ACSOV handler ran once
    CHECK_EQ(t.dsp.pending_exceptions(), 0U);
    CHECK(t.s().st[0].empty());
    CHECK(t.s().st[1].empty());
}

TEST_CASE(dsp_accelerator_adpcm_matches_reference_over_frames) {
    std::array<s16, 16> coef{};
    const s16 pairs[16] = {0x0800, 0, 0x0F00, -0x0700, 0x1000, -0x0800, 0x0400, 0x0200,
                           -0x0300, 0x0100, 0x0C00, -0x0400, 0x07FF, -0x07FF, 0x0000, 0x0800};
    for (int i = 0; i < 16; ++i) coef[static_cast<std::size_t>(i)] = pairs[i];
    std::vector<u8> data(8 * 6);
    u32 x = 12345;
    for (std::size_t f = 0; f < 6; ++f) {
        data[f * 8] = static_cast<u8>((f % 8) << 4 | (f * 3 % 13)); // varied predictor / scale
        for (std::size_t i = 1; i < 8; ++i) { x = x * 1103515245U + 12345U; data[f * 8 + i] = static_cast<u8>(x >> 16); }
    }
    const auto ref = reference_adpcm(data, coef, 100, -50);
    Rig t;
    for (std::size_t i = 0; i < data.size(); ++i) t.host.aram[0x200 + i] = data[i];
    std::vector<u16> prog = {SI(ifx::FORMAT), 0x0000};
    for (unsigned i = 0; i < 16; ++i) { prog.push_back(SI(static_cast<u16>(0xFFA0U + i))); prog.push_back(static_cast<u16>(coef[i])); }
    const u16 rest[] = {SI(ifx::ACSAH), 0, SI(ifx::ACSAL), 0x0400, SI(ifx::ACEAH), 0, SI(ifx::ACEAL), 0x045F,
                        SI(ifx::ACCAH), 0, SI(ifx::ACCAL), 0x0400, SI(ifx::YN1), 100, SI(ifx::YN2), static_cast<u16>(-50),
                        LRI(AR0), ifx::ACDSAMP, LRI(1), 0x0100};
    prog.insert(prog.end(), std::begin(rest), std::end(rest));
    const u16 loop_at = static_cast<u16>(prog.size());
    prog.push_back(0x1154); // BLOOPI 84
    prog.push_back(static_cast<u16>(loop_at + 3));
    prog.push_back(LRR_AX0L_AR0);
    prog.push_back(SRRI_AR1_AX0L);
    prog.push_back(HALT);
    for (std::size_t i = 0; i < prog.size(); ++i) t.dsp.iram()[i] = prog[i];
    t.dsp.reset(0);
    t.dsp.set_halted(false);
    t.dsp.run(5000);
    CHECK(t.dsp.halted() && !t.dsp.failed());
    if (t.dsp.failed()) std::fprintf(stderr, "    %s\n", t.dsp.error().c_str());
    CHECK_EQ(ref.size(), 84U);
    unsigned bad = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) bad += t.dsp.dram()[0x100 + i] != static_cast<u16>(ref[i]) ? 1U : 0U;
    CHECK_EQ(bad, 0U);
    // the whole range (0x400-0x45F nibbles) was consumed: ACCA wrapped to ACSA, ACSOV pending (IE clear)
    CHECK_EQ(t.dsp.pending_exceptions(), 1U << 5);
}

TEST_CASE(dsp_accelerator_adpcm_clamps) {
    Rig t;
    t.host.aram[0] = 0x0F; t.host.aram[1] = 0x78; // scale 2^15, nibbles 7 and -8
    CHECK(t.run({SI(ifx::FORMAT), 0, SI(ifx::ACEAH), 0, SI(ifx::ACEAL), 0x000F, SI(ifx::ACCAH), 0, SI(ifx::ACCAL), 0,
                 LR(AX0L), ifx::ACDSAMP, LR(AX0H), ifx::ACDSAMP, LR(AX1L), ifx::PRED_SCALE, LR(AX1H), ifx::YN1,
                 LR(AC0L), ifx::YN2, LR(AC1L), ifx::ACCAL, HALT}));
    CHECK_EQ(t.s().ax_l[0], 0x7FFFU);
    CHECK_EQ(t.s().ax_h[0], 0x8000U);
    CHECK_EQ(t.s().ax_l[1], 0x000FU); // PRED_SCALE loaded from the frame header
    CHECK_EQ(t.s().ax_h[1], 0x8000U); // YN1 = last sample
    CHECK_EQ(t.s().ac_l[0], 0x7FFFU); // YN2 = previous one
    CHECK_EQ(t.s().ac_l[1], 0x0004U); // header (2 nibbles) + 2 samples
}

TEST_CASE(dsp_accelerator_raw_16bit_and_overflow) {
    Rig t;
    t.load({0x029F, 0x0050}, 0x0006); // ACRROV vector
    t.load({LRI(AC1M), 0x00BB, 0x02FF}, 0x0050);
    CHECK(run_main(t, {SI(ifx::FORMAT), 0x0002, SI(ifx::ACSAH), 0, SI(ifx::ACSAL), 0x0008,
                 SI(ifx::ACEAH), 0xFFFF, SI(ifx::ACEAL), 0xFFFF,
                 SI(ifx::ACCAH), 0x8000, SI(ifx::ACCAL), 0x0010, SI(ifx::ACDRAW), 0xBEEF, SI(ifx::ACDRAW), 0x1234,
                 LR(AX0L), ifx::ACCAL,                                      // 0x0012 after two writes
                 SI(ifx::ACEAH), 0, SI(ifx::ACEAL), 0x0010,
                 SI(ifx::ACCAH), 0, SI(ifx::ACCAL), 0x0010, LR(AX0H), ifx::ACDRAW, // reads 0xBEEF, then wraps
                 LR(AX1L), ifx::ACCAL,
                 LRI(SR_), SR_IE, NOP,                                      // the pending ACRROV is taken now
                 HALT}));
    CHECK_EQ(t.host.aram[0x20], 0xBEU);
    CHECK_EQ(t.host.aram[0x21], 0xEFU);
    CHECK_EQ(t.host.aram[0x22], 0x12U);
    CHECK_EQ(t.s().ax_l[0], 0x0012U);
    CHECK_EQ(t.s().ax_h[0], 0xBEEFU);
    CHECK_EQ(t.s().ax_l[1], 0x0008U); // 0x11 > ACEA 0x10 -> reloaded from ACSA
    CHECK_EQ(t.s().ac_m[1], 0x00BBU); // ACRROV handler
    // ARAM out of range / no ARAM is a host stop
    Rig u;
    u.host.aram.clear();
    u.load({SI(ifx::FORMAT), 0, LR(AC0M), ifx::ACDSAMP, HALT});
    u.dsp.reset(0);
    u.dsp.set_halted(false);
    u.dsp.run(10);
    CHECK(u.dsp.failed());
    CHECK(u.dsp.error().find("no ARAM") != std::string::npos);
}

TEST_CASE(dsp_accelerator_reads_the_system_aram) {
    system::System gc({});
    gc.dsp().aram()[0x1000] = 0x00;
    gc.dsp().aram()[0x1001] = 0x70; // nibble 7, scale 1, pair 0 = 0 -> (7*2048 + 1024) >> 11 = 7
    auto& core = gc.dsp().core();
    const u16 prog[] = {SI(ifx::FORMAT), 0, SI(ifx::ACEAH), 0, SI(ifx::ACEAL), 0x2FFF, SI(ifx::ACCAH), 0,
                        SI(ifx::ACCAL), 0x2000, LR(AC0M), ifx::ACDSAMP, HALT};
    for (std::size_t i = 0; i < std::size(prog); ++i) core.iram()[i] = prog[i];
    core.reset(0);
    core.set_halted(false);
    core.run(100);
    CHECK(!core.failed());
    CHECK_EQ(core.state().ac_m[0], 7U);
    system::System wii({system::Console::Wii, 0, 1});
    auto& wc = wii.dsp().core();
    for (std::size_t i = 0; i < std::size(prog); ++i) wc.iram()[i] = prog[i];
    wc.reset(0);
    wc.set_halted(false);
    wc.run(100);
    CHECK(wc.failed()); // Wii has no ARAM; what the accelerator reads there is not documented
}

// ------------------------------------------------------------------ IFX: mailboxes, DMA, IRQ
TEST_CASE(dsp_mailboxes_irq_and_dma) {
    Rig t;
    t.host.ram[0x100] = 0x12; t.host.ram[0x101] = 0x34; t.host.ram[0x102] = 0xAB; t.host.ram[0x103] = 0xCD;
    CHECK(t.run({SI(ifx::DMBH), 0x0054, SI(ifx::DMBL), 0x4348, // "TCH" mail from the SDK boot code
                 SI(ifx::DSMAH), 0x0000, SI(ifx::DSMAL), 0x0100, SI(ifx::DSPA), 0x0200, SI(ifx::DSCR), 0x0000,
                 SI(ifx::DSBL), 0x0004,                          // main -> DMEM, 4 bytes
                 SI(ifx::DSMAL), 0x0200, SI(ifx::DSCR), 0x0001, SI(ifx::DSBL), 0x0004, // DMEM -> main
                 SI(ifx::DIRQ), 0x0001, HALT}));
    CHECK_EQ(t.dsp.cpu_read_dmb_high(), 0x8054U);
    CHECK_EQ(t.dsp.cpu_read_dmb_low(), 0x4348U);
    CHECK_EQ(t.dsp.cpu_read_dmb_high(), 0x0054U); // read consumed the mail
    CHECK_EQ(t.dsp.dram()[0x200], 0x1234U);
    CHECK_EQ(t.dsp.dram()[0x201], 0xABCDU);
    CHECK_EQ(t.host.ram[0x200], 0x12U);
    CHECK_EQ(t.host.ram[0x203], 0xCDU);
    CHECK_EQ(t.host.irqs, 1);
    // CPU -> DSP mail: DSP polls CMBH bit 15, then reads CMBH and CMBL (which clears it)
    Rig u;
    u.dsp.cpu_write_cmb_high(0x1234);
    u.dsp.cpu_write_cmb_low(0x5678);
    CHECK_EQ(u.dsp.cpu_read_cmb_high(), 0x9234U);
    CHECK(u.run({LR(AC0M), 0xFFFE, LR(AC1M), 0xFFFF, LR(AX0L), 0xFFFE, HALT}));
    CHECK_EQ(u.s().ac_m[0], 0x9234U);
    CHECK_EQ(u.s().ac_m[1], 0x5678U);
    CHECK_EQ(u.s().ax_l[0], 0x1234U);
    CHECK_EQ(u.dsp.cpu_read_cmb_high(), 0x1234U);
}

TEST_CASE(dsp_external_interrupt_and_rti) {
    Rig t;
    t.load({0x029F, 0x0010}, 0x000E); // INT vector: JMP 0x10
    t.load({LRI(AC1M), 0x0042, 0x02FF}, 0x0010); // handler: LRI; RTI
    t.load({LRI(SR_), static_cast<u16>(SR_IE | SR_EIE), NOP, NOP, HALT}, 0x0000);
    t.dsp.reset(0);
    t.dsp.set_halted(false);
    t.dsp.run(3); // LRI $sr, NOP
    t.dsp.request_external_interrupt();
    t.dsp.run(100);
    CHECK(!t.dsp.failed());
    CHECK(t.dsp.halted());
    CHECK_EQ(t.s().ac_m[1], 0x0042U);
    CHECK_EQ(t.s().sr, static_cast<u16>(SR_IE | SR_EIE)); // restored by RTI
    CHECK(t.s().st[0].empty());
    CHECK(t.s().st[1].empty());
    // masked when EIE is clear: stays pending
    Rig u;
    u.load({LRI(SR_), SR_IE, NOP, HALT});
    u.dsp.reset(0); // reset clears a pending request, so request after it
    u.dsp.request_external_interrupt();
    u.dsp.set_halted(false);
    u.dsp.run(100);
    CHECK(u.dsp.halted());
    CHECK(u.dsp.external_interrupt_pending());
    CHECK(u.s().st[0].empty());
}

// ------------------------------------------------------------------ host stops
TEST_CASE(dsp_undocumented_behaviour_stops_clearly) {
    struct Case { std::initializer_list<u16> prog; const char* text; };
    const Case cases[] = {
        {{0x2A00, HALT}, "unknown opcode 2A00"},
        {{0x00A0, HALT}, "unknown opcode 00A0"},
        {{MRR(AX0L, ST0), HALT}, "$st0"},
        {{LRI(ST0), 1, HALT}, "$st0"},
        {{LRI(WR0), 0x00FF, 0x0008, HALT}, "wrapping"},
        {{0x029F, 0x8000}, "IROM"},
        {{LR(AC0M), 0x1000, HALT}, "COEF"},
        {{SR(AC0M), 0x1000, HALT}, "read-only"},
        {{LR(AC0M), 0x2000, HALT}, "data memory address 2000"},
        {{SI(ifx::FORMAT), 0x000A, LR(AC0M), ifx::ACDSAMP, HALT}, "only ADPCM"},
        {{SI(ifx::FORMAT), 0x0001, LR(AC0M), ifx::ACDRAW, HALT}, "only 16-bit"},
        {{SI(ifx::ACCAH), 0x0000, SI(ifx::ACDRAW), 0x1234, HALT}, "bit 31 clear"},
        {{LR(AC0M), 0xFF00, HALT}, "IFX register FF00"},
        {{0x02DF}, "underflow"},
        {{0x1002, 0x0080, 0x0001, HALT}, "2-word"},
        {{SI(ifx::DSBL), 0x0003, HALT}, "even"},
    };
    for (const Case& c : cases) {
        Rig t;
        t.load(c.prog);
        t.dsp.reset(0);
        t.dsp.set_halted(false);
        t.dsp.run(1000);
        CHECK(t.dsp.failed());
        if (t.dsp.error().find(c.text) == std::string::npos) std::fprintf(stderr, "    got: %s\n", t.dsp.error().c_str());
        CHECK(t.dsp.error().find(c.text) != std::string::npos);
    }
    // call stack holds 8 entries; the 9th push overflows
    Rig o;
    o.load({0x02BF, 0x0000});
    o.dsp.reset(0);
    o.dsp.set_halted(false);
    o.dsp.run(100);
    CHECK(o.dsp.failed());
    CHECK(o.dsp.error().find("$st0 overflow") != std::string::npos);
    CHECK_EQ(o.s().st[0].n, 8U);
    // with dumps loaded, IROM and COEF become readable
    Rig r;
    std::vector<u16> irom(kIromWords, 0x0021), coef(kCoefWords, 0x4444);
    CHECK(!r.dsp.load_irom(std::vector<u16>(10)));
    CHECK(r.dsp.load_irom(irom));
    CHECK(r.dsp.load_coef(coef));
    CHECK(r.run({LR(AC0M), 0x1000, 0x029F, 0x8000}));
    CHECK_EQ(r.s().ac_m[0], 0x4444U);
    CHECK_EQ(r.s().pc, 0x8001U);
}

// ------------------------------------------------------------------ CPU-side integration
TEST_CASE(dsp_interface_boot_mailbox_round_trip) {
    system::System sys({});
    const u16 ucode[] = {0x16FC, 0xDCD1, 0x16FD, 0x0000, 0x00DE, 0xFFFE, 0x02C0, 0x8000, 0x029C, 0x0004,
                         0x00DE, 0xFFFE, 0x00DF, 0xFFFF, 0x0240, 0x7FFF, 0x0501, 0x00FE, 0xFFFC, 0x00FF,
                         0xFFFD, 0x16FB, 0x0001, 0x0021};
    for (std::size_t i = 0; i < std::size(ucode); ++i) CHECK(sys.bus().write16(0x01000000U + 2U * static_cast<u32>(i), ucode[i]) == mem::BusStatus::Ok);
    auto& d = sys.dsp();
    CHECK(d.mmio_write(hw::dsp::CSR, 2, 0x08AD)); // SDK value: RES | HALT | acks | bit 11
    CHECK_EQ(d.core().iram()[0], 0x16FCU);
    CHECK(d.core().halted());
    CHECK(d.mmio_write(hw::dsp::CSR, 2, 0x0000)); // release HALT
    sys.scheduler().advance_to(sys.scheduler().now() + 20000);
    u64 v = 0;
    CHECK(d.mmio_read(hw::dsp::CPUMBH, 4, v));
    CHECK_EQ(v, 0xDCD10000U);
    CHECK(d.mmio_write(hw::dsp::DSPMBH, 4, 0x92345678U));
    sys.scheduler().advance_to(sys.scheduler().now() + 20000);
    CHECK(d.mmio_read(hw::dsp::CPUMBH, 4, v));
    CHECK_EQ(v, 0x92345679U);
    CHECK_EQ(d.csr() & hw::dsp::CSR_DSPINT, hw::dsp::CSR_DSPINT);
    CHECK_EQ(d.csr() & hw::dsp::CSR_HALT, hw::dsp::CSR_HALT); // HALT instruction sets it
    CHECK(!d.dsp_failed());
}

TEST_CASE(dsp_failure_stops_the_system_run) {
    system::System sys({});
    CHECK(sys.bus().write16(0x01000000U, 0x2A00) == mem::BusStatus::Ok); // unknown opcode
    // CPU: an infinite loop at 0x80003000 (b .)
    CHECK(sys.bus().write32(0x3000, 0x48000000U) == mem::BusStatus::Ok);
    sys.hle_boot(0x80003000U);
    CHECK(sys.dsp().mmio_write(hw::dsp::CSR, 2, 0x0801));
    const auto r = sys.run(100000);
    CHECK(r.device_stop);
    CHECK(r.device_error.find("unknown opcode 2A00") != std::string::npos);
    CHECK(r.cycles < 100000U);
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(dsp_suite_program_boots_a_ucode_and_exchanges_mail) {
    std::ifstream in(ORCA_PROGRAMS_DIR "/dsp_suite.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(50000000ULL);
    if (r.device_stop) std::fprintf(stderr, "    device stop: %s\n", r.device_error.c_str());
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    dsp_suite stopped: %s (%s) pc=%08X\n", ppc::to_string(r.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc);
    CHECK(r.stop == ppc::StepResult::Halted);
    CHECK_EQ(port.exit_code, 0U);
    const auto& L = port.log;
    for (u32 v : L) std::printf("    dsp_suite value %08X\n", v);
    CHECK_EQ(L.size(), 6U);
    if (L.size() != 6U) return;
    CHECK_EQ(L[0], 0x00005CD1U); // DMBH carries bits 30-16 only (bit 15 is the pending flag)
    CHECK_EQ(L[1], 0x00000000U);
    CHECK_EQ(L[2], 0x00001234U);
    CHECK_EQ(L[3], 0x00005679U);
    CHECK_EQ(L[4], 0x00000080U); // DSPINT
    CHECK_EQ(L[5], 0x00000004U); // HALT set by the HALT instruction
}
#endif

TEST_MAIN("dsp_core")
