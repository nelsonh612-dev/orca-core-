// PowerPC -> AArch64 JIT: translation on any host, and on AArch64 hosts the
// equivalence of translated execution with the interpreter (registers, CR, XER,
// LR, CTR, memory, stops and retired instruction counts).
#include "orca/jit/code_buffer.hpp"
#include "orca/ppc/jit.hpp"
#include "machine.hpp"
#include "test_framework.hpp"

#include "orca/ppc/fpu.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::ppc;
using orca::test::Machine;

namespace {

// --- encoders (PowerPC UISA) -------------------------------------------------
constexpr u32 D(u32 op, u32 rt, u32 ra, u32 imm) { return op << 26 | rt << 21 | ra << 16 | (imm & 0xFFFFU); }
constexpr u32 X31(u32 xo, u32 rt, u32 ra, u32 rb, bool rc = false) { return 31U << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | (rc ? 1U : 0U); }
constexpr u32 XO(u32 xo9, u32 rt, u32 ra, u32 rb, bool oe, bool rc) { return 31U << 26 | rt << 21 | ra << 16 | rb << 11 | (oe ? 1U : 0U) << 10 | xo9 << 1 | (rc ? 1U : 0U); }
constexpr u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, bool rc) { return op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | (rc ? 1U : 0U); }
constexpr u32 BC(u32 bo, u32 bi, s32 disp, bool lk = false) { return 16U << 26 | bo << 21 | bi << 16 | (static_cast<u32>(disp) & 0xFFFCU) | (lk ? 1U : 0U); }
constexpr u32 B(s32 disp, bool lk = false) { return 18U << 26 | (static_cast<u32>(disp) & 0x03FFFFFCU) | (lk ? 1U : 0U); }
constexpr u32 XL(u32 xo, u32 bt, u32 ba, u32 bb, bool lk = false) { return 19U << 26 | bt << 21 | ba << 16 | bb << 11 | xo << 1 | (lk ? 1U : 0U); }
constexpr u32 SPR(u32 n) { return ((n & 31U) << 5) | (n >> 5); }

struct Rng {
    u64 s;
    u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<u32>(s >> 11); }
    u32 u(u32 n) { return next() % n; }
};

// Under ASan/UBSan (Debug) the randomised tests run a quarter of their cases: the
// sanitizers look for memory and UB errors, the full counts run in the other builds.
#ifdef ORCA_SANITIZED
constexpr int kScale = 4;
#else
constexpr int kScale = 1;
#endif

constexpr u32 kCode = 0x00004000U, kData = 0x00010000U, kDataSize = 0x2000U;

// A random instruction from the translated set (plus a few the JIT leaves to the
// interpreter). r1-r27: values; r28/r29: data pointers; r30/r31: index values.
u32 random_insn(Rng& r, u32 at, u32 code_end) {
    auto reg = [&] { return 1U + r.u(26); }; // r27 holds a DBAT0L value
    auto anyreg = [&] { return r.u(32); };
    const bool rc = r.u(2) != 0U;
    auto freg = [&] { return r.u(32); };
    if (r.u(3) == 0U) { // floating point and paired singles
        const u32 fa = freg(), fb = freg(), fc = freg(), fd = freg();
        switch (r.u(14)) {
        case 0: { const u32 xs[] = {21, 20, 25, 18, 29, 28, 31, 30, 23, 26};                 // A-form double
                  return 63U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | xs[r.u(10)] << 1 | (rc ? 1U : 0U); }
        case 1: { const u32 xs[] = {21, 20, 25, 18, 29, 28, 31, 30, 24};                     // A-form single
                  return 59U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | xs[r.u(9)] << 1 | (rc ? 1U : 0U); }
        case 2: { const u32 xs[] = {0, 32, 12, 14, 15, 40, 72, 264, 136};                    // X-form 63
                  const u32 x = xs[r.u(9)];
                  return 63U << 26 | (x == 0U || x == 32U ? (r.u(8) << 2) : fd) << 21 | fa << 16 | fb << 11 | x << 1 | (rc && x != 0U && x != 32U ? 1U : 0U); }
        case 3: return 63U << 26 | fd << 21 | 583U << 1;                                     // mffs
        case 4: return 63U << 26 | r.u(256) << 17 | fb << 11 | 711U << 1;                    // mtfsf
        case 5: return 63U << 26 | (r.u(8) << 2) << 21 | (r.u(8) << 2) << 16 | 64U << 1;     // mcrfs
        case 6: { const u32 xs[] = {21, 20, 25, 18, 29, 28, 31, 30, 10, 11, 12, 13, 14, 15, 23, 24, 26}; // paired A-form
                  return 4U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | xs[r.u(17)] << 1 | (rc ? 1U : 0U); }
        case 7: { const u32 xs[] = {0, 32, 64, 96, 40, 72, 136, 264, 528, 560, 592, 624};   // paired X-form
                  const u32 x = xs[r.u(12)];
                  return 4U << 26 | (x < 128U ? (r.u(8) << 2) : fd) << 21 | fa << 16 | fb << 11 | x << 1; }
        case 8: case 9: { // lfs/lfsu/lfd/lfdu/stfs/stfsu/stfd/stfdu off the data pointers
            const u32 op = 48U + r.u(8);
            return D(op, fd, 28U + r.u(2), r.u(256) - 64U + (r.u(10) == 0U ? 0x0FFDU - 0x80U : 0U)); }
        case 10: { const u32 xs[] = {535, 567, 599, 631, 663, 695, 727, 759, 983};           // indexed FP
                   return X31(xs[r.u(9)], fd, 28U + r.u(2), 30U + r.u(2)); }
        case 11: case 12: { // psq_l/psq_lu/psq_st/psq_stu
            const u32 ops[] = {56, 57, 60, 61};
            return ops[r.u(4)] << 26 | fd << 21 | (28U + r.u(2)) << 16 | r.u(2) << 15 | r.u(8) << 12 | (r.u(0x100U) - 0x40U) & 0xFFFU; }
        default: { const u32 xs[] = {6, 7, 38, 39};                                          // psq_lx/stx/lux/stux
                   return 4U << 26 | fd << 21 | (28U + r.u(2)) << 16 | (30U + r.u(2)) << 11 | r.u(2) << 10 | r.u(8) << 7 | xs[r.u(4)] << 1; }
        }
    }
    switch (r.u(30)) {
    case 0: return D(14, reg(), r.u(4) == 0U ? 0U : anyreg(), r.next());          // addi
    case 1: return D(15, reg(), anyreg(), r.next());                                 // addis
    case 2: return D(12 + r.u(2), reg(), anyreg(), r.next());                        // addic(.)
    case 3: return D(8, reg(), anyreg(), r.next());                                  // subfic
    case 4: return D(7, reg(), anyreg(), r.next());                                  // mulli
    case 5: return D(10 + r.u(2), r.u(8) << 2, anyreg(), r.next());                  // cmpli/cmpi
    case 6: return D(24 + r.u(6), anyreg(), reg(), r.next());                        // ori..andis.
    case 7: return M(21, anyreg(), reg(), r.u(32), r.u(32), r.u(32), rc);            // rlwinm
    case 8: return M(23, anyreg(), reg(), anyreg(), r.u(32), r.u(32), rc);           // rlwnm
    case 9: return M(20, anyreg(), reg(), r.u(32), r.u(32), r.u(32), rc);            // rlwimi
    case 10: { const u32 xs[] = {266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104, 235, 491, 459};
               return XO(xs[r.u(14)], reg(), anyreg(), anyreg(), r.u(2) != 0U, rc); }
    case 11: return XO(r.u(2) ? 75U : 11U, reg(), anyreg(), anyreg(), false, rc);    // mulhw(u)
    case 12: { const u32 xs[] = {28, 60, 124, 284, 316, 412, 444, 476, 922, 954, 26, 24, 536, 792};
               return X31(xs[r.u(14)], anyreg(), reg(), anyreg(), rc); }
    case 13: return X31(824, anyreg(), reg(), r.u(32), rc);                          // srawi
    case 14: return X31(r.u(2) ? 0U : 32U, r.u(8) << 2, anyreg(), anyreg());         // cmp/cmpl
    case 15: return X31(19, reg(), 0, 0);                                            // mfcr
    case 16: return 31U << 26 | anyreg() << 21 | r.u(256) << 12 | 144U << 1;        // mtcrf
    case 17: { const u32 sprs[] = {1, 8, 9};                                         // mfspr/mtspr XER LR CTR
               const u32 n = sprs[r.u(3)];
               return 31U << 26 | (r.u(2) ? reg() : anyreg()) << 21 | SPR(n) << 11 | (r.u(2) ? 339U : 467U) << 1; }
    case 18: { const u32 xos[] = {257, 129, 289, 225, 33, 449, 417, 193};          // CR logical
               return XL(xos[r.u(8)], r.u(32), r.u(32), r.u(32)); }
    case 19: return XL(0, r.u(8) << 2, r.u(8) << 2, 0);                              // mcrf
    case 20: case 21: case 22: { // D-form loads/stores off r28/r29 (data area), some misaligned
        const u32 ops[] = {32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45};
        const u32 op = ops[r.u(14)];
        u32 base = 28U + r.u(2);
        u32 off = r.u(256) - 64U + (r.u(8) == 0U ? 0x0FFDU - 0x80U : 0U); // occasionally near the page end
        if (r.u(10) == 0U) { base = 0; off = kData + r.u(0x400U); }      // (0): absolute address
        if (r.u(6) == 0U) off = r.u(2) ? 0x1000U + r.u(0x800U) : 0xF000U + r.u(0x800U); // displacement > 12 bits
        u32 rt = reg();
        if ((op & 1U) != 0U && rt == base) rt = 1;
        return D(op, rt, base, off);
    }
    case 23: { // X-form indexed (r28/r29 + r30/r31, both kept small), byte-reversed too
        const u32 xs[] = {23, 55, 87, 119, 279, 311, 343, 375, 151, 183, 215, 247, 407, 439, 534, 790, 662, 918};
        const u32 xo = xs[r.u(18)];
        const u32 base = 28U + r.u(2);
        u32 rt = reg();
        return X31(xo, rt, base, 30U + r.u(2));
    }
    case 24: { // forward conditional branch inside the program (any BO, CR bit)
        const u32 bos[] = {4, 12, 20, 0, 2, 8, 10, 16, 18};
        const u32 maxd = (code_end - at) / 4U;
        const s32 d = static_cast<s32>(4U * (1U + r.u(maxd > 6U ? 6U : (maxd > 1U ? maxd - 1U : 1U))));
        return BC(bos[r.u(9)], r.u(32), d, r.u(6) == 0U);
    }
    case 25: { const u32 maxd = (code_end - at) / 4U;                               // b forward
               return B(static_cast<s32>(4U * (1U + r.u(maxd > 4U ? 4U : (maxd > 1U ? maxd - 1U : 1U))))); }
    case 26: switch (r.u(9)) {                                                        // run in the block by the interpreter
        case 0: return D(46, 30U + r.u(2), 28U + r.u(2), r.u(0x80U) & ~3U);         // lmw r30/r31 (RA < RT)
        case 1: return D(47, 20U + r.u(12), 28U + r.u(2), r.u(0x200U) - 0x80U);     // stmw (sometimes misaligned)
        case 2: return X31(20, reg(), 28U + r.u(2), 30U + r.u(2));                  // lwarx
        case 3: return X31(150, reg(), 28U + r.u(2), 30U + r.u(2), true);           // stwcx.
        case 4: return X31(1014, 0, 28U + r.u(2), 30U + r.u(2));                    // dcbz
        case 5: { const u32 xs[] = {86, 54, 278, 246, 598, 854, 982}; return X31(xs[r.u(7)], 0, 28U + r.u(2), 30U + r.u(2)); }
        case 6: return D(3, r.u(32), reg(), r.next());                               // twi (may trap)
        case 7: return X31(83, reg(), 0, 0);                                         // mfmsr
        default: return X31(r.u(2) ? 597U : 725U, 1U + r.u(20), 28U + r.u(2), r.u(17)); // lswi / stswi
        }
    case 27: return r.u(4) == 0U ? 31U << 26 | reg() << 21 | SPR(268) << 11 | 371U << 1 // mftb: interpreter
                                 : D(36, reg(), 0, kCode + 4U * r.u(64));            // store into the code: invalidation
    case 28: switch (r.u(4)) {
        case 0: return 31U << 26 | 27U << 21 | SPR(537) << 11 | 467U << 1;        // mtspr DBAT0L, r27: new data mapping
        case 1: return 4U << 26 | (28U + r.u(2)) << 16 | (30U + r.u(2)) << 11 | 1014U << 1; // dcbz_l: locked-cache line
        default: return D(14, 30U + r.u(2), 0, r.u(64));                         // keep the indexes small
        }
    default: { // move a data pointer inside the data area (addis + addi pair is not needed: r28/r29 += small)
        const u32 base = 28U + r.u(2);
        return D(14, base, base, static_cast<u32>(static_cast<s32>(r.u(0x200U)) - 0x100));
    }
    }
}

