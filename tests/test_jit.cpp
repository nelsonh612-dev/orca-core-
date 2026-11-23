// PowerPC -> AArch64 JIT: translation on any host, and on AArch64 hosts the
// equivalence of translated execution with the interpreter (registers, CR, XER,
// LR, CTR, memory, stops and retired instruction counts).
#include "orca/jit/code_buffer.hpp"
#include "orca/ppc/jit.hpp"
#include "machine.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>
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

constexpr u32 kCode = 0x00004000U, kData = 0x00010000U, kDataSize = 0x2000U;

// A random instruction from the translated set (plus a few the JIT leaves to the
// interpreter). r1-r27: values; r28/r29: data pointers; r30/r31: index values.
u32 random_insn(Rng& r, u32 at, u32 code_end) {
    auto reg = [&] { return 1U + r.u(27); };
    auto anyreg = [&] { return r.u(32); };
    const bool rc = r.u(2) != 0U;
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
    case 26: return 31U << 26 | reg() << 21 | SPR(268) << 11 | 371U << 1;            // mftb: interpreter
    case 27: return D(36, reg(), 0, kCode + 4U * r.u(64));                           // store into the code: invalidation
    case 28: return D(14, 30U + r.u(2), 0, r.u(64));                                // keep the indexes small
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
    m.s().msr = 0; // real mode, no FP: FP instructions take the FP-unavailable exception in both
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
    // an untranslated first instruction gives no block
    m.code(kCode + 0x100U, {31U << 26 | 3U << 21 | SPR(268) << 11 | 371U << 1}); // mftb
    CHECK(!j.translate_for_test(kCode + 0x100U, words, n, err));
}

TEST_CASE(jit_matches_interpreter_on_random_programs) {
    if (!jit::host_can_execute()) {
        std::printf("[jit] host is not AArch64: execution equivalence runs on the ARM64 CI\n");
        return;
    }
    Rng rng{0x0DDB1A5E5EEDULL};
    u64 total_steps = 0, block_steps = 0, programs = 0, interp_steps = 0, invalidations = 0;
    unsigned failures = 0;
    for (int p = 0; p < 3000; ++p) {
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
        // all memory a program can reach with its 16-bit displacements and pointers
        for (u32 x = 0; same && x < kData + kDataSize + 0x10000U; x += 4) {
            u32 va = 0, vb = 0;
            (void)a.bus.read32(x, va);
            (void)b.bus.read32(x, vb);
            same = va == vb;
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
