// Orca Core — EFB copies and clears. See efb_copy.hpp for sources and choices.
#include "orca/gpu/sw/efb_copy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace orca::gpu::sw {
namespace {

constexpr u32 kPixels = kEfbWidth * kEfbHeight;

u32 bits(u32 v, unsigned sh, unsigned n) { return (v >> sh) & ((1U << n) - 1U); }
u32 x6(u32 v) { return ((v << 2) | (v >> 4)) & 0xFFU; }

enum Fmt : u32 { R4 = 0, R8_1 = 1, RA4 = 2, RA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, A8 = 7,
                 R8 = 8, G8 = 9, B8 = 10, RG8 = 11, GB8 = 12 };

struct Rect { u32 left, top, right, bottom; };

// Seven copy filter weights from BP 53 (w0-w3) and BP 54 (w4-w6).
std::array<u32, 7> filter_weights(const BpMemory& bp) {
    const u32 lo = bp.reg(bp::COPY_FILTER0), hi = bp.reg(bp::COPY_FILTER1);
    return {bits(lo, 0, 6), bits(lo, 6, 6), bits(lo, 12, 6), bits(lo, 18, 6),
            bits(hi, 0, 6), bits(hi, 6, 6), bits(hi, 12, 6)};
}

float gamma_of(u32 trigger) {
    switch (bits(trigger, 7, 2)) { case 0: return 1.0f; case 1: return 1.7f; default: return 2.2f; }
}

u32 apply_gamma(u32 c, float gamma_rcp) {
    const float v = std::clamp(std::pow(static_cast<float>(c) / 255.0f, gamma_rcp) * 255.0f, 0.0f, 255.0f);
    return static_cast<u32>(static_cast<u8>(v));
}

// Reads EFB pixels the way the copy hardware is modelled: colour components,
// optional vertical filter + gamma, optional 2x2 box filter.
class PixelReader {
public:
    PixelReader(const Efb& efb, u32 fmt, bool depth, const Rect& r, u32 trigger, const BpMemory& bp)
        : efb_(efb), depth_(depth), six_(fmt == 1U && !depth), rect_(r) {
        w_ = filter_weights(bp);
        gamma_rcp_ = 1.0f / gamma_of(trigger);
        const bool identity = w_[0] == 0U && w_[1] == 0U && w_[5] == 0U && w_[6] == 0U && w_[2] + w_[3] + w_[4] == 64U;
        filter_ = !depth && (!identity || bits(trigger, 7, 2) != 0U);
        clamp_top_ = (trigger & 1U) != 0U;
        clamp_bottom_ = (trigger & 2U) != 0U;
    }

    // RGBA8 of the texel whose top-left source pixel has linear index `lin`.
    std::array<u32, 4> texel(u32 lin, bool half) const {
        if (!half) {
            const auto p = fetch(lin);
            if (six_) return {filter_ ? p[0] : x6(p[0]), filter_ ? p[1] : x6(p[1]), filter_ ? p[2] : x6(p[2]), x6(p[3])};
            return p;
        }
        std::array<u32, 4> s{};
        for (const u32 o : {0U, 1U, kEfbWidth, kEfbWidth + 1U}) {
            const auto p = fetch(lin + o);
            for (unsigned i = 0; i < 4; ++i) s[i] += p[i];
        }
        std::array<u32, 4> out{};
        for (unsigned i = 0; i < 4; ++i) {
            const bool is6 = six_ && (i == 3U || !filter_);
            out[i] = is6 ? (s[i] + (s[i] >> 6)) & 0xFFU : s[i] >> 2;
        }
        return out;
    }

private:
    u32 raw(u32 lin) const {
        if (lin >= kPixels) return 0U;
        const u32 x = lin % kEfbWidth, y = lin / kEfbWidth;
        return depth_ ? efb_.depth24(x, y) : efb_.color24(x, y);
    }
    // Components: 6-bit for RGBA6 (alpha always 6-bit; RGB 8-bit once filtered),
    // bytes high/mid/low otherwise with alpha 255.
    std::array<u32, 4> comps(u32 c) const {
        if (six_) return {bits(c, 18, 6), bits(c, 12, 6), bits(c, 6, 6), bits(c, 0, 6)};
        return {bits(c, 16, 8), bits(c, 8, 8), bits(c, 0, 8), 255U};
    }
    std::array<u32, 4> fetch(u32 lin) const {
        auto p = comps(raw(lin));
        if (!filter_) return p;
        if (six_) for (unsigned i = 0; i < 3; ++i) p[i] = x6(p[i]);
        if (lin >= kPixels) return p;
        const s32 y = static_cast<s32>(lin / kEfbWidth), x = static_cast<s32>(lin % kEfbWidth);
        const s32 yp = std::max(clamp_top_ ? static_cast<s32>(rect_.top) : 0, y - 1);
        const s32 yn = std::min((clamp_bottom_ ? static_cast<s32>(rect_.bottom) : static_cast<s32>(kEfbHeight)) - 1, y + 1);
        auto row = [&](s32 yy) {
            auto q = comps(efb_.color24(static_cast<u32>(x), static_cast<u32>(yy)));
            if (six_) for (unsigned i = 0; i < 3; ++i) q[i] = x6(q[i]);
            return q;
        };
        const auto a = row(yp), c = row(yn);
        for (unsigned i = 0; i < 3; ++i) {
            const u32 sum = a[i] * (w_[0] + w_[1]) + p[i] * (w_[2] + w_[3] + w_[4]) + c[i] * (w_[5] + w_[6]);
            p[i] = apply_gamma(std::min(255U, sum >> 6), gamma_rcp_);
        }
        return p;
    }