void setup(Machine& m, Rng& r, const std::vector<u32>& prog) {
    for (u32 i = 0; i < prog.size(); ++i) m.bus.write32(kCode + 4U * i, prog[i]);
    for (u32 a = 0; a < kDataSize; a += 4) m.bus.write32(kData + a, r.next());
    const u32 specials[] = {0, 1, 0xFFFFFFFFU, 0x80000000U, 0x7FFFFFFFU, 31, 32, 63, 0xFFFF8000U};
    for (unsigned g = 1; g < 28; ++g) m.s().gpr[g] = r.u(3) == 0U ? specials[r.u(9)] : r.next();
    m.s().gpr[0] = r.next();
    m.s().gpr[28] = kData + 0x200U;
    m.s().gpr[29] = kData + 0xF00U;
    m.s().gpr[30] = r.u(64);
    m.s().gpr[31] = r.u(64);
    m.s().cr = r.next();
    m.s().xer = r.next() & 0xE000007FU;
    m.s().lr = kCode + 4U * r.u(static_cast<u32>(prog.size()));
    m.s().ctr = r.u(5);
    m.s().pc = kCode;
    // real mode; FP enabled in most programs (otherwise FP instructions take the
    // FP-unavailable exception in both), paired singles and quantized loads enabled
    m.s().msr = r.u(5) == 0U ? 0U : MSR_FP;
    m.s().hid2 = HID2_PSE | HID2_LSQE;
    // half of the programs translate data through DBAT0 (EA 0-256 KiB -> 256 KiB or
    // 512 KiB, sometimes read-only), and r27 holds another DBAT0L for mtspr
    const u32 lowers[] = {0x00040002U, 0x00080002U, 0x00040001U};
    m.s().gpr[27] = lowers[r.u(3)];
    if (r.u(2) != 0U) {
        m.s().msr |= MSR_DR;
        m.s().dbat[0] = BatPair{0x00000007U, lowers[r.u(3)]};
    }
    if (r.u(3) == 0U) m.s().hid2 |= HID2_LCE;
    const u64 fspecial[] = {0x7FF8000000000000ULL, 0x7FF0000000000001ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
                            0x0000000000000001ULL, 0x8000000000000000ULL, 0, 0x3FF0000000000000ULL, 0x47EFFFFFE0000000ULL,
                            0x380FFFFFFFFFFFFFULL, 0xC1E0000000000000ULL};
    for (auto& f : m.s().fpr) {
        auto pick = [&] { return r.u(3) == 0U ? fspecial[r.u(11)] : (static_cast<u64>(r.next()) << 32 | r.next()) & 0xC7FFFFFFFFFFFFFFULL; };
        f.ps0 = pick();
        f.ps1 = pick();
    }
    m.s().fpscr = r.next() & 0x07FFF8FFU & ~0x000000F8U; // exceptions disabled (no FE in MSR anyway), any RN/flags
    const u32 types[] = {0, 4, 5, 6, 7};
    for (auto& g : m.s().gqr) g = types[r.u(5)] | (r.u(64) << 8) | (types[r.u(5)] << 16) | (r.u(64) << 24);
}

