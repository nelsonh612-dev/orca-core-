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
    CHECK_EQ(b.guest_instructions, 4U);
    CHECK_EQ(b.end_pc, 0x1010U);
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
    CHECK(has_word(0x6B09014BU)); // SUBS w11,w10,w9 (subf/subfc)
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

TEST_CASE(jit_arm64_translates_paired_single_unary_and_merge_operations) {
    const std::array<u32, 8> code{
        A::psX(40, 1, 0, 2, true), A::psX(72, 3, 0, 4),
        A::psX(136, 5, 0, 6), A::psX(264, 7, 0, 8),
        A::psX(528, 9, 10, 11), A::psX(560, 12, 13, 14),
        A::psX(592, 15, 16, 17), A::psX(624, 18, 19, 20)
    };
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(block.requires_fp);
    CHECK(block.requires_paired);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_paired_single_unary_and_merge_operations) {
    const std::array<u32, 8> code{
        A::psX(40, 1, 0, 2, true), A::psX(72, 3, 0, 4),
        A::psX(136, 5, 0, 6), A::psX(264, 7, 0, 8),
        A::psX(528, 9, 10, 11), A::psX(560, 12, 13, 14),
        A::psX(592, 15, 16, 17), A::psX(624, 18, 19, 20)
    };
    Machine reference;
    reference.code(0x1000U, {code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7]});
    reference.s().msr |= MSR_FP;
    reference.s().hid2 |= HID2_PSE;
    reference.s().cr = 0xA5963CC3U;
    for (unsigned i = 0; i < 32U; ++i) {
        reference.s().fpr[i].ps0 = 0xFFF0000000001000ULL + i;
        reference.s().fpr[i].ps1 = 0x7FF8000000002000ULL + i;
    }
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.msr = MSR_FP;
    actual.hid2 = HID2_PSE;
    actual.cr = 0xA5963CC3U;
    for (unsigned i = 0; i < 32U; ++i) {
        actual.fpr[i].ps0 = 0xFFF0000000001000ULL + i;
        actual.fpr[i].ps1 = 0x7FF8000000002000ULL + i;
    }
    const Block block = compile_arm64(code, 0x1000U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}

TEST_CASE(jit_arm64_run_jit_preserves_paired_single_unavailable_exception) {
    const u32 instruction = A::psX(40, 1, 0, 2);
    Machine reference;
    reference.code(0x1000U, {instruction});
    reference.s().msr |= MSR_FP;
    reference.s().hid2 &= ~HID2_PSE;
    CHECK(reference.cpu->step() == StepResult::ExceptionTaken);
    const CpuState expected = reference.s();

    Machine native_path;
    native_path.code(0x1000U, {instruction});
    native_path.s().msr |= MSR_FP;
    native_path.s().hid2 &= ~HID2_PSE;
    u64 completed = 0;
    CHECK(native_path.cpu->run_jit(1U, &completed) == StepResult::Executed);
    CHECK_EQ(completed, 1U);
    CHECK(native_path.s() == expected);
}
#endif

TEST_CASE(jit_arm64_translates_fpr_unary_sign_and_move_operations) {
    const std::array<u32, 4> code{
        A::fmr(1, 2), A::fneg(4, 5, true), A::fnabs(6, 7), A::fabs_(8, 9)
    };
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK_EQ(block.end_pc, 0x1010U);
    CHECK(block.requires_fp);
    CHECK(!block.host_words.empty());
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_fpr_unary_sign_and_move_operations) {
    const std::array<u32, 4> code{
        A::fmr(1, 2), A::fneg(4, 5, true), A::fnabs(6, 7), A::fabs_(8, 9)
    };
    Machine reference;
    reference.code(0x1000U, {code[0], code[1], code[2], code[3]});
    reference.s().msr |= MSR_FP;
    reference.s().cr = 0xA5963CC3U;
    for (unsigned i = 0; i < 32U; ++i) {
        reference.s().fpr[i].ps0 = 0xFFF0000000000001ULL + i;
        reference.s().fpr[i].ps1 = 0x7FF8000000000100ULL + i;
    }
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.msr = MSR_FP;
    actual.cr = 0xA5963CC3U;
    for (unsigned i = 0; i < 32U; ++i) {
        actual.fpr[i].ps0 = 0xFFF0000000000001ULL + i;
        actual.fpr[i].ps1 = 0x7FF8000000000100ULL + i;
    }
    const Block block = compile_arm64(code, 0x1000U);
    CHECK(block.requires_fp);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}

