// Exact reference model for IEEE-754 binary32/binary64 arithmetic.
//
// Values are exact rationals m * 2^e with an arbitrary-precision integer m.
// Rounding to a target precision is implemented directly from the IEEE-754
// definitions (four modes, subnormals, overflow, tininess before rounding).
// It shares no code and no technique with src/ppc/fpu.cpp (which relies on
// the host FPU and <cfenv>), so it can validate that engine.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace orca::test::fpref {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Big {
    std::vector<u32> l; // little-endian limbs, no leading zero limbs
    static Big of(u64 v) { Big b; while (v) { b.l.push_back(static_cast<u32>(v)); v >>= 32; } return b; }
    bool zero() const { return l.empty(); }
    void trim() { while (!l.empty() && l.back() == 0) l.pop_back(); }
    int bits() const {
        if (l.empty()) return 0;
        u32 top = l.back(); int n = 0;
        while (top) { ++n; top >>= 1; }
        return static_cast<int>((l.size() - 1) * 32) + n;
    }
    bool bit(int i) const { const auto w = static_cast<std::size_t>(i / 32); return i >= 0 && w < l.size() && ((l[w] >> (i % 32)) & 1U); }
    Big shl(int n) const {
        if (zero() || n == 0) return *this;
        Big r; r.l.assign(static_cast<std::size_t>(n / 32), 0);
        const int s = n % 32; u32 carry = 0;
        for (u32 w : l) { r.l.push_back(s ? (w << s) | carry : w); carry = s ? (w >> (32 - s)) : 0; }
        if (carry) r.l.push_back(carry);
        r.trim(); return r;
    }
    Big shr(int n) const {
        const auto ws = static_cast<std::size_t>(n / 32);
        if (ws >= l.size()) return {};
        Big r; const int s = n % 32;
        for (std::size_t i = ws; i < l.size(); ++i) {
            u64 v = l[i] >> s;
            if (s && i + 1 < l.size()) v |= static_cast<u64>(l[i + 1]) << (32 - s);
            r.l.push_back(static_cast<u32>(v));
        }
        r.trim(); return r;
    }
    bool any_below(int n) const { for (int i = 0; i < n; ++i) if (bit(i)) return true; return false; }
    static int cmp(const Big& a, const Big& b) {
        if (a.l.size() != b.l.size()) return a.l.size() < b.l.size() ? -1 : 1;
        for (std::size_t i = a.l.size(); i-- > 0;) if (a.l[i] != b.l[i]) return a.l[i] < b.l[i] ? -1 : 1;
        return 0;
    }
    static Big add(const Big& a, const Big& b) {
        Big r; u64 c = 0;
        for (std::size_t i = 0; i < std::max(a.l.size(), b.l.size()); ++i) {
            c += (i < a.l.size() ? a.l[i] : 0) + static_cast<u64>(i < b.l.size() ? b.l[i] : 0);
            r.l.push_back(static_cast<u32>(c)); c >>= 32;
        }
        if (c) r.l.push_back(static_cast<u32>(c));
        r.trim(); return r;
    }
    static Big sub(const Big& a, const Big& b) { // requires a >= b
        Big r; std::int64_t br = 0;
        for (std::size_t i = 0; i < a.l.size(); ++i) {
            std::int64_t v = static_cast<std::int64_t>(a.l[i]) - (i < b.l.size() ? b.l[i] : 0) - br;
            br = v < 0; if (v < 0) v += (1LL << 32);
            r.l.push_back(static_cast<u32>(v));
        }
        r.trim(); return r;
    }
    static Big mul(const Big& a, const Big& b) {
        if (a.zero() || b.zero()) return {};
        std::vector<u64> t(a.l.size() + b.l.size() + 1, 0);
        for (std::size_t i = 0; i < a.l.size(); ++i) {
            u64 c = 0;
            for (std::size_t j = 0; j < b.l.size(); ++j) {
                const u64 cur = t[i + j] + static_cast<u64>(a.l[i]) * b.l[j] + c;
                t[i + j] = cur & 0xFFFFFFFFULL; c = cur >> 32;
            }
            std::size_t k = i + b.l.size();
            while (c) { const u64 cur = t[k] + c; t[k] = cur & 0xFFFFFFFFULL; c = cur >> 32; ++k; }
        }
        Big r; for (u64 v : t) r.l.push_back(static_cast<u32>(v));
        r.trim(); return r;
    }
    static void divmod(const Big& a, const Big& b, Big& q, Big& r) { // schoolbook binary division
        q = {}; r = {};
        for (int i = a.bits() - 1; i >= 0; --i) {
            r = r.shl(1); if (a.bit(i)) { if (r.l.empty()) r.l.push_back(1); else r.l[0] |= 1U; }
            if (cmp(r, b) >= 0) { r = sub(r, b); q = add(q, Big::of(1).shl(i)); }
        }
    }
};

