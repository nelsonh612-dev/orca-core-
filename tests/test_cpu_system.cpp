// Branches, condition register, SPRs, MSR, exceptions, interrupts, time base.
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace orca;
using namespace orca::ppc;
using namespace orca::test;
namespace A = orca::test::ppc_asm;

// ------------------------------------------------------------------ branches
TEST_CASE(branch_absolute_and_link) {
    Machine m;
    CHECK(m.exec(A::b(0x100, true, true)) == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x100U);
    CHECK_EQ(m.s().lr, 0x1004U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::b(-0x1000, true)) == StepResult::Executed); // absolute targets are sign-extended
    CHECK_EQ(m.s().pc, 0xFFFFF000U);
}

TEST_CASE(bc_ctr_decrement_semantics) {
    Machine m;
    // bdnz -> branch while --CTR != 0
    m.s().ctr = 2;
    CHECK(m.exec(A::bc(16, 0, 0x40)) == StepResult::Executed);
    CHECK_EQ(m.s().ctr, 1U); CHECK_EQ(m.s().pc, 0x1040U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::bc(16, 0, 0x40)) == StepResult::Executed);
    CHECK_EQ(m.s().ctr, 0U); CHECK_EQ(m.s().pc, 0x1004U);
    // bdz with CTR=0 decrements to 0xFFFFFFFF and does not branch
    m.s().pc = 0x1000;
    CHECK(m.exec(A::bc(18, 0, 0x40)) == StepResult::Executed);
    CHECK_EQ(m.s().ctr, 0xFFFFFFFFU); CHECK_EQ(m.s().pc, 0x1004U);
    // bdnzt: needs CTR != 0 and CR bit set
    m.s().pc = 0x1000; m.s().ctr = 5; m.s().cr = 0;
    CHECK(m.exec(A::bc(8, 2, 0x40)) == StepResult::Executed);
    CHECK_EQ(m.s().ctr, 4U); CHECK_EQ(m.s().pc, 0x1004U);
    m.s().pc = 0x1000; m.s().cr = 0x20000000U; // CR bit 2 (EQ of cr0)
    CHECK(m.exec(A::bc(8, 2, 0x40)) == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x1040U);
}

TEST_CASE(bclr_bcctr_targets_and_link) {
    Machine m;
    m.s().lr = 0x2003; // low bits ignored
    CHECK(m.exec(A::bclr(20, 0, true)) == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x2000U);
    CHECK_EQ(m.s().lr, 0x1004U); // new LR written after the old target was captured
    m.s().pc = 0x1000; m.s().ctr = 0x3002;
    CHECK(m.exec(A::bcctr(20, 0, true)) == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x3000U);
    CHECK_EQ(m.s().lr, 0x1004U);
    m.s().pc = 0x1000; m.s().cr = 0;
    CHECK(m.exec(A::bcctr(12, 0)) == StepResult::Executed); // condition false
    CHECK_EQ(m.s().pc, 0x1004U);
    m.s().pc = 0x1000;
    const CpuState before = m.s();
    CHECK(m.exec(A::bcctr(16, 0)) == StepResult::InvalidForm); // CTR decrement form
    CHECK(m.s() == before);
}

TEST_CASE(link_register_written_even_when_branch_not_taken) {
    Machine m;
    m.s().cr = 0; m.s().lr = 0xDEAD0000U;
    CHECK(m.exec(A::bc(12, 0, 0x40, false, true)) == StepResult::Executed); // bcl, condition false
    CHECK_EQ(m.s().pc, 0x1004U);
    CHECK_EQ(m.s().lr, 0x1004U);
    m.s().pc = 0x1000; m.s().lr = 0x3000;
    CHECK(m.exec(A::bclr(12, 0, true)) == StepResult::Executed); // bclrl, condition false
    CHECK_EQ(m.s().pc, 0x1004U);
    CHECK_EQ(m.s().lr, 0x1004U);
}

TEST_CASE(retired_instruction_counter) {
    Machine m;
    m.code(0x1000, {A::addi(3, 0, 1), A::sc(), 0x00000000U});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK(m.cpu->step() == StepResult::ExceptionTaken); // sc completes
    CHECK_EQ(m.cpu->instructions_retired(), 2U);
    m.s().pc = 0x1008;
    CHECK(m.cpu->step() == StepResult::UnsupportedInstruction);
    CHECK_EQ(m.cpu->instructions_retired(), 2U);
}

