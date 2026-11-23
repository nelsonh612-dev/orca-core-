// Address translation: BAT, hashed page table, protection, ISI/DSI.
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

using namespace orca;
using namespace orca::ppc;
using namespace orca::test;
namespace A = orca::test::ppc_asm;

namespace {
// 256 MiB block at EA 0x80000000 -> PA 0, supervisor-valid, read/write.
constexpr BatPair kCachedBat{0x80001FFEU, 0x00000002U};

// Page table at PA 0x00100000, HTABMASK = 0 (64 KiB table).
constexpr u32 kSdr1 = 0x00100000U;
constexpr u32 kVsid = 0x123U;
// Hand-computed PTEG addresses (PEM 7.6.1.x):
//   EA 0x10005000: page index 5, hash1 = 0x123 ^ 5 = 0x126 -> PTEG 0x00100000 | 0x126<<6 = 0x00104980
//   EA 0x10006000: page index 6, hash1 = 0x125            -> PTEG 0x00104940
//   secondary hash for page 5: ~0x126 & 0x7FFFF = 0x7FED9 -> PTEG 0x00100000 | 0x2D9<<6 = 0x0010B640
constexpr u32 kPtegPage5Primary = 0x00104980U;
constexpr u32 kPtegPage6Primary = 0x00104940U;
constexpr u32 kPtegPage5Secondary = 0x0010B640U;

u32 pte_w0(u32 vsid, bool secondary, u32 api) { return 0x80000000U | (vsid << 7) | (secondary ? 0x40U : 0U) | api; }

void setup_paging(Machine& m, u32 sr_flags = 0) {
    m.s().sdr1 = kSdr1;
    m.s().sr[1] = sr_flags | kVsid;
    m.s().msr |= MSR_DR;
}
} // namespace

TEST_CASE(dbat_translation_and_store) {
    Machine m;
    m.s().dbat[0] = kCachedBat;
    m.s().msr = MSR_DR;
    m.bus.write32(0x2000, 0x13572468U);
    m.s().gpr[4] = 0x80002000U; m.s().gpr[6] = 0xABCDEF01U;
    m.code(0x1000, {A::lwz(3, 0, 4), A::stw(6, 4, 4)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x13572468U);
    CHECK_EQ(m.r32(0x2004), 0xABCDEF01U);
}

TEST_CASE(small_bat_block_bounds) {
    Machine m;
    m.s().dbat[1] = {0xC0000002U, 0x00400002U}; // BL=0 -> 128 KiB, EA 0xC0000000 -> PA 0x00400000
    m.s().msr = MSR_DR;
    m.bus.write32(0x0041FFFCU, 77);
    m.s().gpr[4] = 0xC001FFFCU;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 77U);
    m.s().pc = 0x1000; m.s().gpr[4] = 0xC0020000U; // outside the block and no page table
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dsisr, DSISR_NOT_FOUND);
    CHECK_EQ(m.s().dar, 0xC0020000U);
}

TEST_CASE(bat_protection_and_problem_state_validity) {
    Machine m;
    m.s().dbat[0] = {0x80001FFEU, 0x00000001U}; // read-only
    m.s().msr = MSR_DR;
    m.s().gpr[4] = 0x80002000U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::stw(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dsisr, DSISR_PROTECTION | DSISR_STORE);
    CHECK_EQ(m.s().srr0, 0x1000U);
    // Vs=1, Vp=0: invisible in problem state
    Machine u;
    u.s().dbat[0] = kCachedBat;
    u.s().msr = MSR_DR | MSR_PR;
    u.s().gpr[4] = 0x80002000U;
    CHECK(u.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(u.s().dsisr, DSISR_NOT_FOUND);
}

TEST_CASE(ibat_fetch_and_isi) {
    Machine m(0x80001000U);
    m.s().ibat[0] = kCachedBat;
    m.s().msr = MSR_IR;
    // jump to EA 0x10000000 (unmapped); out of reach of "ba", so go through CTR
    m.s().ctr = 0x10000000U;
    m.code(0x1000, {A::addi(3, 0, 9), A::bcctr(20, 0)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 9U);
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x10000000U);
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::ISI);
    CHECK_EQ(m.s().srr0, 0x10000000U);
    CHECK_EQ(m.s().srr1 & SRR1_ISI_NOT_FOUND, SRR1_ISI_NOT_FOUND);
    CHECK_EQ(m.s().srr1 & MSR_IR, MSR_IR);   // old MSR saved
    CHECK_EQ(m.s().msr & MSR_IR, 0U);        // translation off in the handler
    CHECK_EQ(m.s().pc, 0x400U);
}

