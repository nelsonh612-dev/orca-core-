// FP engine vs. exact rational reference, plus special-value and conversion tests.
#include "fp_reference.hpp"
#include "orca/ppc/fpu.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace orca;
namespace F = orca::ppc::fpu;
namespace R = orca::test::fpref;

namespace {

struct Rng {
    u64 s;
    u64 next() { s += 0x9E3779B97F4A7C15ULL; u64 z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
    u32 u(u32 n) { return static_cast<u32>(next() % n); }
};

u64 dbits(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }

// Random finite double. `near` (if non-zero) biases the exponent towards another value's.
u64 rand_double(Rng& r, u64 near = 0) {
    static const u64 edges[] = {0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x000FFFFFFFFFFFFFULL,
                                0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0x3FF0000000000000ULL, 0xBFF0000000000000ULL,
                                0x7FE0000000000000ULL, 0x0020000000000000ULL, 0x3CA0000000000000ULL};
    const u32 pick = r.u(16);
    if (pick == 0) return edges[r.u(sizeof edges / sizeof edges[0])] ^ (r.u(2) ? 0x8000000000000000ULL : 0);
    u64 exp;
    if (near != 0 && pick < 9) {
        const int base = static_cast<int>((near >> 52) & 0x7FF);
        exp = static_cast<u64>(std::clamp(base + static_cast<int>(r.u(120)) - 60, 0, 2046));
    } else if (pick < 12) {
        exp = r.u(2) ? r.u(80) : 2046 - r.u(80); // extremes: denormal/underflow and overflow territory
    } else {
        exp = r.u(2047);
    }
    u64 frac = r.next() & 0x000FFFFFFFFFFFFFULL;
    if (r.u(4) == 0) frac &= ~((1ULL << r.u(52)) - 1); // short mantissas -> exact results
    return (r.next() & 0x8000000000000000ULL) | (exp << 52) | frac;
}

u64 rand_single(Rng& r, u64 near = 0) {
    u32 w;
    const u32 pick = r.u(12);
    if (near != 0 && pick < 7) {
        float nf; const double nd = F::to_double(near); nf = static_cast<float>(nd);
        u32 nw; std::memcpy(&nw, &nf, 4);
        const int base = static_cast<int>((nw >> 23) & 0xFF);
        const u32 exp = static_cast<u32>(std::clamp(base + static_cast<int>(r.u(50)) - 25, 0, 254));
        w = (static_cast<u32>(r.next()) & 0x807FFFFFU) | (exp << 23);
    } else if (pick < 9) {
        const u32 exp = r.u(2) ? r.u(30) : 254 - r.u(30);
        w = (static_cast<u32>(r.next()) & 0x807FFFFFU) | (exp << 23);
    } else {
        w = (static_cast<u32>(r.next()) & 0x807FFFFFU) | (r.u(255) << 23);
    }
    if (r.u(4) == 0) w &= ~((1U << r.u(23)) - 1);
    float f; std::memcpy(&f, &w, 4);
    return dbits(static_cast<double>(f));
}

// Independent FPRF classifier for the reference side.
u32 ref_class(u64 bits, bool single) {
    const bool neg = bits >> 63;
    const u64 mag = bits & ~F::kSignBit;
    if (mag == 0) return neg ? 0x12 : 0x02;
    if (mag == 0x7FF0000000000000ULL) return neg ? 0x09 : 0x05;
    double d; std::memcpy(&d, &mag, 8);
    const bool denorm = single ? d < 1.1754943508222875e-38 : ((bits >> 52) & 0x7FF) == 0;
    return denorm ? (neg ? 0x18 : 0x14) : (neg ? 0x08 : 0x04);
}

R::Exact negate(R::Exact x) { x.neg = !x.neg; return x; }

struct Expected { u64 bits; u32 exceptions; bool fr, fi; u32 fprf; };

// Sign of an exactly-zero sum (IEEE 754 6.3).
bool zero_sum_sign(bool sa, bool sb, bool both_zero, unsigned rn) {
    if (both_zero && sa == sb) return sa;
    return rn == 3;
}

Expected reference(F::Op op, u64 a, u64 b, u64 c, bool single, unsigned rn) {
    const R::Exact A = R::decode(a), B = R::decode(b), C = R::decode(c);
    R::Exact x;
    bool zero_sign = false;
    const bool fused = op == F::Op::MAdd || op == F::Op::MSub || op == F::Op::NMAdd || op == F::Op::NMSub;
    const bool sub_b = op == F::Op::Sub || op == F::Op::MSub || op == F::Op::NMSub;
    switch (op) {
    case F::Op::Add: case F::Op::Sub: {
        const R::Exact b2 = sub_b ? negate(B) : B;
        x = R::add(A, b2);
        zero_sign = zero_sum_sign(A.neg, b2.neg, A.m.zero() && b2.m.zero(), rn);
        break;
    }
    case F::Op::Mul: x = R::mul(A, C); zero_sign = A.neg != C.neg; break;
    case F::Op::Div: x = R::div(A, B); zero_sign = A.neg != B.neg; break;
    case F::Op::Round: x = B; zero_sign = B.neg; break;
    default: {
        const R::Exact p = R::mul(A, C), b2 = sub_b ? negate(B) : B;
        x = R::add(p, b2);
        zero_sign = zero_sum_sign(A.neg != C.neg, b2.neg, p.m.zero() && b2.m.zero(), rn);
        break;
    }
    }
    Expected e{};
    if (x.m.zero()) {
        e.bits = zero_sign ? F::kSignBit : 0;
    } else {
        const R::Rounded r = R::round(x, single, rn);
        e.bits = r.bits;
        e.fi = r.inexact;
        e.fr = r.up;
        if (r.overflow) e.exceptions |= F::OX;
        if (r.tiny && r.inexact) e.exceptions |= F::UX;
        if (r.inexact) e.exceptions |= F::XX;
    }
    if (fused && (op == F::Op::NMAdd || op == F::Op::NMSub)) e.bits ^= F::kSignBit;
    e.fprf = ref_class(e.bits, single);
    return e;
}

const char* op_name(F::Op op) {
    switch (op) {
    case F::Op::Add: return "add"; case F::Op::Sub: return "sub"; case F::Op::Mul: return "mul";
    case F::Op::Div: return "div"; case F::Op::MAdd: return "madd"; case F::Op::MSub: return "msub";
    case F::Op::NMAdd: return "nmadd"; case F::Op::NMSub: return "nmsub"; case F::Op::Round: return "round";
    default: return "?";
    }
}

int g_compared = 0;

} // namespace

TEST_CASE(engine_matches_exact_reference) {
    Rng rng{0xF00DF00DULL};
    const F::Op ops[] = {F::Op::Add, F::Op::Sub, F::Op::Mul, F::Op::Div, F::Op::MAdd, F::Op::MSub,
                         F::Op::NMAdd, F::Op::NMSub, F::Op::Round};
    int reported = 0;
    for (F::Op op : ops) {
        for (int single = 0; single < 2; ++single) {
            if (op == F::Op::Round && single == 0) continue;
            for (unsigned rn = 0; rn < 4; ++rn) {
                for (int i = 0; i < 1500; ++i) {
                    u64 a, b, c;
                    if (single && op != F::Op::Round) { a = rand_single(rng); b = rand_single(rng, a); c = rand_single(rng, a); }
                    else { a = rand_double(rng); b = rand_double(rng, a); c = rand_double(rng, b); }
                    if (op == F::Op::Div && F::is_zero(b)) continue; // divide-by-zero tested separately
                    const F::Result got = F::arith(op, a, b, c, single != 0, rn);
                    const Expected want = reference(op, a, b, c, single != 0, rn);
                    ++g_compared;
                    const bool ok = got.bits == want.bits && got.exceptions == want.exceptions &&
                                    got.fr == want.fr && got.fi == want.fi && got.fprf == want.fprf;
                    CHECK(ok);
                    if (!ok && reported++ < 10)
                        std::fprintf(stderr, "    %s%s rn=%u a=%016llx b=%016llx c=%016llx\n      got  %016llx exc=%08x fr=%d fi=%d fprf=%02x\n      want %016llx exc=%08x fr=%d fi=%d fprf=%02x\n",
                                     op_name(op), single ? "s" : "", rn, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                                     (unsigned long long)got.bits, got.exceptions, got.fr, got.fi, got.fprf,
                                     (unsigned long long)want.bits, want.exceptions, want.fr, want.fi, want.fprf);
                }
            }
        }
    }
}

TEST_CASE(nan_propagation_priority_and_quieting) {
    const u64 qa = 0x7FF8000000000AAAULL, sb = 0x7FF0000000000BBBULL, qc = 0xFFF8000000000CCCULL, one = 0x3FF0000000000000ULL;
    F::Result r = F::arith(F::Op::MAdd, qa, sb, qc, false, 0);
    CHECK_EQ(r.bits, qa);                    // frA first
    CHECK_EQ(r.exceptions, F::VXSNAN);       // SNaN anywhere still signals
    r = F::arith(F::Op::MAdd, one, sb, qc, false, 0);
    CHECK_EQ(r.bits, sb | F::kQuietBit);     // then frB, quieted
    r = F::arith(F::Op::MAdd, one, one, qc, false, 0);
    CHECK_EQ(r.bits, qc);                    // then frC
    CHECK_EQ(r.exceptions, 0U);
    r = F::arith(F::Op::Mul, one, sb, qc, false, 0);
    CHECK_EQ(r.bits, qc);                    // fmul ignores frB
    r = F::arith(F::Op::NMAdd, qc, one, one, false, 0);
    CHECK_EQ(r.bits, qc);                    // NaN result is not negated
    r = F::arith(F::Op::Add, 0x7FF8000012345678ULL, one, 0, true, 0);
    CHECK_EQ(r.bits, 0x7FF8000000000000ULL); // single result: low fraction bits dropped
    CHECK_EQ(r.fprf, F::CLASS_QNAN);
    CHECK(!r.fr && !r.fi);
}

TEST_CASE(invalid_operations_produce_default_nan) {
    const u64 inf = 0x7FF0000000000000ULL, ninf = 0xFFF0000000000000ULL, zero = 0, one = 0x3FF0000000000000ULL;
    CHECK_EQ(F::arith(F::Op::Add, inf, ninf, 0, false, 0).exceptions, F::VXISI);
    CHECK_EQ(F::arith(F::Op::Sub, inf, inf, 0, false, 0).exceptions, F::VXISI);
    CHECK_EQ(F::arith(F::Op::Add, inf, inf, 0, false, 0).bits, inf);
    CHECK_EQ(F::arith(F::Op::Mul, inf, 0, zero, false, 0).exceptions, F::VXIMZ);
    CHECK_EQ(F::arith(F::Op::Div, inf, ninf, 0, false, 0).exceptions, F::VXIDI);
    CHECK_EQ(F::arith(F::Op::Div, zero, zero, 0, false, 0).exceptions, F::VXZDZ);
    CHECK_EQ(F::arith(F::Op::MAdd, zero, one, inf, false, 0).exceptions, F::VXIMZ);
    CHECK_EQ(F::arith(F::Op::MAdd, inf, ninf, one, false, 0).exceptions, F::VXISI);  // inf*1 + -inf
    CHECK_EQ(F::arith(F::Op::MSub, inf, inf, one, false, 0).exceptions, F::VXISI);   // inf*1 - inf
    CHECK_EQ(F::arith(F::Op::MSub, inf, ninf, one, false, 0).bits, inf);             // inf*1 - -inf
    const F::Result r = F::arith(F::Op::Sub, inf, inf, 0, false, 0);
    CHECK_EQ(r.bits, F::kDefaultQNaN);
    CHECK_EQ(r.fprf, F::CLASS_QNAN);
    CHECK_EQ(F::arith(F::Op::RsqrtEstimate, 0, 0xBFF0000000000000ULL, 0, false, 0).exceptions, F::VXSQRT);
}

TEST_CASE(divide_by_zero_and_estimates) {
    const u64 one = 0x3FF0000000000000ULL, four = 0x4010000000000000ULL, nzero = F::kSignBit;
    F::Result r = F::arith(F::Op::Div, one, nzero, 0, false, 0);
    CHECK_EQ(r.bits, 0xFFF0000000000000ULL);
    CHECK_EQ(r.exceptions, F::ZX);
    r = F::arith(F::Op::Div, 0x7FF0000000000000ULL, 0, 0, false, 0); // inf/0: exact, no ZX
    CHECK_EQ(r.exceptions, 0U);
    r = F::arith(F::Op::ReciprocalEstimate, 0, four, 0, true, 0);
    CHECK_EQ(r.bits, dbits(0.25));
    r = F::arith(F::Op::ReciprocalEstimate, 0, nzero, 0, true, 0);
    CHECK_EQ(r.bits, 0xFFF0000000000000ULL);
    CHECK_EQ(r.exceptions, F::ZX);
    r = F::arith(F::Op::RsqrtEstimate, 0, four, 0, false, 0);
    CHECK_EQ(r.bits, dbits(0.5));
    r = F::arith(F::Op::RsqrtEstimate, 0, 0x7FF0000000000000ULL, 0, false, 0);
    CHECK_EQ(r.bits, 0U);
}

TEST_CASE(single_format_memory_conversions) {
    // normal, zero, inf, SNaN preserved bit-exactly, denormal normalised
    CHECK_EQ(F::load_single(0x3F800000U), dbits(1.0));
    CHECK_EQ(F::load_single(0x80000000U), F::kSignBit);
    CHECK_EQ(F::load_single(0xFF800000U), 0xFFF0000000000000ULL);
    CHECK_EQ(F::load_single(0x7F800001U), 0x7FF0000020000000ULL); // SNaN stays signalling
    CHECK_EQ(F::load_single(0x00000001U), dbits(0x1p-149));
    CHECK_EQ(F::load_single(0x807FFFFFU), dbits(-0x1.fffffcp-127));
    for (u32 w : {0x3F800000U, 0x80000000U, 0xFF800000U, 0x7F800001U, 0x00000001U, 0x807FFFFFU, 0x7F7FFFFFU, 0x00800000U})
        CHECK_EQ(F::store_single(F::load_single(w)), w); // round trip
    CHECK_EQ(F::store_single(dbits(1.0 + 0x1p-40)), 0x3F800000U); // stfs truncates, it does not round
    CHECK_EQ(F::store_single(dbits(0x1p-130)), 0x00080000U);      // denormalised
    CHECK_EQ(F::store_single(dbits(-0x1p-149)), 0x80000001U);
}

TEST_CASE(convert_to_integer) {
    auto ti = [](double d, unsigned rn, bool rz) { return F::to_int32(dbits(d), rn, rz); };
    CHECK_EQ(ti(2.5, 0, false).value, 2U);
    CHECK_EQ(ti(3.5, 0, false).value, 4U);
    CHECK_EQ(ti(-2.5, 3, false).value, static_cast<u32>(-3));
    CHECK_EQ(ti(-2.5, 2, false).value, static_cast<u32>(-2));
    CHECK_EQ(ti(-2.7, 0, true).value, static_cast<u32>(-2));
    F::IntResult r = ti(2.5, 0, false);
    CHECK(r.fi && !r.fr);
    CHECK_EQ(r.exceptions, F::XX);
    r = ti(2.7, 0, false);
    CHECK(r.fi && r.fr);
    r = ti(3e9, 0, false);
    CHECK_EQ(r.value, 0x7FFFFFFFU); CHECK_EQ(r.exceptions, F::VXCVI);
    r = ti(-3e9, 0, false);
    CHECK_EQ(r.value, 0x80000000U); CHECK_EQ(r.exceptions, F::VXCVI);
    r = ti(-2147483648.0, 0, false);
    CHECK_EQ(r.value, 0x80000000U); CHECK_EQ(r.exceptions, 0U);
    r = F::to_int32(0x7FF0000000000001ULL, 0, false);
    CHECK_EQ(r.value, 0x80000000U); CHECK_EQ(r.exceptions, F::VXCVI | F::VXSNAN);
}

TEST_CASE(compare_and_classify) {
    CHECK_EQ(F::compare(dbits(1.0), dbits(2.0)), 8U);
    CHECK_EQ(F::compare(dbits(2.0), dbits(1.0)), 4U);
    CHECK_EQ(F::compare(dbits(0.0), dbits(-0.0)), 2U);
    CHECK_EQ(F::compare(F::kDefaultQNaN, dbits(1.0)), 1U);
    CHECK_EQ(F::classify(dbits(0x1p-130), true), F::CLASS_POS_DENORM);
    CHECK_EQ(F::classify(dbits(0x1p-130), false), F::CLASS_POS_NORM);
    CHECK_EQ(F::classify(0x8000000000000001ULL, false), F::CLASS_NEG_DENORM);
}

TEST_CASE(host_rounding_mode_restored) {
    (void)F::arith(F::Op::Div, dbits(1.0), dbits(3.0), 0, false, 2);
    (void)F::to_int32(dbits(2.5), 3, false);
    volatile double x = 1.0, y = 3.0;
    CHECK_EQ(dbits(x / y), 0x3FD5555555555555ULL); // round-to-nearest result
}

TEST_CASE(engine_summary) {
    std::printf("[fpu_engine] %d random operations compared bit-exactly (value, OX/UX/XX, FR, FI, FPRF)\n", g_compared);
    CHECK(g_compared > 80000);
}

TEST_MAIN("fpu_engine")
