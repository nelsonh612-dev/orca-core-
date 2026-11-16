#include "orca/ppc/jit/arm64.hpp"
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"
#include <array>
#include <cstdio>
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

TEST_CASE(jit_arm64_unsupported_first_instruction_produces_no_executable_block) {
    const std::array<u32, 1> code{0xFFFFFFFFU};
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

TEST_CASE(jit_arm64_run_honors_stop_pc_after_native_branch) {
    Machine m;
    m.code(0x1000U, {A::b(8), A::addi(3, 0, 77)});
    const u32 stop_pc = 0x1008U;
    u64 completed = 0;
    CHECK(m.cpu->run_jit(4U, &completed, &stop_pc) == StepResult::Executed);
    CHECK_EQ(completed, 1U);
    CHECK_EQ(m.s().pc, stop_pc);
    CHECK_EQ(m.s().gpr[3], 0U);
}

TEST_CASE(jit_arm64_native_branch_exits_block_and_continues_at_target) {
    Machine m;
    m.code(0x1000U, {A::addi(3, 0, 5), A::b(8), A::addi(4, 0, 99), A::addi(5, 0, 7)});
    CHECK(m.cpu->run_jit(3U) == StepResult::Executed);
    CHECK_EQ(m.s().gpr[3], 5U);
    CHECK_EQ(m.s().gpr[4], 0U);
    CHECK_EQ(m.s().gpr[5], 7U);
    CHECK_EQ(m.s().pc, 0x1010U);
}


#if defined(__aarch64__)
TEST_CASE(jit_arm64_executes_direct_branch_and_link) {
    const std::array<u32,1> code{A::b(0x20,false,true)};
    const Block block=compile_arm64(code,0x1000U);
    CHECK_EQ(block.guest_instructions,1U);
    CpuState state{};
    state.pc=0x1000U;
    CHECK(execute_arm64(block,state));
    CHECK_EQ(state.pc,0x1020U);
    CHECK_EQ(state.lr,0x1004U);
}

TEST_CASE(jit_arm64_executes_conditional_branch_on_cr_bit) {
    const std::array<u32,1> code{A::bc(12U,0U,8)};
    const Block block=compile_arm64(code,0x1000U);
    CHECK_EQ(block.guest_instructions,1U);
    CpuState taken{};
    taken.pc=0x1000U;
    taken.cr=0x80000000U;
    CHECK(execute_arm64(block,taken));
    CHECK_EQ(taken.pc,0x1008U);
    CpuState not_taken{};
    not_taken.pc=0x1000U;
    CHECK(execute_arm64(block,not_taken));
    CHECK_EQ(not_taken.pc,0x1004U);
}


TEST_CASE(jit_arm64_executes_common_bclr_and_bcctr_forms) {
    const std::array<u32,1> bclr_code{A::bclr(20U,0U)};
    const Block bclr_block=compile_arm64(bclr_code,0x1000U);
    CpuState link_state{};
    link_state.pc=0x1000U;
    link_state.lr=0x2003U;
    CHECK(execute_arm64(bclr_block,link_state));
    CHECK_EQ(link_state.pc,0x2000U);

    const std::array<u32,1> bcctr_code{A::bcctr(20U,0U)};
    const Block bcctr_block=compile_arm64(bcctr_code,0x1000U);
    CpuState ctr_state{};
    ctr_state.pc=0x1000U;
    ctr_state.ctr=0x3003U;
    CHECK(execute_arm64(bcctr_block,ctr_state));
    CHECK_EQ(ctr_state.pc,0x3000U);

    const std::array<u32,1> link_code{A::bclr(20U,0U,true)};
    const Block link_block=compile_arm64(link_code,0x1000U);
    CpuState link_and_return{};
    link_and_return.pc=0x1000U;
    link_and_return.lr=0x4000U;
    CHECK(execute_arm64(link_block,link_and_return));
    CHECK_EQ(link_and_return.pc,0x4000U);
    CHECK_EQ(link_and_return.lr,0x1004U);
}

TEST_CASE(jit_arm64_executes_conditional_branch_with_ctr_decrement) {
    const std::array<u32,1> code{A::bc(16U,0U,8)};
    const Block block=compile_arm64(code,0x1000U);
    CpuState state{};
    state.pc=0x1000U;
    state.ctr=2U;
    CHECK(execute_arm64(block,state));
    CHECK_EQ(state.ctr,1U);
    CHECK_EQ(state.pc,0x1008U);
    CpuState zero{};
    zero.pc=0x1000U;
    zero.ctr=1U;
    CHECK(execute_arm64(block,zero));
    CHECK_EQ(zero.ctr,0U);
    CHECK_EQ(zero.pc,0x1004U);
}
#endif

#if defined(__aarch64__)

TEST_CASE(jit_arm64_executes_andi_and_andis_with_cr0_updates) {
    Machine reference;
    Machine candidate;
    const std::array<u32,2> code{A::andi_(3U,4U,0xF0F0U),A::andis_(5U,4U,0xF0F0U)};
    reference.code(0x1000U,{code[0],code[1]});
    candidate.code(0x1000U,{code[0],code[1]});
    reference.s().gpr[4]=0xFFFF00FFU;
    candidate.s().gpr[4]=0xFFFF00FFU;
    CHECK(reference.cpu->run(2U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(2U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0x000000F0U);
    CHECK_EQ(candidate.s().gpr[5],0xF0F00000U);
}




TEST_CASE(jit_arm64_executes_slw_srw_and_zeroes_shift_counts_at_least_32) {
    Machine reference;
    Machine candidate;
    const std::array<u32,4> code{
        A::slw(3U,4U,5U,true), A::srw(6U,7U,8U,true),
        A::slw(9U,10U,11U,false), A::srw(12U,13U,14U,false)
    };
    reference.code(0x1000U,{code[0],code[1],code[2],code[3]});
    candidate.code(0x1000U,{code[0],code[1],code[2],code[3]});
    reference.s().gpr[4]=0x12345678U; candidate.s().gpr[4]=0x12345678U;
    reference.s().gpr[5]=4U; candidate.s().gpr[5]=4U;
    reference.s().gpr[7]=0x12345678U; candidate.s().gpr[7]=0x12345678U;
    reference.s().gpr[8]=4U; candidate.s().gpr[8]=4U;
    reference.s().gpr[10]=0xFFFFFFFFU; candidate.s().gpr[10]=0xFFFFFFFFU;
    reference.s().gpr[11]=32U; candidate.s().gpr[11]=32U;
    reference.s().gpr[13]=0xFFFFFFFFU; candidate.s().gpr[13]=0xFFFFFFFFU;
    reference.s().gpr[14]=32U; candidate.s().gpr[14]=32U;
    CHECK(reference.cpu->run(4U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(4U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0x23456780U);
    CHECK_EQ(candidate.s().gpr[6],0x01234567U);
    CHECK_EQ(candidate.s().gpr[9],0U);
    CHECK_EQ(candidate.s().gpr[12],0U);
}









TEST_CASE(jit_arm64_executes_all_cr_logical_boolean_operations) {
    Machine reference;
    Machine candidate;
    const std::array<u32,8> code{
        A::crand(0U,1U,2U), A::crandc(3U,4U,5U),
        A::creqv(6U,7U,8U), A::crnand(9U,10U,11U),
        A::crnor(12U,13U,14U), A::cror(15U,16U,17U),
        A::crorc(18U,19U,20U), A::crxor(21U,22U,23U)
    };
    reference.code(0x1000U,{code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7]});
    candidate.code(0x1000U,{code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7]});
    reference.s().cr=0xA5C3F00FU; candidate.s().cr=0xA5C3F00FU;
    CHECK(reference.cpu->run(8U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(8U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
}


TEST_CASE(jit_arm64_diagnostic_isolates_cr_boolean_operation) {
    const std::array<u32,8> code{
        A::crand(0U,1U,2U), A::crandc(3U,4U,5U),
        A::creqv(6U,7U,8U), A::crnand(9U,10U,11U),
        A::crnor(12U,13U,14U), A::cror(15U,16U,17U),
        A::crorc(18U,19U,20U), A::crxor(21U,22U,23U)
    };
    for (std::size_t i=0; i<code.size(); ++i) {
        Machine reference;
        Machine candidate;
        reference.code(0x1000U,{code[i]});
        candidate.code(0x1000U,{code[i]});
        reference.s().cr=0xA5C3F00FU;
        candidate.s().cr=0xA5C3F00FU;
        const auto ref_result=reference.cpu->run(1U);
        const auto jit_result=candidate.cpu->run_jit(1U);
        if (reference.s()!=candidate.s()) {
            std::printf("CR logical diagnostic index=%zu ref=%08x jit=%08x pc_ref=%08x pc_jit=%08x\\n",
                i, reference.s().cr, candidate.s().cr, reference.s().pc, candidate.s().pc);
            CHECK(false);
        }
        CHECK(ref_result==StepResult::Executed);
        CHECK(jit_result==StepResult::Executed);
    }
}

TEST_CASE(jit_arm64_executes_mcrf_copy_between_cr_fields) {
    Machine reference;
    Machine candidate;
    const std::array<u32,1> code{A::mcrf(6U,1U)};
    reference.code(0x1000U,{code[0]});
    candidate.code(0x1000U,{code[0]});
    reference.s().cr=0x12345678U; candidate.s().cr=0x12345678U;
    CHECK(reference.cpu->run(1U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(1U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().cr,0x12345628U);
}

TEST_CASE(jit_arm64_executes_mcrxr_and_clears_xer_status_nibble) {
    Machine reference;
    Machine candidate;
    const std::array<u32,1> code{A::mcrxr(5U)};
    reference.code(0x1000U,{code[0]});
    candidate.code(0x1000U,{code[0]});
    reference.s().cr=0xABCDEF01U; candidate.s().cr=0xABCDEF01U;
    reference.s().xer=0xE1234567U; candidate.s().xer=0xE1234567U;
    CHECK(reference.cpu->run(1U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(1U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().cr,0xABCDEE01U);
    CHECK_EQ(candidate.s().xer,0x01234567U);
}

TEST_CASE(jit_arm64_executes_mfcr_and_masked_mtcrf) {
    Machine reference;
    Machine candidate;
    const std::array<u32,2> code{A::mtcrf(0xAAU,4U),A::mfcr(3U)};
    reference.code(0x1000U,{code[0],code[1]});
    candidate.code(0x1000U,{code[0],code[1]});
    reference.s().cr=0xABCDEF01U; candidate.s().cr=0xABCDEF01U;
    reference.s().gpr[4]=0x12345678U; candidate.s().gpr[4]=0x12345678U;
    CHECK(reference.cpu->run(2U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(2U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().cr,0x1B3D5F71U);
    CHECK_EQ(candidate.s().gpr[3],0x1B3D5F71U);
}

TEST_CASE(jit_arm64_executes_signed_unsigned_compare_and_updates_selected_cr_fields) {
    Machine reference;
    Machine candidate;
    const std::array<u32,4> code{
        A::cmpwi(2U,4U,-1), A::cmplwi(3U,5U,0xFFFFU),
        A::cmpw(1U,6U,7U), A::cmplw(4U,8U,9U)
    };
    reference.code(0x1000U,{code[0],code[1],code[2],code[3]});
    candidate.code(0x1000U,{code[0],code[1],code[2],code[3]});
    reference.s().gpr[4]=0U; candidate.s().gpr[4]=0U;
    reference.s().gpr[5]=0x0000FFFFU; candidate.s().gpr[5]=0x0000FFFFU;
    reference.s().gpr[6]=0xFFFFFFFFU; candidate.s().gpr[6]=0xFFFFFFFFU;
    reference.s().gpr[7]=1U; candidate.s().gpr[7]=1U;
    reference.s().gpr[8]=0xFFFFFFFFU; candidate.s().gpr[8]=0xFFFFFFFFU;
    reference.s().gpr[9]=1U; candidate.s().gpr[9]=1U;
    reference.s().xer=XER_SO; candidate.s().xer=XER_SO;
    CHECK(reference.cpu->run(4U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(4U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().cr,0x09535000U);
}

TEST_CASE(jit_arm64_executes_signed_and_unsigned_high_word_multiply) {
    Machine reference;
    Machine candidate;
    const std::array<u32,2> code{A::mulhw(3U,4U,5U,true),A::mulhwu(6U,7U,8U,true)};
    reference.code(0x1000U,{code[0],code[1]});
    candidate.code(0x1000U,{code[0],code[1]});
    reference.s().gpr[4]=0x80000000U; candidate.s().gpr[4]=0x80000000U;
    reference.s().gpr[5]=2U; candidate.s().gpr[5]=2U;
    reference.s().gpr[7]=0xFFFFFFFFU; candidate.s().gpr[7]=0xFFFFFFFFU;
    reference.s().gpr[8]=0xFFFFFFFFU; candidate.s().gpr[8]=0xFFFFFFFFU;
    CHECK(reference.cpu->run(2U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(2U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0xFFFFFFFFU);
    CHECK_EQ(candidate.s().gpr[6],0xFFFFFFFEU);
}

TEST_CASE(jit_arm64_executes_mullw_low_word_multiply) {
    Machine reference;
    Machine candidate;
    const std::array<u32,2> code{A::mullw(3U,4U,5U,false,true),A::mullw(6U,7U,8U,false,true)};
    reference.code(0x1000U,{code[0],code[1]});
    candidate.code(0x1000U,{code[0],code[1]});
    reference.s().gpr[4]=0x12345678U; candidate.s().gpr[4]=0x12345678U;
    reference.s().gpr[5]=16U; candidate.s().gpr[5]=16U;
    reference.s().gpr[7]=0xFFFFFFFEU; candidate.s().gpr[7]=0xFFFFFFFEU;
    reference.s().gpr[8]=3U; candidate.s().gpr[8]=3U;
    CHECK(reference.cpu->run(2U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(2U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0x23456780U);
    CHECK_EQ(candidate.s().gpr[6],0xFFFFFFFAU);
}

TEST_CASE(jit_arm64_executes_sraw_variable_shift_and_xer_carry) {
    Machine reference;
    Machine candidate;
    const std::array<u32,4> code{
        A::sraw(3U,4U,5U,true), A::sraw(6U,7U,8U,true),
        A::sraw(9U,10U,11U,true), A::sraw(12U,13U,14U,true)
    };
    reference.code(0x1000U,{code[0],code[1],code[2],code[3]});
    candidate.code(0x1000U,{code[0],code[1],code[2],code[3]});
    reference.s().gpr[4]=0xFFFFFFF9U; candidate.s().gpr[4]=0xFFFFFFF9U;
    reference.s().gpr[5]=2U; candidate.s().gpr[5]=2U;
    reference.s().gpr[7]=7U; candidate.s().gpr[7]=7U;
    reference.s().gpr[8]=2U; candidate.s().gpr[8]=2U;
    reference.s().gpr[10]=0xFFFFFFF9U; candidate.s().gpr[10]=0xFFFFFFF9U;
    reference.s().gpr[11]=32U; candidate.s().gpr[11]=32U;
    reference.s().gpr[13]=0xFFFFFFF9U; candidate.s().gpr[13]=0xFFFFFFF9U;
    reference.s().gpr[14]=0U; candidate.s().gpr[14]=0U;
    CHECK(reference.cpu->run(4U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(4U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0xFFFFFFFEU);
    CHECK_EQ(candidate.s().gpr[6],1U);
    CHECK_EQ(candidate.s().gpr[9],0xFFFFFFFFU);
    CHECK_EQ(candidate.s().gpr[12],0xFFFFFFF9U);
}

TEST_CASE(jit_arm64_executes_srawi_with_exact_xer_carry) {
    Machine reference;
    Machine candidate;
    const std::array<u32,1> code{A::srawi(3U,4U,2U,true)};
    reference.code(0x1000U,{code[0]});
    candidate.code(0x1000U,{code[0]});
    reference.s().gpr[4]=0xFFFFFFF9U;
    candidate.s().gpr[4]=0xFFFFFFF9U;
    reference.s().xer=XER_SO;
    candidate.s().xer=XER_SO;
    CHECK(reference.cpu->run(1U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(1U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0xFFFFFFFEU);
    CHECK((candidate.s().xer&XER_CA)!=0U);
}

TEST_CASE(jit_arm64_executes_sign_extend_clz_and_logical_xo_operations) {
    Machine reference;
    Machine candidate;
    const std::array<u32,7> code{
        A::extsb(3U,4U,true), A::extsh(5U,6U,true), A::cntlzw(7U,8U,true),
        A::nand(9U,10U,11U,true), A::nor(12U,13U,14U,true),
        A::eqv(15U,16U,17U,true), A::andc(18U,19U,20U,true)
    };
    reference.code(0x1000U,{code[0],code[1],code[2],code[3],code[4],code[5],code[6]});
    candidate.code(0x1000U,{code[0],code[1],code[2],code[3],code[4],code[5],code[6]});
    reference.s().gpr[4]=0x00000080U; candidate.s().gpr[4]=0x00000080U;
    reference.s().gpr[6]=0x00008001U; candidate.s().gpr[6]=0x00008001U;
    reference.s().gpr[8]=1U; candidate.s().gpr[8]=1U;
    reference.s().gpr[10]=0x12345678U; candidate.s().gpr[10]=0x12345678U;
    reference.s().gpr[11]=0xFFFF0000U; candidate.s().gpr[11]=0xFFFF0000U;
    reference.s().gpr[13]=0xAAAA5555U; candidate.s().gpr[13]=0xAAAA5555U;
    reference.s().gpr[14]=0x0F0F0F0FU; candidate.s().gpr[14]=0x0F0F0F0FU;
    reference.s().gpr[16]=0x12345678U; candidate.s().gpr[16]=0x12345678U;
    reference.s().gpr[17]=0xFFFF0000U; candidate.s().gpr[17]=0xFFFF0000U;
    reference.s().gpr[19]=0xFF00FF00U; candidate.s().gpr[19]=0xFF00FF00U;
    reference.s().gpr[20]=0x0F0F0F0FU; candidate.s().gpr[20]=0x0F0F0F0FU;
    CHECK(reference.cpu->run(code.size())==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(code.size())==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0xFFFFFF80U);
    CHECK_EQ(candidate.s().gpr[5],0xFFFF8001U);
    CHECK_EQ(candidate.s().gpr[7],31U);
}

TEST_CASE(jit_arm64_executes_rlwinm_and_rlwimi_against_interpreter) {
    Machine reference;
    Machine candidate;
    const std::array<u32,3> code{
        A::rlwinm(3U,4U,8U,0U,23U,true),
        A::rlwimi(5U,6U,8U,8U,15U,true),
        A::rlwnm(7U,8U,9U,0U,23U,true)
    };
    reference.code(0x1000U,{code[0],code[1],code[2]});
    candidate.code(0x1000U,{code[0],code[1],code[2]});
    reference.s().gpr[4]=0x12345678U; candidate.s().gpr[4]=0x12345678U;
    reference.s().gpr[5]=0xAABBCCDDU; candidate.s().gpr[5]=0xAABBCCDDU;
    reference.s().gpr[6]=0x11223344U; candidate.s().gpr[6]=0x11223344U;
    reference.s().gpr[8]=0x12345678U; candidate.s().gpr[8]=0x12345678U;
    reference.s().gpr[9]=8U; candidate.s().gpr[9]=8U;
    CHECK(reference.cpu->run(3U)==StepResult::Executed);
    CHECK(candidate.cpu->run_jit(3U)==StepResult::Executed);
    CHECK(candidate.s()==reference.s());
    CHECK_EQ(candidate.s().gpr[3],0x34567800U);
    CHECK_EQ(candidate.s().gpr[5],0xAA33CCDDU);
    CHECK_EQ(candidate.s().gpr[7],0x34567800U);
}

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