    const Efb& efb_;
    bool depth_, six_;
    Rect rect_;
    std::array<u32, 7> w_{};
    float gamma_rcp_{1.0f};
    bool filter_{false}, clamp_top_{false}, clamp_bottom_{false};
};

u32 intensity(u32 r, u32 g, u32 b) { return ((4096U + 66U * r + 129U * g + 25U * b) >> 8) & 0xFFU; }

bool texture_copy(u32 trigger, const BpMemory& bp, const Efb& efb, mem::Bus& bus, u32 phys_mask, const Rect& r,
                  u32 fmt, CopyResult& res, std::string& error) {
    char b[128];
    const u32 tf = bits(trigger, 3, 4);
    const u32 cf = tf / 2U + (tf & 1U) * 8U;
    const bool depth = fmt == 3U;
    const bool half = (trigger & 0x200U) != 0U;
    const bool yuv = !depth && (trigger & 0x8000U) != 0U && (trigger & 0x10000U) != 0U;
    if (cf > GB8 || (depth && (cf == RA4 || cf == RA8 || cf == RGB565 || cf == RGB5A3 || cf == A8))) {
        std::snprintf(b, sizeof b, "EFB copy format %u from %s is not a valid copy", cf, depth ? "depth" : "colour");
        error = b;
        return false;
    }
    u32 bw_log = 2, bh_log = 2;
    if (cf == R4) { bw_log = 3; bh_log = 3; }
    else if (cf == R8_1 || cf == R8 || cf == RA4 || cf == A8 || cf == G8 || cf == B8) bw_log = 3;
    const u32 bw = 1U << bw_log, bh = 1U << bh_log;
    const u32 block_bytes = cf == RGBA8 ? 64U : 32U;
    const u32 wx = (r.right - r.left - 1U) >> (half ? 1U : 0U), wy = (r.bottom - r.top - 1U) >> (half ? 1U : 0U);
    const u32 bx = (wx >> bw_log) + 1U, by = (wy >> bh_log) + 1U;
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
                    const u32 lin = (r.top + py * step) * kEfbWidth + r.left + px * step;
                    const auto c = rd.texel(lin, half);
                    const u32 i = t * bw + s;
                    const u32 lum = yuv ? intensity(c[0], c[1], c[2]) : c[0];
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

struct Yuv { s32 y, u, v; };

Yuv to_yuv(u32 r, u32 g, u32 b) {
    const u32 y = (66U * r + 129U * g + 25U * b) & 0xFFFFU;
    const s32 u = static_cast<s16>(-38 * static_cast<s32>(r) - 74 * static_cast<s32>(g) + 112 * static_cast<s32>(b));
    const s32 v = static_cast<s16>(112 * static_cast<s32>(r) - 94 * static_cast<s32>(g) - 18 * static_cast<s32>(b));
    auto rnd = [](s32 x) { return (x >> 8) + ((x >> 7) & 1); };
    return {static_cast<s32>(static_cast<u8>(rnd(static_cast<s32>(y)))), static_cast<s8>(rnd(u)), static_cast<s8>(rnd(v))};
}

bool xfb_copy(u32 trigger, const BpMemory& bp, const Efb& efb, mem::Bus& bus, u32 phys_mask, const Rect& r, u32 fmt,
              CopyResult& res, std::string& error) {
    char b[160];
    const u32 ys_reg = bp.reg(bp::COPY_YSCALE);
    const bool invert = (trigger & 0x400U) != 0U;
    if (invert && ys_reg == 0U) { error = "XFB copy with scale invert and a y scale of 0"; return false; }
    const float ys = invert ? 256.0f / static_cast<float>(ys_reg) : static_cast<float>(ys_reg) / 256.0f;
    if (ys != 1.0f) {
        std::snprintf(b, sizeof b, "XFB copy with vertical scale %g (BP 4E = %06X) is not implemented yet", static_cast<double>(ys), ys_reg);
        error = b;
        return false;
    }
    if ((r.left & 1U) != 0U || ((r.right - r.left) & 1U) != 0U) {
        std::snprintf(b, sizeof b, "XFB copy from an odd column or with an odd width (%u..%u) is not handled", r.left, r.right);
        error = b;
        return false;
    }
    const auto w = filter_weights(bp);
    const float gamma_rcp = 1.0f / gamma_of(trigger);
    const bool ct = (trigger & 1U) != 0U, cb = (trigger & 2U) != 0U;
    const u32 width = r.right - r.left;
    const u32 dest = (bp.reg(bp::EFB_ADDR) << 5) & phys_mask, stride = bp.reg(bp::EFB_STRIDE) << 5;
    std::vector<Yuv> line(width + 1U);
    std::vector<u8> out(static_cast<std::size_t>(width) * 2U);
    auto color = [&](u32 x, u32 y) {
        u8 c[4];
        Renderer::efb_color_rgba(efb.color24(x, y), fmt == 1U ? 1U : 0U, c);
        return std::array<u32, 3>{c[0], c[1], c[2]};
    };
    for (u32 y = r.top; y < r.bottom; ++y) {
        const u32 yp = std::max(ct ? r.top : 0U, y == 0U ? 0U : y - 1U);
        const u32 yn = std::min((cb ? r.bottom : kEfbHeight) - 1U, y + 1U);
        for (u32 x = r.left, i = 1; x < r.right; ++x, ++i) {
            const auto a = color(x, yp), c = color(x, y), n = color(x, yn);
            std::array<u32, 3> f{};
            for (unsigned k = 0; k < 3; ++k) {
                const u32 sum = a[k] * (w[0] + w[1]) + c[k] * (w[2] + w[3] + w[4]) + n[k] * (w[5] + w[6]);
                f[k] = apply_gamma(std::min(255U, sum >> 6), gamma_rcp);
            }
            line[i] = to_yuv(f[0], f[1], f[2]);
        }
        line[0] = line[1]; // the border repeats the first pixel (pairs never read past the last one)
        for (u32 i = 1; i < width + 1U; i += 2) {
            const std::size_t o = static_cast<std::size_t>(i - 1U) * 2U;
            out[o] = static_cast<u8>(line[i].y + 16);
            out[o + 1] = static_cast<u8>(128 + ((line[i - 1].u + (line[i].u * 2) + line[i + 1].u) >> 2));
            out[o + 2] = static_cast<u8>(line[i + 1].y + 16);
            out[o + 3] = static_cast<u8>(128 + ((line[i - 1].v + (line[i].v * 2) + line[i + 1].v) >> 2));
        }
        const u32 addr = (dest + (y - r.top) * stride) & phys_mask;
        if (bus.dma_write(addr, out) != mem::BusStatus::Ok) {
            std::snprintf(b, sizeof b, "XFB copy writes %zu bytes at %08X outside RAM", out.size(), addr);
            error = b;
            return false;
        }
    }
    res.copied = true;
    res.xfb = true;
    res.address = dest;
    res.stride = stride;
    res.width = width;
    res.height = r.bottom - r.top;
    res.bytes = static_cast<u32>(out.size()) * res.height;
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