struct RunEnd { StepResult r; u64 steps; };

RunEnd run_interp(Machine& m, u64 n) {
    u64 k = 0;
    while (k < n) {
        const StepResult r = m.cpu->step();
        ++k;
        if (r != StepResult::Executed && r != StepResult::ExceptionTaken) return {r, k};
    }
    return {StepResult::Executed, k};
}

RunEnd run_jit(Jit& j, u64 n) {
    u64 k = 0;
    while (k < n) {
        const Jit::Result r = j.run(n - k);
        k += r.steps;
        if (r.result != StepResult::Executed && r.result != StepResult::ExceptionTaken) return {r.result, k};
    }
    return {StepResult::Executed, k};
}

} // namespace

// What run() interprets in place after a block stops before it (any host): the
// SDK's interrupt on/off functions (mfmsr / mtmsr) and other CPU-only reads;
// never memory accesses or mtspr, whose effects the caller must time first.
TEST_CASE(jit_resumes_only_after_cpu_only_instructions) {
    CHECK(ppc::Jit::resumes_after_interpreting(0x7C6000A6U));  // mfmsr r3
    CHECK(ppc::Jit::resumes_after_interpreting(0x7C800124U));  // mtmsr r4
    CHECK(ppc::Jit::resumes_after_interpreting(0x7C7A02A6U));  // mfspr r3, SRR0
    CHECK(!ppc::Jit::resumes_after_interpreting(0x7C6C42E6U)); // mftb r3: the caller advances the time base
    CHECK(!ppc::Jit::resumes_after_interpreting(0x7C7602A6U)); // mfspr r3, DEC
    CHECK(ppc::Jit::resumes_after_interpreting(0x4C000064U));  // rfi
    CHECK(!ppc::Jit::resumes_after_interpreting(0x7C7603A6U)); // mtspr DEC, r3
    CHECK(!ppc::Jit::resumes_after_interpreting(0x7C6001A4U)); // mtsr 0, r3
    CHECK(!ppc::Jit::resumes_after_interpreting(0x80630000U)); // lwz r3,0(r3) (MMIO possible)
    CHECK(!ppc::Jit::resumes_after_interpreting(0x90830000U)); // stw
    CHECK(!ppc::Jit::resumes_after_interpreting(0x7C63202EU)); // lwzx
    CHECK(!ppc::Jit::resumes_after_interpreting(0x44000002U)); // sc
    CHECK(!ppc::Jit::resumes_after_interpreting(0x4C00012CU)); // isync
}

TEST_CASE(jit_translates_blocks_on_any_host) {
    Machine m(kCode);
    m.code(kCode, {D(14, 3, 0, 5), XO(266, 4, 3, 3, false, true), M(21, 4, 5, 3, 0, 28, false),
                   D(36, 5, 28, 8), D(32, 6, 28, 8), X31(316, 6, 7, 5, true), BC(12, 2, -16)});
    m.s().gpr[28] = kData;
    Jit j(*m.cpu, m.bus);
    std::vector<u32> words;
    u32 n = 0;
    std::string err;
    CHECK(j.translate_for_test(kCode, words, n, err));
    CHECK_EQ(n, 7U);
    CHECK(words.size() > 40U);
    // every word must be a valid AArch64 instruction (llvm-mc disassembly, when present)
#ifdef ORCA_LLVM_MC
    const std::string mc = ORCA_LLVM_MC;
    if (!mc.empty() && mc.find("NOTFOUND") == std::string::npos) {
        {
            std::ofstream f("/tmp/orca_jit_block.txt");
            for (u32 w : words) f << "0x" << std::hex << (w & 0xFF) << " 0x" << ((w >> 8) & 0xFF) << " 0x" << ((w >> 16) & 0xFF) << " 0x" << (w >> 24) << "\n";
        }
        const std::string cmd = mc + " --disassemble -triple=aarch64 /tmp/orca_jit_block.txt > /tmp/orca_jit_block.s 2>&1";
        CHECK(std::system(cmd.c_str()) == 0);
        std::ifstream in("/tmp/orca_jit_block.s");
        std::string line;
        unsigned lines = 0, bad = 0;
        while (std::getline(in, line)) {
            if (line.find("invalid") != std::string::npos || line.find("warning") != std::string::npos) ++bad;
            if (!line.empty() && line[0] == '\t' && line.find(".text") == std::string::npos) ++lines;
        }
        CHECK_EQ(bad, 0U);
        CHECK_EQ(lines, static_cast<unsigned>(words.size()));
        std::printf("[jit] block of %u guest instructions -> %zu AArch64 instructions, all valid for llvm-mc\n", n, words.size());
    }
#endif
    // a block with native floating point: every word valid too
    {
        const u32 fp_block[] = {
            63U << 26 | 1U << 21 | 2U << 16 | 3U << 11 | 21U << 1,              // fadd
            59U << 26 | 4U << 21 | 5U << 16 | 6U << 11 | 7U << 6 | 29U << 1 | 1U, // fmadds.
            4U << 26 | 8U << 21 | 9U << 16 | 10U << 11 | 11U << 6 | 30U << 1,   // ps_nmsub
            4U << 26 | 8U << 21 | 9U << 16 | 10U << 11 | 11U << 6 | 18U << 1,   // ps_div
            4U << 26 | 8U << 21 | 9U << 16 | 10U << 11 | 11U << 6 | 10U << 1,   // ps_sum0
            63U << 26 | 1U << 21 | 2U << 16 | 3U << 11 | 4U << 6 | 23U << 1,    // fsel
            63U << 26 | 1U << 21 | 3U << 11 | 12U << 1,                          // frsp
            63U << 26 | 4U << 21 | 2U << 16 | 3U << 11 | 32U << 1,              // fcmpo cr1
            4U << 26 | 1U << 21 | 2U << 16 | 3U << 11 | 592U << 1 | 1U,         // ps_merge10.
            BC(12, 2, -36)};
        for (u32 n = 0; n < 10U; ++n) m.bus.write32(kCode + 0x200U + 4U * n, fp_block[n]);
        std::vector<u32> fw;
        u32 fn = 0;
        CHECK(j.translate_for_test(kCode + 0x200U, fw, fn, err));
        CHECK_EQ(fn, 10U);
#ifdef ORCA_LLVM_MC
        const std::string mc2 = ORCA_LLVM_MC;
        if (!mc2.empty() && mc2.find("NOTFOUND") == std::string::npos) {
            {
                std::ofstream f("/tmp/orca_jit_fp_block.txt");
                for (u32 w : fw) f << "0x" << std::hex << (w & 0xFF) << " 0x" << ((w >> 8) & 0xFF) << " 0x" << ((w >> 16) & 0xFF) << " 0x" << (w >> 24) << "\n";
            }
            const std::string cmd = mc2 + " --disassemble -triple=aarch64 /tmp/orca_jit_fp_block.txt > /tmp/orca_jit_fp_block.s 2>&1";
            CHECK(std::system(cmd.c_str()) == 0);
            std::ifstream in("/tmp/orca_jit_fp_block.s");
            std::string line;
            unsigned lines = 0, bad = 0;
            while (std::getline(in, line)) {
                if (line.find("invalid") != std::string::npos || line.find("warning") != std::string::npos) ++bad;
                if (!line.empty() && line[0] == '\t' && line.find(".text") == std::string::npos) ++lines;
            }
            CHECK_EQ(bad, 0U);
            CHECK_EQ(lines, static_cast<unsigned>(fw.size()));
            std::printf("[jit] FP block of %u guest instructions -> %zu AArch64 instructions, all valid for llvm-mc\n", fn, fw.size());
        }
#endif
    }
    // an untranslated first instruction gives no block
    m.code(kCode + 0x100U, {0x7C7603A6U}); // mtspr DEC, r3
    CHECK(!j.translate_for_test(kCode + 0x100U, words, n, err));
    // the SDK's interrupt on/off (mfmsr, mtmsr) stay inside a block (in-block
    // interpreter; mtmsr leaves when it turns interrupts on or changes more than
    // EE); mftb ends it (the System advances the time base between run() calls)
    {
        const u32 sys_block[] = {0x7C6000A6U, 0x7C800124U, 0x38630001U, 0x4E800020U};
        for (u32 k = 0; k < 4U; ++k) m.bus.write32(kCode + 0x300U + 4U * k, sys_block[k]);
        std::vector<u32> sw;
        u32 sn = 0;
        CHECK(j.translate_for_test(kCode + 0x300U, sw, sn, err));
        CHECK_EQ(sn, 4U);
        m.bus.write32(kCode + 0x380U, 0x38630001U);
        m.bus.write32(kCode + 0x384U, 0x7CAC42E6U); // mftb r5
        std::vector<u32> tw;
        u32 tn = 0;
        CHECK(j.translate_for_test(kCode + 0x380U, tw, tn, err));
        CHECK_EQ(tn, 1U);
#ifdef ORCA_LLVM_MC
        const std::string mc2 = ORCA_LLVM_MC;
        if (!mc2.empty() && mc2.find("NOTFOUND") == std::string::npos) {
            {
                std::ofstream f("/tmp/orca_jit_sys_block.txt");
                for (u32 w : sw) f << "0x" << std::hex << (w & 0xFF) << " 0x" << ((w >> 8) & 0xFF) << " 0x" << ((w >> 16) & 0xFF) << " 0x" << (w >> 24) << "\n";
            }
            const std::string cmd = mc2 + " --disassemble -triple=aarch64 /tmp/orca_jit_sys_block.txt > /tmp/orca_jit_sys_block.s 2>&1";
            CHECK(std::system(cmd.c_str()) == 0);
            std::ifstream in("/tmp/orca_jit_sys_block.s");
            std::string line;
            unsigned bad = 0;
            while (std::getline(in, line))
                if (line.find("invalid") != std::string::npos || line.find("warning") != std::string::npos) ++bad;
            CHECK_EQ(bad, 0U);
        }
#endif
    }
}

