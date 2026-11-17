#include "orca/ppc/jit/arm64.hpp"
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"
#include <array>
#include <vector>
using namespace orca;
using namespace orca::ppc;
using namespace orca::ppc::jit;
using namespace orca::test;
namespace A = orca::test::ppc_asm;

TEST_CASE(jit_arm64_translates_supported_integer_prefix_and_stops_at_branch) {
    const std::array<u32, 5> code{A::addi(3, 0, -7), A::addis(4, 3, 2), A::ori(4, 4, 0x1234), A::b(8), A::addi(5, 0, 1)};
    const Block b = compile_arm64(code, 0x1000);
    CHECK_EQ(b.start_pc, 0x1000U);
    CHECK_EQ(b.guest_instructions, 3U);
    CHECK_EQ(b.end_pc, 0x100CU);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U); // RET
}

TEST_CASE(jit_arm64_translates_mulli_and_preserves_gpr_zero_as_source) {
    const std::array<u32, 2> code{A::mulli(4, 0, -3), A::mulli(0, 4, 7)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 2U);
    CHECK_EQ(b.end_pc, 0x1008U);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
}

TEST_CASE(jit_arm64_translates_addic_and_subfic_with_xer_carry_update) {
    const std::array<u32, 2> code{A::addic(3, 4, 1), A::subfic(5, 6, -2)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 2U);
    CHECK_EQ(b.end_pc, 0x1008U);
    // Each instruction must read NZCV and update only XER[CA].
    const auto has_word = [&](u32 word) {
        for (u32 emitted : b.host_words) if (emitted == word) return true;
        return false;
    };
    CHECK(has_word(0xD53B420CU)); // MRS x12, NZCV
    CHECK(has_word(0x531D758CU)); // UBFX w12, w12, #29, #1 (C flag)
    CHECK(has_word(0x5303098CU)); // LSL w12, w12, #29
}

TEST_CASE(jit_arm64_translates_addic_record_form_and_builds_cr0) {
    const std::array<u32, 1> code{A::addic_(3, 4, -1)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 1U);
    const auto has_word = [&](u32 word) {
        for (u32 emitted : b.host_words) if (emitted == word) return true;
        return false;
    };
    CHECK(has_word(0x7100017FU)); // CMP w11, #0
    CHECK(has_word(0x1A9FA7ECU)); // CSET w12, LT
    CHECK(has_word(0x1A9FD7EEU)); // CSET w14, GT
    CHECK(has_word(0x1A9F17EFU)); // CSET w15, EQ
    CHECK(has_word(0x5303098CU)); // LSL w12, w12, #29
}

TEST_CASE(jit_arm64_translates_add_addc_subf_subfc_without_overflow_enable) {
    const std::array<u32, 4> code{A::add(3, 4, 5), A::addc(6, 7, 8),
                                  A::subf(9, 10, 11), A::subfc(12, 13, 14)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 4U);
    CHECK_EQ(b.end_pc, 0x1010U);
    const auto has_word = [&](u32 word) {
        for (u32 emitted : b.host_words) if (emitted == word) return true;
        return false;
    };
    CHECK(has_word(0x0B0A012BU)); // ADD w11,w9,w10
    CHECK(has_word(0x2B0A012BU)); // ADDS w11,w9,w10 (addc)
    CHECK(has_word(0x2A2903E9U)); // MVN w9,w9 (subf/subfc source inversion)
    CHECK(has_word(0x6B09014BU)); // SUBS w11,w10,w9 (subfc)
}

TEST_CASE(jit_arm64_translates_adde_and_subfe_using_xer_carry_in) {
    const std::array<u32, 2> code{A::adde(3, 4, 5, false, true), A::subfe(6, 7, 8, false, true)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 2U);
    const auto has_word = [&](u32 word) {
        for (u32 emitted : b.host_words) if (emitted == word) return true;
        return false;
    };
    CHECK(has_word(0xD51B420CU)); // MSR NZCV, x12 (carry-in)
    CHECK(has_word(0x3A0A012BU)); // ADCS w11,w9,w10
    CHECK(has_word(0x7A09014BU)); // SBCS w11,w10,w9
    CHECK(has_word(0xD53B420CU)); // MRS NZCV (carry-out)
}

TEST_CASE(jit_arm64_does_not_translate_overflow_enabled_xo_arithmetic_yet) {
    const std::array<u32, 1> code{A::add(3, 4, 5, true, false)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 0U);
    CHECK(b.host_words.empty());
}