TEST_CASE(jit_arm64_run_jit_preserves_floating_point_unavailable_exception) {
    Machine reference;
    const u32 instruction = A::fmr(1, 2);
    reference.code(0x1000U, {instruction});
    reference.s().msr &= ~MSR_FP;
    const StepResult expected_result = reference.cpu->step();
    const CpuState expected = reference.s();

    Machine native_path;
    native_path.code(0x1000U, {instruction});
    native_path.s().msr &= ~MSR_FP;
    u64 completed = 0;
    const StepResult actual_result = native_path.cpu->run_jit(1U, &completed);
    // run_jit reports Executed when the requested budget is consumed, while
    // the exception delivery is reflected in CpuState and retirement count.
    CHECK(expected_result == StepResult::ExceptionTaken);
    CHECK(actual_result == StepResult::Executed);
    CHECK_EQ(completed, 1U);
    CHECK(native_path.s() == expected);
}
#endif

TEST_CASE(jit_arm64_translates_addze_addme_subfze_subfme_and_neg) {
    const std::array<u32, 5> code{
        A::addze(3, 4, false, true), A::addme(5, 6),
        A::subfze(7, 8), A::subfme(9, 10), A::neg(11, 12, false, true)
    };
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK_EQ(block.end_pc, 0x1014U);
    CHECK(!block.host_words.empty());
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_addze_addme_subfze_subfme_and_neg) {
    const std::array<u32, 5> code{
        A::addze(3, 4, false, true), A::addme(5, 6),
        A::subfze(7, 8), A::subfme(9, 10), A::neg(11, 12, false, true)
    };
    for (u32 instruction : code) {
        for (u32 carry : {0U, 1U}) {
            Machine reference;
            reference.code(0x1000U, {instruction});
            reference.s().gpr[4] = 0x7FFFFFFFU;
            reference.s().gpr[6] = 0x80000000U;
            reference.s().gpr[8] = 0xFFFFFFFFU;
            reference.s().gpr[10] = 0x00000001U;
            reference.s().gpr[12] = 0x80000000U;
            reference.s().xer = XER_SO | (carry != 0U ? XER_CA : 0U);
            reference.s().cr = 0xA5963CC3U;
            CHECK(reference.cpu->run(1U) == StepResult::Executed);
            const CpuState expected = reference.s();

            CpuState actual{};
            actual.pc = 0x1000U;
            actual.gpr[4] = 0x7FFFFFFFU;
            actual.gpr[6] = 0x80000000U;
            actual.gpr[8] = 0xFFFFFFFFU;
            actual.gpr[10] = 0x00000001U;
            actual.gpr[12] = 0x80000000U;
            actual.xer = XER_SO | (carry != 0U ? XER_CA : 0U);
            actual.cr = 0xA5963CC3U;
            const std::array<u32, 1> one{instruction};
            const Block block = compile_arm64(one, 0x1000U);
            CHECK_EQ(block.guest_instructions, 1U);
            CHECK(execute_arm64(block, actual));
            CHECK(actual == expected);
        }
    }
}
#endif

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_overflow_enabled_integer_arithmetic) {
    const std::array<u32, 11> code{
        A::add(3, 4, 5, true, true), A::addc(6, 7, 8, true, true),
        A::adde(9, 10, 11, true, true), A::addze(12, 13, true, true),
        A::addme(14, 15, true, true), A::subf(16, 17, 18, true, true),
        A::subfc(19, 20, 21, true, true), A::subfe(22, 23, 24, true, true),
        A::subfze(25, 26, true, true), A::subfme(27, 28, true, true),
        A::neg(29, 30, true, true)
    };
    for (std::size_t index = 0; index < code.size(); ++index) {
        for (u32 initial_xer : {0U, XER_SO | XER_OV, XER_CA | XER_OV}) {
            Machine reference;
            reference.code(0x1000U, {code[index]});
            reference.s().gpr[4] = 0x7FFFFFFFU; reference.s().gpr[5] = 1U;
            reference.s().gpr[7] = 0xFFFFFFFFU; reference.s().gpr[8] = 1U;
            reference.s().gpr[10] = 0x7FFFFFFFU; reference.s().gpr[11] = 0U;
            reference.s().gpr[13] = 0x7FFFFFFFU;
            reference.s().gpr[15] = 0x80000000U;
            reference.s().gpr[17] = 1U; reference.s().gpr[18] = 0x80000000U;
            reference.s().gpr[20] = 1U; reference.s().gpr[21] = 0x80000000U;
            reference.s().gpr[23] = 1U; reference.s().gpr[24] = 0x80000000U;
            reference.s().gpr[26] = 0x80000000U;
            reference.s().gpr[28] = 0x80000000U;
            reference.s().gpr[30] = 0x80000000U;
            reference.s().xer = initial_xer;
            reference.s().cr = 0xA5963CC3U;
            CHECK(reference.cpu->run(1U) == StepResult::Executed);
            const CpuState expected = reference.s();

            CpuState actual{};
            actual.pc = 0x1000U;
            actual.gpr = reference.s().gpr;
            // Restore pre-instruction state, not the reference result.
            actual.gpr[3] = 0U; actual.gpr[6] = 0U; actual.gpr[9] = 0U;
            actual.gpr[12] = 0U; actual.gpr[14] = 0U; actual.gpr[16] = 0U;
            actual.gpr[19] = 0U; actual.gpr[22] = 0U; actual.gpr[25] = 0U;
            actual.gpr[27] = 0U; actual.gpr[29] = 0U;
            actual.xer = initial_xer;
            actual.cr = 0xA5963CC3U;
            const std::array<u32, 1> one{code[index]};
            const Block block = compile_arm64(one, 0x1000U);
            CHECK_EQ(block.guest_instructions, 1U);
            CHECK(execute_arm64(block, actual));
            CHECK(actual == expected);
        }
    }
}
#endif

