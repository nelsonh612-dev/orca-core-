// Randomised differential test: interpreter vs. an independent reference model.
//
// The reference model is written from the PEM definitions using *different*
// techniques from the interpreter (64-bit signed arithmetic for overflow,
// floor division for algebraic shifts, bit loops for masks, std::countl_zero),
// so a shared mistake is unlikely. After each instruction the *entire*
// architectural state is compared, which also catches stray side effects.
// Register operands are random and may alias (RT == RA == RB, RA = 0, ...).
#include "machine.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

#include <bit>
#include <climits>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::ppc;
using namespace orca::test;
namespace A = orca::test::ppc_asm;

namespace {

struct Rng {
    u64 s;
    u64 next() { s += 0x9E3779B97F4A7C15ULL; u64 z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
    u32 u(u32 n) { return static_cast<u32>(next() % n); }
    u32 value() {
        static constexpr u32 edges[] = {0, 1, 2, 31, 32, 33, 63, 64, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000U,
                                        0x80000001U, 0xFFFFFFFEU, 0xFFFFFFFFU, 0x55555555U, 0xAAAAAAAAU};
        return u(10) < 3 ? edges[u(sizeof edges / sizeof edges[0])] : static_cast<u32>(next());
    }
};

constexpr s64 kMin = INT32_MIN, kMax = INT32_MAX;
u32 cr0_of(u32 v, u32 xer) { const auto s = static_cast<s32>(v); return (s < 0 ? 8U : s > 0 ? 4U : 2U) | ((xer >> 31) & 1U); }
void set_field(CpuState& st, unsigned f, u32 nib) { const unsigned sh = 28 - 4 * f; st.cr = (st.cr & ~(0xFU << sh)) | (nib << sh); }

struct Ref { u32 value; bool ca; bool ov; };
// Three-operand add in both unsigned (carry) and signed (overflow) arithmetic.
Ref ref_add(u32 x, u32 y, u32 cin) {
    const u64 uf = static_cast<u64>(x) + y + cin;
    const s64 sf = static_cast<s64>(static_cast<s32>(x)) + static_cast<s32>(y) + static_cast<s64>(cin);
    return {static_cast<u32>(uf), uf > 0xFFFFFFFFULL, sf < kMin || sf > kMax};
}
u32 ref_mask(unsigned mb, unsigned me) {
    u32 m = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const bool in = mb <= me ? (bit >= mb && bit <= me) : (bit >= mb || bit <= me);
        if (in) m |= 1U << (31 - bit);
    }
    return m;
}
u32 ref_rotl(u32 a, unsigned n) { const u64 aa = (static_cast<u64>(a) << 32) | a; return static_cast<u32>(aa >> (32 - (n & 31))); }
Ref ref_sraw(u32 a, unsigned n) {
    if (n > 32) n = 32;
    const s64 q = static_cast<s32>(a);
    const s64 d = 1LL << n;
    const s64 r = q >= 0 ? q / d : -((-q + d - 1) / d); // floor division
    return {static_cast<u32>(r), q < 0 && r * d != q, false};
}

int g_cases = 0;

// Executes `ins` from `init` and compares with `expected` (pc advanced automatically).
void run_case(const char* name, u32 ins, const CpuState& init, CpuState expected) {
    static Machine machine;
    Machine* m = &machine;
    m->s() = init;
    m->s().pc = 0x1000;
    expected.pc = 0x1004;
    m->bus.write32(0x1000, ins);
    const StepResult r = m->cpu->step();
    ++g_cases;
    ++orca::test::Registry::get().checks;
    if (r != StepResult::Executed || !(m->s() == expected)) {
        ++orca::test::Registry::get().failures;
        if (orca::test::Registry::get().failures < 20) {
            std::fprintf(stderr, "  MISMATCH %s ins=%08x result=%s\n", name, ins, to_string(r));
            for (unsigned i = 0; i < 32; ++i)
                if (m->s().gpr[i] != expected.gpr[i]) std::fprintf(stderr, "    r%u got %08x want %08x\n", i, m->s().gpr[i], expected.gpr[i]);
            if (m->s().cr != expected.cr) std::fprintf(stderr, "    cr got %08x want %08x\n", m->s().cr, expected.cr);
            if (m->s().xer != expected.xer) std::fprintf(stderr, "    xer got %08x want %08x\n", m->s().xer, expected.xer);
        }
    }
}

CpuState random_state(Rng& rng) {
    CpuState st{};
    for (auto& g : st.gpr) g = rng.value();
    st.cr = static_cast<u32>(rng.next());
    st.xer = static_cast<u32>(rng.next()) & 0xE000007FU;
    st.lr = static_cast<u32>(rng.next());
    st.ctr = static_cast<u32>(rng.next());
    return st;
}

constexpr int kIterations = 4000;

} // namespace

