// Orca Core — EFB copies and clears. See efb_copy.hpp for sources and choices.
#include "orca/gpu/sw/efb_copy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace orca::gpu::sw {
namespace {


u32 bits(u32 v, unsigned sh, unsigned n) { return (v >> sh) & ((1U << n) - 1U); }
u32 x6(u32 v) { return ((v << 2) | (v >> 4)) & 0xFFU; }

enum Fmt : u32 { R4 = 0, R8_1 = 1, RA4 = 2, RA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, A8 = 7,
                 R8 = 8, G8 = 9, B8 = 10, RG8 = 11, GB8 = 12 };

struct Rect { u32 left, top, right, bottom; };
using Px = std::array<u32, 4>; // R, G, B, A (8-bit, or 6-bit before the box filter of RGBA6)

// Copy filter weights per row (TextureCacheBase.cpp GetRAMCopyFilterCoefficients):
// previous row = w0 + w1, current = w2 + w3 + w4, next = w5 + w6.
std::array<u32, 3> filter_rows(const BpMemory& bp) {
    const u32 lo = bp.reg(bp::COPY_FILTER0), hi = bp.reg(bp::COPY_FILTER1);
    return {bits(lo, 0, 6) + bits(lo, 6, 6), bits(lo, 12, 6) + bits(lo, 18, 6) + bits(hi, 0, 6),
            bits(hi, 6, 6) + bits(hi, 12, 6)};
}

float gamma_of(u32 trigger) {
    switch (bits(trigger, 7, 2)) { case 0: return 1.0f; case 1: return 1.7f; default: return 2.2f; }
}

// Reads the EFB as the copy unit does (TextureConversionShader.cpp SampleEFB): RGB8 values per
// pixel (6-bit channels expanded; depth bytes high/mid/low as R/G/B with alpha 255), coordinates
// clamped to the EFB like a clamp-to-edge sampler; the box filter of half scale; the vertical
// copy filter on RGB with the rows clamped by clamp_top/clamp_bottom, >> 6, wrapped to 9 bits
// when the weights sum to 128 or more, then min(255); gamma rounded.
class PixelReader {
public:
    PixelReader(const Efb& efb, u32 fmt, bool depth, const Rect& r, u32 trigger, const BpMemory& bp)
        : efb_(efb), depth_(depth), six_(fmt == 1U && !depth) {
        w_ = filter_rows(bp);
        wrap_ = w_[0] + w_[1] + w_[2] >= 128U;
        gamma_ = bits(trigger, 7, 2) != 0U;
        gamma_rcp_ = 1.0f / gamma_of(trigger);
        top_ = (trigger & 1U) != 0U ? r.top : 0U;
        bottom_ = ((trigger & 2U) != 0U ? r.bottom : kEfbHeight) - 1U;
    }