TEST_CASE(jit_arm64_translates_andi_and_andis_with_cr0_updates) {
    const std::array<u32, 2> code{A::andi_(3, 4, 0xF0F0), A::andis_(5, 6, 0x8001)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 2U);
    CHECK_EQ(b.end_pc, 0x1008U);
    const auto has_word = [&](u32 word) {
        for (u32 emitted : b.host_words) if (emitted == word) return true;
        return false;
    };
    CHECK(has_word(0x0A0A012BU)); // AND w11,w9,w10
    CHECK(has_word(0x7100017FU)); // CMP w11,#0 for CR0
}

TEST_CASE(jit_arm64_translates_all_register_logical_forms_and_record_bit) {
    const std::array<u32, 8> code{
        A::and_(3, 4, 5, true), A::andc(6, 7, 8), A::nor(9, 10, 11),
        A::eqv(12, 13, 14), A::xor_(15, 16, 17), A::orc(18, 19, 20),
        A::or_(21, 22, 23), A::nand(24, 25, 26)
    };
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, code.size());
    CHECK_EQ(b.end_pc, 0x1020U);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
}


TEST_CASE(jit_arm64_translates_rotate_mask_and_insert_forms) {
    const std::array<u32, 3> code{
        A::rlwinm(3, 4, 7, 4, 19, true),
        A::rlwimi(5, 6, 11, 8, 23, false),
        A::rlwnm(7, 8, 9, 0, 31, true)
    };
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, code.size());
    CHECK_EQ(b.end_pc, 0x100CU);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
    bool has_rorv = false;
    for (u32 emitted : b.host_words)
        has_rorv = has_rorv || ((emitted & 0xFFE0FC00U) == 0x1AC02C00U);
    CHECK(has_rorv);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_rotate_mask_matches_reference_interpreter) {
    Machine reference;
    const std::array<u32, 3> code{
        A::rlwinm(3, 4, 7, 4, 19, true),
        A::rlwimi(5, 6, 11, 8, 23, false),
        A::rlwnm(7, 8, 9, 0, 31, true)
    };
    reference.code(0x1000U, {code[0], code[1], code[2]});
    reference.s().gpr[4] = 0x81234567U;
    reference.s().gpr[5] = 0xA5A5A5A5U;
    reference.s().gpr[6] = 0xFEDCBA98U;
    reference.s().gpr[8] = 0x12345678U;
    reference.s().gpr[9] = 13U;
    reference.s().cr = 0x12345678U;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.gpr[4] = 0x81234567U;
    actual.gpr[5] = 0xA5A5A5A5U;
    actual.gpr[6] = 0xFEDCBA98U;
    actual.gpr[8] = 0x12345678U;
    actual.gpr[9] = 13U;
    actual.cr = 0x12345678U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_unsupported_first_instruction_produces_no_executable_block) {
    const std::array<u32, 1> code{A::b(4)};
    const Block b = compile_arm64(code, 0x1000);
    CHECK_EQ(b.guest_instructions, 0U);
    CHECK(b.host_words.empty());
}


TEST_CASE(jit_arm64_block_does_not_cross_guest_code_page) {
    const std::array<u32, 3> code{A::addi(3, 0, 1), A::addi(4, 0, 2), A::addi(5, 0, 3)};
    const Block b = compile_arm64(code, 0x1FF8U);
    CHECK_EQ(b.guest_instructions, 2U);
    CHECK_EQ(b.end_pc, 0x2000U);
}

TEST_CASE(jit_arm64_rejects_unaligned_entry_pc) {
    const std::array<u32, 1> code{A::addi(3, 0, 1)};
    const Block b = compile_arm64(code, 0x1002U);
    CHECK_EQ(b.guest_instructions, 0U);
    CHECK(b.host_words.empty());
}


TEST_CASE(jit_arm64_cpu_run_jit_preserves_reference_state_and_retirement) {
    Machine reference;
    Machine candidate;
    const std::array<u32, 8> code{A::addi(3, 0, -7), A::addis(4, 3, 2), A::ori(4, 4, 0x1234), A::xori(5, 4, 0x55AA), A::and_(6, 4, 5), A::xor_(7, 6, 3), A::mulli(0, 7, -17), A::mulli(9, 0, 13)};
    for (u32 ins : code) { reference.bus.write32(reference.s().pc, ins); candidate.bus.write32(candidate.s().pc, ins); reference.s().pc += 4U; candidate.s().pc += 4U; }
    reference.s().pc = 0x1000U;
    candidate.s().pc = 0x1000U;
    reference.s().gpr[8] = 0xAABBCCDDU;
    candidate.s().gpr[8] = 0xAABBCCDDU;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    CHECK(candidate.cpu->run_jit(code.size()) == StepResult::Executed);
    CHECK(candidate.s() == reference.s());
    CHECK_EQ(candidate.cpu->instructions_retired(), reference.cpu->instructions_retired());
}

