// Orca Core — PowerPC floating-point engine. See fpu.hpp for the design.
// This file must be compiled with -frounding-math (set in CMakeLists.txt) so
// the compiler does not fold or reorder arithmetic across rounding-mode changes.
#include "orca/ppc/fpu.hpp"

#include <cfenv>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace orca::ppc::fpu {

double to_double(u64 b) noexcept { double d; std::memcpy(&d, &b, sizeof d); return d; }
u64 from_double(double d) noexcept { u64 b; std::memcpy(&b, &d, sizeof b); return b; }

namespace {

int host_mode(unsigned rn) noexcept {
    switch (rn & 3U) {
    case 0: return FE_TONEAREST;
    case 1: return FE_TOWARDZERO;
    case 2: return FE_UPWARD;
    default: return FE_DOWNWARD;
    }
}

// Restores the host default rounding mode on every exit path.
struct RoundingGuard {
    RoundingGuard() = default;
    RoundingGuard(const RoundingGuard&) = delete;
    RoundingGuard& operator=(const RoundingGuard&) = delete;
    ~RoundingGuard() { std::fesetround(FE_TONEAREST); }
};

// Operands go through volatile objects so the computation is performed
// exactly where it appears, after the preceding fesetround()/feclearexcept().
double compute(Op op, double a, double b, double c) {
    volatile double va = a, vb = b, vc = c;
    switch (op) {
    case Op::Add: return va + vb;
    case Op::Sub: return va - vb;
    case Op::Mul: return va * vc;
    case Op::Div: return va / vb;
    case Op::MAdd: case Op::NMAdd: return std::fma(va, vc, vb);
    case Op::MSub: case Op::NMSub: return std::fma(va, vc, -vb);
    case Op::Round: return vb;
    default: return 0.0;
    }
}

constexpr u64 quiet(u64 b) noexcept { return b | kQuietBit; }
constexpr u64 truncate_to_single_format(u64 b) noexcept { return b & 0xFFFFFFFFE0000000ULL; }

Result special(u64 bits, u32 exceptions, bool single) {
    Result r;
    r.bits = bits;
    r.exceptions = exceptions;
    r.fprf = classify(bits, single);
    return r;
}

} // namespace

u32 classify(u64 bits, bool single) noexcept {
    if (is_nan(bits)) return CLASS_QNAN;
    const bool neg = sign_of(bits);
    if (is_inf(bits)) return neg ? CLASS_NEG_INF : CLASS_POS_INF;
    if (is_zero(bits)) return neg ? CLASS_NEG_ZERO : CLASS_POS_ZERO;
    const bool denormal = single ? std::fabs(to_double(bits)) < 0x1p-126 : (bits & kExpMask) == 0U;
    if (denormal) return neg ? CLASS_NEG_DENORM : CLASS_POS_DENORM;
    return neg ? CLASS_NEG_NORM : CLASS_POS_NORM;
}