TEST_CASE(page_table_primary_hit_sets_r_and_c) {
    Machine m;
    setup_paging(m);
    m.bus.write32(kPtegPage5Primary + 8, pte_w0(kVsid, false, 0)); // slot 1 of the PTEG
    m.bus.write32(kPtegPage5Primary + 12, 0x00200000U | 2U);       // RPN 0x00200, PP=2
    m.bus.write32(0x00200010U, 0x600DCAFEU);
    m.s().gpr[4] = 0x10005010U; m.s().gpr[6] = 0x12345678U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x600DCAFEU);
    CHECK_EQ(m.r32(kPtegPage5Primary + 12) & 0x180U, 0x100U); // R set, C clear
    m.s().pc = 0x1000;
    CHECK(m.exec(A::stw(6, 4, 4)) == StepResult::Executed);
    CHECK_EQ(m.r32(0x00200014U), 0x12345678U);
    CHECK_EQ(m.r32(kPtegPage5Primary + 12) & 0x180U, 0x180U); // R and C set
}

TEST_CASE(page_table_secondary_hash) {
    Machine m;
    setup_paging(m);
    m.bus.write32(kPtegPage5Secondary + 56, pte_w0(kVsid, true, 0)); // slot 7
    m.bus.write32(kPtegPage5Secondary + 60, 0x00210000U | 2U);
    m.bus.write32(0x00210AB8U, 0x51515151U);
    m.s().gpr[4] = 0x10005AB8U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x51515151U);
    // A PTE in the secondary group with H=0 must not match.
    Machine n;
    setup_paging(n);
    n.bus.write32(kPtegPage5Secondary, pte_w0(kVsid, false, 0));
    n.bus.write32(kPtegPage5Secondary + 4, 0x00210000U | 2U);
    n.s().gpr[4] = 0x10005000U;
    CHECK(n.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(n.s().dsisr, DSISR_NOT_FOUND);
}

TEST_CASE(page_table_with_htabmask_uses_upper_hash_bits) {
    // 256 KiB table: HTABORG = 0x00400000, HTABMASK = 3. VSID 0xC00, EA 0x20005000 (SR2, page 5).
    //   hash1 = 0xC00 ^ 5 = 0xC05 -> PTEG = ((0x40 | (0xC05>>10 & 3)) << 16) | (0x005 << 6) = 0x00430140
    //   hash2 = ~0xC05 & 0x7FFFF = 0x7F3FA -> ((0x40 | (0x7F3FA>>10 & 3)) << 16) | (0x3FA << 6) = 0x0040FE80
    Machine m;
    m.s().sdr1 = 0x00400003U;
    m.s().sr[2] = 0xC00U;
    m.s().msr = MSR_DR;
    m.bus.write32(0x00430140U, pte_w0(0xC00U, false, 0));
    m.bus.write32(0x00430144U, 0x00500000U | 2U);
    m.bus.write32(0x0040FE80U, pte_w0(0xC00U, true, 0)); // same page via the secondary group
    m.bus.write32(0x0040FE84U, 0x00600000U | 2U);
    m.bus.write32(0x00500008U, 0x11111111U);
    m.bus.write32(0x00600008U, 0x22222222U);
    m.s().gpr[4] = 0x20005008U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x11111111U); // primary group wins
    m.bus.write32(0x00430140U, 0); // invalidate the primary PTE
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x22222222U);
}

