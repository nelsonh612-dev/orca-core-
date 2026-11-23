// Orca Core — AArch64 emitter. See a64_emitter.hpp.
#include "orca/jit/a64_emitter.hpp"

#include <bit>

namespace orca::jit {

// Logical immediates (Arm ARM, DecodeBitMasks): an element of size e (2..64) that is
// a run of `ones` set bits rotated right by r, repeated to fill the register.
std::optional<u32> encode_logical_imm(u64 value, bool is64) noexcept {
    if (!is64) {
        value &= 0xFFFFFFFFULL;
        value |= value << 32; // a 32-bit pattern must repeat with size <= 32
    }
    if (value == 0 || value == ~0ULL) return std::nullopt;
    // smallest element size whose pattern repeats
    unsigned e = 64;
    while (e > 2) {
        const unsigned h = e / 2;
        const u64 mask = (1ULL << h) - 1ULL;
        if ((value & mask) != ((value >> h) & mask)) break;
        e = h;
    }
    const u64 emask = e == 64 ? ~0ULL : (1ULL << e) - 1ULL;
    const u64 elem = value & emask;
    const unsigned ones = static_cast<unsigned>(std::popcount(elem));
    if (ones == 0 || ones == e) return std::nullopt;
    // find rotation r such that rotating elem left by r gives the run 0..ones-1
    for (unsigned r = 0; r < e; ++r) {
        const u64 rot = e == 64 ? std::rotl(elem, static_cast<int>(r))
                                : (((elem << r) | (elem >> (e - r))) & emask);
        if (rot == (ones == 64 ? ~0ULL : (1ULL << ones) - 1ULL)) {
            // encoded "immr" rotates right: the run rotated right by immr gives elem
            const unsigned immr = r % e;
            const unsigned n = e == 64 ? 1U : 0U;
            // imms: high bits mark the element size (NOT of size, as in the ARM ARM table)
            const unsigned imms = ((~(e - 1U) << 1) & 0x3FU) | (ones - 1U);
            if (!is64 && n == 1U) return std::nullopt;
            return (n << 12) | (immr << 6) | imms;
        }
    }
    return std::nullopt;
}

bool Emitter::logical_imm(u32 op, bool x, Reg d, Reg n, u64 imm) {
    const auto enc = encode_logical_imm(imm, x);
    if (!enc) return false;
    emit(op | sf(x) | ((*enc >> 12) & 1U) << 22 | ((*enc >> 6) & 63U) << 16 | (*enc & 63U) << 10 | u32{n} << 5 | d);
    return true;
}

void Emitter::mov_imm(bool x, Reg d, u64 value) {
    if (!x) value &= 0xFFFFFFFFULL;
    const unsigned parts = x ? 4U : 2U;
    unsigned zero_halves = 0, ones_halves = 0;
    for (unsigned i = 0; i < parts; ++i) {
        const u16 h = static_cast<u16>(value >> (16U * i));
        zero_halves += h == 0U ? 1U : 0U;
        ones_halves += h == 0xFFFFU ? 1U : 0U;
    }
    if (zero_halves < parts - 1U && ones_halves < parts - 1U && encode_logical_imm(value, x)) {
        orr_imm(x, d, ZR, value); // one instruction
        return;
    }
    const bool use_movn = ones_halves > zero_halves;
    bool first = true;
    for (unsigned i = 0; i < parts; ++i) {
        const u16 h = static_cast<u16>(value >> (16U * i));
        if (h == (use_movn ? 0xFFFFU : 0U)) continue;
        if (first) {
            if (use_movn) movn(x, d, static_cast<u16>(~h), 16U * i);
            else movz(x, d, h, 16U * i);
            first = false;
        } else {
            movk(x, d, h, 16U * i);
        }
    }
    if (first) { // all halves equal to the default
        if (use_movn) movn(x, d, 0);
        else movz(x, d, 0);
    }
}

void Emitter::ls_imm(unsigned size, u32 base, Reg t, Reg n, u32 imm) {
    const u32 scale = size == 1U ? 0U : size == 2U ? 1U : size == 4U ? 2U : 3U;
    emit(base | scale << 30 | ((imm >> scale) & 0xFFFU) << 10 | u32{n} << 5 | t);
}

void Emitter::ls_reg(unsigned size, u32 base, Reg t, Reg n, Reg m) {
    const u32 scale = size == 1U ? 0U : size == 2U ? 1U : size == 4U ? 2U : 3U;
    emit(base | scale << 30 | u32{m} << 16 | u32{n} << 5 | t);
}

bool Emitter::bind(Label from, Label to) {
    const s64 off = static_cast<s64>(to) - static_cast<s64>(from);
    u32& w = w_[from];
    if ((w & 0x7C000000U) == 0x14000000U) { // B / BL: imm26
        if (off < -(1LL << 25) || off >= (1LL << 25)) return false;
        w = (w & 0xFC000000U) | (static_cast<u32>(off) & 0x03FFFFFFU);
    } else if ((w & 0xFF000010U) == 0x54000000U || (w & 0x7E000000U) == 0x34000000U) { // B.cond, CBZ/CBNZ: imm19
        if (off < -(1LL << 18) || off >= (1LL << 18)) return false;
        w = (w & 0xFF00001FU) | (static_cast<u32>(off) & 0x7FFFFU) << 5;
    } else if ((w & 0x7E000000U) == 0x36000000U) { // TBZ/TBNZ: imm14
        if (off < -(1LL << 13) || off >= (1LL << 13)) return false;
        w = (w & 0xFFF8001FU) | (static_cast<u32>(off) & 0x3FFFU) << 5;
    } else {
        return false;
    }
    return true;
}

} // namespace orca::jit