TEST_CASE(bclr_with_ctr_decrement_is_valid) {
    Machine m;
    m.s().lr = 0x2000; m.s().ctr = 2;
    CHECK(m.exec(A::bclr(16, 0)) == StepResult::Executed); // bdnzlr
    CHECK_EQ(m.s().pc, 0x2000U);
    CHECK_EQ(m.s().ctr, 1U);
}

// ------------------------------------------------------------- CR operations
TEST_CASE(cr_logical_operations) {
    Machine m;
    m.s().cr = 0xA0000000U; // bits 0 and 2 set
    m.code(0x1000, {A::crand(4, 0, 2), A::cror(5, 1, 2), A::crxor(6, 0, 2), A::crnand(7, 0, 2),
                    A::crnor(8, 1, 3), A::creqv(9, 0, 1), A::crandc(10, 0, 1), A::crorc(11, 1, 0)});
    CHECK(m.cpu->run(8) == StepResult::Executed);
    auto bit = [&](unsigned b) { return (m.s().cr >> (31U - b)) & 1U; };
    CHECK_EQ(bit(4), 1U); CHECK_EQ(bit(5), 1U); CHECK_EQ(bit(6), 0U); CHECK_EQ(bit(7), 0U);
    CHECK_EQ(bit(8), 1U); CHECK_EQ(bit(9), 0U); CHECK_EQ(bit(10), 1U); CHECK_EQ(bit(11), 0U);
    CHECK_EQ(m.s().cr >> 28, 0xAU); // sources untouched
}

TEST_CASE(cr_moves) {
    Machine m;
    m.s().cr = 0x12345678U; m.s().gpr[3] = 0xFEDCBA98U; m.s().xer = 0xE0000005U;
    m.code(0x1000, {A::mcrf(0, 7), A::mfcr(4), A::mtcrf(0x81, 3), A::mcrxr(1)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0x82345678U);
    // mcrf 0,7 -> 0x82345678; mtcrf 0x81 (fields 0,7 from 0xFEDCBA98) -> 0xF2345678;
    // mcrxr 1 (XER[0:3] = 0xE) -> 0xFE345678
    CHECK_EQ(m.s().cr, 0xFE345678U);
    CHECK_EQ(m.s().xer, 0x00000005U);
}

// ------------------------------------------------------------- SPRs
TEST_CASE(spr_round_trips) {
    Machine m;
    const std::vector<u32> sprs = {spr::LR, spr::CTR, spr::SRR0, spr::SRR1, spr::SPRG0, spr::SPRG3, spr::DAR,
                                   spr::DSISR, spr::SDR1, spr::HID0, spr::HID2, spr::GQR0 + 7, spr::IBAT0U + 7,
                                   spr::DBAT0U, spr::EAR, spr::L2CR, spr::WPAR};
    for (u32 n : sprs) {
        m.s().pc = 0x1000; m.s().gpr[3] = 0xA5000000U | n; m.s().gpr[4] = 0;
        m.code(0x1000, {A::mtspr(n, 3), A::mfspr(4, n)});
        CHECK(m.cpu->run(2) == StepResult::Executed);
        CHECK_EQ(m.s().gpr[4], 0xA5000000U | n);
    }
    CHECK_EQ(m.s().dbat[0].upper, 0xA5000000U | spr::DBAT0U);
    CHECK_EQ(m.s().ibat[3].lower, 0xA5000000U | (spr::IBAT0U + 7));
}

TEST_CASE(xer_reserved_bits_and_pvr) {
    Machine m;
    m.s().gpr[3] = 0xFFFFFFFFU;
    m.code(0x1000, {A::mtspr(spr::XER, 3), A::mfspr(4, spr::XER), A::mfspr(5, spr::PVR)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0xE000007FU);
    CHECK_EQ(m.s().gpr[5], 0xABCD1234U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::mtspr(spr::PVR, 3)) == StepResult::UnsupportedInstruction);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::mfspr(3, 1000)) == StepResult::UnsupportedInstruction); // unknown SPR
    m.s().pc = 0x1000;
    CHECK(m.exec(A::mtspr(spr::DMAL, 3)) == StepResult::UnsupportedInstruction);
}