TEST_CASE(jit_matches_interpreter_on_random_programs) {
    if (!jit::host_can_execute()) {
        std::printf("[jit] host is not AArch64: execution equivalence runs on the ARM64 CI\n");
        return;
    }
    Rng rng{0x0DDB1A5E5EEDULL};
    u64 total_steps = 0, block_steps = 0, programs = 0, interp_steps = 0, invalidations = 0;
    unsigned failures = 0;
    for (int p = 0; p < 3000 / kScale; ++p) {
        const u32 len = 8U + rng.u(56);
        std::vector<u32> prog;
        for (u32 i = 0; i < len; ++i) prog.push_back(random_insn(rng, kCode + 4U * i, kCode + 4U * len));
        prog.push_back(B(-static_cast<s32>(4U * len))); // loop back to the start
        const u64 seed = rng.s;
        Machine a(kCode), b(kCode);
        Rng ra{seed}, rb{seed};
        setup(a, ra, prog);
        setup(b, rb, prog);
        Jit j(*b.cpu, b.bus);
        CHECK(j.can_execute());
        const u64 n = 200U + rng.u(800);
        const RunEnd ea = run_interp(a, n);
        const RunEnd eb = run_jit(j, n);
        bool same = ea.r == eb.r && ea.steps == eb.steps && a.s() == b.s() &&
                    a.cpu->instructions_retired() == b.cpu->instructions_retired();
        // all memory a program can reach with its 16-bit displacements and pointers,
        // untranslated and through the DBAT0 mappings
        for (const u32 at : {0x0U, 0x40000U, 0x80000U}) {
            const u32 n_bytes = kData + kDataSize + 0x10000U;
            const u8* pa = a.bus.ram_pointer(at, n_bytes);
            const u8* pb = b.bus.ram_pointer(at, n_bytes);
            same = same && pa != nullptr && pb != nullptr && std::memcmp(pa, pb, n_bytes) == 0;
            // the bus's write stamps (texture cache) advance exactly as with the interpreter
            for (u32 page = at; same && page < at + n_bytes; page += 0x1000U)
                same = *a.bus.page_epoch_pointer(page) == *b.bus.page_epoch_pointer(page);
        }
        same = same && a.bus.write_epoch() == b.bus.write_epoch();
        if (!same && failures < 3) {
            // lockstep replay: find the first translated block whose result differs
            Machine c(kCode), d(kCode);
            Rng rc{seed}, rd{seed};
            setup(c, rc, prog);
            setup(d, rd, prog);
            Jit jd(*d.cpu, d.bus);
            u64 k = 0;
            while (k < n) {
                const CpuState before = d.s();
                const Jit::Result r = jd.run(n - k);
                for (u64 t = 0; t < r.steps; ++t) (void)c.cpu->step();
                k += r.steps;
                if (!(c.s() == d.s())) {
                    std::printf("    first divergence: block at %08X, %llu steps\n", before.pc, static_cast<unsigned long long>(r.steps));
                    for (u64 t = 0; t < r.steps + 1U && t < 70U; ++t) {
                        u32 w = 0;
                        (void)d.bus.read32(before.pc + 4U * static_cast<u32>(t), w);
                        std::printf("      %08X: %08X\n", before.pc + 4U * static_cast<u32>(t), w);
                    }
                    for (unsigned g = 0; g < 32; ++g)
                        if (c.s().gpr[g] != d.s().gpr[g] || before.gpr[g] != c.s().gpr[g])
                            std::printf("      r%-2u before %08X  interp %08X  jit %08X\n", g, before.gpr[g], c.s().gpr[g], d.s().gpr[g]);
                    std::printf("      cr %08X/%08X/%08X xer %08X/%08X/%08X lr %08X/%08X ctr %08X/%08X\n", before.cr, c.s().cr, d.s().cr,
                                before.xer, c.s().xer, d.s().xer, c.s().lr, d.s().lr, c.s().ctr, d.s().ctr);
                    break;
                }
                if (r.result != StepResult::Executed && r.result != StepResult::ExceptionTaken) break;
            }
        }
        if (!same) {
            if (failures++ < 5) {
                std::printf("    program %d (seed %llx): interp %s/%llu jit %s/%llu pc %08X/%08X\n", p, static_cast<unsigned long long>(seed),
                            to_string(ea.r), static_cast<unsigned long long>(ea.steps), to_string(eb.r), static_cast<unsigned long long>(eb.steps),
                            a.s().pc, b.s().pc);
                for (unsigned g = 0; g < 32; ++g)
                    if (a.s().gpr[g] != b.s().gpr[g]) std::printf("      r%u %08X vs %08X\n", g, a.s().gpr[g], b.s().gpr[g]);
                if (a.s().cr != b.s().cr) std::printf("      cr %08X vs %08X\n", a.s().cr, b.s().cr);
                if (a.s().xer != b.s().xer) std::printf("      xer %08X vs %08X\n", a.s().xer, b.s().xer);
                if (a.s().lr != b.s().lr) std::printf("      lr %08X vs %08X\n", a.s().lr, b.s().lr);
                if (a.s().ctr != b.s().ctr) std::printf("      ctr %08X vs %08X\n", a.s().ctr, b.s().ctr);
            }
        }
        total_steps += eb.steps;
        block_steps += j.stats().block_steps;
        interp_steps += j.stats().interpreter_steps;
        invalidations += j.stats().invalidations;
        ++programs;
    }
    CHECK_EQ(failures, 0U);
    std::printf("[jit] %llu random programs, %llu steps: %llu in translated blocks, %llu by the interpreter, %llu invalidations\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(total_steps),
                static_cast<unsigned long long>(block_steps), static_cast<unsigned long long>(interp_steps),
                static_cast<unsigned long long>(invalidations));
    CHECK(block_steps > total_steps / 2U); // most of the work really ran as native code
    CHECK(invalidations > 0U);
}

TEST_MAIN("jit")