Result arith(Op op, u64 a, u64 b, u64 c, bool single, unsigned rn) {
    // Operands in NaN-propagation priority order (PEM 3.3.6.x: frA, frB, frC).
    u64 operands[3] = {0, 0, 0};
    unsigned count = 0;
    switch (op) {
    case Op::Add: case Op::Sub: case Op::Div: operands[0] = a; operands[1] = b; count = 2; break;
    case Op::Mul: operands[0] = a; operands[1] = c; count = 2; break;
    case Op::MAdd: case Op::MSub: case Op::NMAdd: case Op::NMSub:
        operands[0] = a; operands[1] = b; operands[2] = c; count = 3; break;
    case Op::Round: case Op::ReciprocalEstimate: case Op::RsqrtEstimate:
        operands[0] = b; count = 1; break;
    }
    bool any_snan = false;
    for (unsigned i = 0; i < count; ++i) any_snan = any_snan || is_snan(operands[i]);
    for (unsigned i = 0; i < count; ++i) {
        if (is_nan(operands[i])) {
            u64 n = quiet(operands[i]);
            if (single) n = truncate_to_single_format(n);
            return special(n, any_snan ? VXSNAN : 0U, single);
        }
    }

    // Invalid operations (result: default QNaN)
    const bool ai = is_inf(a), bi = is_inf(b), ci = is_inf(c);
    const bool az = is_zero(a), bz = is_zero(b), cz = is_zero(c);
    u32 invalid = 0;
    switch (op) {
    case Op::Add: if (ai && bi && sign_of(a) != sign_of(b)) invalid = VXISI; break;
    case Op::Sub: if (ai && bi && sign_of(a) == sign_of(b)) invalid = VXISI; break;
    case Op::Mul: if ((ai && cz) || (az && ci)) invalid = VXIMZ; break;
    case Op::Div:
        if (ai && bi) invalid = VXIDI;
        else if (az && bz) invalid = VXZDZ;
        break;
    case Op::MAdd: case Op::MSub: case Op::NMAdd: case Op::NMSub: {
        if ((ai && cz) || (az && ci)) { invalid = VXIMZ; break; }
        if ((ai || ci) && bi) {
            const bool product_negative = sign_of(a) != sign_of(c);
            const bool subtract = op == Op::MSub || op == Op::NMSub;
            const bool addend_negative = sign_of(b) != subtract;
            if (product_negative != addend_negative) invalid = VXISI;
        }
        break;
    }
    case Op::RsqrtEstimate: if (sign_of(b) && !bz) invalid = VXSQRT; break;
    default: break;
    }
    if (invalid != 0U) return special(kDefaultQNaN, invalid, single);

    // Divide by zero
    if (op == Op::Div && bz && !ai) return special(kExpMask | ((a ^ b) & kSignBit), ZX, single);
    if ((op == Op::ReciprocalEstimate || op == Op::RsqrtEstimate) && bz)
        return special(kExpMask | (b & kSignBit), ZX, single);

    if (op == Op::RsqrtEstimate) {
        // Estimate. Architecturally the result only needs 1/32 relative
        // accuracy; this returns the correctly rounded 1/sqrt in double, which
        // is NOT bit-identical to Gekko's hardware estimate table (see README).
        if (bi) return special(0, 0, single);
        double est = 1.0 / std::sqrt(to_double(b));
        if (single) est = static_cast<double>(static_cast<float>(est)); // ps_rsqrte: single result
        return special(from_double(est), 0, single);
    }
    if (op == Op::ReciprocalEstimate) {
        // Architecturally an estimate (1/4096 accuracy); correctly rounded 1/B
        // in single precision is returned (not Gekko's hardware table).
        return arith(Op::Div, 0x3FF0000000000000ULL, b, 0, true, rn);
    }

    const double fa = to_double(a), fb = to_double(b), fc = to_double(c);
    RoundingGuard guard;

    // 1) Exact result truncated toward zero to 53 bits, plus "was it exact?".
    std::fesetround(FE_TOWARDZERO);
    std::feclearexcept(FE_ALL_EXCEPT);
    volatile double rtz = compute(op, fa, fb, fc);
    const bool inexact_tz = std::fetestexcept(FE_INEXACT) != 0;

    double result = 0.0, truncated = 0.0;
    bool overflow = false, inexact = false;
    if (!single) {
        // 2a) Correctly rounded double in the requested mode.
        std::fesetround(host_mode(rn));
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile double rr = compute(op, fa, fb, fc);
        overflow = std::fetestexcept(FE_OVERFLOW) != 0;
        inexact = std::fetestexcept(FE_INEXACT) != 0;
        result = rr;
        truncated = rtz;
    } else {
        // 2b) Round-to-odd in double, then one rounding to single: no double rounding.
        u64 odd = from_double(rtz);
        if (inexact_tz && !is_inf(odd)) odd |= 1U;
        std::fesetround(host_mode(rn));
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile double odd_value = to_double(odd);
        volatile float rounded = static_cast<float>(odd_value);
        overflow = std::fetestexcept(FE_OVERFLOW) != 0;
        inexact = inexact_tz || std::fetestexcept(FE_INEXACT) != 0;
        result = static_cast<double>(rounded); // exact widening
        std::fesetround(FE_TOWARDZERO);
        volatile double rtz_copy = rtz;
        volatile float rounded_tz = static_cast<float>(rtz_copy);
        truncated = static_cast<double>(rounded_tz);
    }
    std::fesetround(FE_TONEAREST);

    if (op == Op::NMAdd || op == Op::NMSub) { result = -result; truncated = -truncated; }

    const double min_normal = single ? 0x1p-126 : 0x1p-1022;
    const double rtz_value = rtz;
    const bool exact_nonzero = rtz_value != 0.0 || inexact_tz;
    const bool tiny = exact_nonzero && std::fabs(rtz_value) < min_normal; // tininess before rounding

    Result r;
    r.bits = from_double(result);
    r.fi = inexact;
    r.fr = inexact && std::fabs(result) != std::fabs(truncated);
    if (overflow) r.exceptions |= OX;
    if (tiny && inexact) r.exceptions |= UX;
    r.tiny = tiny;
    if (inexact) r.exceptions |= XX;
    r.fprf = classify(r.bits, single);
    return r;
}

