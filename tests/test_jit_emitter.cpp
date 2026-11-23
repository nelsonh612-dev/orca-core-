// AArch64 emitter encodings checked against the LLVM assembler.
//
// Each case emits one instruction with Orca's emitter and gives the same instruction
// as assembly text. All texts are assembled in one run of `llvm-mc -show-encoding`
// and every word must match. Without llvm-mc the test is skipped (exit code 77).
#include "orca/jit/a64_emitter.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::jit;

namespace {

struct Case {
    std::function<void(Emitter&)> emit;
    std::string text;
};

std::vector<Case> cases() {
    std::vector<Case> c;
    auto add = [&](std::function<void(Emitter&)> f, std::string t) { c.push_back({std::move(f), std::move(t)}); };
    // moves
    add([](Emitter& e) { e.movz(false, 3, 0x1234); }, "movz w3, #0x1234");
    add([](Emitter& e) { e.movz(true, 3, 0xBEEF, 48); }, "movz x3, #0xbeef, lsl #48");
    add([](Emitter& e) { e.movk(false, 0, 0xFFFF, 16); }, "movk w0, #0xffff, lsl #16");
    add([](Emitter& e) { e.movk(true, 30, 7, 32); }, "movk x30, #7, lsl #32");
    add([](Emitter& e) { e.movn(false, 9, 0); }, "movn w9, #0");
    add([](Emitter& e) { e.mov(true, 1, 2); }, "mov x1, x2");
    add([](Emitter& e) { e.mov(false, 1, 2); }, "mov w1, w2");
    // arithmetic
    add([](Emitter& e) { e.add_imm(false, 1, 2, 4095); }, "add w1, w2, #4095");
    add([](Emitter& e) { e.add_imm(true, SP, SP, 16); }, "add sp, sp, #16");
    add([](Emitter& e) { e.add_imm(true, 4, 5, 3, true); }, "add x4, x5, #3, lsl #12");
    add([](Emitter& e) { e.sub_imm(true, SP, SP, 32); }, "sub sp, sp, #32");
    add([](Emitter& e) { e.adds_imm(false, 0, 1, 1); }, "adds w0, w1, #1");
    add([](Emitter& e) { e.subs_imm(false, 0, 1, 77); }, "subs w0, w1, #77");
    add([](Emitter& e) { e.cmp_imm(false, 7, 0); }, "cmp w7, #0");
    add([](Emitter& e) { e.add(false, 1, 2, 3); }, "add w1, w2, w3");
    add([](Emitter& e) { e.add(true, 1, 2, 3, Shift::LSL, 2); }, "add x1, x2, x3, lsl #2");
    add([](Emitter& e) { e.adds(false, 1, 2, 3); }, "adds w1, w2, w3");
    add([](Emitter& e) { e.sub(false, 1, 2, 3, Shift::ASR, 5); }, "sub w1, w2, w3, asr #5");
    add([](Emitter& e) { e.subs(true, 1, 2, 3); }, "subs x1, x2, x3");
    add([](Emitter& e) { e.cmp(false, 4, 5); }, "cmp w4, w5");
    add([](Emitter& e) { e.neg(false, 6, 7); }, "neg w6, w7");
    add([](Emitter& e) { e.adc(false, 1, 2, 3); }, "adc w1, w2, w3");
    add([](Emitter& e) { e.adcs(false, 1, 2, 3); }, "adcs w1, w2, w3");
    add([](Emitter& e) { e.sbc(false, 1, 2, 3); }, "sbc w1, w2, w3");
    add([](Emitter& e) { e.sbcs(true, 1, 2, 3); }, "sbcs x1, x2, x3");
    add([](Emitter& e) { e.mul(false, 1, 2, 3); }, "mul w1, w2, w3");
    add([](Emitter& e) { e.madd(true, 1, 2, 3, 4); }, "madd x1, x2, x3, x4");
    add([](Emitter& e) { e.msub(false, 1, 2, 3, 4); }, "msub w1, w2, w3, w4");
    add([](Emitter& e) { e.smull(1, 2, 3); }, "smull x1, w2, w3");
    add([](Emitter& e) { e.umull(1, 2, 3); }, "umull x1, w2, w3");
    add([](Emitter& e) { e.sdiv(false, 1, 2, 3); }, "sdiv w1, w2, w3");
    add([](Emitter& e) { e.udiv(false, 1, 2, 3); }, "udiv w1, w2, w3");
    // logical
    add([](Emitter& e) { e.and_(false, 1, 2, 3); }, "and w1, w2, w3");
    add([](Emitter& e) { e.bic(false, 1, 2, 3); }, "bic w1, w2, w3");
    add([](Emitter& e) { e.orr(true, 1, 2, 3, Shift::LSR, 32); }, "orr x1, x2, x3, lsr #32");
    add([](Emitter& e) { e.orn(false, 1, 2, 3); }, "orn w1, w2, w3");
    add([](Emitter& e) { e.eor(false, 1, 2, 3, Shift::ROR, 7); }, "eor w1, w2, w3, ror #7");
    add([](Emitter& e) { e.eon(false, 1, 2, 3); }, "eon w1, w2, w3");
    add([](Emitter& e) { e.ands(false, 1, 2, 3); }, "ands w1, w2, w3");
    add([](Emitter& e) { e.tst(false, 2, 3); }, "tst w2, w3");
    add([](Emitter& e) { e.mvn(false, 1, 3); }, "mvn w1, w3");
    add([](Emitter& e) { CHECK(e.and_imm(false, 1, 2, 0x0000FFFFU)); }, "and w1, w2, #0xffff");
    add([](Emitter& e) { CHECK(e.and_imm(false, 1, 2, 0xFFFF0000U)); }, "and w1, w2, #0xffff0000");
    add([](Emitter& e) { CHECK(e.and_imm(false, 1, 2, 0x80000001U)); }, "and w1, w2, #0x80000001");
    add([](Emitter& e) { CHECK(e.orr_imm(false, 1, 2, 0x55555555U)); }, "orr w1, w2, #0x55555555");
    add([](Emitter& e) { CHECK(e.eor_imm(false, 1, 2, 0x0F0F0F0FU)); }, "eor w1, w2, #0xf0f0f0f");
    add([](Emitter& e) { CHECK(e.ands_imm(false, 1, 2, 1)); }, "ands w1, w2, #0x1");
    add([](Emitter& e) { CHECK(e.and_imm(true, 1, 2, 0xFFFFFFFFULL)); }, "and x1, x2, #0xffffffff");
    add([](Emitter& e) { CHECK(e.orr_imm(true, 1, 2, 0x8000000000000000ULL)); }, "orr x1, x2, #0x8000000000000000");
    add([](Emitter& e) { CHECK(e.and_imm(true, 1, 2, 0x00FF00FF00FF00FFULL)); }, "and x1, x2, #0xff00ff00ff00ff");
    add([](Emitter& e) { CHECK(e.and_imm(false, 1, 2, 0x7FFFFFF8U)); }, "and w1, w2, #0x7ffffff8");
    add([](Emitter& e) { CHECK(e.and_imm(false, 1, 2, 0xF000000FU)); }, "and w1, w2, #0xf000000f");
    // shifts and bit fields
    add([](Emitter& e) { e.lslv(false, 1, 2, 3); }, "lsl w1, w2, w3");
    add([](Emitter& e) { e.lsrv(true, 1, 2, 3); }, "lsr x1, x2, x3");
    add([](Emitter& e) { e.asrv(false, 1, 2, 3); }, "asr w1, w2, w3");
    add([](Emitter& e) { e.rorv(false, 1, 2, 3); }, "ror w1, w2, w3");
    add([](Emitter& e) { e.lsl_imm(false, 1, 2, 5); }, "lsl w1, w2, #5");
    add([](Emitter& e) { e.lsl_imm(true, 1, 2, 32); }, "lsl x1, x2, #32");
    add([](Emitter& e) { e.lsr_imm(false, 1, 2, 31); }, "lsr w1, w2, #31");
    add([](Emitter& e) { e.lsr_imm(true, 1, 2, 32); }, "lsr x1, x2, #32");
    add([](Emitter& e) { e.asr_imm(false, 1, 2, 3); }, "asr w1, w2, #3");
    add([](Emitter& e) { e.ubfx(false, 1, 2, 4, 8); }, "ubfx w1, w2, #4, #8");
    add([](Emitter& e) { e.sxtb(false, 1, 2); }, "sxtb w1, w2");
    add([](Emitter& e) { e.sxth(false, 1, 2); }, "sxth w1, w2");
    add([](Emitter& e) { e.sxtw(1, 2); }, "sxtw x1, w2");
    add([](Emitter& e) { e.uxtb(1, 2); }, "uxtb w1, w2");
    add([](Emitter& e) { e.uxth(1, 2); }, "uxth w1, w2");
    add([](Emitter& e) { e.bfi(false, 1, 2, 8, 4); }, "bfi w1, w2, #8, #4");
    add([](Emitter& e) { e.bfxil(false, 1, 2, 3, 5); }, "bfxil w1, w2, #3, #5");
    add([](Emitter& e) { e.ror_imm(false, 1, 2, 7); }, "ror w1, w2, #7");
    add([](Emitter& e) { e.extr(true, 1, 2, 3, 32); }, "extr x1, x2, x3, #32");
    add([](Emitter& e) { e.clz(false, 1, 2); }, "clz w1, w2");
    add([](Emitter& e) { e.rev(false, 1, 2); }, "rev w1, w2");
    add([](Emitter& e) { e.rev(true, 1, 2); }, "rev x1, x2");
    add([](Emitter& e) { e.rev16(false, 1, 2); }, "rev16 w1, w2");
    // conditional
    add([](Emitter& e) { e.csel(false, 1, 2, 3, Cond::LT); }, "csel w1, w2, w3, lt");
    add([](Emitter& e) { e.csinc(true, 1, 2, 3, Cond::HI); }, "csinc x1, x2, x3, hi");
    add([](Emitter& e) { e.cset(false, 4, Cond::EQ); }, "cset w4, eq");
    add([](Emitter& e) { e.cset(false, 4, Cond::HS); }, "cset w4, hs");
    add([](Emitter& e) { e.csetm(true, 4, Cond::VS); }, "csetm x4, vs");
    // loads / stores
    add([](Emitter& e) { e.ldr(4, 1, 19, 124); }, "ldr w1, [x19, #124]");
    add([](Emitter& e) { e.ldr(8, 1, 19, 4088); }, "ldr x1, [x19, #4088]");
    add([](Emitter& e) { e.str(4, 1, 19, 16380); }, "str w1, [x19, #16380]");
    add([](Emitter& e) { e.str(8, 30, SP, 8); }, "str x30, [sp, #8]");
    add([](Emitter& e) { e.ldr(1, 1, 2, 3); }, "ldrb w1, [x2, #3]");
    add([](Emitter& e) { e.str(1, 1, 2, 4095); }, "strb w1, [x2, #4095]");
    add([](Emitter& e) { e.ldr(2, 1, 2, 6); }, "ldrh w1, [x2, #6]");
    add([](Emitter& e) { e.str(2, 1, 2, 8190); }, "strh w1, [x2, #8190]");
    add([](Emitter& e) { e.ldrsb_w(1, 2, 1); }, "ldrsb w1, [x2, #1]");
    add([](Emitter& e) { e.ldrsh_w(1, 2, 2); }, "ldrsh w1, [x2, #2]");
    add([](Emitter& e) { e.ldr_reg(4, 1, 2, 3); }, "ldr w1, [x2, x3]");
    add([](Emitter& e) { e.str_reg(8, 1, 2, 3); }, "str x1, [x2, x3]");
    add([](Emitter& e) { e.ldr_reg(1, 1, 2, 3); }, "ldrb w1, [x2, x3]");
    add([](Emitter& e) { e.str_reg(2, 1, 2, 3); }, "strh w1, [x2, x3]");
    add([](Emitter& e) { e.stp_pre(29, 30, SP, -16); }, "stp x29, x30, [sp, #-16]!");
    add([](Emitter& e) { e.ldp_post(29, 30, SP, 16); }, "ldp x29, x30, [sp], #16");
    add([](Emitter& e) { e.stp(19, 20, SP, 16); }, "stp x19, x20, [sp, #16]");
    add([](Emitter& e) { e.ldp(19, 20, SP, -512); }, "ldp x19, x20, [sp, #-512]");
    // branches (offsets in bytes in the assembly text)
    add([](Emitter& e) { const auto l = e.b(); CHECK(e.bind(l, l + 3)); }, "b #12");
    add([](Emitter& e) { const auto l = e.b(); e.words().push_back(0); e.words().pop_back(); CHECK(e.bind(l, l - 2 + 2)); }, "b #0");
    add([](Emitter& e) { const auto l = e.b_cond(Cond::NE); CHECK(e.bind(l, l + 5)); }, "b.ne #20");
    add([](Emitter& e) { const auto l = e.cbz(false, 3); CHECK(e.bind(l, l + 2)); }, "cbz w3, #8");
    add([](Emitter& e) { const auto l = e.cbnz(true, 3); CHECK(e.bind(l, l + 1)); }, "cbnz x3, #4");
    add([](Emitter& e) { const auto l = e.tbz(3, 31); CHECK(e.bind(l, l + 4)); }, "tbz w3, #31, #16");
    add([](Emitter& e) { const auto l = e.tbnz(3, 40); CHECK(e.bind(l, l + 4)); }, "tbnz x3, #40, #16");
    add([](Emitter& e) { e.br(16); }, "br x16");
    add([](Emitter& e) { e.blr(17); }, "blr x17");
    add([](Emitter& e) { e.adr(9, -12); }, "adr x9, #-12");
    add([](Emitter& e) { e.adr(1, 0x400); }, "adr x1, #1024");
    add([](Emitter& e) { e.emit(Emitter::b_encoding(-3)); }, "b #-12");
    add([](Emitter& e) { e.emit(Emitter::movz_encoding(false, 10, 0xFFFF)); }, "movz w10, #0xffff");
    // scalar floating point
    add([](Emitter& e) { e.ldr_d(3, 19, 128); }, "ldr d3, [x19, #128]");
    add([](Emitter& e) { e.str_d(31, 1, 32760); }, "str d31, [x1, #32760]");
    add([](Emitter& e) { e.fadd(0, 1, 2); }, "fadd d0, d1, d2");
    add([](Emitter& e) { e.fsub(31, 16, 7); }, "fsub d31, d16, d7");
    add([](Emitter& e) { e.fmul(4, 5, 6); }, "fmul d4, d5, d6");
    add([](Emitter& e) { e.fdiv(20, 21, 22); }, "fdiv d20, d21, d22");
    add([](Emitter& e) { e.fmadd(1, 2, 3, 4); }, "fmadd d1, d2, d3, d4");
    add([](Emitter& e) { e.fmsub(1, 2, 3, 4); }, "fmsub d1, d2, d3, d4");
    add([](Emitter& e) { e.fnmsub(17, 18, 19, 20); }, "fnmsub d17, d18, d19, d20");
    add([](Emitter& e) { e.fneg(5, 6); }, "fneg d5, d6");
    add([](Emitter& e) { e.fmov_d(5, 6); }, "fmov d5, d6");
    add([](Emitter& e) { e.fcvt_s_from_d(7, 8); }, "fcvt s7, d8");
    add([](Emitter& e) { e.fcvt_d_from_s(9, 10); }, "fcvt d9, s10");
    add([](Emitter& e) { e.fmov_to_x(13, 3); }, "fmov x13, d3");
    add([](Emitter& e) { e.fmov_from_x(24, 15); }, "fmov d24, x15");
    add([](Emitter& e) { e.fcmp(0, 1); }, "fcmp d0, d1");
    add([](Emitter& e) { e.fcmp_zero(2); }, "fcmp d2, #0.0");
    add([](Emitter& e) { e.fcsel(3, 4, 5, Cond::GE); }, "fcsel d3, d4, d5, ge");
    add([](Emitter& e) { e.ret(); }, "ret");
    add([](Emitter& e) { e.nop(); }, "nop");
    return c;
}

std::vector<u32> assemble(const std::vector<std::string>& lines, std::string& error) {
    const std::string src = "/tmp/orca_jit_emitter_cases.s", out = "/tmp/orca_jit_emitter_cases.txt";
    {
        std::ofstream f(src);
        for (const auto& l : lines) f << l << "\n";
    }
    const std::string cmd = std::string(ORCA_LLVM_MC) + " -triple=aarch64 -show-encoding " + src + " > " + out + " 2>&1";
    if (std::system(cmd.c_str()) != 0) { error = "llvm-mc failed"; }
    std::ifstream in(out);
    std::vector<u32> words;
    std::string line;
    while (std::getline(in, line)) {
        const auto p = line.find("encoding: [");
        if (p == std::string::npos) {
            if (line.find("error") != std::string::npos) error += line + "\n";
            continue;
        }
        unsigned b[4] = {0, 0, 0, 0};
        if (std::sscanf(line.c_str() + p, "encoding: [0x%x,0x%x,0x%x,0x%x]", &b[0], &b[1], &b[2], &b[3]) == 4)
            words.push_back(b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24);
    }
    return words;
}

} // namespace