TEST_CASE(reference_xo_arithmetic) {
    Rng rng{0x0C0FFEE5ULL};
    struct Op { const char* name; u32 xo; bool has_b; bool sets_ca; bool no_oe;
                std::function<Ref(u32, u32, u32)> f; };
    const std::vector<Op> ops = {
        {"add", 266, true, false, false, [](u32 a, u32 b, u32) { return ref_add(a, b, 0); }},
        {"addc", 10, true, true, false, [](u32 a, u32 b, u32) { return ref_add(a, b, 0); }},
        {"adde", 138, true, true, false, [](u32 a, u32 b, u32 c) { return ref_add(a, b, c); }},
        {"addze", 202, false, true, false, [](u32 a, u32, u32 c) { return ref_add(a, 0, c); }},
        {"addme", 234, false, true, false, [](u32 a, u32, u32 c) { return ref_add(a, 0xFFFFFFFFU, c); }},
        {"subf", 40, true, false, false, [](u32 a, u32 b, u32) { return ref_add(~a, b, 1); }},
        {"subfc", 8, true, true, false, [](u32 a, u32 b, u32) { return ref_add(~a, b, 1); }},
        {"subfe", 136, true, true, false, [](u32 a, u32 b, u32 c) { return ref_add(~a, b, c); }},
        {"subfze", 200, false, true, false, [](u32 a, u32, u32 c) { return ref_add(~a, 0, c); }},
        {"subfme", 232, false, true, false, [](u32 a, u32, u32 c) { return ref_add(~a, 0xFFFFFFFFU, c); }},
        {"neg", 104, false, false, false, [](u32 a, u32, u32) {
             const s64 r = -static_cast<s64>(static_cast<s32>(a));
             return Ref{static_cast<u32>(r), false, r > kMax}; }},
        {"mullw", 235, true, false, false, [](u32 a, u32 b, u32) {
             const s64 p = static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(b);
             return Ref{static_cast<u32>(p), false, p < kMin || p > kMax}; }},
        {"mulhw", 75, true, false, true, [](u32 a, u32 b, u32) {
             const s64 p = static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(b);
             return Ref{static_cast<u32>(p >> 32), false, false}; }},
        {"mulhwu", 11, true, false, true, [](u32 a, u32 b, u32) {
             return Ref{static_cast<u32>((static_cast<u64>(a) * b) >> 32), false, false}; }},
        {"divw", 491, true, false, false, [](u32 a, u32 b, u32) {
             const s64 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
             if (sb == 0 || (sa == kMin && sb == -1)) return Ref{sa < 0 ? 0xFFFFFFFFU : 0U, false, true}; // documented choice
             return Ref{static_cast<u32>(sa / sb), false, false}; }},
        {"divwu", 459, true, false, false, [](u32 a, u32 b, u32) {
             if (b == 0) return Ref{0U, false, true}; // documented choice
             return Ref{a / b, false, false}; }},
    };
    for (const Op& op : ops) {
        for (int i = 0; i < kIterations; ++i) {
            CpuState st = random_state(rng);
            if (op.name[0] == 'd' && rng.u(8) == 0) st.gpr[rng.u(32)] = 0; // exercise divide-by-zero
            const u32 rt = rng.u(32), ra = rng.u(32), rb = op.has_b ? rng.u(32) : 0;
            const bool oe = !op.no_oe && rng.u(2) == 1, rc = rng.u(2) == 1;
            const u32 cin = (st.xer >> 29) & 1U;
            const Ref r = op.f(st.gpr[ra], st.gpr[rb], cin);
            CpuState exp = st;
            exp.gpr[rt] = r.value;
            if (op.sets_ca) exp.xer = r.ca ? (exp.xer | XER_CA) : (exp.xer & ~XER_CA);
            if (oe) exp.xer = r.ov ? (exp.xer | XER_OV | XER_SO) : (exp.xer & ~XER_OV);
            if (rc) set_field(exp, 0, cr0_of(r.value, exp.xer));
            run_case(op.name, A::XO(op.xo, rt, ra, rb, oe, rc), st, exp);
        }
    }
}