TEST_CASE(jit_arm64_translates_mullw_and_overflow_check) {
    const std::array<u32, 1> code{A::mullw(3, 4, 5, true, true)};
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(block.host_words.size() > 1U);
    CHECK(block.host_words.back() == 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_mullw_overflow_and_sticky_so) {
    const std::array<std::array<u32, 2>, 3> operands{{
        {{0x7FFFFFFFU, 2U}}, {{0xFFFFFFFEU, 3U}}, {{0x80000000U, 0xFFFFFFFFU}}
    }};
    for (const auto& pair : operands) {
        for (u32 initial_xer : {0U, XER_SO | XER_OV}) {
            const u32 instruction = A::mullw(3, 4, 5, true, true);
            Machine reference;
            reference.code(0x1000U, {instruction});
            reference.s().gpr[4] = pair[0];
            reference.s().gpr[5] = pair[1];
            reference.s().xer = initial_xer;
            reference.s().cr = 0xA5963CC3U;
            CHECK(reference.cpu->run(1U) == StepResult::Executed);
            const CpuState expected = reference.s();

            CpuState actual{};
            actual.pc = 0x1000U;
            actual.gpr[4] = pair[0];
            actual.gpr[5] = pair[1];
            actual.xer = initial_xer;
            actual.cr = 0xA5963CC3U;
            const std::array<u32, 1> one{instruction};
            const Block block = compile_arm64(one, 0x1000U);
            CHECK(execute_arm64(block, actual));
            CHECK(actual == expected);
        }
    }
}
#endif

TEST_CASE(jit_arm64_translates_overflow_enabled_xo_arithmetic) {
    const std::array<u32, 4> code{
        A::add(3, 4, 5, true), A::addc(6, 7, 8, true),
        A::subf(9, 10, 11, true), A::neg(12, 13, true)
    };
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK_EQ(block.end_pc, 0x1010U);
    CHECK(!block.host_words.empty());
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
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


TEST_CASE(jit_arm64_translates_integer_shift_and_sign_extension_forms) {
    const std::array<u32, 7> code{
        A::slw(3, 4, 5), A::srw(6, 7, 8),
        A::sraw(9, 10, 11, true), A::srawi(12, 13, 7, true),
        A::cntlzw(14, 15), A::extsb(16, 17), A::extsh(18, 19)
    };
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, code.size());
    CHECK_EQ(b.end_pc, 0x101CU);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_integer_shifts_match_reference_interpreter) {
    Machine reference;
    const std::array<u32, 7> code{
        A::slw(3, 4, 5), A::srw(6, 7, 8),
        A::sraw(9, 10, 11, true), A::srawi(12, 13, 7, true),
        A::cntlzw(14, 15), A::extsb(16, 17), A::extsh(18, 19)
    };
    reference.code(0x1000U, {code[0], code[1], code[2], code[3], code[4], code[5], code[6]});
    reference.s().gpr[4] = 0x81234567U; reference.s().gpr[5] = 5U;
    reference.s().gpr[7] = 0xF1234567U; reference.s().gpr[8] = 36U;
    reference.s().gpr[10] = 0x80000003U; reference.s().gpr[11] = 36U;
    reference.s().gpr[13] = 0x80000003U;
    reference.s().gpr[15] = 0x00100000U;
    reference.s().gpr[17] = 0x00000080U; reference.s().gpr[19] = 0x00008001U;
    reference.s().cr = 0x12345678U;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.gpr[4] = 0x81234567U; actual.gpr[5] = 5U;
    actual.gpr[7] = 0xF1234567U; actual.gpr[8] = 36U;
    actual.gpr[10] = 0x80000003U; actual.gpr[11] = 36U;
    actual.gpr[13] = 0x80000003U;
    actual.gpr[15] = 0x00100000U;
    actual.gpr[17] = 0x00000080U; actual.gpr[19] = 0x00008001U;
    actual.cr = 0x12345678U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif



#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_logical_record_forms_match_reference_interpreter) {
    Machine reference;
    const std::array<u32, 10> code{
        A::andi_(3, 4, 0xF0F0), A::andis_(5, 6, 0x8001),
        A::and_(7, 8, 9, true), A::andc(10, 11, 12, true),
        A::nor(13, 14, 15, true), A::eqv(16, 17, 18, true),
        A::xor_(19, 20, 21, true), A::orc(22, 23, 24, true),
        A::or_(25, 26, 27, true), A::nand(28, 29, 30, true)
    };
    reference.code(0x1000U, {code[0],code[1],code[2],code[3],code[4],
                             code[5],code[6],code[7],code[8],code[9]});
    for (unsigned i = 4; i < 31; ++i)
        reference.s().gpr[i] = 0x9E3779B9U * static_cast<u32>(i);
    reference.s().xer = XER_SO | XER_OV;
    reference.s().cr = 0xA5A5A5A5U;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    for (unsigned i = 4; i < 31; ++i)
        actual.gpr[i] = 0x9E3779B9U * static_cast<u32>(i);
    actual.xer = XER_SO | XER_OV;
    actual.cr = 0xA5A5A5A5U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif


TEST_CASE(jit_arm64_translates_mcrxr) {
    const std::array<u32, 1> code{A::mcrxr(3)};
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK_EQ(block.end_pc, 0x1004U);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_mcrxr_matches_interpreter) {
    const std::array<u32, 1> code{A::mcrxr(3)};
    Machine reference;
    reference.code(0x1000U, {code[0]});
    reference.s().xer = 0xF1234567U;
    reference.s().cr = 0xA5963CC3U;
    CHECK(reference.cpu->run(1U) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.xer = 0xF1234567U;
    actual.cr = 0xA5963CC3U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_translates_cr_field_moves_and_gpr_transfers) {
    const std::array<u32, 3> code{A::mcrf(2, 5), A::mfcr(7), A::mtcrf(0xA5, 8)};
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK_EQ(block.end_pc, 0x100CU);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_cr_field_moves_and_gpr_transfers) {
    const std::array<u32, 3> code{A::mcrf(2, 5), A::mfcr(7), A::mtcrf(0xA5, 8)};
    Machine reference;
    reference.code(0x1000U, {code[0],code[1],code[2]});
    reference.s().cr = 0xA5963CC3U;
    reference.s().gpr[8] = 0x5AA5F00FU;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.cr = 0xA5963CC3U;
    actual.gpr[8] = 0x5AA5F00FU;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_translates_condition_register_logical_forms) {
    const std::array<u32, 8> code{
        A::crand(0, 1, 2), A::crandc(3, 4, 5), A::cror(6, 7, 8),
        A::crorc(9, 10, 11), A::crxor(12, 13, 14), A::crnand(15, 16, 17),
        A::crnor(18, 19, 20), A::creqv(21, 22, 23)
    };
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK_EQ(block.end_pc, 0x1020U);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_condition_register_logical_forms_match_interpreter) {
    const std::array<u32, 8> code{
        A::crand(0, 1, 2), A::crandc(3, 4, 5), A::cror(6, 7, 8),
        A::crorc(9, 10, 11), A::crxor(12, 13, 14), A::crnand(15, 16, 17),
        A::crnor(18, 19, 20), A::creqv(21, 22, 23)
    };
    for (u32 instruction : code) {
        Machine reference;
        reference.code(0x1000U, {instruction});
        reference.s().cr = 0xA5963CC3U;
        CHECK(reference.cpu->run(1U) == StepResult::Executed);
        const CpuState expected = reference.s();

        CpuState actual{};
        actual.pc = 0x1000U;
        actual.cr = 0xA5963CC3U;
        const std::array<u32, 1> one{instruction};
        const Block block = compile_arm64(one, 0x1000U);
        CHECK_EQ(block.guest_instructions, 1U);
        CHECK(execute_arm64(block, actual));
        CHECK(actual == expected);
    }
}
#endif

TEST_CASE(jit_arm64_translates_signed_and_unsigned_compare_forms) {
    const std::array<u32, 4> code{
        A::cmpwi(0, 3, -5), A::cmplwi(1, 4, 0xFF00),
        A::cmpw(2, 5, 6), A::cmplw(3, 7, 8)
    };
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, code.size());
    CHECK_EQ(b.end_pc, 0x1010U);
    CHECK(!b.host_words.empty());
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_compare_forms_match_reference_interpreter) {
    Machine reference;
    const std::array<u32, 4> code{
        A::cmpwi(0, 3, -5), A::cmplwi(1, 4, 0xFF00),
        A::cmpw(2, 5, 6), A::cmplw(3, 7, 8)
    };
    reference.code(0x1000U, {code[0],code[1],code[2],code[3]});
    reference.s().gpr[3] = 0xFFFFFFFCU;
    reference.s().gpr[4] = 0x0000FE00U;
    reference.s().gpr[5] = 0x80000000U;
    reference.s().gpr[6] = 1U;
    reference.s().gpr[7] = 0xFFFFFFFFU;
    reference.s().gpr[8] = 0x7FFFFFFFU;
    reference.s().xer = XER_SO;
    reference.s().cr = 0x12345678U;
    CHECK(reference.cpu->run(code.size()) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.gpr[3] = 0xFFFFFFFCU;
    actual.gpr[4] = 0x0000FE00U;
    actual.gpr[5] = 0x80000000U;
    actual.gpr[6] = 1U;
    actual.gpr[7] = 0xFFFFFFFFU;
    actual.gpr[8] = 0x7FFFFFFFU;
    actual.xer = XER_SO;
    actual.cr = 0x12345678U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, code.size());
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_translates_unconditional_branch_and_link_as_block_exit) {
    const std::array<u32, 2> code{A::addi(3, 0, 7), A::b(8, false, true)};
    const Block b = compile_arm64(code, 0x1000U);
    CHECK_EQ(b.guest_instructions, 2U);
    CHECK_EQ(b.end_pc, 0x1008U);
    CHECK_EQ(b.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_conditional_branch_matches_interpreter) {
    Machine reference;
    const std::array<u32, 1> code{A::bc(12, 2, 16, false, true)};
    reference.code(0x1000U, {code[0]});
    reference.s().cr = 0x20000000U; // CR bit BI=2 is set
    reference.s().lr = 0xCAFEBABEU;
    CHECK(reference.cpu->run(1U) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.cr = 0x20000000U;
    actual.lr = 0xCAFEBABEU;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_translates_indirect_branch_forms_as_block_exits) {
    const std::array<u32, 2> code{A::bclr(20, 0), A::bcctr(20, 0, true)};
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK_EQ(block.end_pc, 0x1004U);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_bclr_and_bcctr_match_interpreter) {
    for (u32 instruction : {A::bclr(20, 0), A::bcctr(20, 0, true)}) {
        Machine reference;
        reference.code(0x1000U, {instruction});
        reference.s().lr = 0x2003U;
        reference.s().ctr = 0x3003U;
        CHECK(reference.cpu->run(1U) == StepResult::Executed);
        const CpuState expected = reference.s();

        CpuState actual{};
        actual.pc = 0x1000U;
        actual.lr = 0x2003U;
        actual.ctr = 0x3003U;
        const std::array<u32, 1> one{instruction};
        const Block block = compile_arm64(one, 0x1000U);
        CHECK_EQ(block.guest_instructions, 1U);
        CHECK(execute_arm64(block, actual));
        CHECK(actual == expected);
    }
}
#endif

TEST_CASE(jit_arm64_translates_conditional_branch_as_block_exit) {
    const std::array<u32, 1> code{A::bc(12, 2, -8, true, false)};
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK_EQ(block.end_pc, 0x1004U);
    CHECK_EQ(block.host_words.back(), 0xD65F03C0U);
}

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_conditional_branch_not_taken_updates_link_and_falls_through) {
    Machine reference;
    const std::array<u32, 1> code{A::bc(12, 2, 16, false, true)};
    reference.code(0x1000U, {code[0]});
    reference.s().cr = 0U; // CR bit BI=2 is clear: branch not taken
    reference.s().lr = 0xCAFEBABEU;
    CHECK(reference.cpu->run(1U) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.cr = 0U;
    actual.lr = 0xCAFEBABEU;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_conditional_branch_decrements_ctr_and_takes) {
    Machine reference;
    const std::array<u32, 1> code{A::bc(0, 0, 20, false, false)};
    reference.code(0x1000U, {code[0]});
    reference.s().ctr = 2U;
    reference.s().cr = 0U; // BO requests CR[0] == 0
    CHECK(reference.cpu->run(1U) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.ctr = 2U;
    actual.cr = 0U;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

#if defined(__aarch64__)
TEST_CASE(jit_arm64_differential_unconditional_branch_and_link_matches_interpreter) {
    Machine reference;
    const std::array<u32, 1> code{A::b(12, false, true)};
    reference.code(0x1000U, {code[0]});
    reference.s().lr = 0xDEADBEEFU;
    CHECK(reference.cpu->run(1U) == StepResult::Executed);
    const CpuState expected = reference.s();

    CpuState actual{};
    actual.pc = 0x1000U;
    actual.lr = 0xDEADBEEFU;
    const Block block = compile_arm64(code, 0x1000U);
    CHECK_EQ(block.guest_instructions, 1U);
    CHECK(execute_arm64(block, actual));
    CHECK(actual == expected);
}
#endif

TEST_CASE(jit_arm64_unsupported_first_instruction_produces_no_executable_block) {
    // bcctr with BO[2] clear is architecturally invalid and must stay in fallback.
    const std::array<u32, 1> code{A::bcctr(0, 0)};
    const Block b = compile_arm64(code, 0x1000U);
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
