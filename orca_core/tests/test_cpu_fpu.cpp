// CPU-level floating point, paired single, quantized load/store and locked cache.
#include "fp_reference.hpp"
#include "machine.hpp"
#include "orca/ppc/fpu.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace orca;
using namespace orca::ppc;
using namespace orca::test;
namespace A = orca::test::ppc_asm;
namespace F = orca::ppc::fpu;

namespace {
u64 d(double v) { u64 b; std::memcpy(&b, &v, 8); return b; }
double v(u64 b) { double x; std::memcpy(&x, &b, 8); return x; }
u64 sgl(float f) { return d(static_cast<double>(f)); }

struct FpMachine : Machine {
    FpMachine() { s().msr = MSR_FP; s().hid2 = HID2_PSE | HID2_LSQE; }
};
} // namespace

// ------------------------------------------------------------ availability
TEST_CASE(fp_unavailable_for_every_fp_class) {
    const u32 cases[] = {A::lfd(1, 0, 3), A::stfs(1, 0, 3), A::lfsx(1, 3, 4), A::fadd(1, 2, 3), A::fadds(1, 2, 3),
                         A::mffs(1), A::mtfsfi(0, 1), A::fcmpu(0, 1, 2), A::ps_add(1, 2, 3), A::psq_l(1, 0, 3, 0, 0)};
    for (u32 ins : cases) {
        Machine m;
        m.s().hid2 = HID2_PSE | HID2_LSQE;
        CHECK(m.exec(ins) == StepResult::ExceptionTaken);
        CHECK(m.cpu->last_exception() == Exception::FloatingPointUnavailable);
        CHECK_EQ(m.s().srr0, 0x1000U);
    }
}

TEST_CASE(paired_single_and_quantized_gating) {
    FpMachine m;
    m.s().hid2 = 0;
    CHECK(m.exec(A::ps_add(1, 2, 3)) == StepResult::ExceptionTaken);
    CHECK_EQ(m.s().srr1 & SRR1_PROGRAM_ILLEGAL, SRR1_PROGRAM_ILLEGAL);
    FpMachine q;
    q.s().hid2 = HID2_PSE; // LSQE clear
    CHECK(q.exec(A::psq_l(1, 0, 3, 0, 0)) == StepResult::ExceptionTaken);
    CHECK(q.cpu->last_exception() == Exception::Program);
    q.s().pc = 0x1000; q.s().msr = MSR_FP;
    CHECK(q.exec(A::dcbz_l(0, 3)) == StepResult::ExceptionTaken); // HID2[LCE] clear
    CHECK(q.cpu->last_exception() == Exception::Program);
}

// ------------------------------------------------------------ loads / stores
TEST_CASE(fp_loads_and_stores) {
    FpMachine m;
    m.bus.write32(0x2000, 0x40490FDBU);           // pi as single
    m.bus.write64(0x2008, d(2.718281828459045));
    m.s().gpr[3] = 0x2000; m.s().gpr[4] = 8;
    m.s().fpr[2].ps1 = d(-7.0);
    m.code(0x1000, {A::lfs(1, 0, 3), A::lfd(2, 8, 3), A::stfs(2, 0x10, 3), A::stfd(1, 0x18, 3), A::lfdux(5, 3, 4),
                    A::stfsu(1, 0x20, 3)});
    CHECK(m.cpu->run(6) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, sgl(3.14159274f));
    CHECK_EQ(m.s().fpr[1].ps1, sgl(3.14159274f)); // lfs fills both slots
    CHECK_EQ(m.s().fpr[2].ps0, d(2.718281828459045));
    CHECK_EQ(m.s().fpr[2].ps1, d(-7.0));          // lfd leaves ps1 alone
    CHECK_EQ(m.r32(0x2010), 0x402DF854U);         // stfs truncates e to single
    CHECK_EQ(m.r32(0x2018), 0x400921FBU);
    CHECK_EQ(m.s().fpr[5].ps0, d(2.718281828459045));
    CHECK_EQ(m.s().gpr[3], 0x2028U);              // 0x2000 + 8 (lfdux) + 0x20 (stfsu)
    CHECK_EQ(m.r32(0x2028), 0x40490FDBU);
}