TEST_CASE(reference_logical_shift_and_misc) {
    Rng rng{0xBADC0DE1ULL};
    struct Op { const char* name; u32 xo; bool unary; bool sets_ca; std::function<Ref(u32, u32)> f; };
    const std::vector<Op> ops = {
        {"and", 28, false, false, [](u32 s, u32 b) { return Ref{s & b, false, false}; }},
        {"andc", 60, false, false, [](u32 s, u32 b) { return Ref{s & ~b, false, false}; }},
        {"or", 444, false, false, [](u32 s, u32 b) { return Ref{s | b, false, false}; }},
        {"orc", 412, false, false, [](u32 s, u32 b) { return Ref{s | ~b, false, false}; }},
        {"xor", 316, false, false, [](u32 s, u32 b) { return Ref{s ^ b, false, false}; }},
        {"nand", 476, false, false, [](u32 s, u32 b) { return Ref{~(s & b), false, false}; }},
        {"nor", 124, false, false, [](u32 s, u32 b) { return Ref{~(s | b), false, false}; }},
        {"eqv", 284, false, false, [](u32 s, u32 b) { return Ref{~(s ^ b), false, false}; }},
        {"slw", 24, false, false, [](u32 s, u32 b) { const u32 n = b & 63; return Ref{n > 31 ? 0U : static_cast<u32>(static_cast<u64>(s) << n), false, false}; }},
        {"srw", 536, false, false, [](u32 s, u32 b) { const u32 n = b & 63; return Ref{n > 31 ? 0U : static_cast<u32>(static_cast<u64>(s) >> n), false, false}; }},
        {"sraw", 792, false, true, [](u32 s, u32 b) { return ref_sraw(s, b & 63); }},
        {"cntlzw", 26, true, false, [](u32 s, u32) { return Ref{static_cast<u32>(std::countl_zero(s)), false, false}; }},
        {"extsb", 954, true, false, [](u32 s, u32) { return Ref{static_cast<u32>(static_cast<s32>(static_cast<s8>(s & 0xFF))), false, false}; }},
        {"extsh", 922, true, false, [](u32 s, u32) { return Ref{static_cast<u32>(static_cast<s32>(static_cast<s16>(s & 0xFFFF))), false, false}; }},
    };
    for (const Op& op : ops) {
        for (int i = 0; i < kIterations; ++i) {
            CpuState st = random_state(rng);
            const u32 rs = rng.u(32), ra = rng.u(32), rb = op.unary ? 0 : rng.u(32);
            const bool rc = rng.u(2) == 1;
            const Ref r = op.f(st.gpr[rs], st.gpr[rb]);
            CpuState exp = st;
            exp.gpr[ra] = r.value;
            if (op.sets_ca) exp.xer = r.ca ? (exp.xer | XER_CA) : (exp.xer & ~XER_CA);
            if (rc) set_field(exp, 0, cr0_of(r.value, exp.xer));
            run_case(op.name, A::X(op.xo, rs, ra, rb, rc), st, exp);
        }
    }
    for (int i = 0; i < kIterations; ++i) { // srawi
        CpuState st = random_state(rng);
        const u32 rs = rng.u(32), ra = rng.u(32), sh = rng.u(32);
        const bool rc = rng.u(2) == 1;
        const Ref r = ref_sraw(st.gpr[rs], sh);
        CpuState exp = st;
        exp.gpr[ra] = r.value;
        exp.xer = r.ca ? (exp.xer | XER_CA) : (exp.xer & ~XER_CA);
        if (rc) set_field(exp, 0, cr0_of(r.value, exp.xer));
        run_case("srawi", A::srawi(ra, rs, sh, rc), st, exp);
    }
}

TEST_CASE(reference_rotates) {
    Rng rng{0x5EED0005ULL};
    for (int i = 0; i < kIterations * 3; ++i) {
        CpuState st = random_state(rng);
        const u32 kind = rng.u(3);
        const u32 rs = rng.u(32), ra = rng.u(32), rb = rng.u(32), mb = rng.u(32), me = rng.u(32);
        const bool rc = rng.u(2) == 1;
        const u32 n = kind == 2 ? (st.gpr[rb] & 31U) : rb;
        const u32 rot = ref_rotl(st.gpr[rs], n), mask = ref_mask(mb, me);
        CpuState exp = st;
        exp.gpr[ra] = kind == 0 ? ((rot & mask) | (st.gpr[ra] & ~mask)) : (rot & mask);
        if (rc) set_field(exp, 0, cr0_of(exp.gpr[ra], exp.xer));
        const u32 ins = kind == 0 ? A::rlwimi(ra, rs, rb, mb, me, rc) : kind == 1 ? A::rlwinm(ra, rs, rb, mb, me, rc) : A::rlwnm(ra, rs, rb, mb, me, rc);
        run_case(kind == 0 ? "rlwimi" : kind == 1 ? "rlwinm" : "rlwnm", ins, st, exp);
    }
}

