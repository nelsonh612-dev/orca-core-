// Integer instruction tests with hand-computed expected values.
// Includes every Milestone 002 test, ported to the new bus/CPU API.
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

using namespace orca;
using namespace orca::ppc;
using namespace orca::test;
namespace A = orca::test::ppc_asm;

constexpr u32 CR0(u32 cr) { return cr >> 28; }

// ---------------------------------------------------------------- M002 ports
TEST_CASE(m002_addi_and_sign_extension) {
    Machine m;
    CHECK(m.exec(A::addi(3, 0, -2)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFFFFFEU);
    CHECK_EQ(sign_extend(0x8000U, 16), -32768);
}

TEST_CASE(m002_ori_and_record_form) {
    Machine m;
    m.s().gpr[4] = 0xABCD0000U;
    m.code(0x1000, {A::ori(4, 4, 0x1234), A::andi_(5, 4, 0)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0xABCD1234U);
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[5], 0U);
    CHECK_EQ(CR0(m.s().cr), 2U);
}

TEST_CASE(m002_relative_branch_negative) {
    Machine m;
    CHECK(m.exec(A::b(-4)) == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x0FFCU);
}

TEST_CASE(m002_addc_and_subf_destination) {
    Machine m;
    m.s().gpr[3] = 0xFFFFFFFFU; m.s().gpr[4] = 2;
    m.code(0x1000, {A::addc(5, 3, 4), A::subf(6, 3, 4)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[5], 1U);
    CHECK((m.s().xer & XER_CA) != 0U);
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[6], 3U); // r4 - r3 = 2 - (-1)
}

TEST_CASE(m002_load_store_big_endian_and_sign_extension) {
    Machine m;
    m.bus.write32(0x1040, 0x80FF7F01U);
    m.s().gpr[4] = 0x1000;
    m.code(0x1000, {A::lwz(3, 0x40, 4), A::lha(5, 0x40, 4), A::stw(3, 0x44, 4)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x80FF7F01U);
    CHECK_EQ(m.s().gpr[5], 0xFFFF80FFU);
    CHECK_EQ(m.r32(0x1044), 0x80FF7F01U);
}

TEST_CASE(m002_shift_semantics) {
    Machine m;
    m.s().gpr[3] = 0x80000001U; m.s().gpr[5] = 1;
    m.code(0x1000, {A::slw(4, 3, 5), A::srw(6, 3, 5)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 2U);
    CHECK_EQ(m.s().gpr[6], 0x40000000U);
    m.s().gpr[5] = 32; m.s().pc = 0x1000;
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0U);
}

TEST_CASE(m002_conditional_and_link_register_branches) {
    Machine m;
    m.code(0x1000, {A::bc(20, 0, 8)});
    m.code(0x1008, {A::bc(12, 0, 12)});
    m.code(0x1014, {A::bclr(20, 0, true)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x1008U);
    m.s().cr = 0x80000000U;
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x1014U);
    m.s().lr = 0x1020;
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().pc, 0x1020U);
    CHECK_EQ(m.s().lr, 0x1018U);
}

TEST_CASE(m002_faults_do_not_advance_pc) {
    Machine m;
    CHECK(m.exec(0x00000000U) == StepResult::UnsupportedInstruction);
    CHECK_EQ(m.s().pc, 0x1000U);
    m.s().pc = 0x1001;
    CHECK(m.cpu->step() == StepResult::AlignmentFault);
    CHECK_EQ(m.s().pc, 0x1001U);
}

TEST_CASE(m002_unsupported_is_not_nop_and_fetch_is_big_endian) {
    Machine m;
    m.bus.write8(0x1000, 0x38); m.bus.write8(0x1001, 0x60); m.bus.write8(0x1002, 0x00); m.bus.write8(0x1003, 0x2A);
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 42U);
    const CpuState before = m.s();
    CHECK(m.exec(0x00000000U) == StepResult::UnsupportedInstruction);
    CHECK(m.s() == before);
}