    // Filtered RGBA8 of the output texel whose first source pixel is (x, y).
    Px texel(u32 x, u32 y, bool half) const {
        const u32 step = half ? 2U : 1U;
        const Px cur = sample(x, clamp_row(static_cast<s32>(y)), clamp_row(static_cast<s32>(y) + 1), half);
        Px prev{}, next{};
        if (w_[0] != 0U) prev = sample(x, clamp_row(static_cast<s32>(y) - static_cast<s32>(step)),
                                       clamp_row(static_cast<s32>(y) - 1), half);
        if (w_[2] != 0U) next = sample(x, clamp_row(static_cast<s32>(y + step)), clamp_row(static_cast<s32>(y + step + 1U)), half);
        Px out = cur;
        for (unsigned i = 0; i < 3; ++i) {
            u32 v = (prev[i] * w_[0] + cur[i] * w_[1] + next[i] * w_[2]) >> 6;
            if (wrap_) v &= 0x1FFU;
            v = std::min(v, 255U);
            if (gamma_) v = static_cast<u32>(std::round(std::pow(static_cast<float>(v) / 255.0f, gamma_rcp_) * 255.0f));
            out[i] = v;
        }
        return out;
    }

private:
    u32 clamp_row(s32 y) const { return static_cast<u32>(std::clamp(y, static_cast<s32>(top_), static_cast<s32>(bottom_))); }
    Px raw(u32 x, u32 y) const {
        x = std::min(x, kEfbWidth - 1U);
        y = std::min(y, kEfbHeight - 1U);
        const u32 c = depth_ ? efb_.depth24(x, y) : efb_.color24(x, y);
        if (six_) return {bits(c, 18, 6), bits(c, 12, 6), bits(c, 6, 6), bits(c, 0, 6)};
        return {bits(c, 16, 8), bits(c, 8, 8), bits(c, 0, 8), 255U};
    }
    // One texel before the vertical filter: a pixel, or (half scale) the 2x2 box of the pixels at
    // columns x, x+1 and rows y0, y1 (TextureEncoder.cpp: 6-bit sum s -> s + (s >> 6); 8-bit sum >> 2).
    Px sample(u32 x, u32 y0, u32 y1, bool half) const {
        if (!half) {
            const Px p = raw(x, y0);
            return six_ ? Px{x6(p[0]), x6(p[1]), x6(p[2]), x6(p[3])} : p;
        }
        Px sum{};
        for (const Px& p : {raw(x, y0), raw(x + 1U, y0), raw(x, y1), raw(x + 1U, y1)})
            for (unsigned i = 0; i < 4; ++i) sum[i] += p[i];
        Px out{};
        for (unsigned i = 0; i < 4; ++i) out[i] = six_ ? (sum[i] + (sum[i] >> 6)) & 0xFFU : sum[i] >> 2;
        return out;
    }

