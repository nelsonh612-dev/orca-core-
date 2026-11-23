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
    CHECK_EQ(m.r16(0x00200FFEU), 0U);
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