// ---------------------------------------------------------------------------
// Native floating point (src/ppc/jit.cpp, "Native floating point")
// ---------------------------------------------------------------------------
namespace {
namespace F = fpu;

double dbl(u64 b) { double x; std::memcpy(&x, &b, 8); return x; }
u64 bits_of(double x) { u64 b; std::memcpy(&b, &x, 8); return b; }
constexpr u64 kS = 0x8000000000000000ULL;
unsigned bexp(u64 b) { return static_cast<unsigned>((b >> 52) & 0x7FFU); }
bool is_zero(u64 b) { return (b & ~kS) == 0U; }

// The algorithm of Compiler::fp_slot in C++, step by step (same conditions for the
// slow path). Checked against fpu::arith on any host; the AArch64 code is checked
// against the interpreter by the lockstep test below.
struct FastSlot { bool native; u64 bits; bool fi, fr; };
void two_sum(double x, double y, double& s, double& err) { s = x + y; const double bb = s - x; err = (x - (s - bb)) + (y - bb); }
FastSlot fast_slot(F::Op op, u64 A, u64 B, u64 C, bool single) {
    FastSlot r{false, 0, false, false};
    const double a = dbl(A), c = dbl(C);
    double b = dbl(B);
    if (op == F::Op::Sub || op == F::Op::MSub || op == F::Op::NMSub) b = -b;
    double s = 0;
    u64 eb = 0;
    bool allow_zero = true;
    switch (op) {
    case F::Op::Add: case F::Op::Sub: { double err = 0; two_sum(a, b, s, err); eb = bits_of(err); break; }
    case F::Op::Mul: s = a * c; eb = bits_of(std::fma(a, c, -s)); allow_zero = false; break;
    case F::Op::Div:
        if (is_zero(A) || bexp(A) < 256U) return r;
        s = a / b;
        eb = bits_of(std::fma(-s, b, a)) ^ (B & kS);
        allow_zero = false;
        break;
    case F::Op::MAdd: case F::Op::MSub: case F::Op::NMAdd: case F::Op::NMSub: {
        const double u1 = a * c;
        const u64 ub = bits_of(u1);
        if (is_zero(ub) ? !(is_zero(A) || is_zero(C)) : (bexp(ub) < 256U || bexp(ub) > 2045U)) return r;
        if (!is_zero(bits_of(b)) && bexp(bits_of(b)) < 256U) return r;
        s = std::fma(a, c, b);
        const double u2 = std::fma(a, c, -u1);
        double al = 0, z = 0, b1 = 0, b2 = 0;
        two_sum(b, u2, al, z);
        two_sum(u1, al, b1, b2);
        const double g = (b1 - s) + b2, r2 = g + z, r3 = z - (r2 - g);
        eb = is_zero(bits_of(r2)) ? bits_of(r3) : bits_of(r2);
        break;
    }
    case F::Op::Round: s = b; break;
    default: return r;
    }
    const u64 sb = bits_of(s);
    const bool fie = !is_zero(eb), fre = fie && ((eb ^ sb) >> 63) != 0U;
    if (!single) {
        if (is_zero(sb) ? !allow_zero : (bexp(sb) < 256U || bexp(sb) > 2045U)) return r;
        r.bits = sb; r.fi = fie; r.fr = fre;
    } else {
        if (is_zero(sb) && !allow_zero) return r;
        if (fie && (sb & 0x1FFFFFFFULL) == 0x10000000ULL) return r;
        const u64 tb = bits_of(static_cast<double>(static_cast<float>(s)));
        if (!is_zero(sb) && (bexp(tb) < 898U || bexp(tb) > 1150U)) return r;
        const bool diff = tb != sb;
        r.bits = tb; r.fi = diff || fie; r.fr = diff ? (tb & ~kS) > (sb & ~kS) : fre;
    }
    if (op == F::Op::NMAdd || op == F::Op::NMSub) r.bits ^= kS;
    r.native = true;
    return r;
}

// Operands that reach the hard cases: cancellation, operands 1-2 ulp apart, exact
// results, single-precision midpoints, extremes, specials.
u64 fp_operand(Rng& g, u64 ref) {
    const u64 m = static_cast<u64>(g.next()) << 32 | g.next();
    switch (g.u(10)) {
    case 9: return (m & 0x800FFFFFFFFFFFFFULL) | (static_cast<u64>(g.u(300)) << 52); // tiny and subnormal
    case 0: return (m & 0x800FFFFFFFFFFFFFULL) | (static_cast<u64>(1023U - 40U + g.u(80)) << 52);
    case 1: return (m & 0x800FFFFFE0000000ULL) | (static_cast<u64>(1023U - 20U + g.u(40)) << 52); // single values
    case 2: return (m & 0x800FF00000000000ULL) | (static_cast<u64>(1023U - 8U + g.u(16)) << 52);  // few bits
    case 3: return ref + g.u(5) - 2U;
    case 4: return (ref ^ kS) + g.u(5) - 2U;
    case 5: return (m & 0x800FFFFFFFFFFFFFULL) | (static_cast<u64>(g.u(2048)) << 52);
    case 6: {
        const u64 sp[] = {0, 1, 0x3FF0000000000000ULL, 0x7FF0000000000000ULL, 0x7FF8000000000000ULL, 0x7FF0000000000001ULL,
                          0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0x47EFFFFFE0000000ULL, 0x3810000000000000ULL, 0x36A0000000000000ULL};
        return sp[g.u(11)] ^ (g.u(2) ? kS : 0U);
    }
    case 7: return (m & 0x800FFFFFFFFFFFFFULL) | (static_cast<u64>(1023U - 150U + g.u(300)) << 52);
    default: return (m & 0x800FFFFFF0000000ULL) | (static_cast<u64>(1023U - 3U + g.u(6)) << 52) | 0x10000000ULL; // single midpoints
    }
}
} // namespace