TEST_CASE(fp_load_store_invalid_and_alignment) {
    FpMachine m;
    m.s().gpr[3] = 0x2002;
    const CpuState before = m.s();
    CHECK(m.exec(A::lfsu(1, 0, 0)) == StepResult::InvalidForm);
    CHECK(m.s() == before);
    CHECK(m.exec(A::lfd(1, 0, 3)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.s().dar, 0x2002U);
}

TEST_CASE(fctiwz_then_stfiwx) {
    FpMachine m;
    m.s().fpr[1].ps0 = d(-2.7);
    m.s().gpr[3] = 0x2000; m.s().gpr[0] = 0;
    m.code(0x1000, {A::fctiwz(2, 1), A::stfiwx(2, 0, 3)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.r32(0x2000), static_cast<u32>(-2));
    CHECK_EQ(m.s().fpr[2].ps0 >> 32, 0xFFF80000ULL); // documented choice for the undefined high word
    CHECK((m.s().fpscr & (F::XX | F::FI | F::FX)) == (F::XX | F::FI | F::FX));
    CHECK((m.s().fpscr & F::FR) == 0U); // truncation toward zero: magnitude decreased
}

// ------------------------------------------------------------ arithmetic and FPSCR
TEST_CASE(single_ops_round_and_fill_both_slots) {
    FpMachine m;
    m.s().fpr[2].ps0 = d(1.0); m.s().fpr[3].ps0 = d(0x1p-30);
    CHECK(m.exec(A::fadds(1, 2, 3, true)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(1.0));           // 1 + 2^-30 rounds to 1 in single
    CHECK_EQ(m.s().fpr[1].ps1, d(1.0));
    CHECK_EQ(m.s().fpscr & (F::FX | F::XX | F::FI | F::FR), F::FX | F::XX | F::FI);
    CHECK_EQ((m.s().fpscr & F::FPRF_MASK) >> 12, F::CLASS_POS_NORM);
    CHECK_EQ((m.s().cr >> 24) & 0xFU, 0x8U);      // CR1 = FX FEX VX OX
    m.s().pc = 0x1000; m.s().fpr[1].ps1 = d(42.0);
    CHECK(m.exec(A::fadd(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(1.0 + 0x1p-30)); // exact in double
    CHECK_EQ(m.s().fpr[1].ps1, d(42.0));          // double ops leave ps1 alone
}

TEST_CASE(fx_only_on_new_exceptions_and_sticky_bits) {
    FpMachine m;
    m.s().fpr[2].ps0 = d(1.0); m.s().fpr[3].ps0 = d(3.0);
    CHECK(m.exec(A::fdiv(1, 2, 3)) == StepResult::Executed);
    CHECK((m.s().fpscr & (F::FX | F::XX)) == (F::FX | F::XX));
    m.s().fpscr &= ~F::FX; // software clears FX; XX stays set
    m.s().pc = 0x1000;
    CHECK(m.exec(A::fdiv(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpscr & F::FX, 0U);            // XX was already set: no new exception
    m.s().pc = 0x1000; m.s().fpr[3].ps0 = d(2.0);
    CHECK(m.exec(A::fdiv(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpscr & (F::XX | F::FI), F::XX); // exact: FI cleared, XX sticky
}

TEST_CASE(rounding_modes_from_fpscr) {
    FpMachine m;
    m.s().fpr[2].ps0 = d(1.0); m.s().fpr[3].ps0 = d(3.0);
    m.code(0x1000, {A::fdiv(1, 2, 3), A::mtfsfi(7, 1), A::fdiv(4, 2, 3), A::mtfsfi(7, 2), A::fdiv(5, 2, 3)});
    CHECK(m.cpu->run(5) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, 0x3FD5555555555555ULL); // nearest
    CHECK_EQ(m.s().fpr[4].ps0, 0x3FD5555555555555ULL); // toward zero (same: nearest rounds down here)
    CHECK_EQ(m.s().fpr[5].ps0, 0x3FD5555555555556ULL); // toward +inf
    CHECK((m.s().fpscr & F::FR) != 0U);
}

TEST_CASE(invalid_operation_enabled_and_disabled) {
    FpMachine m;
    m.s().fpr[2].ps0 = d(INFINITY); m.s().fpr[3].ps0 = d(INFINITY); m.s().fpr[1].ps0 = d(5.0);
    CHECK(m.exec(A::fsub(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, F::kDefaultQNaN);
    CHECK((m.s().fpscr & (F::VXISI | F::VX | F::FX)) == (F::VXISI | F::VX | F::FX));
    CHECK_EQ(m.s().fpscr & F::FEX, 0U);
    FpMachine e; // VE = 1: target unchanged, FEX set
    e.s().fpscr = F::VE;
    e.s().fpr[2].ps0 = d(INFINITY); e.s().fpr[3].ps0 = d(INFINITY); e.s().fpr[1].ps0 = d(5.0);
    CHECK(e.exec(A::fsub(1, 2, 3, true)) == StepResult::Executed);
    CHECK_EQ(e.s().fpr[1].ps0, d(5.0));
    CHECK((e.s().fpscr & (F::FEX | F::VX)) == (F::FEX | F::VX));
    CHECK_EQ((e.s().cr >> 24) & 0xFU, 0xEU); // FX FEX VX
    FpMachine p; // ...and with MSR[FE0] the precise program exception is not implemented: clean stop
    p.s().fpscr = F::VE; p.s().msr = MSR_FP | MSR_FE0;
    p.s().fpr[2].ps0 = d(INFINITY); p.s().fpr[3].ps0 = d(INFINITY);
    const CpuState before = p.s();
    CHECK(p.exec(A::fsub(1, 2, 3)) == StepResult::UnsupportedInstruction);
    CHECK(p.s() == before);
}

TEST_CASE(zero_divide_and_overflow_enables) {
    FpMachine m;
    m.s().fpscr = F::ZE;
    m.s().fpr[2].ps0 = d(1.0); m.s().fpr[3].ps0 = d(0.0); m.s().fpr[1].ps0 = d(9.0);
    CHECK(m.exec(A::fdiv(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(9.0));
    CHECK((m.s().fpscr & (F::ZX | F::FEX)) == (F::ZX | F::FEX));
    FpMachine o;
    o.s().fpscr = F::OE;
    o.s().fpr[2].ps0 = d(1e300); o.s().fpr[3].ps0 = d(1e300);
    const CpuState before = o.s();
    CHECK(o.exec(A::fmul(1, 2, 3)) == StepResult::UnsupportedInstruction); // scaled result not implemented
    CHECK(o.s() == before);
    FpMachine n;
    n.s().fpr[2].ps0 = d(1e300); n.s().fpr[3].ps0 = d(1e300);
    CHECK(n.exec(A::fmul(1, 2, 3)) == StepResult::Executed);
    CHECK_EQ(n.s().fpr[1].ps0, d(INFINITY));
    CHECK((n.s().fpscr & (F::OX | F::XX)) == (F::OX | F::XX));
}

TEST_CASE(compares_set_cr_and_fpcc) {
    FpMachine m;
    m.s().fpr[1].ps0 = d(1.0); m.s().fpr[2].ps0 = d(2.0);
    m.s().fpr[3].ps0 = F::kDefaultQNaN; m.s().fpr[4].ps0 = 0x7FF0000000000001ULL; // SNaN
    m.code(0x1000, {A::fcmpu(3, 1, 2), A::fcmpu(4, 2, 1), A::fcmpu(5, 1, 3), A::fcmpo(6, 1, 3)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ((m.s().cr >> 16) & 0xFU, 8U);
    CHECK_EQ((m.s().cr >> 12) & 0xFU, 4U);
    CHECK_EQ((m.s().cr >> 8) & 0xFU, 1U);
    CHECK_EQ((m.s().cr >> 4) & 0xFU, 1U);
    CHECK_EQ((m.s().fpscr >> 12) & 0xFU, 1U);
    CHECK((m.s().fpscr & F::VXVC) != 0U);         // fcmpo with QNaN
    CHECK((m.s().fpscr & F::VXSNAN) == 0U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::fcmpu(0, 1, 4)) == StepResult::Executed);
    CHECK((m.s().fpscr & F::VXSNAN) != 0U);
}

TEST_CASE(fcmpu_with_qnan_sets_no_exception) {
    FpMachine m;
    m.s().fpr[1].ps0 = d(1.0); m.s().fpr[3].ps0 = F::kDefaultQNaN;
    CHECK(m.exec(A::fcmpu(2, 1, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpscr & (F::VXVC | F::VXSNAN | F::FX | F::VX), 0U); // unordered compare of a QNaN is quiet
    CHECK_EQ((m.s().fpscr >> 12) & 0xFU, 1U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::ps_cmpo0(2, 1, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpscr & F::VXVC, F::VXVC);
}

TEST_CASE(fpscr_moves) {
    FpMachine m;
    m.s().fpr[2].ps0 = 0xFFFFFFFFU; // all ones in the low word
    m.code(0x1000, {A::mtfsf(0x80, 2), A::mffs(3)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    // field 0 from source = FX FEX VX OX; FEX and VX are recomputed (no VX* / enables set) -> FX OX
    CHECK_EQ(m.s().fpscr, F::FX | F::OX);
    CHECK_EQ(m.s().fpr[3].ps0 & 0xFFFFFFFFULL, static_cast<u64>(F::FX | F::OX));
    m.s().pc = 0x1000;
    m.code(0x1000, {A::mtfsb1(7), A::mtfsb1(1), A::mtfsb0(0), A::mcrfs(2, 0)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    // mtfsb1 7 (VXSNAN) sets VXSNAN, FX and the VX summary; mtfsb1 1 (FEX) is ignored;
    // mtfsb0 0 clears FX; mcrfs 2,0 copies field 0 (FX=0 FEX=0 VX=1 OX=1 -> 0x3) and
    // clears that field's exception bits (FX, OX). VX stays: it summarises VXSNAN.
    CHECK_EQ((m.s().cr >> 20) & 0xFU, 0x3U);
    CHECK_EQ(m.s().fpscr & (F::OX | F::FX), 0U);
    CHECK((m.s().fpscr & (F::VXSNAN | F::VX)) == (F::VXSNAN | F::VX)); // VXSNAN is in field 1: untouched
    m.s().pc = 0x1000;
    CHECK(m.exec(A::mtfsfi(7, 3)) == StepResult::Executed);
    CHECK_EQ(m.s().fpscr & F::RN_MASK, 3U);
}

TEST_CASE(moves_select_and_frsp) {
    FpMachine m;
    const u64 snan = 0x7FF0000000000123ULL;
    m.s().fpr[2].ps0 = snan; m.s().fpr[1].ps1 = d(11.0);
    m.s().fpr[4].ps0 = d(-0.0); m.s().fpr[5].ps0 = d(5.0); m.s().fpr[6].ps0 = d(6.0);
    m.s().fpr[7].ps0 = d(1.0 + 0x1p-40);
    m.code(0x1000, {A::fmr(1, 2), A::fneg(3, 2), A::fsel(8, 4, 5, 6), A::fsel(9, 2, 5, 6), A::frsp(10, 7)});
    const u32 fpscr0 = m.s().fpscr;
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, snan);             // bit-exact move, no quieting
    CHECK_EQ(m.s().fpr[1].ps1, d(11.0));
    CHECK_EQ(m.s().fpr[3].ps0, snan | F::kSignBit);
    CHECK_EQ(m.s().fpr[8].ps0, d(5.0));           // -0 >= 0
    CHECK_EQ(m.s().fpr[9].ps0, d(6.0));           // NaN selects frB
    CHECK_EQ(m.s().fpscr, fpscr0);                // no FPSCR effects
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().fpr[10].ps0, d(1.0));
    CHECK_EQ(m.s().fpr[10].ps1, d(1.0));
}

TEST_CASE(fused_multiply_add_single_rounding) {
    FpMachine m;
    // a*c = 1 - 2^-54 exactly; + 2^-54 -> exactly 1 only if fused (separately rounded it would be 1 + 2^-54 -> 1? check frB)
    m.s().fpr[2].ps0 = d(1.0 + 0x1p-27); m.s().fpr[3].ps0 = d(1.0 - 0x1p-27); m.s().fpr[4].ps0 = d(-1.0);
    CHECK(m.exec(A::fmadd(1, 2, 3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(-0x1p-54)); // (1 - 2^-54) - 1, unrounded product
    m.s().pc = 0x1000;
    CHECK(m.exec(A::fnmsub(5, 2, 3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[5].ps0, d(-(2.0 - 0x1p-54))); // -(a*c - b) rounded to nearest
    m.s().pc = 0x1000;
    CHECK(m.exec(A::fsqrt(1, 2)) == StepResult::UnsupportedInstruction); // not in the 750CL
}

// ------------------------------------------------------------ paired singles
TEST_CASE(paired_single_arithmetic) {
    FpMachine m;
    auto set = [&](unsigned r, float a, float b) { m.s().fpr[r].ps0 = sgl(a); m.s().fpr[r].ps1 = sgl(b); };
    set(1, 1.5f, -2.0f); set(2, 4.0f, 0.25f); set(3, 10.0f, 100.0f);
    m.code(0x1000, {A::ps_add(4, 1, 2), A::ps_mul(5, 1, 2), A::ps_madd(6, 1, 2, 3), A::ps_sub(7, 1, 2),
                    A::ps_div(8, 2, 1), A::ps_nmsub(9, 1, 2, 3)});
    CHECK(m.cpu->run(6) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[4].ps0, sgl(5.5f)); CHECK_EQ(m.s().fpr[4].ps1, sgl(-1.75f));
    CHECK_EQ(m.s().fpr[5].ps0, sgl(6.0f)); CHECK_EQ(m.s().fpr[5].ps1, sgl(-0.5f));
    CHECK_EQ(m.s().fpr[6].ps0, sgl(16.0f)); CHECK_EQ(m.s().fpr[6].ps1, sgl(99.5f));
    CHECK_EQ(m.s().fpr[7].ps0, sgl(-2.5f)); CHECK_EQ(m.s().fpr[7].ps1, sgl(-2.25f));
    CHECK_EQ(m.s().fpr[8].ps0, sgl(4.0f / 1.5f)); CHECK_EQ(m.s().fpr[8].ps1, sgl(-0.125f));
    CHECK_EQ(m.s().fpr[9].ps0, sgl(4.0f)); CHECK_EQ(m.s().fpr[9].ps1, sgl(100.5f)); // -(a*c - b)
}

TEST_CASE(paired_single_cross_slot_forms) {
    FpMachine m;
    auto set = [&](unsigned r, float a, float b) { m.s().fpr[r].ps0 = sgl(a); m.s().fpr[r].ps1 = sgl(b); };
    set(1, 1.0f, 2.0f); set(2, 3.0f, 4.0f); set(3, 5.0f, 6.0f);
    m.code(0x1000, {A::ps_sum0(4, 1, 3, 2), A::ps_sum1(5, 1, 3, 2), A::ps_muls0(6, 1, 3), A::ps_muls1(7, 1, 3),
                    A::ps_madds0(8, 1, 3, 2), A::ps_madds1(9, 1, 3, 2), A::ps_merge00(10, 1, 2), A::ps_merge01(11, 1, 2),
                    A::ps_merge10(12, 1, 2), A::ps_merge11(13, 1, 2)});
    CHECK(m.cpu->run(10) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[4].ps0, sgl(5.0f)); CHECK_EQ(m.s().fpr[4].ps1, sgl(6.0f));   // A0+B1, C1
    CHECK_EQ(m.s().fpr[5].ps0, sgl(5.0f)); CHECK_EQ(m.s().fpr[5].ps1, sgl(5.0f));   // C0, A0+B1
    CHECK_EQ(m.s().fpr[6].ps0, sgl(5.0f)); CHECK_EQ(m.s().fpr[6].ps1, sgl(10.0f));  // A*C0
    CHECK_EQ(m.s().fpr[7].ps0, sgl(6.0f)); CHECK_EQ(m.s().fpr[7].ps1, sgl(12.0f));  // A*C1
    CHECK_EQ(m.s().fpr[8].ps0, sgl(8.0f)); CHECK_EQ(m.s().fpr[8].ps1, sgl(14.0f));  // A*C0 + B
    CHECK_EQ(m.s().fpr[9].ps0, sgl(9.0f)); CHECK_EQ(m.s().fpr[9].ps1, sgl(16.0f));  // A*C1 + B
    CHECK_EQ(m.s().fpr[10].ps0, sgl(1.0f)); CHECK_EQ(m.s().fpr[10].ps1, sgl(3.0f));
    CHECK_EQ(m.s().fpr[11].ps0, sgl(1.0f)); CHECK_EQ(m.s().fpr[11].ps1, sgl(4.0f));
    CHECK_EQ(m.s().fpr[12].ps0, sgl(2.0f)); CHECK_EQ(m.s().fpr[12].ps1, sgl(3.0f));
    CHECK_EQ(m.s().fpr[13].ps0, sgl(2.0f)); CHECK_EQ(m.s().fpr[13].ps1, sgl(4.0f));
}

TEST_CASE(paired_single_rounds_each_slot_to_single) {
    FpMachine m;
    m.s().fpr[1].ps0 = sgl(1.0f); m.s().fpr[1].ps1 = sgl(16777216.0f);
    m.s().fpr[2].ps0 = sgl(0x1p-30f); m.s().fpr[2].ps1 = sgl(1.0f);
    CHECK(m.exec(A::ps_add(3, 1, 2, true)) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[3].ps0, sgl(1.0f));
    CHECK_EQ(m.s().fpr[3].ps1, sgl(16777216.0f)); // 2^24 + 1 is not representable: ties to even
    CHECK((m.s().fpscr & (F::XX | F::FI)) == (F::XX | F::FI));
    CHECK_EQ((m.s().cr >> 24) & 0xFU, 0x8U);
}

TEST_CASE(paired_single_moves_compares_select) {
    FpMachine m;
    m.s().fpr[1].ps0 = sgl(-1.0f); m.s().fpr[1].ps1 = sgl(2.0f);
    m.s().fpr[2].ps0 = sgl(-1.0f); m.s().fpr[2].ps1 = sgl(3.0f);
    m.s().fpr[3].ps0 = sgl(7.0f); m.s().fpr[3].ps1 = sgl(8.0f);
    m.code(0x1000, {A::ps_neg(4, 1), A::ps_abs(5, 1), A::ps_nabs(6, 1), A::ps_mr(7, 1), A::ps_cmpu0(1, 1, 2),
                    A::ps_cmpo1(2, 1, 2), A::ps_sel(8, 1, 3, 2)});
    CHECK(m.cpu->run(7) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[4].ps0, sgl(1.0f)); CHECK_EQ(m.s().fpr[4].ps1, sgl(-2.0f));
    CHECK_EQ(m.s().fpr[5].ps0, sgl(1.0f)); CHECK_EQ(m.s().fpr[5].ps1, sgl(2.0f));
    CHECK_EQ(m.s().fpr[6].ps0, sgl(-1.0f)); CHECK_EQ(m.s().fpr[6].ps1, sgl(-2.0f));
    CHECK_EQ(m.s().fpr[7].ps1, sgl(2.0f));
    CHECK_EQ((m.s().cr >> 24) & 0xFU, 2U);  // cr1: slot 0 equal
    CHECK_EQ((m.s().cr >> 20) & 0xFU, 8U);  // cr2: slot 1: 2 < 3
    CHECK_EQ(m.s().fpr[8].ps0, sgl(-1.0f)); // A0 < 0 -> B0
    CHECK_EQ(m.s().fpr[8].ps1, sgl(8.0f));  // A1 >= 0 -> C1
}

TEST_CASE(paired_single_estimates) {
    FpMachine m;
    m.s().fpr[1].ps0 = sgl(4.0f); m.s().fpr[1].ps1 = sgl(0.25f);
    m.code(0x1000, {A::ps_res(2, 1), A::ps_rsqrte(3, 1)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[2].ps0, sgl(0.25f)); CHECK_EQ(m.s().fpr[2].ps1, sgl(4.0f));
    CHECK_EQ(m.s().fpr[3].ps0, sgl(0.5f)); CHECK_EQ(m.s().fpr[3].ps1, sgl(2.0f));
}

// ------------------------------------------------------------ quantized load/store
TEST_CASE(psq_float_pair_and_single) {
    FpMachine m;
    m.bus.write32(0x2000, 0x3F800000U); m.bus.write32(0x2004, 0xC0000000U);
    m.s().gpr[3] = 0x2000; m.s().gqr[0] = 0;
    m.code(0x1000, {A::psq_l(1, 0, 3, 0, 0), A::psq_l(2, 4, 3, 1, 0), A::psq_st(1, 0x10, 3, 0, 0)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(1.0)); CHECK_EQ(m.s().fpr[1].ps1, d(-2.0));
    CHECK_EQ(m.s().fpr[2].ps0, d(-2.0)); CHECK_EQ(m.s().fpr[2].ps1, d(1.0)); // W=1: ps1 = 1.0
    CHECK_EQ(m.r32(0x2010), 0x3F800000U); CHECK_EQ(m.r32(0x2014), 0xC0000000U);
}

TEST_CASE(psq_integer_types_and_scales) {
    FpMachine m;
    m.s().gpr[3] = 0x2000;
    // GQR1: load u8 scale 2 (x/4); store s16 scale -1 (x*2^-1)
    m.s().gqr[1] = (2U << 24) | (4U << 16) | (0x3FU << 8) | 7U;
    m.bus.write8(0x2000, 200); m.bus.write8(0x2001, 6);
    m.code(0x1000, {A::psq_l(1, 0, 3, 0, 1), A::psq_st(1, 0x10, 3, 0, 1)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(50.0)); CHECK_EQ(m.s().fpr[1].ps1, d(1.5));
    CHECK_EQ(m.r16(0x2010), 25U);        // 50 * 2^-1
    CHECK_EQ(m.r16(0x2012), 0U);         // 1.5 * 2^-1 = 0.75 truncates to 0
    // s8 load with negative scale (x * 2^3), s16 load
    m.s().pc = 0x1000;
    m.s().gqr[2] = (0x3DU << 24) | (6U << 16);   // scale -3, type s8
    m.bus.write8(0x2020, 0xFE);                  // -2
    m.code(0x1000, {A::psq_l(2, 0x20, 3, 1, 2)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().fpr[2].ps0, d(-16.0));
    m.s().pc = 0x1000; m.s().gqr[3] = (7U << 16); m.bus.write16(0x2030, 0x8000);
    m.code(0x1000, {A::psq_l(4, 0x30, 3, 1, 3)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().fpr[4].ps0, d(-32768.0));
}

TEST_CASE(psq_store_saturation_truncation_nan) {
    FpMachine m;
    m.s().gpr[3] = 0x2000;
    m.s().gqr[1] = 4U;            // store u8, scale 0
    m.s().gqr[2] = 6U;            // store s8
    m.s().fpr[1].ps0 = sgl(300.0f); m.s().fpr[1].ps1 = sgl(-5.0f);
    m.s().fpr[2].ps0 = sgl(-200.0f); m.s().fpr[2].ps1 = sgl(-2.7f);
    m.s().fpr[3].ps0 = F::kDefaultQNaN; m.s().fpr[3].ps1 = sgl(2.7f);
    m.code(0x1000, {A::psq_st(1, 0, 3, 0, 1), A::psq_st(2, 2, 3, 0, 2), A::psq_st(3, 4, 3, 0, 1)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.r8(0x2000), 255U); CHECK_EQ(m.r8(0x2001), 0U);
    CHECK_EQ(m.r8(0x2002), 0x80U); CHECK_EQ(m.r8(0x2003), 0xFEU); // -128, -2 (toward zero)
    CHECK_EQ(m.r8(0x2004), 0U); CHECK_EQ(m.r8(0x2005), 2U);
}

TEST_CASE(psq_update_indexed_and_undefined_type) {
    FpMachine m;
    m.bus.write32(0x2008, 0x40400000U); m.bus.write32(0x200C, 0x40800000U);
    m.s().gpr[3] = 0x2000; m.s().gpr[4] = 8; m.s().gqr[0] = 0;
    m.code(0x1000, {A::psq_lux(1, 3, 4, 0, 0), A::psq_stu(1, 0x10, 3, 0, 0), A::psq_stx(1, 0, 4, 1, 0)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().fpr[1].ps0, d(3.0)); CHECK_EQ(m.s().fpr[1].ps1, d(4.0));
    CHECK_EQ(m.s().gpr[3], 0x2018U);
    CHECK_EQ(m.r32(0x2018), 0x40400000U);
    CHECK_EQ(m.r32(0x0008), 0x40400000U); // psq_stx with RA=0: EA = r4, W=1 stores one value
    m.s().pc = 0x1000; m.s().gqr[5] = 2U << 16; // load type 2: undefined
    const CpuState before = m.s();
    CHECK(m.exec(A::psq_l(1, 0, 3, 0, 5)) == StepResult::UnsupportedInstruction);
    CHECK(m.s() == before);
    CHECK(m.exec(A::psq_lu(1, 0, 0, 0, 0)) == StepResult::InvalidForm);
}

// ------------------------------------------------------------ locked cache
TEST_CASE(locked_cache_dcbi_invalidates_matching_line) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    m.s().dbat[3] = {0xE0000002U, 0xE0000002U};
    m.s().msr |= MSR_DR;
    m.s().gpr[3] = 0xE0000000U;
    m.s().gpr[4] = 0;
    CHECK(m.exec(A::dcbz_l(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 1U);
    // dcbi r3,r4: primary opcode 31, XO 470.
    const u32 dcbi = (31U << 26U) | (3U << 16U) | (4U << 11U) | (470U << 1U);
    CHECK(m.exec(dcbi) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 0U);
    std::array<u8, 32> line{};
    CHECK(!m.cpu->locked_cache_line(0xE0000000U, line));
    // A miss must be harmless and must not allocate a locked-cache line.
    CHECK(m.exec(dcbi) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 0U);
}

TEST_CASE(locked_cache_dcbz_l_and_access) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    m.s().dbat[3] = {0xE0000002U, 0xE0000002U}; // 128 KiB block EA 0xE0000000 -> PA 0xE0000000 (no RAM there)
    m.s().msr |= MSR_DR;
    m.s().gpr[3] = 0xE0000000U; m.s().gpr[4] = 0x20; m.s().gpr[5] = 0x12345678U;
    m.code(0x1000, {A::dcbz_l(3, 4), A::stw(5, 0x24, 3), A::lwz(6, 0x24, 3), A::lwz(7, 0x20, 3)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 1U);
    CHECK_EQ(m.s().gpr[6], 0x12345678U);
    CHECK_EQ(m.s().gpr[7], 0U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lwz(8, 0x40, 3)) == StepResult::MemoryFault); // not allocated, no RAM behind it
}


TEST_CASE(locked_cache_writes_invalidate_reservations) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    m.s().dbat[3] = {0xE0000002U, 0xE0000002U};
    m.s().msr |= MSR_DR;
    m.s().gpr[3] = 0xE0000000U;
    m.s().gpr[4] = 0;
    m.s().gpr[5] = 0x12345678U;
    m.code(0x1000, {A::dcbz_l(3, 4), A::stw(5, 0, 3), A::lwarx(6, 0, 3),
                    A::dcbz_l(3, 4), A::stwcx_(5, 0, 3)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK(m.cpu->step() == StepResult::Executed); // dcbz_l overwrites the reserved line
    CHECK(m.cpu->step() == StepResult::Executed); // stwcx. must fail
    CHECK_EQ(CR0(m.s().cr), 0U);
    CHECK_EQ(m.s().gpr[6], 0x12345678U);
    std::array<u8, 32> line{};
    CHECK(m.cpu->locked_cache_line(0xE0000000U, line));
    for (u8 byte : line) CHECK_EQ(byte, 0U);
}

TEST_CASE(locked_cache_dma_fill_invalidates_reservations) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    m.s().dbat[3] = {0xE0000002U, 0xE0000002U};
    m.s().msr |= MSR_DR;
    for (u32 i = 0; i < 32; i += 4) {
        m.bus.write32(0x3000U + i, 0x11110000U + i);
        m.bus.write32(0x3040U + i, 0x22220000U + i);
    }
    m.s().gpr[3] = 0x3000U;
    m.s().gpr[4] = 0xE0000000U | 0x10U | (1U << 2U) | 0x2U;
    m.s().gpr[6] = 0xE0000000U;
    m.s().gpr[7] = 0;
    m.s().gpr[5] = 0xAABBCCDDU;
    m.code(0x1000, {A::mtspr(spr::DMAU, 3), A::mtspr(spr::DMAL, 4),
                    A::lwarx(8, 6, 7), A::mtspr(spr::DMAU, 3), A::mtspr(spr::DMAL, 4),
                    A::stwcx_(5, 6, 7)});
    CHECK(m.cpu->run(2) == StepResult::Executed); // first DMA fill
    CHECK(m.cpu->step() == StepResult::Executed); // reserve the filled line
    CHECK_EQ(m.s().gpr[8], 0x11110000U);
    m.s().gpr[3] = 0x3040U; // refill the same locked-cache line from another RAM line
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK(m.cpu->step() == StepResult::Executed); // stwcx. must fail after DMA overwrite
    CHECK_EQ(CR0(m.s().cr), 0U);
    std::array<u8, 32> line{};
    CHECK(m.cpu->locked_cache_line(0xE0000000U, line));
    CHECK_EQ(line[0], 0x22U);
    CHECK_EQ(line[1], 0x22U);
    CHECK_EQ(line[2], 0x00U);
    CHECK_EQ(line[3], 0x00U);
}

TEST_CASE(locked_cache_dma_round_trip) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    for (u32 i = 0; i < 64; i += 4) m.bus.write32(0x3000 + i, 0xA0000000U | i);
    // DMA 2 lines memory -> locked cache: DMAU = mem | LEN_U(0), DMAL = lc | LD | LEN_L(2) | T
    m.s().gpr[3] = 0x3000U; m.s().gpr[4] = 0xE0000000U | 0x10U | (2U << 2) | 0x2U;
    m.code(0x1000, {A::mtspr(spr::DMAU, 3), A::mtspr(spr::DMAL, 4), A::mfspr(5, spr::DMAL)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 2U);
    CHECK_EQ(m.s().gpr[5] & 2U, 0U); // T cleared: transfer complete
    std::array<u8, 32> line{};
    CHECK(m.cpu->locked_cache_line(0xE0000020U, line));
    CHECK_EQ(line[3], 0x20U);
    // locked cache -> memory at 0x4000 (LD = 0)
    m.s().pc = 0x1000; m.s().gpr[3] = 0x4000U; m.s().gpr[4] = 0xE0000000U | (2U << 2) | 0x2U;
    m.code(0x1000, {A::mtspr(spr::DMAU, 3), A::mtspr(spr::DMAL, 4)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.r32(0x4000), 0xA0000000U);
    CHECK_EQ(m.r32(0x403C), 0xA000003CU);
    // disabling the locked cache discards it
    m.s().pc = 0x1000; m.s().gpr[3] = HID2_PSE;
    CHECK(m.exec(A::mtspr(spr::HID2, 3)) == StepResult::Executed);
    CHECK_EQ(m.cpu->locked_cache_lines(), 0U);
}

TEST_CASE(locked_cache_capacity_and_unallocated_dma) {
    FpMachine m;
    m.s().hid2 |= HID2_LCE;
    m.s().gpr[3] = 0xE0000000U; m.s().gpr[0] = 0;
    for (u32 i = 0; i < 512; ++i) {
        m.s().pc = 0x1000; m.s().gpr[4] = i * 32U;
        CHECK(m.exec(A::dcbz_l(3, 4)) == StepResult::Executed);
    }
    m.s().pc = 0x1000; m.s().gpr[4] = 512U * 32U;
    CHECK(m.exec(A::dcbz_l(3, 4)) == StepResult::UnsupportedInstruction);
    CHECK_EQ(m.cpu->locked_cache_lines(), 512U);
    FpMachine n;
    n.s().hid2 |= HID2_LCE;
    n.s().gpr[3] = 0x4000U; n.s().gpr[4] = 0xE0000000U | (1U << 2) | 0x2U;
    n.code(0x1000, {A::mtspr(spr::DMAU, 3), A::mtspr(spr::DMAL, 4)});
    CHECK(n.cpu->run(2) == StepResult::UnsupportedInstruction); // store DMA from an unallocated line
}

// ------------------------------------------------------------ end-to-end differential
TEST_CASE(end_to_end_fp_vs_exact_reference) {
    // Random finite operands, random rounding mode and random sticky FPSCR bits;
    // instruction executed by the CPU; FPR and the complete FPSCR compared with
    // values derived from the exact reference (tests/fp_reference.hpp).
    u64 seed = 0xABCDEF1234567ULL;
    auto next = [&]() { seed += 0x9E3779B97F4A7C15ULL; u64 z = seed; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); };
    auto rand_fin = [&](bool single) -> u64 {
        if (single) { u32 w = static_cast<u32>(next()) & 0x807FFFFFU; w |= static_cast<u32>(next() % 255) << 23; float f; std::memcpy(&f, &w, 4); return sgl(f); }
        return (next() & 0x800FFFFFFFFFFFFFULL) | ((next() % 2047) << 52);
    };
    struct K { u32 (*enc)(u32, u32, u32, u32); bool single; int kind; };
    // kind: 0 add, 1 sub, 2 mul, 3 div, 4 madd, 5 msub, 6 nmadd, 7 nmsub
    const K kinds[] = {
        {[](u32 dd, u32 a, u32 b, u32) { return A::fadd(dd, a, b); }, false, 0},
        {[](u32 dd, u32 a, u32 b, u32) { return A::fsubs(dd, a, b); }, true, 1},
        {[](u32 dd, u32 a, u32, u32 c) { return A::fmul(dd, a, c); }, false, 2},
        {[](u32 dd, u32 a, u32, u32 c) { return A::fmuls(dd, a, c); }, true, 2},
        {[](u32 dd, u32 a, u32 b, u32) { return A::fdiv(dd, a, b); }, false, 3},
        {[](u32 dd, u32 a, u32 b, u32) { return A::fdivs(dd, a, b); }, true, 3},
        {[](u32 dd, u32 a, u32 b, u32 c) { return A::fmadd(dd, a, c, b); }, false, 4},
        {[](u32 dd, u32 a, u32 b, u32 c) { return A::fmsubs(dd, a, c, b); }, true, 5},
        {[](u32 dd, u32 a, u32 b, u32 c) { return A::fnmadds(dd, a, c, b); }, true, 6},
        {[](u32 dd, u32 a, u32 b, u32 c) { return A::fnmsub(dd, a, c, b); }, false, 7},
    };
    namespace R = orca::test::fpref;
    FpMachine m;
    int compared = 0;
    for (const K& k : kinds) {
        for (int it = 0; it < 1500; ++it) {
            const u64 a = rand_fin(k.single), b = rand_fin(k.single), c = rand_fin(k.single);
            if (k.kind == 3 && (b & ~F::kSignBit) == 0) continue;
            const unsigned rn = static_cast<unsigned>(next() % 4);
            const u32 sticky = static_cast<u32>(next()) & (F::OX | F::UX | F::XX | F::FX);
            const u32 fpscr0 = sticky | rn;
            m.s().pc = 0x1000; m.s().fpscr = fpscr0; m.s().cr = 0;
            m.s().fpr[2].ps0 = a; m.s().fpr[3].ps0 = b; m.s().fpr[4].ps0 = c; m.s().fpr[1].ps1 = 0x1234;
            const u32 ins = k.enc(1, 2, 3, 4) | 1U; // Rc = 1
            m.bus.write32(0x1000, ins);
            CHECK(m.cpu->step() == StepResult::Executed);
            // reference
            const R::Exact Ae = R::decode(a), Be = R::decode(b), Ce = R::decode(c);
            R::Exact x; bool zs = false;
            auto zero_sum = [&](bool s1, bool s2, bool both_zero) { return (both_zero && s1 == s2) ? s1 : rn == 3; };
            switch (k.kind) {
            case 0: x = R::add(Ae, Be); zs = zero_sum(Ae.neg, Be.neg, Ae.m.zero() && Be.m.zero()); break;
            case 1: { R::Exact nb = Be; nb.neg = !nb.neg; x = R::add(Ae, nb); zs = zero_sum(Ae.neg, nb.neg, Ae.m.zero() && nb.m.zero()); break; }
            case 2: x = R::mul(Ae, Ce); zs = Ae.neg != Ce.neg; break;
            case 3: x = R::div(Ae, Be); zs = Ae.neg != Be.neg; break;
            default: {
                const R::Exact p = R::mul(Ae, Ce); R::Exact bb = Be;
                if (k.kind == 5 || k.kind == 7) bb.neg = !bb.neg;
                x = R::add(p, bb); zs = zero_sum(p.neg, bb.neg, p.m.zero() && bb.m.zero());
                break;
            }
            }
            u64 want = zs ? F::kSignBit : 0; u32 exc = 0; bool fr = false, fi = false;
            if (!x.m.zero()) {
                const R::Rounded r = R::round(x, k.single, rn);
                want = r.bits; fi = r.inexact; fr = r.up;
                if (r.overflow) exc |= F::OX;
                if (r.tiny && r.inexact) exc |= F::UX;
                if (r.inexact) exc |= F::XX;
            }
            if (k.kind >= 6) want ^= F::kSignBit;
            u32 fpscr = fpscr0 | exc;
            if (exc & ~fpscr0) fpscr |= F::FX;
            if (fr) fpscr |= F::FR;
            if (fi) fpscr |= F::FI;
            fpscr |= F::classify(want, k.single) << 12;
            ++compared;
            const bool ok = m.s().fpr[1].ps0 == want && m.s().fpscr == fpscr && ((m.s().cr >> 24) & 0xF) == (fpscr >> 28) &&
                            m.s().fpr[1].ps1 == (k.single ? want : 0x1234ULL);
            CHECK(ok);
            if (!ok && compared < 2000)
                std::fprintf(stderr, "    kind=%d single=%d rn=%u got %016llx/%08x want %016llx/%08x\n", k.kind, k.single, rn,
                             (unsigned long long)m.s().fpr[1].ps0, m.s().fpscr, (unsigned long long)want, fpscr);
        }
    }
    std::printf("[cpu_fpu] %d end-to-end FP instructions compared with the exact reference\n", compared);
}

TEST_MAIN("cpu_fpu")
