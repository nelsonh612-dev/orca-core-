// Orca Core — CPU access to the EFB. See efb_access.hpp.
#include "orca/gpu/efb_access.hpp"

#include "orca/gpu/sw/renderer.hpp"

#include <bit>
#include <cstdio>

namespace orca::gpu {

u32 compress_z16(u32 z24, u32 zformat) noexcept {
    z24 &= 0xFFFFFFU;
    if (zformat == 0U) return z24 >> 8; // linear
    // near/mid/far: 2/3/4 exponent bits = number of leading ones removed (max 3/7/12);
    // the first zero after them is implied, the mantissa holds the next 14/13/12 bits.
    const u32 exp_bits = zformat == 1U ? 2U : (zformat == 2U ? 3U : 4U);
    const u32 max_ones = zformat == 1U ? 3U : (zformat == 2U ? 7U : 12U);
    u32 ones = static_cast<u32>(std::countl_one(z24 << 8));
    bool clamped = false;
    if (ones >= max_ones) { ones = max_ones; clamped = true; }
    const u32 mant_bits = 16U - exp_bits;
    u32 top = 24U - ones > mant_bits ? 24U - ones : mant_bits;
    if (!clamped) top -= 1U; // the implied zero is not stored
    const u32 bottom = top - mant_bits;
    const u32 mantissa = (z24 >> bottom) & ((1U << mant_bits) - 1U);
    return (ones << mant_bits) | mantissa;
}

namespace {
struct Decoded { u32 x, y; bool z; bool zc; };
Decoded decode(u32 offset) { return {(offset & 0xFFFU) >> 2, (offset >> 12) & 0x3FFU, (offset & 0x400000U) != 0U, (offset & 0x800000U) != 0U}; }
bool inside(u32 x, u32 y) { return x < sw::kEfbWidth && y < sw::kEfbHeight; }
} // namespace

bool EfbAccess::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("EFB: only 32-bit CPU reads are implemented");
    const Decoded d = decode(offset);
    if (d.zc) return fail("EFB: Z+colour read (address bit 23) is not documented");
    const u32 zc = gpu_.bp().reg(bp::ZCOMPARE), fmt = zc & 7U;
    if (fmt > 3U) return fail("EFB: CPU read in pixel format 4-7 (YUV) is not implemented");
    if (!inside(d.x, d.y)) { value = 0; return true; }
    u32 c24 = 0, z24 = 0;
    std::string e;
    if (!gpu_.efb_peek(d.x, d.y, c24, z24, e)) return fail("EFB: " + e);
    if (d.z) {
        value = fmt == 2U ? compress_z16(z24, (zc >> 3) & 7U) : z24;
        if (fmt == 2U && ((zc >> 3) & 7U) > 3U) return fail("EFB: Z16 read with inverted Z format 4-7");
        return true;
    }
    u8 rgba[4];
    sw::Renderer::efb_color_rgba(c24, fmt, rgba);
    if (fmt == 2U) { // RGB565: what the real 16-bit buffer would hold
        rgba[0] = static_cast<u8>((rgba[0] & 0xF8U) | (rgba[0] >> 5));
        rgba[1] = static_cast<u8>((rgba[1] & 0xFCU) | (rgba[1] >> 6));
        rgba[2] = static_cast<u8>((rgba[2] & 0xF8U) | (rgba[2] >> 5));
    }
    u32 argb = (static_cast<u32>(fmt == 1U ? rgba[3] : 0xFFU) << 24) | (static_cast<u32>(rgba[0]) << 16) |
               (static_cast<u32>(rgba[1]) << 8) | rgba[2];
    switch (pe_.config(hw::pe::ALPHAREAD) & 3U) {
    case 0: argb &= 0x00FFFFFFU; break;
    case 1: argb |= 0xFF000000U; break;
    case 2: break;
    default: return fail("EFB: PE alpha read mode 3 is not defined");
    }
    value = argb;
    return true;
}

bool EfbAccess::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return fail("EFB: only 32-bit CPU writes are implemented");
    const Decoded d = decode(offset);
    if (d.zc) return fail("EFB: Z+colour write (address bit 23) is not documented");
    const u32 fmt = gpu_.bp().reg(bp::ZCOMPARE) & 7U;
    if (fmt > 3U) return fail("EFB: CPU write in pixel format 4-7 (YUV) is not implemented");
    if (!inside(d.x, d.y)) return true;
    const auto v = static_cast<u32>(value);
    u32 stored;
    if (d.z) {
        stored = v & 0xFFFFFFU;
    } else if (fmt == 1U) {
        const u32 a = (v >> 24) & 0xFFU, r = (v >> 16) & 0xFFU, g = (v >> 8) & 0xFFU, b = v & 0xFFU;
        stored = ((r >> 2) << 18) | ((g >> 2) << 12) | ((b >> 2) << 6) | (a >> 2);
    } else {
        stored = v & 0xFFFFFFU;
    }
    std::string e;
    if (!gpu_.efb_poke(d.x, d.y, d.z, stored, e)) return fail("EFB: " + e);
    return true;
}

} // namespace orca::gpu
