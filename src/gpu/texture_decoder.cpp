// Orca Core — texture decoder. See texture_decoder.hpp for sources.
#include "orca/gpu/texture_decoder.hpp"

#include <array>
#include <cstdio>
#include <numeric>

namespace orca::gpu {
namespace {

u8 x3(u32 v) { return static_cast<u8>((v << 5) | (v << 2) | (v >> 1)); }
u8 x4(u32 v) { return static_cast<u8>((v << 4) | v); }
u8 x5(u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); }
u8 x6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }
u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }

void set(u8* o, u32 r, u32 g, u32 b, u32 a) {
    o[0] = static_cast<u8>(r); o[1] = static_cast<u8>(g); o[2] = static_cast<u8>(b); o[3] = static_cast<u8>(a);
}
void rgb565(u8* o, u32 v) { set(o, x5((v >> 11) & 31U), x6((v >> 5) & 63U), x5(v & 31U), 255); }
void rgb5a3(u8* o, u32 v) {
    if ((v & 0x8000U) != 0U) set(o, x5((v >> 10) & 31U), x5((v >> 5) & 31U), x5(v & 31U), 255);
    else set(o, x4((v >> 8) & 15U), x4((v >> 4) & 15U), x4(v & 15U), x3((v >> 12) & 7U));
}
void ia8(u8* o, u8 alpha, u8 i) { set(o, i, i, i, alpha); }

bool palette(u8* o, u32 index, std::span<const u8> tlut, TlutFormat fmt, std::string& error) {
    if (static_cast<std::size_t>(index) * 2U + 2U > tlut.size()) {
        char b[80];
        std::snprintf(b, sizeof b, "palette index %u is outside the %zu-byte TLUT", index, tlut.size());
        error = b;
        return false;
    }
    const u8* e = tlut.data() + index * 2U;
    switch (fmt) {
    case TlutFormat::IA8: ia8(o, e[0], e[1]); return true;
    case TlutFormat::RGB565: rgb565(o, be16(e)); return true;
    case TlutFormat::RGB5A3: rgb5a3(o, be16(e)); return true;
    }
    error = "invalid TLUT format";
    return false;
}

// The 4 colours of a DXT1 block.
void dxt_colors(const u8* blk, std::array<std::array<u8, 4>, 4>& c) {
    const u32 c1 = be16(blk), c2 = be16(blk + 2);
    const int r1 = x5((c1 >> 11) & 31U), g1 = x6((c1 >> 5) & 63U), b1 = x5(c1 & 31U);
    const int r2 = x5((c2 >> 11) & 31U), g2 = x6((c2 >> 5) & 63U), b2 = x5(c2 & 31U);
    auto blend = [](int v1, int v2) { return static_cast<u8>((v1 * 3 + v2 * 5) >> 3); };
    c[0] = {static_cast<u8>(r1), static_cast<u8>(g1), static_cast<u8>(b1), 255};
    c[1] = {static_cast<u8>(r2), static_cast<u8>(g2), static_cast<u8>(b2), 255};
    if (c1 > c2) {
        c[2] = {blend(r2, r1), blend(g2, g1), blend(b2, b1), 255};
        c[3] = {blend(r1, r2), blend(g1, g2), blend(b1, b2), 255};
    } else {
        const u8 mr = static_cast<u8>(std::midpoint(r1, r2)), mg = static_cast<u8>(std::midpoint(g1, g2)),
                 mb = static_cast<u8>(std::midpoint(b1, b2));
        c[2] = {mr, mg, mb, 255};
        c[3] = {mr, mg, mb, 0};
    }
}

} // namespace

bool tex_format_valid(u32 f) { return f <= 6U || f == 8U || f == 9U || f == 10U || f == 14U; }
bool tex_format_paletted(TexFormat f) { return f == TexFormat::C4 || f == TexFormat::C8 || f == TexFormat::C14X2; }

u32 tex_block_width(TexFormat f) {
    switch (f) {
    case TexFormat::I4: case TexFormat::C4: case TexFormat::CMPR: case TexFormat::I8: case TexFormat::IA4:
    case TexFormat::C8: return 8;
    default: return 4;
    }
}
u32 tex_block_height(TexFormat f) {
    return (f == TexFormat::I4 || f == TexFormat::C4 || f == TexFormat::CMPR) ? 8U : 4U;
}
u32 tex_size_bytes(TexFormat f, u32 width, u32 height) {
    const u32 bw = tex_block_width(f), bh = tex_block_height(f);
    const u32 tiles = ((width + bw - 1U) / bw) * ((height + bh - 1U) / bh);
    return tiles * (f == TexFormat::RGBA8 ? 64U : 32U);
}