    const Efb& efb_;
    bool depth_, six_;
    std::array<u32, 3> w_{};
    bool wrap_{false}, gamma_{false};
    float gamma_rcp_{1.0f};
    u32 top_{0}, bottom_{0};
};

// YUV of an RGB8 value, divided by 256 with .5 rounded up (TextureConversionShader.cpp).
Px yuv_of(const Px& c) {
    auto rnd = [](s32 v) { return static_cast<u32>((v >> 8) + ((v >> 7) & 1)); };
    const s32 r = static_cast<s32>(c[0]), g = static_cast<s32>(c[1]), b = static_cast<s32>(c[2]);
    return {rnd(66 * r + 129 * g + 25 * b + 16 * 256) & 0xFFU, rnd(-38 * r - 74 * g + 112 * b + 128 * 256) & 0xFFU,
            rnd(112 * r - 94 * g - 18 * b + 128 * 256) & 0xFFU, c[3]};
}

bool texture_copy(u32 trigger, const BpMemory& bp, const Efb& efb, mem::Bus& bus, u32 phys_mask, const Rect& r,
                  u32 fmt, CopyResult& res, std::string& error) {
    char b[128];
    const u32 tf = bits(trigger, 3, 4);
    const u32 cf = tf / 2U + (tf & 1U) * 8U;
    const bool depth = fmt == 3U;
    const bool half = (trigger & 0x200U) != 0U;
    const bool yuv = (trigger & 0x8000U) != 0U && (trigger & 0x10000U) != 0U;
    if (cf > GB8) {
        std::snprintf(b, sizeof b, "EFB copy format %u from %s is not a valid copy", cf, depth ? "depth" : "colour");
        error = b;
        return false;
    }
    u32 bw_log = 2, bh_log = 2;
    if (cf == R4) { bw_log = 3; bh_log = 3; }
    else if (cf == R8_1 || cf == R8 || cf == RA4 || cf == A8 || cf == G8 || cf == B8) bw_log = 3;
    const u32 bw = 1U << bw_log, bh = 1U << bh_log;
    const u32 block_bytes = cf == RGBA8 ? 64U : 32U;
    // size in texels and whole blocks (TextureCacheBase.cpp ResolveFramebufferCopyData)
    const u32 tw = (r.right - r.left) >> (half ? 1U : 0U), th = (r.bottom - r.top) >> (half ? 1U : 0U);
    const u32 bx = (tw + bw - 1U) >> bw_log, by = (th + bh - 1U) >> bh_log;
    const u32 dest = (bp.reg(bp::EFB_ADDR) << 5) & phys_mask, stride = bp.reg(bp::EFB_STRIDE) << 5;
    const u32 step = half ? 2U : 1U;
    const PixelReader rd(efb, fmt, depth, r, trigger, bp);

    std::vector<u8> row(static_cast<std::size_t>(bx) * block_bytes);
    for (u32 tb = 0; tb < by; ++tb) {
        std::fill(row.begin(), row.end(), u8{0});
        for (u32 sb = 0; sb < bx; ++sb) {
            u8* blk = row.data() + static_cast<std::size_t>(sb) * block_bytes;
            for (u32 t = 0; t < bh; ++t) {
                for (u32 s = 0; s < bw; ++s) {
                    const u32 px = sb * bw + s, py = tb * bh + t;
                    const Px tv = rd.texel(r.left + px * step, r.top + py * step, half);
                    const Px c = yuv ? yuv_of(tv) : tv; // the YUV conversion replaces R, G, B
                    const u32 i = t * bw + s;
                    const u32 lum = c[0];
                    auto put16 = [&](u32 v) { blk[2 * i] = static_cast<u8>(v >> 8); blk[2 * i + 1] = static_cast<u8>(v); };
                    switch (cf) {
                    case R4:
                        if ((i & 1U) == 0U) blk[i >> 1] = static_cast<u8>(lum & 0xF0U);
                        else blk[i >> 1] = static_cast<u8>(blk[i >> 1] | (lum >> 4));
                        break;
                    case R8_1: case R8: blk[i] = static_cast<u8>(lum); break;
                    case RA4: blk[i] = static_cast<u8>((c[3] & 0xF0U) | (lum >> 4)); break;
                    case RA8: blk[2 * i] = static_cast<u8>(c[3]); blk[2 * i + 1] = static_cast<u8>(lum); break;
                    case RGB565: put16(((c[0] << 8) & 0xF800U) | ((c[1] << 3) & 0x07E0U) | (c[2] >> 3)); break;
                    case RGB5A3:
                        if (c[3] >= 224U) put16(0x8000U | ((c[0] << 7) & 0x7C00U) | ((c[1] << 2) & 0x03E0U) | (c[2] >> 3));
                        else put16(((c[3] << 7) & 0x7000U) | ((c[0] << 4) & 0x0F00U) | (c[1] & 0xF0U) | (c[2] >> 4));
                        break;
                    case RGBA8:
                        blk[2 * i] = static_cast<u8>(c[3]); blk[2 * i + 1] = static_cast<u8>(c[0]);
                        blk[32 + 2 * i] = static_cast<u8>(c[1]); blk[33 + 2 * i] = static_cast<u8>(c[2]);
                        break;
                    case A8: blk[i] = static_cast<u8>(c[3]); break;
                    case G8: blk[i] = static_cast<u8>(c[1]); break;
                    case B8: blk[i] = static_cast<u8>(c[2]); break;
                    case RG8: blk[2 * i] = static_cast<u8>(c[1]); blk[2 * i + 1] = static_cast<u8>(c[0]); break;
                    default: blk[2 * i] = static_cast<u8>(c[2]); blk[2 * i + 1] = static_cast<u8>(c[1]); break; // GB8
                    }
                }
            }
        }
        const u32 addr = (dest + tb * stride) & phys_mask;
        if (bus.dma_write(addr, row) != mem::BusStatus::Ok) {
            std::snprintf(b, sizeof b, "EFB copy writes %zu bytes at %08X outside RAM", row.size(), addr);
            error = b;
            return false;
        }
    }
    res.copied = true;
    res.address = dest;
    res.stride = stride;
    res.width = bx * bw;
    res.height = by * bh;
    res.bytes = static_cast<u32>(row.size()) * by;
    return true;
}

bool xfb_copy(u32 trigger, const BpMemory& bp, const Efb& efb, mem::Bus& bus, u32 phys_mask, const Rect& r, u32 fmt,
              CopyResult& res, std::string& error) {
    char b[160];
    const u32 ys_reg = bp.reg(bp::COPY_YSCALE);
    const bool invert = (trigger & 0x400U) != 0U;
    if (invert && ys_reg == 0U) { error = "XFB copy with scale invert and a y scale of 0"; return false; }
    if (!invert && ys_reg == 0U) { error = "XFB copy with a y scale of 0"; return false; }
    const float ys = invert ? 256.0f / static_cast<float>(ys_reg) : static_cast<float>(ys_reg) / 256.0f;
    if ((r.left & 1U) != 0U || ((r.right - r.left) & 1U) != 0U) {
        std::snprintf(b, sizeof b, "XFB copy from an odd column or with an odd width (%u..%u) is not handled", r.left, r.right);
        error = b;
        return false;
    }
    const u32 width = r.right - r.left, src_h = r.bottom - r.top;
    // BPStructs.cpp: lines written = 1 + (BP 4A height field) * scale; SWEfbInterface.cpp encodes
    // src_h * scale rows with CopyRegion.h (nearest row: round(row * src_h / dst_h))
    const u32 lines = static_cast<u32>(1.0f + static_cast<float>(bits(bp.reg(bp::EFB_WH), 10, 10)) * ys);
    const u32 dst_h = static_cast<u32>(static_cast<float>(src_h) * ys);
    if (dst_h == 0U) { error = "XFB copy scaled to 0 lines"; return false; }
    const double ratio = static_cast<double>(src_h) / static_cast<double>(dst_h);
    const u32 dest = (bp.reg(bp::EFB_ADDR) << 5) & phys_mask, stride = bp.reg(bp::EFB_STRIDE) << 5;
    const PixelReader rd(efb, fmt, false, r, trigger, bp);
    // encode every source row once
    std::vector<std::vector<u8>> rows(src_h, std::vector<u8>(static_cast<std::size_t>(width) * 2U));
    std::vector<Px> line(width + 1U);
    for (u32 y = 0; y < src_h; ++y) {
        for (u32 x = 0; x < width; ++x) {
            // BT.601 with rounding (SWEfbInterface.cpp ConvertColorToYUV): Y, U, V before the offsets
            const Px c = rd.texel(r.left + x, r.top + y, false);
            const s32 cr = static_cast<s32>(c[0]), cg = static_cast<s32>(c[1]), cb = static_cast<s32>(c[2]);
            const s32 yy = (66 * cr + 129 * cg + 25 * cb) & 0xFFFF;
            const s32 uu = static_cast<s16>(-38 * cr - 74 * cg + 112 * cb), vv = static_cast<s16>(112 * cr - 94 * cg - 18 * cb);
            auto rnd = [](s32 v) { return (v >> 8) + ((v >> 7) & 1); };
            line[x + 1U] = {static_cast<u32>(static_cast<u8>(rnd(yy))), static_cast<u32>(static_cast<s8>(rnd(uu))),
                            static_cast<u32>(static_cast<s8>(rnd(vv))), 0U};
        }
        line[0] = line[1]; // the border repeats the first pixel (pairs never read past the last one)
        auto sv = [&](u32 i, unsigned k) { return static_cast<s32>(static_cast<s8>(static_cast<u8>(line[i][k]))); };
        std::vector<u8>& out = rows[y];
        for (u32 i = 1; i < width + 1U; i += 2) {
            const std::size_t o = static_cast<std::size_t>(i - 1U) * 2U;
            out[o] = static_cast<u8>(line[i][0] + 16U);
            out[o + 1] = static_cast<u8>(128 + ((sv(i - 1U, 1) + sv(i, 1) * 2 + sv(i + 1U, 1)) >> 2));
            out[o + 2] = static_cast<u8>(line[i + 1U][0] + 16U);
            out[o + 3] = static_cast<u8>(128 + ((sv(i - 1U, 2) + sv(i, 2) * 2 + sv(i + 1U, 2)) >> 2));
        }
    }
    for (u32 y = 0; y < lines; ++y) {
        const u32 src_y = std::min(src_h - 1U, static_cast<u32>(std::round(static_cast<double>(y) * ratio)));
        const u32 addr = (dest + y * stride) & phys_mask;
        if (bus.dma_write(addr, rows[src_y]) != mem::BusStatus::Ok) {
            std::snprintf(b, sizeof b, "XFB copy writes %u bytes at %08X outside RAM", width * 2U, addr);
            error = b;
            return false;
        }
    }
    res.copied = true;
    res.xfb = true;
    res.address = dest;
    res.stride = stride;
    res.width = width;
    res.height = lines;
    res.bytes = width * 2U * lines;
    return true;
}

void clear(const BpMemory& bp, Efb& efb, const Rect& r, u32 fmt) {
    const u32 bm = bp.reg(bp::BLENDMODE);
    const bool cen = (bm & 8U) != 0U, aen = (bm & 0x10U) != 0U; // alpha exists only in RGBA6
    const bool zen = (bp.reg(bp::ZMODE) & 0x10U) != 0U;
    const u32 ar = bp.reg(bp::CLEAR_AR), gb = bp.reg(bp::CLEAR_GB);
    u32 cr = bits(ar, 0, 8), cg = bits(gb, 8, 8), cb = bits(gb, 0, 8);
    const u32 ca = bits(ar, 8, 8);
    u32 z = bp.reg(bp::CLEAR_Z) & 0xFFFFFFU;
    if (fmt == 2U) {
        cr = ((cr >> 3) << 3) | (cr >> 5);
        cg = ((cg >> 2) << 2) | (cg >> 6);
        cb = ((cb >> 3) << 3) | (cb >> 5);
        z = (z & 0xFFFF00U) | (z >> 16);
    }
    for (u32 y = r.top; y < r.bottom; ++y) {
        for (u32 x = r.left; x < r.right; ++x) {
            if (fmt == 1U) {
                u32 c = efb.color24(x, y);
                if (cen) c = (c & 0x3FU) | ((cr >> 2) << 18) | ((cg >> 2) << 12) | ((cb >> 2) << 6);
                if (aen) c = (c & 0xFFFFC0U) | (ca >> 2);
                if (cen || aen) efb.set_color24(x, y, c);
            } else if (cen) {
                efb.set_color24(x, y, (cr << 16) | (cg << 8) | cb);
            }
            if (zen) efb.set_depth24(x, y, z);
        }
    }
}

} // namespace