TEST_CASE(page_protection_keys) {
    // Ks=1 and PP=0: no access for supervisor
    Machine m;
    setup_paging(m, 0x40000000U);
    m.bus.write32(kPtegPage5Primary, pte_w0(kVsid, false, 0));
    m.bus.write32(kPtegPage5Primary + 4, 0x00200000U | 0U);
    m.s().gpr[4] = 0x10005000U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(m.s().dsisr, DSISR_PROTECTION);
    CHECK_EQ(m.r32(kPtegPage5Primary + 4) & 0x100U, 0U); // R not set on a violation
    // Key 0 and PP=3: read-only
    Machine n;
    setup_paging(n);
    n.bus.write32(kPtegPage5Primary, pte_w0(kVsid, false, 0));
    n.bus.write32(kPtegPage5Primary + 4, 0x00200000U | 3U);
    n.s().gpr[4] = 0x10005000U;
    CHECK(n.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    n.s().pc = 0x1000;
    CHECK(n.exec(A::stw(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(n.s().dsisr, DSISR_PROTECTION | DSISR_STORE);
}

TEST_CASE(no_execute_and_direct_store_segments) {
    Machine m(0x10005000U);
    m.s().sdr1 = kSdr1;
    m.s().sr[1] = 0x10000000U | kVsid; // N=1
    m.s().msr = MSR_IR;
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::ISI);
    CHECK_EQ(m.s().srr1 & SRR1_ISI_NO_EXECUTE, SRR1_ISI_NO_EXECUTE);
    Machine d;
    d.s().sr[1] = 0x80000000U; // T=1
    d.s().msr = MSR_DR;
    d.s().gpr[4] = 0x10000000U;
    CHECK(d.exec(A::lwz(3, 0, 4)) == StepResult::UnsupportedInstruction);
}

TEST_CASE(page_table_outside_ram_is_bus_error) {
    Machine m;
    m.s().sdr1 = 0x40000000U; // nothing mapped there
    m.s().sr[1] = kVsid;
    m.s().msr = MSR_DR;
    m.s().gpr[4] = 0x10005000U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::MemoryFault);
    CHECK_EQ(m.s().pc, 0x1000U);
}

TEST_CASE(page_crossing_access_uses_both_translations) {
    Machine m;
    setup_paging(m);
    m.bus.write32(kPtegPage5Primary, pte_w0(kVsid, false, 0));
    m.bus.write32(kPtegPage5Primary + 4, 0x00200000U | 2U);
    m.bus.write32(kPtegPage6Primary, pte_w0(kVsid, false, 0));
    m.bus.write32(kPtegPage6Primary + 4, 0x00300000U | 2U); // not contiguous with page 5
    m.bus.write16(0x00200FFEU, 0xAABBU);
    m.bus.write16(0x00300000U, 0xCCDDU);
    m.s().gpr[4] = 0x10005FFEU;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xAABBCCDDU);
    m.s().pc = 0x1000; m.s().gpr[6] = 0x11223344U;
    CHECK(m.exec(A::stw(6, 0, 4)) == StepResult::Executed);
    CHECK_EQ(m.r16(0x00200FFEU), 0x1122U);
    CHECK_EQ(m.r16(0x00300000U), 0x3344U);
}

TEST_CASE(page_crossing_fault_writes_nothing) {
    Machine m;
    setup_paging(m);
    m.bus.write32(kPtegPage5Primary, pte_w0(kVsid, false, 0));
    m.bus.write32(kPtegPage5Primary + 4, 0x00200000U | 2U); // page 6 is NOT mapped
    m.s().gpr[4] = 0x10005FFEU; m.s().gpr[6] = 0x11223344U;
    CHECK(m.exec(A::stw(6, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dsisr, DSISR_NOT_FOUND | DSISR_STORE);
    // Same segment: "first word accessed in the segment ... that caused the DSI" is the original EA.
    CHECK_EQ(m.s().dar, 0x10005FFEU);
    CHECK_EQ(m.r16(0x00200FFEU), 0U);
}

TEST_CASE(page_crossing_fault_dar_in_other_area) {
    // First page through a BAT, second page through the page table (not found):
    // the faulting area starts at the second page.
    Machine m;
    setup_paging(m);
    m.s().dbat[0] = {0x0FFE0002U, 0x00400002U}; // EA 0x0FFE0000-0x0FFFFFFF -> PA 0x00400000
    m.s().gpr[4] = 0x0FFFFFFEU;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dsisr, DSISR_NOT_FOUND);
    CHECK_EQ(m.s().dar, 0x10000000U);
    // Crossing into another segment, both through the page table.
    Machine s;
    setup_paging(s);
    s.s().sr[0] = kVsid + 1U;
    // EA 0x0FFFF000 (segment 0): page index 0xFFFF, hash1 = 0x124 ^ 0xFFFF = 0xFEDB
    //   -> PTEG 0x00100000 | (0xFEDB & 0x3FF) << 6 = 0x0010B6C0, API = 0xFFFF >> 10 = 0x3F
    s.bus.write32(0x0010B6C0U, pte_w0(kVsid + 1U, false, 0x3FU));
    s.bus.write32(0x0010B6C4U, 0x00200000U | 2U);
    s.s().gpr[4] = 0x0FFFFFFEU;
    CHECK(s.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(s.s().dar, 0x10000000U); // segment 1 page 0 has no PTE
    // Same segment, but the first page is a BAT area and the second goes to the
    // page table: different areas -> DAR = first byte of the second page.
    Machine b;
    setup_paging(b);
    b.s().dbat[0] = {0x10000002U, 0x00400002U}; // EA 0x10000000-0x1001FFFF -> PA 0x00400000
    b.s().gpr[4] = 0x1001FFFEU;
    CHECK(b.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK_EQ(b.s().dar, 0x10020000U);
}

TEST_CASE(dcbz_cache_inhibited_raises_alignment) {
    Machine m;
    m.s().dbat[0] = {0x80001FFEU, 0x00000022U}; // WIMG = 0100 (I), PP=2
    m.s().msr = MSR_DR;
    m.s().gpr[3] = 0x80002000U;
    m.bus.write32(0x2000, 0xFFFFFFFFU);
    CHECK(m.exec(A::dcbz(0, 3)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.r32(0x2000), 0xFFFFFFFFU);
}

TEST_CASE(mtsr_mfsrin_and_sdr1) {
    Machine m;
    m.s().gpr[3] = 0x20000123U; m.s().gpr[4] = 0x30000000U;
    m.code(0x1000, {A::mtsr(5, 3), A::mtsrin(3, 4), A::mfsr(6, 3), A::mfsrin(7, 4)});
    m.s().gpr[3] = 0x20000123U;
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().sr[5], 0x20000123U);
    CHECK_EQ(m.s().sr[3], 0x20000123U);
    CHECK_EQ(m.s().gpr[6], 0x20000123U);
    CHECK_EQ(m.s().gpr[7], 0x20000123U);
}

TEST_MAIN("mmu")

// --- Broadway extended BATs (HID4[SBE], SPR 560-575; Dolphin Gekko.h/MMU.cpp) ---
TEST_CASE(broadway_extended_dbat_needs_sbe) {
    Machine m(0x1000, true);
    m.s().msr = MSR_DR;
    m.bus.write32(0x00400010U, 0xCAFEF00DU);
    // DBAT5: EA 0x90000000, BL=0 (128 KiB), Vs=1 -> PA 0x00400000, PP=10
    m.s().gpr[5] = 0x90000002U; m.s().gpr[6] = 0x00400002U;
    m.code(0x1000, {A::mtspr(570, 5), A::mtspr(571, 6), A::mfspr(7, 570), A::mfspr(8, 571)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().dbat[5].upper, 0x90000002U);
    CHECK_EQ(m.s().dbat[5].lower, 0x00400002U);
    CHECK_EQ(m.s().gpr[7], 0x90000002U);
    CHECK_EQ(m.s().gpr[8], 0x00400002U);
    // HID4[SBE] = 0: BATs 4-7 are ignored -> DSI (no page table entry)
    m.s().gpr[4] = 0x90000010U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
    CHECK_EQ(m.s().dar, 0x90000010U);
    // HID4[SBE] = 1 (through mtspr 1011)
    m.s().pc = 0x2000; m.s().msr = MSR_DR; m.s().gpr[9] = HID4_SBE;
    m.code(0x2000, {A::mtspr(1011, 9), A::lwz(3, 0, 4)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().hid4, HID4_SBE);
    CHECK_EQ(m.s().gpr[3], 0xCAFEF00DU);
}

TEST_CASE(broadway_extended_ibat_fetch) {
    Machine m(0x1000, true);
    m.s().ibat[7] = {0xA0000002U, 0x00500002U}; // EA 0xA0000000 -> PA 0x00500000
    m.s().hid4 = HID4_SBE;
    m.s().msr = MSR_IR;
    m.code(0x00500000U, {A::addi(3, 0, 42)});
    m.s().pc = 0xA0000000U;
    m.bus.write32(0x1000, 0); // not used
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 42U);
    CHECK_EQ(m.s().pc, 0xA0000004U);
    m.s().hid4 = 0; m.s().pc = 0xA0000000U;
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::ISI);
}

TEST_CASE(gekko_has_no_extended_bats) {
    Machine m; // Gekko
    m.s().gpr[5] = 0x90000002U;
    CHECK(m.exec(A::mtspr(570, 5)) == StepResult::UnsupportedInstruction);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::mfspr(3, 575)) == StepResult::UnsupportedInstruction);
    // Even with SBE set, translation only searches BATs 0-3.
    m.s().pc = 0x1000; m.s().hid4 = HID4_SBE; m.s().msr = MSR_DR;
    m.s().dbat[4] = {0x90000002U, 0x00400002U};
    m.s().gpr[4] = 0x90000000U;
    CHECK(m.exec(A::lwz(3, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::DSI);
}

// --- DSISR of alignment exceptions (layout from Linux align.c make_dsisr) ---
TEST_CASE(alignment_dsisr_from_instruction) {
    // lfd f1,0(r4): D-form opcode 50 = 110010 -> DSISR[17]=0, [18:21]=1001, rD=1, rA=4
    CHECK_EQ(alignment_dsisr(A::lfd(1, 0, 4)), 0x2424U);
    // lfdx f1,r4,r5: X-form XO 599 -> [15:16]=11, [17]=0, [18:21]=1001 (Linux table: "11 0 1001: lfdx")
    CHECK_EQ(alignment_dsisr(A::lfdx(1, 4, 5)), 0x1A424U);
    // lfdux f1,r4,r5: XO 631 has instruction bit 25 set -> DSISR[17] = 1 ("11 1 1001")
    CHECK_EQ(alignment_dsisr(A::lfdux(1, 4, 5)), 0x1E424U);
    // lmw r29,0(r4): opcode 46 = 101110 -> [18:21]=0111, rD=29, rA=4
    CHECK_EQ(alignment_dsisr(A::lmw(29, 0, 4)), 0x1FA4U);
    // lwz r3,0(r4): opcode 32 -> only the register fields (Linux: "00 0 0000: lwz")
    CHECK_EQ(alignment_dsisr(A::lwz(3, 0, 4)), 0x64U);

    Machine m;
    m.s().msr = MSR_FP;
    m.s().gpr[4] = 0x2002U;
    CHECK(m.exec(A::lfd(1, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.s().dar, 0x2002U);
    CHECK_EQ(m.s().dsisr, 0x2424U);
    m.s().pc = 0x1000; m.s().msr = 0; m.s().gpr[4] = 0x2001U;
    CHECK(m.exec(A::lmw(29, 0, 4)) == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.s().dsisr, 0x1FA4U);
}
