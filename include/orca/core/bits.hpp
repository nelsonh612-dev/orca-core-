// Orca Core — small bit-manipulation helpers shared by all modules.
#pragma once
#include <cstdint>

namespace orca {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
__extension__ typedef unsigned __int128 u128; // GCC/Clang (all supported hosts); exact 64x64 products
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Sign-extends the low `bits` bits of `value` to 32 bits.
constexpr s32 sign_extend(u32 value, unsigned bits) noexcept {
    const u32 mask = bits >= 32U ? 0xFFFFFFFFU : ((1U << bits) - 1U);
    const u32 sign = 1U << (bits - 1U);
    value &= mask;
    return static_cast<s32>((value ^ sign) - sign);
}

constexpr u32 rotl32(u32 value, unsigned shift) noexcept {
    shift &= 31U;
    return shift == 0U ? value : (value << shift) | (value >> (32U - shift));
}

// IBM bit numbering: bit 0 is the most significant bit of a 32-bit word.
constexpr u32 ibm_bit(unsigned n) noexcept { return 0x80000000U >> n; }

// Extracts IBM-numbered field [first, last] of a 32-bit instruction word.
constexpr u32 field(u32 word, unsigned first, unsigned last) noexcept {
    const unsigned width = last - first + 1U;
    const u32 mask = width >= 32U ? 0xFFFFFFFFU : ((1U << width) - 1U);
    return (word >> (31U - last)) & mask;
}

constexpr u16 bswap16(u16 v) noexcept { return static_cast<u16>((v >> 8U) | (v << 8U)); }
constexpr u32 bswap32(u32 v) noexcept {
    return (v >> 24U) | ((v >> 8U) & 0x0000FF00U) | ((v << 8U) & 0x00FF0000U) | (v << 24U);
}

} // namespace orca