bool decode_texel(TexFormat f, std::span<const u8> src, u32 s, u32 t, u32 width, std::span<const u8> tlut,
                  TlutFormat tlut_format, u8 rgba[4], std::string& error) {
    if (!tex_format_valid(static_cast<u32>(f))) { error = "invalid texture format"; return false; }
    const u32 bw = tex_block_width(f), bh = tex_block_height(f);
    const u32 tiles_x = (width + bw - 1U) / bw;
    const u32 tile = (t / bh) * tiles_x + (s / bw);
    const u32 x = s % bw, y = t % bh;
    const u32 tile_bytes = f == TexFormat::RGBA8 ? 64U : 32U;
    const std::size_t off = static_cast<std::size_t>(tile) * tile_bytes;
    if (off + tile_bytes > src.size()) { error = "texel outside the texture data"; return false; }
    const u8* p = src.data() + off;
    switch (f) {
    case TexFormat::I4: case TexFormat::C4: {
        const u8 byte = p[y * 4U + x / 2U];
        const u32 v = (x & 1U) != 0U ? (byte & 15U) : (byte >> 4);
        if (f == TexFormat::C4) return palette(rgba, v, tlut, tlut_format, error);
        const u8 i = x4(v);
        set(rgba, i, i, i, i);
        return true;
    }
    case TexFormat::I8: { const u8 i = p[y * 8U + x]; set(rgba, i, i, i, i); return true; }
    case TexFormat::C8: return palette(rgba, p[y * 8U + x], tlut, tlut_format, error);
    case TexFormat::IA4: { const u8 v = p[y * 8U + x]; const u8 l = x4(v & 15U); set(rgba, l, l, l, x4(v >> 4)); return true; }
    case TexFormat::IA8: { const u8* e = p + (y * 4U + x) * 2U; ia8(rgba, e[0], e[1]); return true; }
    case TexFormat::RGB565: rgb565(rgba, be16(p + (y * 4U + x) * 2U)); return true;
    case TexFormat::RGB5A3: rgb5a3(rgba, be16(p + (y * 4U + x) * 2U)); return true;
    case TexFormat::C14X2: return palette(rgba, be16(p + (y * 4U + x) * 2U) & 0x3FFFU, tlut, tlut_format, error);
    case TexFormat::RGBA8: {
        const u32 i = (y * 4U + x) * 2U;
        set(rgba, p[i + 1U], p[32U + i], p[32U + i + 1U], p[i]);
        return true;
    }
    case TexFormat::CMPR: {
        const u8* blk = p + ((y / 4U) * 2U + (x / 4U)) * 8U;
        std::array<std::array<u8, 4>, 4> c{};
        dxt_colors(blk, c);
        const u32 sel = (blk[4U + (y & 3U)] >> (6U - 2U * (x & 3U))) & 3U;
        set(rgba, c[sel][0], c[sel][1], c[sel][2], c[sel][3]);
        return true;
    }
    }
    error = "invalid texture format";
    return false;
}

bool decode_texture(TexFormat f, std::span<const u8> src, u32 width, u32 height, std::span<const u8> tlut,
                    TlutFormat tlut_format, std::vector<u8>& out, std::string& error) {
    if (!tex_format_valid(static_cast<u32>(f))) { error = "invalid texture format"; return false; }
    if (src.size() < tex_size_bytes(f, width, height)) { error = "texture data is shorter than the image"; return false; }
    out.assign(static_cast<std::size_t>(width) * height * 4U, 0);
    // Sequential walk over the tiles in storage order: `p` only ever moves forward.
    const u32 bw = tex_block_width(f), bh = tex_block_height(f);
    const u8* p = src.data();
    u8 px[4];
    auto put = [&](u32 x, u32 y, const u8* c) {
        if (x < width && y < height) {
            u8* o = out.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
        }
    };
    for (u32 ty = 0; ty < height; ty += bh) {
        for (u32 tx = 0; tx < width; tx += bw) {
            switch (f) {
            case TexFormat::I4: case TexFormat::C4:
                for (u32 y = 0; y < 8; ++y)
                    for (u32 b = 0; b < 4; ++b, ++p)
                        for (u32 h = 0; h < 2; ++h) {
                            const u32 v = h == 0U ? (*p >> 4) : (*p & 15U);
                            if (f == TexFormat::C4) { if (!palette(px, v, tlut, tlut_format, error)) return false; }
                            else { const u8 i = x4(v); set(px, i, i, i, i); }
                            put(tx + b * 2U + h, ty + y, px);
                        }
                break;
            case TexFormat::I8: case TexFormat::C8: case TexFormat::IA4:
                for (u32 y = 0; y < 4; ++y)
                    for (u32 x = 0; x < 8; ++x, ++p) {
                        if (f == TexFormat::I8) set(px, *p, *p, *p, *p);
                        else if (f == TexFormat::IA4) { const u8 l = x4(*p & 15U); set(px, l, l, l, x4(*p >> 4)); }
                        else if (!palette(px, *p, tlut, tlut_format, error)) return false;
                        put(tx + x, ty + y, px);
                    }
                break;
            case TexFormat::IA8: case TexFormat::RGB565: case TexFormat::RGB5A3: case TexFormat::C14X2:
                for (u32 y = 0; y < 4; ++y)
                    for (u32 x = 0; x < 4; ++x, p += 2) {
                        if (f == TexFormat::IA8) ia8(px, p[0], p[1]);
                        else if (f == TexFormat::RGB565) rgb565(px, be16(p));
                        else if (f == TexFormat::RGB5A3) rgb5a3(px, be16(p));
                        else if (!palette(px, be16(p) & 0x3FFFU, tlut, tlut_format, error)) return false;
                        put(tx + x, ty + y, px);
                    }
                break;
            case TexFormat::RGBA8:
                for (u32 i = 0; i < 16; ++i) {
                    set(px, p[2U * i + 1U], p[32U + 2U * i], p[33U + 2U * i], p[2U * i]);
                    put(tx + (i & 3U), ty + (i >> 2), px);
                }
                p += 64;
                break;
            case TexFormat::CMPR:
                for (u32 sub = 0; sub < 4; ++sub, p += 8) {
                    std::array<std::array<u8, 4>, 4> c{};
                    dxt_colors(p, c);
                    const u32 ox = tx + (sub & 1U) * 4U, oy = ty + (sub >> 1) * 4U;
                    for (u32 y = 0; y < 4; ++y) {
                        u32 line = p[4U + y];
                        for (u32 x = 0; x < 4; ++x, line <<= 2) put(ox + x, oy + y, c[(line >> 6) & 3U].data());
                    }
                }
                break;
            }
        }
    }
    return true;
}

} // namespace orca::gpu