struct Exact { bool neg{false}; Big m; int e{0}; };

inline Exact decode(u64 bits) {
    Exact x; x.neg = (bits >> 63) != 0;
    const int be = static_cast<int>((bits >> 52) & 0x7FF);
    const u64 frac = bits & 0x000FFFFFFFFFFFFFULL;
    if (be == 0) { x.m = Big::of(frac); x.e = -1074; }
    else { x.m = Big::of(frac | (1ULL << 52)); x.e = be - 1075; }
    return x;
}

inline Exact add(const Exact& a, const Exact& b) {
    if (a.m.zero()) return b;
    if (b.m.zero()) return a;
    const int e = std::min(a.e, b.e);
    const Big am = a.m.shl(a.e - e), bm = b.m.shl(b.e - e);
    Exact r; r.e = e;
    if (a.neg == b.neg) { r.m = Big::add(am, bm); r.neg = a.neg; }
    else if (Big::cmp(am, bm) >= 0) { r.m = Big::sub(am, bm); r.neg = a.neg; }
    else { r.m = Big::sub(bm, am); r.neg = b.neg; }
    return r;
}
inline Exact mul(const Exact& a, const Exact& b) { return {a.neg != b.neg, Big::mul(a.m, b.m), a.e + b.e}; }
inline Exact div(const Exact& a, const Exact& b) {
    const int k = std::max(0, 64 + b.m.bits() - a.m.bits());
    Big q, r; Big::divmod(a.m.shl(k), b.m, q, r);
    q = q.shl(1); if (!r.zero()) { if (q.l.empty()) q.l.push_back(1); else q.l[0] |= 1U; } // sticky bit
    return {a.neg != b.neg, q, a.e - b.e - k - 1};
}

struct Rounded { u64 bits{0}; bool inexact{false}, overflow{false}, tiny{false}, up{false}; };

// Rounds a non-zero exact value; result returned in double format.
inline Rounded round(const Exact& x, bool single, unsigned rn) {
    const int p = single ? 24 : 53, emin = single ? -126 : -1022, emax = single ? 127 : 1023, bias = single ? 127 : 1023;
    Rounded out;
    const int E = x.e + x.m.bits() - 1;
    out.tiny = E < emin;
    int lsb = std::max(E - p + 1, emin - p + 1);
    const int shift = lsb - x.e;
    Big kept; bool half = false, sticky = false;
    if (shift <= 0) kept = x.m.shl(-shift);
    else { kept = x.m.shr(shift); half = x.m.bit(shift - 1); sticky = x.m.any_below(shift - 1); }
    out.inexact = half || sticky;
    bool up = false;
    switch (rn) {
    case 0: up = half && (sticky || kept.bit(0)); break;
    case 1: up = false; break;
    case 2: up = out.inexact && !x.neg; break;
    default: up = out.inexact && x.neg; break;
    }
    if (up) { kept = Big::add(kept, Big::of(1)); if (kept.bits() > p) { kept = kept.shr(1); ++lsb; } }
    out.up = up;
    const u64 sign = x.neg ? 1ULL : 0ULL;
    u64 fmt;
    const int Ef = lsb + kept.bits() - 1;
    if (!kept.zero() && Ef > emax) {
        out.overflow = true; out.inexact = true;
        const bool to_inf = rn == 0 || (rn == 2 && !x.neg) || (rn == 3 && x.neg);
        out.up = to_inf;
        if (single) fmt = (sign << 31) | (to_inf ? 0x7F800000ULL : 0x7F7FFFFFULL);
        else fmt = (sign << 63) | (to_inf ? 0x7FF0000000000000ULL : 0x7FEFFFFFFFFFFFFFULL);
    } else {
        u64 k = 0; for (std::size_t i = kept.l.size(); i-- > 0;) k = (k << 32) | kept.l[i];
        u64 biased = 0, frac = k;
        if (kept.bits() == p) { biased = static_cast<u64>(Ef + bias); frac = k - (1ULL << (p - 1)); }
        fmt = single ? (sign << 31) | (biased << 23) | frac : (sign << 63) | (biased << 52) | frac;
    }
    if (single) { float f; const auto w = static_cast<u32>(fmt); std::memcpy(&f, &w, 4); const double d = f; std::memcpy(&out.bits, &d, 8); }
    else out.bits = fmt;
    return out;
}

} // namespace orca::test::fpref