// ------------------------------------------------------- audit regressions
TEST_CASE(audit_nor_and_eqv_are_distinct) {
    Machine m;
    m.s().gpr[4] = 0xF0F0F0F0U; m.s().gpr[5] = 0xFF00FF00U;
    m.code(0x1000, {A::nor(3, 4, 5), A::eqv(6, 4, 5)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x000F000FU);
    CHECK_EQ(m.s().gpr[6], 0xF00FF00FU);
}

TEST_CASE(audit_misaligned_integer_access_is_handled_by_hardware) {
    Machine m;
    m.bus.write32(0x2000, 0x11223344U); m.bus.write32(0x2004, 0x55667788U);
    m.s().gpr[4] = 0x2000;
    m.code(0x1000, {A::lwz(3, 1, 4), A::lha(5, 3, 4), A::stw(3, 0x103, 4)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x22334455U);
    CHECK_EQ(m.s().gpr[5], 0x00004455U);
    CHECK_EQ(m.r32(0x2103), 0x22334455U);
    CHECK(m.cpu->last_exception() == Exception::None);
}

TEST_CASE(audit_update_forms_reject_invalid_encodings) {
    Machine m;
    m.s().gpr[4] = 0x2000;
    const CpuState before = m.s();
    CHECK(m.exec(A::lwzu(4, 4, 4)) == StepResult::InvalidForm);   // RA == RT
    CHECK(m.exec(A::lwzu(3, 4, 0)) == StepResult::InvalidForm);   // RA == 0
    CHECK(m.exec(A::stwu(3, 4, 0)) == StepResult::InvalidForm);   // RA == 0
    CHECK(m.exec(A::lwzux(4, 4, 5)) == StepResult::InvalidForm);  // RA == RT
    CHECK(m.s() == before);
    CHECK(m.exec(A::stwu(4, 8, 4)) == StepResult::Executed);      // RS == RA is valid for stores
    CHECK_EQ(m.r32(0x2008), 0x2000U);
    CHECK_EQ(m.s().gpr[4], 0x2008U);
}

TEST_CASE(audit_overflow_forms_update_xer) {
    Machine m;
    m.s().gpr[4] = 0x7FFFFFFFU; m.s().gpr[5] = 1;
    CHECK(m.exec(A::add(3, 4, 5, true, true)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x80000000U);
    CHECK_EQ(m.s().xer & (XER_SO | XER_OV), XER_SO | XER_OV);
    CHECK_EQ(CR0(m.s().cr), 0x9U); // LT | SO (SO taken after the XER update)
    m.s().pc = 0x1000; m.s().gpr[4] = 1;
    CHECK(m.exec(A::add(3, 4, 5, true, false)) == StepResult::Executed);
    CHECK_EQ(m.s().xer & (XER_SO | XER_OV), XER_SO); // OV cleared, SO sticky
}

// ------------------------------------------------------------- arithmetic
TEST_CASE(addis_ra_zero_and_wrap) {
    Machine m;
    CHECK(m.exec(A::addis(3, 0, -1)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFF0000U);
    m.s().pc = 0x1000; m.s().gpr[0] = 0x12345678U; m.s().gpr[5] = 0x80000000U;
    CHECK(m.exec(A::addis(4, 5, 0x8000)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0U);
    CHECK_EQ(m.s().xer, 0U); // addis never touches XER
}

TEST_CASE(addic_subfic_mulli) {
    Machine m;
    m.s().gpr[4] = 0xFFFFFFFFU;
    m.code(0x1000, {A::addic_(3, 4, 1), A::subfic(5, 4, 0), A::mulli(6, 4, -3)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U);
    CHECK_EQ(CR0(m.s().cr), 2U);
    CHECK_EQ(m.s().gpr[5], 1U);              // 0 - (-1)
    CHECK((m.s().xer & XER_CA) == 0U);       // subfic: CA = 0 >= 0xFFFFFFFF ? -> no
    CHECK_EQ(m.s().gpr[6], 3U);              // -1 * -3
}

TEST_CASE(extended_add_subtract_with_carry_chain) {
    Machine m;
    // 64-bit add: (r3:r4) = (0x00000001:0xFFFFFFFF) + (0x00000002:0x00000001)
    m.s().gpr[5] = 1; m.s().gpr[6] = 0xFFFFFFFFU; m.s().gpr[7] = 2; m.s().gpr[8] = 1;
    m.code(0x1000, {A::addc(4, 6, 8), A::adde(3, 5, 7)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0U);
    CHECK_EQ(m.s().gpr[3], 4U);
    // 64-bit subtract: (0x00000004:0x00000000) - (0x00000000:0x00000001)
    m.s().pc = 0x1000; m.s().gpr[5] = 0; m.s().gpr[6] = 1; m.s().gpr[7] = 4; m.s().gpr[8] = 0;
    m.code(0x1000, {A::subfc(4, 6, 8), A::subfe(3, 5, 7)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[4], 0xFFFFFFFFU);
    CHECK_EQ(m.s().gpr[3], 3U);
}

TEST_CASE(addze_addme_subfze_subfme_neg) {
    Machine m;
    m.s().xer = XER_CA; m.s().gpr[4] = 0xFFFFFFFFU;
    CHECK(m.exec(A::addze(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U); CHECK((m.s().xer & XER_CA) != 0U);
    m.s().pc = 0x1000; m.s().xer = 0; m.s().gpr[4] = 5;
    CHECK(m.exec(A::addme(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 4U); CHECK((m.s().xer & XER_CA) != 0U);
    m.s().pc = 0x1000; m.s().xer = XER_CA; m.s().gpr[4] = 0;
    CHECK(m.exec(A::subfze(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U); CHECK((m.s().xer & XER_CA) != 0U);
    m.s().pc = 0x1000; m.s().xer = 0; m.s().gpr[4] = 0;
    CHECK(m.exec(A::subfme(3, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFFFFFEU); CHECK((m.s().xer & XER_CA) != 0U);
    m.s().pc = 0x1000; m.s().xer = 0; m.s().gpr[4] = 0x80000000U;
    CHECK(m.exec(A::neg(3, 4, true)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x80000000U);
    CHECK_EQ(m.s().xer, XER_SO | XER_OV); // neg never sets CA
}

TEST_CASE(multiply_family) {
    Machine m;
    m.s().gpr[4] = 0x80000000U; m.s().gpr[5] = 2;
    m.code(0x1000, {A::mullw(3, 4, 5, true), A::mulhw(6, 4, 5), A::mulhwu(7, 4, 5)});
    CHECK(m.cpu->run(3) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U);
    CHECK((m.s().xer & XER_OV) != 0U);
    CHECK_EQ(m.s().gpr[6], 0xFFFFFFFFU); // high word of -2^32
    CHECK_EQ(m.s().gpr[7], 1U);          // high word of 2^32
}

TEST_CASE(divide_family_and_overflow) {
    Machine m;
    m.s().gpr[4] = static_cast<u32>(-7); m.s().gpr[5] = 2;
    CHECK(m.exec(A::divw(3, 4, 5)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], static_cast<u32>(-3)); // truncates toward zero
    m.s().pc = 0x1000;
    CHECK(m.exec(A::divwu(3, 4, 5)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x7FFFFFFCU);
    m.s().pc = 0x1000; m.s().gpr[4] = 0x80000000U; m.s().gpr[5] = 0xFFFFFFFFU;
    CHECK(m.exec(A::divw(3, 4, 5, true)) == StepResult::Executed);
    CHECK_EQ(m.s().xer & XER_OV, XER_OV);
    m.s().pc = 0x1000; m.s().xer = 0; m.s().gpr[5] = 0;
    CHECK(m.exec(A::divwu(3, 4, 5, true)) == StepResult::Executed);
    CHECK_EQ(m.s().xer & XER_OV, XER_OV);
}

// ------------------------------------------------------------- logical
TEST_CASE(logical_x_forms) {
    Machine m;
    m.s().gpr[4] = 0xF0F0F0F0U; m.s().gpr[5] = 0xFF00FF00U;
    m.code(0x1000, {A::and_(10, 4, 5), A::andc(11, 4, 5), A::or_(12, 4, 5), A::orc(13, 4, 5),
                    A::xor_(14, 4, 5), A::nand(15, 4, 5), A::eqv(16, 4, 5), A::nor(17, 4, 5)});
    CHECK(m.cpu->run(8) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[10], 0xF000F000U);
    CHECK_EQ(m.s().gpr[11], 0x00F000F0U);
    CHECK_EQ(m.s().gpr[12], 0xFFF0FFF0U);
    CHECK_EQ(m.s().gpr[13], 0xF0FFF0FFU);
    CHECK_EQ(m.s().gpr[14], 0x0FF00FF0U);
    CHECK_EQ(m.s().gpr[15], 0x0FFF0FFFU);
    CHECK_EQ(m.s().gpr[16], 0xF00FF00FU);
    CHECK_EQ(m.s().gpr[17], 0x000F000FU);
}

TEST_CASE(immediate_logical_forms) {
    Machine m;
    m.s().gpr[4] = 0x12345678U;
    m.code(0x1000, {A::oris(3, 4, 0xFFFF), A::xori(5, 4, 0xFFFF), A::xoris(6, 4, 0xFFFF), A::andis_(7, 4, 0x8000)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFF5678U);
    CHECK_EQ(m.s().gpr[5], 0x1234A987U);
    CHECK_EQ(m.s().gpr[6], 0xEDCB5678U);
    CHECK_EQ(m.s().gpr[7], 0U);
    CHECK_EQ(CR0(m.s().cr), 2U);
}

TEST_CASE(sign_extension_and_count_leading_zeros) {
    Machine m;
    m.s().gpr[4] = 0x000080FFU;
    m.code(0x1000, {A::extsb(3, 4, true), A::extsh(5, 4), A::cntlzw(6, 4), A::cntlzw(7, 0)});
    m.s().gpr[0] = 0;
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFFFFFFU);
    CHECK_EQ(CR0(m.s().cr), 8U);
    CHECK_EQ(m.s().gpr[5], 0xFFFF80FFU);
    CHECK_EQ(m.s().gpr[6], 16U);
    CHECK_EQ(m.s().gpr[7], 32U);
}

TEST_CASE(algebraic_shifts_and_carry) {
    Machine m;
    m.s().gpr[4] = 0x80000001U;
    m.code(0x1000, {A::srawi(3, 4, 1)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xC0000000U);
    CHECK((m.s().xer & XER_CA) != 0U); // negative and a 1 shifted out
    m.s().pc = 0x1000; m.s().gpr[4] = 0x80000000U;
    CHECK(m.exec(A::srawi(3, 4, 1)) == StepResult::Executed);
    CHECK((m.s().xer & XER_CA) == 0U); // only zeros shifted out
    m.s().pc = 0x1000; m.s().gpr[4] = 0x80000000U; m.s().gpr[5] = 40;
    CHECK(m.exec(A::sraw(3, 4, 5)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xFFFFFFFFU);
    CHECK((m.s().xer & XER_CA) != 0U);
    m.s().pc = 0x1000; m.s().gpr[4] = 0x7FFFFFFFU; m.s().gpr[5] = 63;
    CHECK(m.exec(A::sraw(3, 4, 5)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U);
    CHECK((m.s().xer & XER_CA) == 0U);
    m.s().pc = 0x1000; m.s().gpr[5] = 0x20; m.s().gpr[4] = 0xFFFFFFFFU; // shift amount uses 6 bits
    CHECK(m.exec(A::srw(3, 4, 5)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0U);
}

TEST_CASE(rotates_and_masks) {
    Machine m;
    m.s().gpr[4] = 0x12345678U; m.s().gpr[3] = 0xFFFFFFFFU; m.s().gpr[6] = 36;
    m.code(0x1000, {A::rlwinm(5, 4, 8, 24, 31), A::rlwinm(7, 4, 0, 28, 3), A::rlwimi(3, 4, 16, 8, 15),
                    A::rlwnm(8, 4, 6, 0, 31)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[5], 0x12U);
    CHECK_EQ(m.s().gpr[7], 0x10000008U); // wrapped mask MB > ME
    CHECK_EQ(m.s().gpr[3], 0xFF78FFFFU);
    CHECK_EQ(m.s().gpr[8], 0x23456781U); // rotate amount 36 & 31 = 4
}

TEST_CASE(compares_signed_unsigned_all_fields) {
    Machine m;
    m.s().gpr[3] = 0xFFFFFFFFU; m.s().gpr[4] = 1; m.s().xer = XER_SO;
    m.code(0x1000, {A::cmpw(7, 3, 4), A::cmplw(6, 3, 4), A::cmpwi(1, 3, -1), A::cmplwi(0, 4, 0xFFFF)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().cr & 0xFU, 0x9U);          // cr7: LT | SO
    CHECK_EQ((m.s().cr >> 4) & 0xFU, 0x5U);   // cr6: GT | SO
    CHECK_EQ((m.s().cr >> 24) & 0xFU, 0x3U);  // cr1: EQ | SO
    CHECK_EQ((m.s().cr >> 28) & 0xFU, 0x9U);  // cr0: LT | SO
    m.s().pc = 0x1000;
    CHECK(m.exec(A::cmpw(0, 3, 4) | (1U << 21)) == StepResult::InvalidForm); // L=1
}

// ------------------------------------------------------------- loads/stores
TEST_CASE(byte_halfword_loads_and_updates) {
    Machine m;
    m.bus.write32(0x2000, 0x8081FEFFU);
    m.s().gpr[4] = 0x1FFF;
    m.code(0x1000, {A::lbzu(3, 1, 4), A::lhzu(5, 1, 4), A::lhau(6, 0, 4), A::lbz(7, 3, 4)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x80U);
    CHECK_EQ(m.s().gpr[5], 0x81FEU);
    CHECK_EQ(m.s().gpr[6], 0xFFFF81FEU);
    CHECK_EQ(m.s().gpr[7], 0x00U); // 0x2001 + 3 = 0x2004
    CHECK_EQ(m.s().gpr[4], 0x2001U);
}

TEST_CASE(stores_all_sizes_and_negative_displacement) {
    Machine m;
    m.s().gpr[3] = 0xAABBCCDDU; m.s().gpr[4] = 0x2010;
    m.code(0x1000, {A::stb(3, -16, 4), A::sth(3, -14, 4), A::sthu(3, -8, 4), A::stbu(3, 1, 4)});
    CHECK(m.cpu->run(4) == StepResult::Executed);
    CHECK_EQ(m.r8(0x2000), 0xDDU);
    CHECK_EQ(m.r16(0x2002), 0xCCDDU);
    CHECK_EQ(m.r16(0x2008), 0xCCDDU);
    CHECK_EQ(m.r8(0x2009), 0xDDU);
    CHECK_EQ(m.s().gpr[4], 0x2009U);
}

TEST_CASE(indexed_and_byte_reversed) {
    Machine m;
    m.bus.write32(0x2004, 0x11223344U);
    m.s().gpr[4] = 0x2000; m.s().gpr[5] = 4; m.s().gpr[6] = 0xA1B2C3D4U;
    m.code(0x1000, {A::lwzx(3, 4, 5), A::lwbrx(7, 4, 5), A::lhbrx(8, 4, 5), A::stwbrx(6, 0, 4),
                    A::sthbrx(6, 4, 5), A::lhax(9, 4, 5), A::lwzux(10, 4, 5)});
    CHECK(m.cpu->run(7) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0x11223344U);
    CHECK_EQ(m.s().gpr[7], 0x44332211U);
    CHECK_EQ(m.s().gpr[8], 0x2211U);
    CHECK_EQ(m.r32(0x2000), 0xD4C3B2A1U);
    CHECK_EQ(m.r16(0x2004), 0xD4C3U);
    CHECK_EQ(m.s().gpr[9], 0xFFFFD4C3U);
    CHECK_EQ(m.s().gpr[10], 0xD4C33344U);
    CHECK_EQ(m.s().gpr[4], 0x2004U);
}

TEST_CASE(load_store_multiple) {
    Machine m;
    for (u32 r = 28; r < 32; ++r) m.s().gpr[r] = 0x1000U * r;
    m.s().gpr[1] = 0x3000;
    m.code(0x1000, {A::stmw(28, 8, 1)});
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(m.r32(0x3008), 0x1C000U);
    CHECK_EQ(m.r32(0x3014), 0x1F000U);
    for (u32 r = 28; r < 32; ++r) m.s().gpr[r] = 0;
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lmw(28, 8, 1)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[28], 0x1C000U);
    CHECK_EQ(m.s().gpr[31], 0x1F000U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lmw(28, 0, 29)) == StepResult::InvalidForm); // RA inside loaded range
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lmw(28, 2, 1)) == StepResult::ExceptionTaken); // misaligned -> alignment exception
    CHECK(m.cpu->last_exception() == Exception::Alignment);
    CHECK_EQ(m.s().dar, 0x3002U);
    CHECK_EQ(m.s().srr0, 0x1000U);
    CHECK_EQ(m.s().gpr[28], 0x1C000U); // unchanged
}

TEST_CASE(string_instructions) {
    Machine m;
    for (u32 i = 0; i < 8; ++i) m.bus.write8(0x2000 + i, static_cast<u8>(0x10 + i));
    m.s().gpr[4] = 0x2000; m.s().gpr[7] = 0xFFFFFFFFU;
    CHECK(m.exec(A::lswi(5, 4, 7)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[5], 0x10111213U);
    CHECK_EQ(m.s().gpr[6], 0x14151600U); // unfilled low byte cleared
    CHECK_EQ(m.s().gpr[7], 0xFFFFFFFFU); // untouched
    m.s().pc = 0x1000; m.s().gpr[3] = 0x2100; m.s().gpr[9] = 0; m.s().xer = 5;
    CHECK(m.exec(A::stswx(5, 3, 9)) == StepResult::Executed);
    CHECK_EQ(m.r32(0x2100), 0x10111213U);
    CHECK_EQ(m.r8(0x2104), 0x14U);
    CHECK_EQ(m.r8(0x2105), 0x00U);
    m.s().pc = 0x1000;
    CHECK(m.exec(A::lswi(3, 4, 8)) == StepResult::InvalidForm); // would load r4 (= RA)
    m.s().pc = 0x1000; m.s().xer = 0;
    const CpuState before = m.s();
    CHECK(m.exec(A::lswx(5, 3, 9)) == StepResult::Executed);    // byte count 0: no transfer
    CpuState after = m.s(); after.pc = before.pc;
    CHECK(after == before);
}

TEST_CASE(reservation_lwarx_stwcx) {
    Machine m;
    m.bus.write32(0x2000, 5);
    m.s().gpr[4] = 0x2000; m.s().gpr[6] = 99;
    m.code(0x1000, {A::lwarx(3, 0, 4), A::stwcx_(6, 0, 4), A::stwcx_(6, 0, 4)});
    CHECK(m.cpu->run(2) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 5U);
    CHECK_EQ(m.r32(0x2000), 99U);
    CHECK_EQ(CR0(m.s().cr), 2U);   // success
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(CR0(m.s().cr), 0U);   // reservation consumed -> fail
    // DMA write into the reserved granule cancels the reservation
    m.s().pc = 0x1000; m.s().gpr[6] = 7;
    CHECK(m.cpu->step() == StepResult::Executed);
    const std::array<u8, 4> dma{0, 0, 0, 1};
    m.bus.dma_write(0x201C, dma);
    CHECK(m.cpu->step() == StepResult::Executed);
    CHECK_EQ(CR0(m.s().cr), 0U);
    CHECK_EQ(m.r32(0x2000), 99U);
    // misaligned lwarx -> alignment exception
    m.s().pc = 0x1000; m.s().gpr[4] = 0x2002;
    CHECK(m.cpu->step() == StepResult::ExceptionTaken);
    CHECK(m.cpu->last_exception() == Exception::Alignment);
}

TEST_CASE(bus_error_stops_without_side_effects) {
    Machine m;
    m.s().gpr[4] = 0x01800000U; // first byte past MEM1
    m.s().gpr[3] = 0x12345678U;
    const CpuState before = m.s();
    CHECK(m.exec(A::lwzu(5, 0, 4)) == StepResult::MemoryFault);
    CHECK(m.s() == before);
    CHECK(m.exec(A::stwu(3, -2, 4)) == StepResult::MemoryFault); // straddles end of RAM
    CHECK(m.s() == before);
    CHECK_EQ(m.r16(0x017FFFFEU), 0U); // no partial store
    CHECK_EQ(m.cpu->last_stop().address, 0x017FFFFEU);
}

TEST_CASE(effective_address_wraps_modulo_2_32) {
    Machine m;
    m.bus.write32(0x10, 0xCAFEF00DU);
    m.s().gpr[4] = 0xFFFFFFF0U;
    CHECK(m.exec(A::lwz(3, 0x20, 4)) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 0xCAFEF00DU);
}

TEST_MAIN("cpu_integer")