TEST_CASE(emitter_matches_llvm_assembler) {
    const auto cs = cases();
    std::vector<std::string> texts;
    std::vector<u32> ours;
    for (const auto& c : cs) {
        Emitter e;
        c.emit(e);
        CHECK_EQ(e.size(), 1U);
        ours.push_back(e.size() == 1U ? e.words()[0] : 0U);
        texts.push_back(c.text);
    }
    std::string error;
    const std::vector<u32> ref = assemble(texts, error);
    if (!error.empty()) std::printf("    llvm-mc: %s\n", error.c_str());
    CHECK_EQ(ref.size(), cs.size());
    unsigned bad = 0;
    for (std::size_t i = 0; i < std::min(ref.size(), ours.size()); ++i)
        if (ref[i] != ours[i]) {
            ++bad;
            std::printf("    %-40s orca %08X llvm %08X\n", texts[i].c_str(), ours[i], ref[i]);
        }
    CHECK_EQ(bad, 0U);
    std::printf("[jit_emitter] %zu instructions identical to llvm-mc\n", ref.size() - bad);
}

TEST_CASE(mov_imm_sequences_build_the_value) {
    // The sequence chosen by mov_imm is assembled by llvm-mc as well, but here we check
    // the value it builds by interpreting MOVZ/MOVN/MOVK/ORR-imm words.
    auto run = [](const std::vector<u32>& ws, bool x) {
        u64 v = 0;
        for (u32 w : ws) {
            const unsigned hw = (w >> 21) & 3U;
            const u64 imm = (w >> 5) & 0xFFFFU;
            if ((w & 0x7F800000U) == 0x52800000U) v = imm << (16U * hw);                       // MOVZ
            else if ((w & 0x7F800000U) == 0x12800000U) v = ~(imm << (16U * hw));              // MOVN
            else if ((w & 0x7F800000U) == 0x72800000U) v = (v & ~(0xFFFFULL << (16U * hw))) | (imm << (16U * hw)); // MOVK
            else if ((w & 0x7F800000U) == 0x32000000U) {                                        // ORR imm from ZR
                // decode via the encoder: try candidates is circular; use DecodeBitMasks here
                const unsigned n = (w >> 22) & 1U, immr = (w >> 16) & 63U, imms = (w >> 10) & 63U;
                unsigned len = 0;
                const unsigned combined = (n << 6) | (~imms & 63U);
                for (unsigned b = 6; b-- > 0;) if ((combined >> b) & 1U) { len = b; break; }
                if (n) len = 6;
                const unsigned esize = 1U << len, levels = esize - 1U;
                const unsigned s = imms & levels, r = immr & levels;
                const u64 welem = s + 1U == 64U ? ~0ULL : (1ULL << (s + 1U)) - 1ULL;
                const u64 emask = esize == 64U ? ~0ULL : (1ULL << esize) - 1ULL;
                const u64 rot = r == 0U ? welem : (((welem >> r) | (welem << (esize - r))) & emask);
                u64 rep = 0;
                for (unsigned i = 0; i < 64U; i += esize) rep |= rot << i;
                v = rep;
            }
        }
        return x ? v : (v & 0xFFFFFFFFULL);
    };
    const u64 vals64[] = {0, 1, 0xFFFF, 0x10000, 0xFFFFFFFFFFFFFFFFULL, 0x123456789ABCDEF0ULL, 0xFFFFFFFF00000000ULL,
                          0x00000000FFFFFFFFULL, 0x5555555555555555ULL, 0xFFFF0000FFFF0000ULL, 0x8000000000000000ULL,
                          0xFFFFFFFFFFFF1234ULL, 0x0000123400005678ULL};
    for (u64 v : vals64) {
        Emitter e;
        e.mov_imm(true, 0, v);
        CHECK(e.size() >= 1U && e.size() <= 4U);
        CHECK_EQ(run(e.words(), true), v);
    }
    const u32 vals32[] = {0, 1, 0xFFFF, 0x10000, 0xFFFFFFFFU, 0x80000000U, 0x12345678U, 0xFFFF1234U, 0x0F0F0F0FU, 0xFFFEFFFFU};
    for (u32 v : vals32) {
        Emitter e;
        e.mov_imm(false, 0, v);
        CHECK(e.size() >= 1U && e.size() <= 2U);
        CHECK_EQ(run(e.words(), false), u64{v});
    }
    // unencodable logical immediates are refused
    CHECK(!encode_logical_imm(0, false));
    CHECK(!encode_logical_imm(0xFFFFFFFFU, false));
    CHECK(!encode_logical_imm(0x12345678U, false));
    Emitter e;
    CHECK(!e.and_imm(false, 1, 2, 0x12345678U));
    CHECK_EQ(e.size(), 0U);
}

int main() {
    const std::string mc = ORCA_LLVM_MC;
    if (mc.empty() || mc.find("NOTFOUND") != std::string::npos) {
        std::printf("[jit_emitter] llvm-mc not found: skipped\n");
        return 77;
    }
    return orca::test::run_all("jit_emitter");
}