TEST_CASE(native_fp_algorithm_matches_the_fpu_engine) {
    const F::Op ops[] = {F::Op::Add, F::Op::Sub, F::Op::Mul, F::Op::Div, F::Op::MAdd, F::Op::MSub, F::Op::NMAdd, F::Op::NMSub, F::Op::Round};
    Rng g{0xF00DFACE1234ULL};
    u64 cases = 0, native = 0, mismatches = 0;
    for (const F::Op op : ops) {
        for (int single = 0; single < 2; ++single) {
            if (op == F::Op::Round && single == 0) continue;
            for (int k = 0; k < 150000 / kScale; ++k) {
                const u64 A = fp_operand(g, 0), C = fp_operand(g, A);
                u64 B = fp_operand(g, g.u(2) ? A : C);
                if (op >= F::Op::MAdd && op <= F::Op::NMSub && g.u(3) == 0U) { // addend near -a*c: cancellation
                    const double p = dbl(A) * dbl(C);
                    B = bits_of(op == F::Op::MSub || op == F::Op::NMSub ? p : -p) + g.u(5) - 2U;
                }
                ++cases;
                const FastSlot m = fast_slot(op, A, B, C, single != 0);
                if (!m.native) continue;
                ++native;
                const F::Result r = F::arith(op, A, B, C, single != 0, 0);
                const u32 fprf = is_zero(m.bits) ? ((m.bits >> 63) != 0U ? 0x12U : 0x02U) : ((m.bits >> 63) != 0U ? 0x08U : 0x04U);
                if (r.bits != m.bits || r.fi != m.fi || r.fr != m.fr || r.exceptions != (m.fi ? F::XX : 0U) || r.fprf != fprf) {
                    if (mismatches++ < 5)
                        std::printf("    op %d single %d A %016llX B %016llX C %016llX: engine %016llX fi %d fr %d, native %016llX fi %d fr %d\n",
                                    static_cast<int>(op), single, static_cast<unsigned long long>(A), static_cast<unsigned long long>(B),
                                    static_cast<unsigned long long>(C), static_cast<unsigned long long>(r.bits), r.fi, r.fr,
                                    static_cast<unsigned long long>(m.bits), m.fi, m.fr);
                }
            }
        }
    }
    CHECK_EQ(mismatches, 0U);
    CHECK(native > cases / 2U);
    std::printf("[jit] native FP algorithm: %llu operations, %llu on the native path, all identical to fpu::arith\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(native));
}

TEST_CASE(jit_native_fp_matches_interpreter) {
    if (!jit::host_can_execute()) {
        std::printf("[jit] host is not AArch64: native FP equivalence runs on the ARM64 CI\n");
        return;
    }
    Rng rng{0xFEEDF00D5EEDULL};
    unsigned failures = 0;
    u64 steps = 0, helper = 0, native_blocks = 0;
    for (int p = 0; p < 2000 / kScale; ++p) {
        // FP and paired-single arithmetic, moves, selects and compares only
        std::vector<u32> prog;
        const u32 len = 8U + rng.u(40);
        for (u32 n = 0; n < len; ++n) {
            const u32 fa = rng.u(32), fb = rng.u(32), fc = rng.u(32), fd = rng.u(32);
            const bool rc = rng.u(4) == 0U;
            const u32 a5[] = {21, 20, 25, 18, 29, 28, 31, 30};
            switch (rng.u(8)) {
            case 0: case 1: prog.push_back(63U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | a5[rng.u(8)] << 1 | (rc ? 1U : 0U)); break;
            case 2: case 3: prog.push_back(59U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | a5[rng.u(8)] << 1 | (rc ? 1U : 0U)); break;
            case 4: case 5: {
                const u32 xs[] = {21, 20, 25, 18, 29, 28, 31, 30, 10, 11, 12, 13, 14, 15, 23};
                prog.push_back(4U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | xs[rng.u(15)] << 1 | (rc ? 1U : 0U));
                break;
            }
            case 6: {
                const u32 xs[] = {0, 32, 12, 40, 72, 136, 264};
                const u32 x = xs[rng.u(7)];
                prog.push_back(63U << 26 | (x == 0U || x == 32U ? rng.u(8) << 2 : fd) << 21 | fa << 16 | fb << 11 | x << 1 |
                               (rc && x != 0U && x != 32U ? 1U : 0U));
                if (rng.u(2)) prog.push_back(63U << 26 | fd << 21 | fa << 16 | fb << 11 | fc << 6 | 23U << 1); // fsel
                break;
            }
            default: {
                const u32 xs[] = {0, 32, 64, 96, 40, 72, 136, 264, 528, 560, 592, 624};
                const u32 x = xs[rng.u(12)];
                prog.push_back(4U << 26 | (x < 128U ? rng.u(8) << 2 : fd) << 21 | fa << 16 | fb << 11 | x << 1 | (rc && x >= 128U ? 1U : 0U));
                break;
            }
            }
        }
        prog.push_back(B(-static_cast<s32>(4U * prog.size())));
        const u64 seed = rng.s;
        Machine a(kCode), b(kCode);
        auto init = [&](Machine& m) {
            Rng r{seed};
            for (u32 n = 0; n < prog.size(); ++n) m.bus.write32(kCode + 4U * n, prog[n]);
            m.s().pc = kCode;
            m.s().msr = r.u(16) == 0U ? 0U : MSR_FP;
            m.s().hid2 = r.u(16) == 0U ? 0U : HID2_PSE;
            m.s().cr = r.next();
            u64 prev = 0x3FF0000000000000ULL;
            for (auto& f : m.s().fpr) { f.ps0 = fp_operand(r, prev); f.ps1 = fp_operand(r, f.ps0); prev = f.ps1; }
            // mostly round-to-nearest with exceptions disabled (the native path); sometimes not
            u32 fpscr = r.next() & 0xFFF7F8FFU;
            if (r.u(8) != 0U) fpscr &= ~0xFFU; else fpscr &= ~(r.u(2) ? 0xF8U : 0U);
            m.s().fpscr = fpscr;
        };
        init(a);
        init(b);
        Jit j(*b.cpu, b.bus);
        const u64 n = 100U + rng.u(400);
        const RunEnd ea = run_interp(a, n);
        const RunEnd eb = run_jit(j, n);
        const bool same = ea.r == eb.r && ea.steps == eb.steps && a.s() == b.s();
        if (!same && failures++ < 3) {
            std::printf("    FP program %d: interp %s/%llu jit %s/%llu fpscr %08X vs %08X cr %08X vs %08X\n", p, to_string(ea.r),
                        static_cast<unsigned long long>(ea.steps), to_string(eb.r), static_cast<unsigned long long>(eb.steps),
                        a.s().fpscr, b.s().fpscr, a.s().cr, b.s().cr);
            // lockstep: first instruction whose result differs
            Machine c(kCode), d(kCode);
            init(c);
            init(d);
            Jit jd(*d.cpu, d.bus);
            u64 k = 0;
            while (k < n) {
                const CpuState before = d.s();
                const Jit::Result r = jd.run(n - k);
                for (u64 t = 0; t < r.steps; ++t) (void)c.cpu->step();
                k += r.steps;
                if (!(c.s() == d.s())) {
                    std::printf("      block at %08X, %llu steps; fpscr before %08X interp %08X jit %08X  cr %08X/%08X\n", before.pc,
                                static_cast<unsigned long long>(r.steps), before.fpscr, c.s().fpscr, d.s().fpscr, c.s().cr, d.s().cr);
                    for (u64 t = 0; t < r.steps && t < 64U; ++t) {
                        u32 w = 0;
                        (void)d.bus.read32(before.pc + 4U * static_cast<u32>(t), w);
                        std::printf("        %08X: %08X\n", before.pc + 4U * static_cast<u32>(t), w);
                    }
                    for (unsigned f = 0; f < 32; ++f)
                        if (!(c.s().fpr[f] == d.s().fpr[f]) || !(before.fpr[f] == c.s().fpr[f]))
                            std::printf("      f%-2u before %016llX/%016llX interp %016llX/%016llX jit %016llX/%016llX\n", f,
                                        static_cast<unsigned long long>(before.fpr[f].ps0), static_cast<unsigned long long>(before.fpr[f].ps1),
                                        static_cast<unsigned long long>(c.s().fpr[f].ps0), static_cast<unsigned long long>(c.s().fpr[f].ps1),
                                        static_cast<unsigned long long>(d.s().fpr[f].ps0), static_cast<unsigned long long>(d.s().fpr[f].ps1));
                    break;
                }
                if (r.result != StepResult::Executed && r.result != StepResult::ExceptionTaken) break;
            }
        }
        steps += j.stats().block_steps;
        helper += j.stats().helper_steps;
        native_blocks += j.stats().blocks_compiled;
    }
    CHECK_EQ(failures, 0U);
    std::printf("[jit] native FP: %d programs, %llu instructions in blocks, %llu of them by the interpreter call (slow path), "
                "the rest native; identical to the interpreter\n",
                2000 / kScale, static_cast<unsigned long long>(steps), static_cast<unsigned long long>(helper));
    CHECK(helper < steps / 2U);
    (void)native_blocks;
}

// Linked blocks follow the instruction-translation context: the same effective
// addresses hold different code with instruction translation off (physical
// 0x4000) and on (IBAT0: effective 0-128 KiB -> physical 0x40000), and the test
// switches MSR[IR] between runs. A link made in one context must not be taken in
// the other.
TEST_CASE(jit_links_follow_the_translation_context) {
    if (!jit::host_can_execute()) {
        std::printf("[jit] host is not AArch64: block-linking contexts run on the ARM64 CI\n");
        return;
    }
    auto prog = [](u32 add_a, u32 add_b) {
        return std::vector<u32>{D(14, 3, 3, add_a), B(8), D(14, 9, 9, 1), D(14, 4, 4, add_b),
                                BC(16, 0, 8), D(14, 7, 7, 1), B(-24)}; // bdnz skips one instruction; loop
    };
    auto init = [&](Machine& m) {
        const auto p0 = prog(1, 3), p1 = prog(100, 300);
        for (u32 k = 0; k < p0.size(); ++k) m.bus.write32(kCode + 4U * k, p0[k]);
        for (u32 k = 0; k < p1.size(); ++k) m.bus.write32(0x40000U + kCode + 4U * k, p1[k]);
        m.s().pc = kCode;
        m.s().ibat[0] = BatPair{0x00000003U, 0x00040002U};
        m.s().ctr = 1000000;
    };
    Machine a(kCode), b(kCode);
    init(a);
    init(b);
    Jit j(*b.cpu, b.bus);
    Rng r{77};
    bool same = true;
    for (int round = 0; round < 60 && same; ++round) {
        const u64 n = 5U + r.u(200);
        (void)run_interp(a, n);
        (void)run_jit(j, n);
        same = a.s() == b.s() && a.cpu->instructions_retired() == b.cpu->instructions_retired();
        if (!same) std::printf("    round %d: r3 %u/%u r4 %u/%u pc %08X/%08X\n", round, a.s().gpr[3], b.s().gpr[3], a.s().gpr[4],
                               b.s().gpr[4], a.s().pc, b.s().pc);
        const u32 msr = a.s().msr ^ MSR_IR;
        a.s().msr = msr;
        b.s().msr = msr;
    }
    CHECK(same);
    CHECK(j.stats().links > 4U);
    CHECK(a.s().gpr[3] > 1000U); // both programs really ran
    std::printf("[jit] linking across 60 context switches: %llu links, identical to the interpreter\n",
                static_cast<unsigned long long>(j.stats().links));
}

// ---------------------------------------------------------------------------
// Directed cases (added after the ARM64 mutation run showed what the random
// programs did not reach).
// ---------------------------------------------------------------------------
namespace {
// Runs `n` steps from `pc` with the interpreter and with the JIT on two machines
// set up by `init`; true when state, retired count, RAM (first 1 MiB) and write
// stamps are identical. `between` runs on both machines after each run of `slice` steps.
template <class Init, class Between>
bool lockstep_same(Init init, u64 n, u64 slice, Between between, Jit::Stats* stats = nullptr) {
    Machine a(kCode), b(kCode);
    init(a);
    init(b);
    Jit j(*b.cpu, b.bus);
    for (u64 done = 0; done < n; done += slice) {
        (void)run_interp(a, slice);
        (void)run_jit(j, slice);
        between(a);
        between(b);
    }
    if (stats != nullptr) *stats = j.stats();
    bool same = a.s() == b.s() && a.cpu->instructions_retired() == b.cpu->instructions_retired() &&
                a.bus.write_epoch() == b.bus.write_epoch();
    const u8* pa = a.bus.ram_pointer(0, 0x100000U);
    const u8* pb = b.bus.ram_pointer(0, 0x100000U);
    same = same && std::memcmp(pa, pb, 0x100000U) == 0;
    if (!same) std::printf("    state differs: pc %08X/%08X fpscr %08X/%08X r3 %08X/%08X\n", a.s().pc, b.s().pc, a.s().fpscr,
                           b.s().fpscr, a.s().gpr[3], b.s().gpr[3]);
    return same;
}
void nothing(Machine&) {}
} // namespace

TEST_CASE(jit_directed_fp_flags_of_the_second_slot) {
    if (!jit::host_can_execute()) return;
    // ps_add with an exact slot 0 and an inexact slot 1, FPSCR clear: XX and FX come from slot 1
    const double pairs[][4] = {{1.0, 1.0, 1.0, 0.1}, {2.0, 0.5, 3.0, 1e-30}, {-1.0, 1.0, 1.0 / 3.0, 1.0 / 7.0}};
    for (const auto& p : pairs) {
        auto init = [&](Machine& m) {
            m.code(kCode, {4U << 26 | 3U << 21 | 1U << 16 | 2U << 11 | 21U << 1,   // ps_add f3, f1, f2
                           4U << 26 | 4U << 21 | 1U << 16 | 2U << 11 | 25U << 1,   // ps_mul f4, f1, f2
                           B(-8)});
            m.s().pc = kCode;
            m.s().msr = MSR_FP;
            m.s().hid2 = HID2_PSE;
            m.s().fpscr = 0;
            std::memcpy(&m.s().fpr[1].ps0, &p[0], 8); std::memcpy(&m.s().fpr[2].ps0, &p[1], 8);
            std::memcpy(&m.s().fpr[1].ps1, &p[2], 8); std::memcpy(&m.s().fpr[2].ps1, &p[3], 8);
        };
        CHECK(lockstep_same(init, 3, 3, nothing));
    }
}

TEST_CASE(jit_directed_division_of_tiny_numbers) {
    if (!jit::host_can_execute()) return;
    // fdiv / fdivs where a and b are tiny (some subnormal) but the quotient is normal
    Rng r{4242};
    u64 slow = 0, in_blocks = 0;
    for (int k = 0; k < 1500 / kScale; ++k) {
        // mostly subnormal or barely normal operands: there the remainder a - q*b underflows
        const unsigned range = k % 3 == 0 ? 200U : 3U;
        const u64 a = (static_cast<u64>(r.next()) << 32 | r.next()) & 0x800FFFFFFFFFFFFFULL | static_cast<u64>(r.u(range)) << 52;
        const u64 b = (static_cast<u64>(r.next()) << 32 | r.next()) & 0x800FFFFFFFFFFFFFULL | static_cast<u64>(r.u(range)) << 52;
        auto init = [&](Machine& m) {
            // fdiv last: its FI/FR are what the comparison sees (fdivs would hide
            // them, its second rounding almost always sets FI on its own)
            m.code(kCode, {59U << 26 | 4U << 21 | 1U << 16 | 2U << 11 | 18U << 1,   // fdivs f4, f1, f2
                           63U << 26 | 3U << 21 | 1U << 16 | 2U << 11 | 18U << 1,   // fdiv f3, f1, f2
                           B(-8)});
            m.s().pc = kCode;
            m.s().msr = MSR_FP;
            m.s().fpscr = 0;
            m.s().fpr[1].ps0 = a;
            m.s().fpr[2].ps0 = b;
        };
        Jit::Stats st{};
        CHECK(lockstep_same(init, 3, 3, nothing, &st));
        slow += st.helper_steps;
        in_blocks += st.block_steps;
    }
    // Below 2^-767 the remainder a - q*b may underflow, so these divisions are
    // expected on the slow path; the counts are printed for the test log.
    std::printf("[jit] tiny divisions: %llu instructions in blocks, %llu by the slow path\n",
                static_cast<unsigned long long>(in_blocks), static_cast<unsigned long long>(slow));
}

TEST_CASE(jit_directed_store_into_a_linked_block) {
    if (!jit::host_can_execute()) return;
    // A -> B -> D -> A. B computes the store address of D: a data word, except on
    // the 5th pass, when it is D's own third instruction. By then D is entered
    // through a link, and its store must stop it before the rewritten instruction.
    constexpr u32 A = kCode, Bb = kCode + 0x100, Dd = kCode + 0x200, data = kData;
    auto init = [](Machine& m) {
        m.code(A, {D(14, 4, 4, 1), B(static_cast<s32>(Bb - A - 4))});
        m.code(Bb, {D(14, 5, 5, 1),                                  // r5++
                    D(26, 5, 9, 5),                                  // xori r9, r5, 5
                    X31(26, 9, 9, 0),                                // cntlzw r9, r9 (32 only when r5 == 5)
                    M(21, 9, 9, 27, 31, 31, false),                  // srwi r9, r9, 5
                    XO(235, 9, 9, 10, false, false),                 // mullw r9, r9, r10
                    XO(266, 8, 11, 9, false, false),                 // add r8, r11, r9
                    B(static_cast<s32>(Dd - (Bb + 0x18)))});
        m.code(Dd, {D(36, 6, 8, 0),                                  // stw r6, 0(r8)
                    D(14, 0, 0, 0),
                    D(14, 3, 3, 1),                                  // rewritten on the 5th pass
                    B(static_cast<s32>(A - (Dd + 0xC)))});
        m.s().pc = A;
        m.s().gpr[6] = 0x38630064U;                                  // addi r3, r3, 100
        m.s().gpr[10] = (Dd + 8U) - data;
        m.s().gpr[11] = data;
    };
    Jit::Stats st{};
    CHECK(lockstep_same(init, 13 * 20, 13 * 20, nothing, &st));
    CHECK(st.links > 0U);
}

TEST_CASE(jit_directed_data_page_that_becomes_code) {
    if (!jit::host_can_execute()) return;
    // Phase 1 stores to page P (the translation cache gets a write entry for it);
    // phase 2 runs code in P that rewrites P+0x10 with a different instruction on
    // every pass, so the block there must be retranslated each time.
    constexpr u32 P = 0x00006000U;
    auto init = [](Machine& m) {
        m.code(kCode, {D(36, 3, 0, P + 0x10),                     // stw r3, P+0x10
                       D(14, 7, 7, 1),                            // r7++
                       D(11, 0, 7, 50),                           // cmpwi r7, 50
                       BC(12, 0, 8),                              // blt -> loop
                       B(static_cast<s32>(P - (kCode + 0x10))),   // after 50 passes: go to P
                       B(-0x14)});
        m.code(P, {D(14, 5, 5, 1),                                // r5++
                   D(14, 3, 3, 1),                                // r3++: the next instruction to write
                   D(36, 3, 0, P + 0x10),                         // stw r3, P+0x10
                   B(4),                                          // b P+0x10
                   D(14, 0, 0, 0),                                // P+0x10: rewritten
                   B(-0x14)});                                    // back to P
        m.s().pc = kCode;
        m.s().gpr[3] = 0x38A50002U;                               // "addi r5, r5, 2", then 3, 4, ...
    };
    CHECK(lockstep_same(init, 2000, 2000, nothing));
}

TEST_CASE(jit_directed_interrupts_off_and_on_stay_in_the_block) {
    if (!jit::host_can_execute()) return;
    // The SDK's OSDisableInterrupts / OSRestoreInterrupts: mfmsr, clear EE, mtmsr,
    // ..., mtmsr back. Only EE changes and nothing is pending, so the block goes
    // on (Dolphin's JIT checks for pending exceptions after mtmsr the same way).
    auto init = [](Machine& m) {
        m.code(kCode, {X31(83, 3, 0, 0),                              // mfmsr r3
                       21U << 26 | 3U << 21 | 4U << 16 | 17U << 6 | 15U << 1, // rlwinm r4, r3, 0, 17, 15 (EE off)
                       X31(146, 4, 0, 0),                             // mtmsr r4
                       D(14, 5, 5, 1),                                // r5++
                       X31(146, 3, 0, 0),                             // mtmsr r3
                       D(14, 6, 6, 1),                                // r6++
                       B(-0x18)});
        m.s().pc = kCode;
        m.s().msr |= MSR_EE;
    };
    Jit::Stats st{};
    CHECK(lockstep_same(init, 7000, 1000, nothing, &st));
    CHECK(st.helper_steps >= 2000U);
    CHECK_EQ(st.recheck_exits, 0U); // nothing pending: interrupts on and off stay in the block
    // EE turned on while an interrupt is pending: the block leaves at once and the
    // interrupt is taken where the interpreter takes it
    auto pending = [](Machine& m) {
        m.code(0x500, {B(0)});                                        // external interrupt handler: b .
        m.code(kCode, {D(14, 7, 7, 1),                                // r7++
                       X31(83, 3, 0, 0),                              // mfmsr r3
                       D(24, 3, 3, 0x8000),                           // ori r3, r3, EE
                       X31(146, 3, 0, 0),                             // mtmsr r3
                       D(14, 8, 8, 1),                                // r8++ (not reached before the interrupt)
                       B(-0x14)});
        m.s().pc = kCode;
        m.s().msr &= ~MSR_EE;
        m.cpu->set_external_interrupt(true);
    };
    CHECK(lockstep_same(pending, 300, 100, nothing, &st));
    CHECK(st.recheck_exits >= 1U);
}

TEST_CASE(jit_directed_returns_go_through_the_dispatch_table) {
    if (!jit::host_can_execute()) return;
    // A loop that calls a function (bl / blr) and one that calls through CTR
    // (bctrl): the returns and indirect calls jump block to block through the
    // dispatch table instead of returning to run() each time.
    constexpr u32 F = kCode + 0x400U;
    auto init = [](Machine& m) {
        m.code(kCode, {D(14, 5, 5, 1),                                   // r5++
                       B(static_cast<s32>(F - (kCode + 4U))) | 1U,       // bl F
                       0x4E800421U,                                      // bctrl (CTR = F)
                       B(-0xC)});
        m.code(F, {D(14, 6, 6, 1), 0x4E800020U});                       // r6++ ; blr
        m.s().pc = kCode;
        m.s().ctr = F;
    };
    Jit::Stats st{};
    CHECK(lockstep_same(init, 20000, 20000, nothing, &st));
    CHECK(st.dispatch_fills >= 1U);
    CHECK(st.block_runs < 100U); // without the table: one run per block, about 5000

    // the table follows the translation context: kCode holds different code with
    // and without instruction translation (IBAT0 maps it to 0x44000); a blr from a
    // block mapped the same in both must not reach the other context's block
    auto ctx = [](Machine& m) {
        m.code(0x80000, {D(14, 4, 4, 1), 0x4E800020U});                 // r4++ ; blr (LR = kCode)
        m.code(kCode, {D(14, 3, 3, 1), B(0x80000 - static_cast<s32>(kCode))});
        m.code(0x40000 + kCode, {D(14, 3, 3, 1000), B(0x80000 - static_cast<s32>(kCode))});
        m.s().pc = 0x80000;
        m.s().lr = kCode;
        m.s().ibat[0] = BatPair{0x00000003U, 0x00040002U};
        m.s().ibat[1] = BatPair{0x00080003U, 0x00080002U};
    };
    auto flip = [](Machine& m) { m.s().msr ^= MSR_IR; };
    CHECK(lockstep_same(ctx, 30 * 37, 37, flip, &st));

    // a dispatch target rewritten by the program: its entry must not be used again
    constexpr u32 G = kCode + 0x800U;
    auto rewrite = [](Machine& m) {
        m.code(kCode, {D(36, 3, 0, G),                                   // stw r3, G (new first instruction)
                       D(14, 3, 3, 1),                                   // next one adds one more
                       0x4E800421U,                                      // bctrl (CTR = G)
                       B(-0xC)});
        m.code(G, {D(14, 5, 5, 0), 0x4E800020U});                       // rewritten ; blr
        m.s().pc = kCode;
        m.s().ctr = G;
        m.s().gpr[3] = 0x38A50001U;                                      // "addi r5, r5, 1", then 2, 3, ...
    };
    CHECK(lockstep_same(rewrite, 3000, 3000, nothing, &st));
    CHECK(st.invalidations > 0U);
}

TEST_CASE(jit_directed_links_across_contexts_with_a_shared_page) {
    if (!jit::host_can_execute()) return;
    // Block A at 0x80000 is mapped identically with and without instruction
    // translation (IBAT1 is an identity map there); it jumps to kCode, which holds
    // different code in each context (IBAT0 maps it to 0x44000). A link made from A
    // in one context must not be followed in the other.
    auto init = [](Machine& m) {
        m.code(0x80000, {D(14, 4, 4, 1), B(static_cast<s32>(kCode) - 0x80004)});
        m.code(kCode, {D(14, 3, 3, 1), B(0x80000 - static_cast<s32>(kCode) - 4)});
        m.code(0x40000 + kCode, {D(14, 3, 3, 1000), B(0x80000 - static_cast<s32>(kCode) - 4)});
        m.s().pc = 0x80000;
        m.s().ibat[0] = BatPair{0x00000003U, 0x00040002U};
        m.s().ibat[1] = BatPair{0x00080003U, 0x00080002U};
    };
    auto flip = [](Machine& m) { m.s().msr ^= MSR_IR; };
    Jit::Stats st{};
    CHECK(lockstep_same(init, 30 * 37, 37, flip, &st));
    CHECK(st.links > 0U);
}