TEST_CASE(jit_arm64_code_write_invalidates_cached_translation) {
    Machine m;
    m.code(0x1000U, {A::addi(3, 0, 1), A::addi(4, 0, 2)});
    CHECK(m.cpu->run_jit(2U) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 1U);
    CHECK_EQ(m.s().gpr[4], 2U);
    CHECK(m.bus.write32(0x1000U, A::addi(3, 0, 9)) == mem::BusStatus::Ok);
    m.s().pc = 0x1000U;
    CHECK(m.cpu->run_jit(1U) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 9U);
}


TEST_CASE(jit_arm64_cpu_run_jit_respects_bat_instruction_translation) {
    Machine reference;
    Machine candidate;
    const u32 physical = 0x1000U;
    const u32 effective = 0x80001000U;
    reference.code(physical, {A::addi(3, 0, 0x1234), A::ori(4, 3, 0x55)});
    candidate.code(physical, {A::addi(3, 0, 0x1234), A::ori(4, 3, 0x55)});
    reference.s().ibat[0] = {0x80001FFFU, 0x00000002U};
    candidate.s().ibat[0] = {0x80001FFFU, 0x00000002U};
    reference.s().msr = MSR_IR;
    candidate.s().msr = MSR_IR;
    reference.s().pc = effective;
    candidate.s().pc = effective;
    CHECK(reference.cpu->run(2U) == StepResult::Executed);
    CHECK(candidate.cpu->run_jit(2U) == StepResult::Executed);
    CHECK(candidate.s() == reference.s());
}

TEST_CASE(jit_arm64_run_honors_stop_pc_after_interpreted_branch) {
    Machine m;
    m.code(0x1000U, {A::b(8), A::addi(3, 0, 77)});
    const u32 stop_pc = 0x1008U;
    u64 completed = 0;
    CHECK(m.cpu->run_jit(4U, &completed, &stop_pc) == StepResult::Executed);
    CHECK_EQ(completed, 1U);
    CHECK_EQ(m.s().pc, stop_pc);
    CHECK_EQ(m.s().gpr[3], 0U);
}

TEST_CASE(jit_arm64_branch_uses_interpreter_fallback_at_block_boundary) {
    Machine m;
    m.code(0x1000U, {A::addi(3, 0, 5), A::b(8), A::addi(4, 0, 99), A::addi(5, 0, 7)});
    CHECK(m.cpu->run_jit(3U) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 5U);
    CHECK_EQ(m.s().gpr[4], 0U);
    CHECK_EQ(m.s().gpr[5], 7U);
    CHECK_EQ(m.s().pc, 0x1010U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_integer_state_matches_reference_interpreter) {
    Machine reference;
    const std::array<u32, 17> code{A::addi(3, 0, -7), A::addis(4, 3, 2), A::ori(4, 4, 0x1234), A::xori(5, 4, 0x55AA), A::and_(6, 4, 5), A::xor_(7, 6, 3), A::mulli(0, 7, -17), A::mulli(9, 0, 13), A::addic(10, 9, 1), A::subfic(11, 10, -5), A::addic_(12, 11, -1), A::add(13, 12, 9, false, true), A::addc(14, 13, 10, false, true), A::subf(15, 14, 11, false, true), A::subfc(16, 15, 12, false, true), A::adde(17, 16, 13, false, true), A::subfe(18, 17, 14, false, true)};
    reference.code(0x1000, {code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7],code[8],code[9],code[10],code[11],code[12],code[13],code[14],code[15],code[16]});
    reference.s().gpr[8] = 0xAABBCCDDU;
    reference.s().xer = XER_SO | XER_OV;
    reference.s().cr = 0x01234567U;
    CpuState expected = reference.s();
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000;
    actual.gpr[8] = 0xAABBCCDDU;
    actual.xer = XER_SO | XER_OV;
    actual.cr = 0x01234567U;
    const Block block = compile_arm64(code, 0x1000);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    for (unsigned i = 0; i < actual.gpr.size(); ++i) CHECK_EQ(actual.gpr[i], expected.gpr[i]);
    CHECK_EQ(actual.pc, expected.pc);
    CHECK_EQ(actual.cr, expected.cr);
    CHECK_EQ(actual.xer, expected.xer);
    CHECK_EQ(actual.lr, expected.lr);
    CHECK_EQ(actual.ctr, expected.ctr);
    CHECK(actual == expected);
}
#endif

TEST_MAIN("jit_arm64")