TEST_CASE(reference_immediates_and_compares) {
    Rng rng{0x1234ABCDULL};
    for (int i = 0; i < kIterations * 2; ++i) {
        CpuState st = random_state(rng);
        const u32 rt = rng.u(32), ra = rng.u(32), rb = rng.u(32), bf = rng.u(8);
        const u32 imm = rng.u(4) == 0 ? (rng.u(2) ? 0x8000U : 0x7FFFU) : rng.u(0x10000);
        const s32 simm = static_cast<s16>(static_cast<u16>(imm));
        const u32 a = st.gpr[ra], b = st.gpr[rb], so = st.xer >> 31;
        CpuState exp = st;
        u32 ins = 0; const char* name = "";
        switch (rng.u(10)) {
        case 0: name = "addi"; exp.gpr[rt] = (ra ? a : 0U) + static_cast<u32>(simm); ins = A::addi(rt, ra, simm); break;
        case 1: name = "addis"; exp.gpr[rt] = (ra ? a : 0U) + (imm << 16); ins = A::addis(rt, ra, static_cast<int>(imm)); break;
        case 2: case 3: {
            const bool dot = rng.u(2) == 1; name = dot ? "addic." : "addic";
            const Ref r = ref_add(a, static_cast<u32>(simm), 0);
            exp.gpr[rt] = r.value; exp.xer = r.ca ? (exp.xer | XER_CA) : (exp.xer & ~XER_CA);
            if (dot) set_field(exp, 0, cr0_of(r.value, exp.xer));
            ins = dot ? A::addic_(rt, ra, simm) : A::addic(rt, ra, simm);
            break;
        }
        case 4: { name = "subfic"; const Ref r = ref_add(~a, static_cast<u32>(simm), 1);
                  exp.gpr[rt] = r.value; exp.xer = r.ca ? (exp.xer | XER_CA) : (exp.xer & ~XER_CA); ins = A::subfic(rt, ra, simm); break; }
        case 5: name = "mulli"; exp.gpr[rt] = static_cast<u32>(static_cast<s64>(static_cast<s32>(a)) * simm); ins = A::mulli(rt, ra, simm); break;
        case 6: { name = "cmpwi"; const s64 x = static_cast<s32>(a);
                  set_field(exp, bf, (x < simm ? 8U : x > simm ? 4U : 2U) | so); ins = A::cmpwi(bf, ra, simm); break; }
        case 7: { name = "cmplwi"; set_field(exp, bf, (a < imm ? 8U : a > imm ? 4U : 2U) | so); ins = A::cmplwi(bf, ra, imm); break; }
        case 8: { name = "cmpw"; const s64 x = static_cast<s32>(a), y = static_cast<s32>(b);
                  set_field(exp, bf, (x < y ? 8U : x > y ? 4U : 2U) | so); ins = A::cmpw(bf, ra, rb); break; }
        default: { name = "cmplw"; set_field(exp, bf, (a < b ? 8U : a > b ? 4U : 2U) | so); ins = A::cmplw(bf, ra, rb); break; }
        }
        run_case(name, ins, st, exp);
    }
}

TEST_CASE(reference_condition_register_logic) {
    Rng rng{0xC0DEC0DEULL};
    struct Op { const char* name; u32 xo; std::function<bool(bool, bool)> f; };
    const std::vector<Op> ops = {
        {"crand", 257, [](bool a, bool b) { return a && b; }}, {"cror", 449, [](bool a, bool b) { return a || b; }},
        {"crxor", 193, [](bool a, bool b) { return a != b; }}, {"crnand", 225, [](bool a, bool b) { return !(a && b); }},
        {"crnor", 33, [](bool a, bool b) { return !(a || b); }}, {"creqv", 289, [](bool a, bool b) { return a == b; }},
        {"crandc", 129, [](bool a, bool b) { return a && !b; }}, {"crorc", 417, [](bool a, bool b) { return a || !b; }},
    };
    for (const Op& op : ops) {
        for (int i = 0; i < kIterations / 4; ++i) {
            CpuState st = random_state(rng);
            const u32 bt = rng.u(32), ba = rng.u(32), bb = rng.u(32);
            const bool va = (st.cr >> (31 - ba)) & 1U, vb = (st.cr >> (31 - bb)) & 1U;
            CpuState exp = st;
            const u32 m = 1U << (31 - bt);
            exp.cr = op.f(va, vb) ? (exp.cr | m) : (exp.cr & ~m);
            run_case(op.name, A::XL(op.xo, bt, ba, bb), st, exp);
        }
    }
}

TEST_CASE(reference_summary) {
    std::printf("[reference] %d randomised instruction executions compared against the reference model\n", g_cases);
    CHECK(g_cases > 100000);
}

TEST_MAIN("reference")