u64 load_single(u32 word) noexcept {
    const u64 sign = static_cast<u64>(word >> 31) << 63;
    const u32 exp = (word >> 23) & 0xFFU;
    u32 frac = word & 0x7FFFFFU;
    if (exp == 0xFFU) return sign | kExpMask | (static_cast<u64>(frac) << 29); // Inf/NaN, SNaN preserved
    if (exp != 0U) return sign | (static_cast<u64>(exp - 127U + 1023U) << 52) | (static_cast<u64>(frac) << 29);
    if (frac == 0U) return sign;
    int e = -126; // denormal: normalise
    while ((frac & 0x800000U) == 0U) { frac <<= 1U; --e; }
    frac &= 0x7FFFFFU;
    return sign | (static_cast<u64>(e + 1023) << 52) | (static_cast<u64>(frac) << 29);
}

u32 store_single(u64 bits) noexcept {
    const u32 dexp = static_cast<u32>((bits >> 52) & 0x7FFU);
    if (dexp > 896U || (bits & ~kSignBit) == 0U) {
        // WORD[0:1] = frS[0:1], WORD[2:31] = frS[5:34] (no rounding)
        return static_cast<u32>(((bits >> 32) & 0xC0000000U) | ((bits >> 29) & 0x3FFFFFFFU));
    }
    if (dexp >= 874U) { // denormalise into the single format
        int exp = static_cast<int>(dexp) - 1023;
        u64 frac = (1ULL << 52) | (bits & kFracMask);
        while (exp < -126) { frac >>= 1U; ++exp; }
        return static_cast<u32>(((bits >> 63) << 31) | ((frac >> 29) & 0x7FFFFFU));
    }
    // PEM: result undefined for magnitudes below the single denormal range;
    // a signed zero is stored (documented choice, see README).
    return static_cast<u32>((bits >> 63) << 31);
}

IntResult to_int32(u64 b, unsigned rn, bool round_toward_zero) {
    IntResult r;
    if (is_nan(b)) {
        r.value = 0x80000000U;
        r.exceptions = VXCVI | (is_snan(b) ? VXSNAN : 0U);
        return r;
    }
    const double d = to_double(b);
    double rounded;
    {
        RoundingGuard guard;
        std::fesetround(round_toward_zero ? FE_TOWARDZERO : host_mode(rn));
        volatile double v = d;
        volatile double ri = std::nearbyint(v);
        rounded = ri;
    }
    if (rounded > 2147483647.0) { r.value = 0x7FFFFFFFU; r.exceptions = VXCVI; return r; }
    if (rounded < -2147483648.0) { r.value = 0x80000000U; r.exceptions = VXCVI; return r; }
    r.value = static_cast<u32>(static_cast<s32>(rounded));
    r.fi = rounded != d;
    r.fr = std::fabs(rounded) > std::fabs(d);
    if (r.fi) r.exceptions = XX;
    return r;
}

u32 compare(u64 a, u64 b) noexcept {
    if (is_nan(a) || is_nan(b)) return 1U;
    const double x = to_double(a), y = to_double(b);
    return x < y ? 8U : (x > y ? 4U : 2U);
}

} // namespace orca::ppc::fpu