bool efb_copy(u32 trigger, const BpMemory& bp, Efb& efb, mem::Bus& bus, u32 phys_mask, CopyResult& res,
              std::string& error) {
    res = CopyResult{};
    const u32 tl = bp.reg(bp::EFB_TL), wh = bp.reg(bp::EFB_WH);
    Rect r{bits(tl, 0, 10), bits(tl, 10, 10), 0, 0};
    r.right = r.left + bits(wh, 0, 10) + 1U;
    r.bottom = r.top + bits(wh, 10, 10) + 1U;
    if (r.right > kEfbWidth || r.bottom > kEfbHeight) {
        if (r.left >= kEfbWidth || r.top >= kEfbHeight) { res.ignored = true; return true; }
        r.right = std::min(r.right, kEfbWidth);
        r.bottom = std::min(r.bottom, kEfbHeight);
    }
    const u32 fmt = bits(bp.reg(bp::ZCOMPARE), 0, 3);
    if (fmt > 3U) {
        char b[80];
        std::snprintf(b, sizeof b, "EFB copy/clear with pixel format %u is not implemented yet", fmt);
        error = b;
        return false;
    }
    if ((bp.reg(bp::EFB_STRIDE) << 5) != 0U) {
        const bool ok = (trigger & 0x4000U) != 0U ? xfb_copy(trigger, bp, efb, bus, phys_mask, r, fmt, res, error)
                                                   : texture_copy(trigger, bp, efb, bus, phys_mask, r, fmt, res, error);
        if (!ok) return false;
    }
    if ((trigger & 0x800U) != 0U) {
        clear(bp, efb, r, fmt);
        res.cleared = true;
    }
    return true;
}

} // namespace orca::gpu::sw