TEST_CASE(time_base_and_mftb) {
    Machine m;
    m.s().gpr[3] = 0xFFFFFFFFU; m.s().gpr[4] = 0x00000010U;
    m.code(0x1000, {A::mtspr(spr::TBL_WRITE, 3), A::mtspr(spr::TBU_WRITE, 4)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().tb, 0x00000010FFFFFFFFULL);
    m.cpu->advance_timebase(1);
    m.code(0x1008, {A::mftb(5), A::mftb(6, 269)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[5], 0U);
    CHECK_EQ(m.s().gpr[6], 0x11U);
}

// ------------------------------------------------------------- exceptions
TEST_CASE(system_call_and_rfi) {
    Machine m;
    m.s().msr = MSR_EE | MSR_PR | MSR_FP | MSR_ME | MSR_IR | MSR_DR | MSR_RI;
    // sc in user mode with translation off would need BATs, so use real mode here.
    m.s().msr &= ~(MSR_IR | MSR_DR);
    const u32 user_msr = m.s().msr;
    CHECK(m.exec(A::sc()) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::SystemCall);
    CHECK_EQ(m.s().pc, 0x00000C00U);
    CHECK_EQ(m.s().srr0, 0x1004U);               // sc: address of next instruction
    CHECK_EQ(m.s().srr1, user_msr & MSR_SAVE_MASK);
    CHECK_EQ(m.s().msr, MSR_ME);                 // EE, PR, FP, RI cleared; ME kept
    m.code(0xC00, {A::rfi()});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x1004U);
    CHECK_EQ(m.s().msr, user_msr);
}

TEST_CASE(exception_prefix_and_ile) {
    Machine m;
    m.s().msr = MSR_IP | MSR_ILE;
    CHECK(m.exec(A::sc()) == StepResult::ExceptionTaken);
    CHECK_EQ(m.s().pc, 0xFFF00C00U);
    CHECK_EQ(m.s().msr, MSR_IP | MSR_ILE | MSR_LE); // LE := ILE
    // Executing with LE set is not implemented: must stop clearly, not misbehave.
    m.bus.map_ram("rom", 0xFFF00000U, 0x100000);
    CHECK(m.cpu->step() == StepResult::UnsupportedInstruction);
}

TEST_CASE(privileged_instructions_in_user_mode) {
    const u32 cases[] = {A::mfmsr(3), A::mtmsr(3), A::rfi(), A::mtspr(spr::SRR0, 3), A::mfspr(3, spr::HID0),
                         A::mtsr(1, 3), A::mfsr(3, 1), A::tlbie(3), A::dcbi(0, 3)};
    for (u32 ins : cases) {
        Machine m;
        m.s().msr = MSR_PR;
        CHECK(m.exec(ins) == StepResult::ExceptionTaken);
        CHECK(m.cpu->last_exception() == Exception::Program);
        CHECK_EQ(m.s().srr1 & SRR1_PROGRAM_PRIVILEGED, SRR1_PROGRAM_PRIVILEGED);
        CHECK_EQ(m.s().srr0, 0x1000U);
        CHECK_EQ(m.s().pc, 0x700U);
    }
    Machine u;
    u.s().msr = MSR_PR; u.s().gpr[3] = 7;
    u.code(0x1000, {A::mtspr(spr::LR, 3), A::mfspr(4, spr::CTR), A::mftb(5)}); // user-level SPRs
    CHECK(u.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(u.s().lr, 7U);
}

TEST_CASE(trap_instructions) {
    Machine m;
    m.s().gpr[3] = static_cast<u32>(-1); m.s().gpr[4] = 1;
    m.code(0x1000, {A::tw(0x02, 3, 4), A::tw(0x10, 3, 4)}); // ltu false, lt true
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Program);
    CHECK_EQ(m.s().srr1 & SRR1_PROGRAM_TRAP, SRR1_PROGRAM_TRAP);
    CHECK_EQ(m.s().srr0, 0x1004U);
    Machine n;
    n.s().gpr[3] = 5;
    CHECK(n.exec(A::twi(4, 3, 5)) == StepResult::ExceptionTaken); // eq
    Machine k;
    CHECK(k.exec(A::twi(0, 3, 5)) == StepResult::Executed);        // TO = 0 never traps
}

TEST_CASE(floating_point_unavailable_and_unimplemented) {
    Machine m;
    CHECK(m.exec(A::lfs(1, 0, 3)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::FloatingPointUnavailable);
    CHECK_EQ(m.s().pc, 0x800U);
    // M004: the FPU exists now. With MSR[FP]=1 lfs executes; paired-single
    // opcode 4 without HID2[PSE] is an illegal instruction.
    Machine f;
    f.s().msr = MSR_FP;
    f.s().gpr[3] = 0x2000;
    f.bus.write32(0x2000, 0x3F800000U);
    CHECK(f.exec(A::lfs(1, 0, 3)) == StepResult::Executed);
    CHECK_EQ(f.s().fpr[1].ps0, 0x3FF0000000000000ULL);
    f.s().pc = 0x1000;
    CHECK(f.exec(0x10000000U) == StepResult::ExceptionTaken); // ps_cmpu0 with HID2[PSE]=0
    CHECK(f.cpu->last_exception() == Exception::Program);
    CHECK_EQ(f.s().srr1 & SRR1_PROGRAM_ILLEGAL, SRR1_PROGRAM_ILLEGAL);
}

TEST_CASE(external_and_decrementer_interrupts) {
    Machine m;
    m.code(0x1000, {A::addi(3, 3, 1), A::addi(3, 3, 1), A::addi(3, 3, 1)});
    m.cpu->set_external_interrupt(true);
    CHECK(m.cpu->step() == StepResult::Executed); // MSR[EE]=0: masked
    m.s().msr = MSR_EE;
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::External);
    CHECK_EQ(m.s().srr0, 0x1004U); // next instruction not yet executed
    CHECK_EQ(m.s().pc, 0x500U);
    CHECK_EQ(m.s().gpr[3], 1U);
    m.cpu->set_external_interrupt(false);
    // Decrementer: 0 -> -1 transition
    Machine d;
    d.code(0x1000, {A::addi(3, 3, 1), A::addi(3, 3, 1)});
    d.s().dec = 2;
    d.cpu->advance_timebase(2);
    CHECK(!d.s().dec_pending);
    CHECK_EQ(d.s().dec, 0U);
    d.cpu->advance_timebase(1);
    CHECK(d.s().dec_pending);
    CHECK_EQ(d.s().dec, 0xFFFFFFFFU);
    CHECK(d.cpu->step() == StepResult::Executed); // masked until EE
    d.s().msr = MSR_EE;
    CHECK(d.cpu->step() == StepResult::ExceptionTaken);
    CHECK(d.cpu->last_exception() == Exception::Decrementer);
    CHECK(!d.s().dec_pending);
    CHECK_EQ(d.s().pc, 0x900U);
}

TEST_CASE(writing_dec_does_not_cancel_a_pending_request) {
    // PEM: the decrementer request is cleared only when the exception is taken.
    Machine d;
    d.cpu->advance_timebase(1); // DEC 0 -> -1: request pending
    CHECK(d.s().dec_pending);
    d.s().gpr[3] = 5000;
    CHECK(d.exec(A::mtspr(spr::DEC, 3)) == StepResult::Executed);
    CHECK(d.s().dec_pending);
    d.s().msr = MSR_EE;
    CHECK(d.cpu->step() == StepResult::ExceptionTaken);
    CHECK(d.cpu->last_exception() == Exception::Decrementer);
}

TEST_CASE(decrementer_large_advances) {
    Machine d;
    d.s().dec = 0xFFFFFFFFU; // -1: next 0->-1 transition is 2^32 ticks away
    d.cpu->advance_timebase(0xFFFFFFFFULL);
    CHECK(!d.s().dec_pending);
    d.cpu->advance_timebase(1);
    CHECK(d.s().dec_pending);
    Machine e;
    e.s().dec = 10;
    e.cpu->advance_timebase(1ULL << 40);
    CHECK(e.s().dec_pending);
    CHECK_EQ(e.s().tb, 1ULL << 40);
}

TEST_CASE(power_saving_idle_until_interrupt) {
    Machine m;
    m.s().hid0 = 0x00800000U; // DOZE
    m.s().msr = MSR_POW | MSR_EE;
    CHECK(m.cpu->step() == StepResult::Idle);
    CHECK(m.cpu->run(10) == StepResult::Idle);
    m.cpu->set_external_interrupt(true);
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK_EQ(m.s().msr & MSR_POW, 0U);
    Machine n; // POW without a HID0 power mode has no effect
    n.s().msr = MSR_POW;
    CHECK(n.exec(A::addi(3, 0, 1)) == StepResult::Executed);
}

TEST_CASE(unsupported_modes_stop_clearly) {
    Machine m;
    m.s().msr = MSR_SE;
    CHECK(m.exec(A::addi(3, 0, 1)) == StepResult::UnsupportedInstruction);
    CHECK_EQ(m.s().gpr[3], 0U);
    CHECK(!m.cpu->last_stop().reason.empty());
    Machine h;
    h.s().halted = true;
    CHECK(h.cpu->step() == StepResult::Halted);
}

// ------------------------------------------------------------- cache ops
TEST_CASE(dcbz_zeroes_aligned_block) {
    Machine m;
    for (u32 a = 0x2000; a < 0x2040; a += 4) m.bus.write32(a, 0xFFFFFFFFU);
    m.s().gpr[3] = 0x2000; m.s().gpr[4] = 0x25; // EA 0x2025 -> block 0x2020
    CHECK(m.exec(A::dcbz(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.r32(0x201C), 0xFFFFFFFFU);
    CHECK_EQ(m.r32(0x2020), 0U);
    CHECK_EQ(m.r32(0x203C), 0U);
}

TEST_CASE(icbi_reports_physical_block_and_code_pages_invalidate) {
    Machine m;
    std::vector<u32> blocks;
    m.cpu->set_icache_invalidate_handler([&](u32 b) { blocks.push_back(b); });
    std::vector<u32> writes;
    m.bus.set_code_invalidation_handler([&](u32 a, u32) { writes.push_back(a); });
    m.bus.mark_code_page(0x5000);
    m.s().gpr[3] = 0x5013; m.s().gpr[6] = 0x60000000U;
    m.code(0x1000, {A::stw(6, 0, 3), A::icbi(0, 3), A::sync(), A::isync(), A::eieio(), A::dcbf(0, 3), A::dcbst(0, 3), A::dcbt(0, 3)});
    CHECK(m.cpu->run(8) == StepResult::Executed);
    CHECK_EQ(writes.size(), 1U);
    CHECK_EQ(writes[0], 0x5013U);
    CHECK_EQ(blocks.size(), 1U);
    CHECK_EQ(blocks[0], 0x5000U);
}

TEST_MAIN("cpu_system")

// mfspr with SPR 268/269 reads the time base like mftb (Dolphin interpreter),
// also in problem state.
TEST_CASE(mfspr_time_base_alias) {
    Machine m;
    m.s().tb = 0x0123456789ABCDEFULL;
    m.s().msr = MSR_PR;
    m.code(0x1000, {A::mfspr(3, 268), A::mfspr(4, 269)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x89ABCDEFU);
    CHECK_EQ(m.s().gpr[4], 0x01234567U);
    // Writing them through mtspr is still not allowed (284/285 are the write SPRs).
    m.s().msr = 0;
    CHECK(m.exec(A::mtspr(268, 3)) == StepResult::UnsupportedInstruction);
}

namespace {
constexpr u32 eciwx(u32 d, u32 a, u32 b) { return A::X(310, d, a, b); }
constexpr u32 ecowx(u32 s, u32 a, u32 b) { return A::X(438, s, a, b); }
} // namespace

TEST_CASE(eciwx_ecowx_external_control) {
    Machine m;
    m.s().gpr[4] = 0x2000; m.s().gpr[5] = 0x10;
    // EAR[E] = 0 -> DSI, DSISR[11], DAR = EA
    CHECK(m.exec(eciwx(3, 4, 5)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dsisr, DSISR_EAR_DISABLED);
    CHECK_EQ(m.s().dar, 0x2010U);
    m.s().pc = 0x1000;
    CHECK(m.exec(ecowx(3, 0, 5)) == StepResult::ExceptionTaken); // rA = 0 -> EA = rB
    CHECK_EQ(m.s().dsisr, DSISR_EAR_DISABLED | DSISR_STORE);
    CHECK_EQ(m.s().dar, 0x10U);
    // EAR[E] = 1, misaligned -> alignment exception
    m.s().pc = 0x1000; m.s().ear = 0x80000000U; m.s().gpr[5] = 0x11;
    CHECK(m.exec(eciwx(3, 4, 5)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.s().dar, 0x2011U);
    // EAR[E] = 1, aligned -> clear stop (no documented device)
    m.s().pc = 0x1000; m.s().gpr[5] = 0x10;
    CHECK(m.exec(ecowx(3, 4, 5)) == StepResult::UnsupportedInstruction);
    CHECK_EQ(m.s().pc, 0x1000U);
}
