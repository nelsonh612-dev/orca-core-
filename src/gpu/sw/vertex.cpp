// Orca Core — vertex decoding and XF transform. See vertex.hpp for sources.
#include "orca/gpu/sw/vertex.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

namespace orca::gpu::sw {
namespace {

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }
u32 elem_size(u32 fmt) { return fmt <= 1U ? 1U : (fmt <= 3U ? 2U : 4U); }

// Raw value of one component; `integer` tells whether scaling applies.
float read_elem(const u8* p, u32 fmt, bool& is_byte) {
    is_byte = fmt <= 1U;
    switch (fmt) {
    case 0: return static_cast<float>(p[0]);
    case 1: return static_cast<float>(static_cast<s8>(p[0]));
    case 2: return static_cast<float>(be16(p));
    case 3: return static_cast<float>(static_cast<s16>(static_cast<u16>(be16(p))));
    default: return std::bit_cast<float>(be32(p));
    }
}

float normal_divisor(u32 fmt) {
    switch (fmt) {
    case 0: return 128.0f;
    case 1: return 64.0f;
    case 2: return 32768.0f;
    case 3: return 16384.0f;
    default: return 1.0f;
    }
}

u8 x4(u32 v) { return static_cast<u8>((v << 4) | v); }
u8 x5(u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); }
u8 x6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }
u32 rgba(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }

u32 decode_color(const u8* p, u32 fmt) {
    switch (fmt) {
    case 0: { const u32 v = be16(p); return rgba(x5(v >> 11), x6((v >> 5) & 63U), x5(v & 31U), 255); }
    case 1: case 2: return rgba(p[0], p[1], p[2], 255);
    case 3: return rgba(x4(p[0] >> 4), x4(p[0] & 15U), x4(p[1] >> 4), x4(p[1] & 15U));
    case 4: {
        const u32 v = (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2];
        return rgba(x6(v >> 18), x6((v >> 12) & 63U), x6((v >> 6) & 63U), x6(v & 63U));
    }
    default: return rgba(p[0], p[1], p[2], p[3]);
    }
}

constexpr u32 kColorBytes[6] = {2, 3, 4, 2, 3, 4};

struct Reader {
    std::span<const u8> d;
    std::size_t pos{0};
    const u8* take(std::size_t n) { const u8* p = d.data() + pos; pos += n; return p; }
};

// Source of one component: inline bytes or an array element in RAM.
bool fetch(Reader& r, u32 desc, u32 bytes, unsigned array, const CpState& cp, mem::Bus& bus, u32 mask,
           std::vector<u8>& buf, const u8*& p, u32* index_out, std::string& error, u32 extra_offset = 0) {
    if (desc == vcf::DIRECT) { p = r.take(bytes); return true; }
    const u32 idx = desc == vcf::INDEX8 ? *r.take(1) : be16(r.take(2));
    if (index_out != nullptr) *index_out = idx;
    buf.resize(bytes);
    const u32 addr = (cp.array_base[array] + idx * cp.array_stride[array] + extra_offset) & mask;
    if (bus.dma_read(addr, buf) != mem::BusStatus::Ok) {
        char b[96];
        std::snprintf(b, sizeof b, "vertex array %u element %u at %08X is outside RAM", array, idx, addr);
        error = b;
        return false;
    }
    p = buf.data();
    return true;
}

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 normalized(const Vec3& v) { const float l = length(v); return {v.x / l, v.y / l, v.z / l}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

} // namespace

bool VertexDecoder::decode(const CpState& cp, const XfMemory& xf, mem::Bus& bus, u32 mask, unsigned vat,
                           std::span<const u8> data, InputVertex& out, std::string& error) {
    out = InputVertex{};
    const VatFields f = decode_vat(cp.vat[vat & 7U]);
    Reader r{data};
    std::vector<u8> buf;
    const u32 ma = xf.word(xf::MATRIX_INDEX_A), mb = xf.word(xf::MATRIX_INDEX_B);
    out.pos_mtx = static_cast<u8>(ma & 63U);
    for (unsigned i = 0; i < 4; ++i) out.tex_mtx[i] = static_cast<u8>((ma >> (6U + 6U * i)) & 63U);
    for (unsigned i = 0; i < 4; ++i) out.tex_mtx[4U + i] = static_cast<u8>((mb >> (6U * i)) & 63U);
    char b[96];
    auto mtx_byte = [&](u8& dst) {
        const u8 v = *r.take(1);
        if (v > 63U) {
            std::snprintf(b, sizeof b, "vertex matrix index %u is above 63", v);
            error = b;
            return false;
        }
        dst = v;
        return true;
    };
    if (cp.pos_mtx_idx() && !mtx_byte(out.pos_mtx)) return false;
    for (unsigned i = 0; i < 8; ++i)
        if (cp.tex_mtx_idx(i) && !mtx_byte(out.tex_mtx[i])) return false;

    // position
    const u32 pd = cp.pos_desc();
    if (pd != vcf::NONE) {
        const u32 n = f.pos_elems != 0U ? 3U : 2U, es = elem_size(f.pos_fmt);
        const u8* p = nullptr;
        u32 idx = 0;
        if (!fetch(r, pd, n * es, 0, cp, bus, mask, buf, p, &idx, error)) return false;
        if (pd != vcf::DIRECT) out.skip = idx == (pd == vcf::INDEX8 ? 0xFFU : 0xFFFFU);
        float c[3] = {0, 0, 0};
        for (u32 i = 0; i < n; ++i) {
            bool byte = false;
            const float v = read_elem(p + i * es, f.pos_fmt, byte);
            const bool scaled = f.pos_fmt < 4U && (!byte || f.byte_dequant);
            c[i] = scaled ? v / static_cast<float>(1U << f.pos_frac) : v;
        }
        out.pos = {c[0], c[1], c[2]};
    }
    // normals
    const u32 nd = cp.normal_desc();
    out.normal = last_normal;
    if (nd != vcf::NONE) {
        const u32 vectors = f.nrm_elems != 0U ? 3U : 1U, es = elem_size(f.nrm_fmt);
        const bool three_indices = vectors == 3U && f.normal_index3 && nd != vcf::DIRECT;
        const u8* p = nullptr;
        if (three_indices) {
            for (u32 k = 0; k < 3; ++k) {
                if (!fetch(r, nd, 3U * es, 1, cp, bus, mask, buf, p, nullptr, error, k * 3U * es)) return false;
                bool byte = false;
                const float d = normal_divisor(f.nrm_fmt);
                out.normal[k] = {read_elem(p, f.nrm_fmt, byte) / d, read_elem(p + es, f.nrm_fmt, byte) / d,
                                 read_elem(p + 2U * es, f.nrm_fmt, byte) / d};
            }
        } else {
            if (!fetch(r, nd, vectors * 3U * es, 1, cp, bus, mask, buf, p, nullptr, error)) return false;
            for (u32 k = 0; k < vectors; ++k) {
                bool byte = false;
                const float d = normal_divisor(f.nrm_fmt);
                const u8* q = p + k * 3U * es;
                out.normal[k] = {read_elem(q, f.nrm_fmt, byte) / d, read_elem(q + es, f.nrm_fmt, byte) / d,
                                 read_elem(q + 2U * es, f.nrm_fmt, byte) / d};
            }
        }
    }
    // colours: present colours fill the channels in order (SWVertexLoader)
    unsigned slot = 0;
    out.color = {0xFFFFFFFFU, 0xFFFFFFFFU};
    for (unsigned i = 0; i < 2; ++i) {
        const u32 cd = cp.color_desc(i);
        if (cd == vcf::NONE) continue;
        if (f.col_fmt[i] > 5U) {
            std::snprintf(b, sizeof b, "colour %u uses invalid format %u", i, f.col_fmt[i]);
            error = b;
            return false;
        }
        const u8* p = nullptr;
        if (!fetch(r, cd, kColorBytes[f.col_fmt[i]], 2U + i, cp, bus, mask, buf, p, nullptr, error)) return false;
        out.color[slot++] = decode_color(p, f.col_fmt[i]);
    }
    // texture coordinates
    for (unsigned i = 0; i < 8; ++i) {
        const u32 td = cp.tex_desc(i);
        if (td == vcf::NONE) continue;
        const u32 n = f.tex_elems[i] != 0U ? 2U : 1U, es = elem_size(f.tex_fmt[i]);
        const u8* p = nullptr;
        if (!fetch(r, td, n * es, 4U + i, cp, bus, mask, buf, p, nullptr, error)) return false;
        for (u32 k = 0; k < n; ++k) {
            bool byte = false;
            const float v = read_elem(p + k * es, f.tex_fmt[i], byte);
            const bool scaled = f.tex_fmt[i] < 4U && (!byte || f.byte_dequant);
            out.tex[i][k] = scaled ? v / static_cast<float>(1U << f.tex_frac[i]) : v;
        }
    }
    if (r.pos != data.size()) {
        std::snprintf(b, sizeof b, "vertex used %zu of %zu bytes", r.pos, data.size());
        error = b;
        return false;
    }
    if (nd != vcf::NONE) last_normal = out.normal;
    return true;
}

void transform_vertex(const XfMemory& xf, const InputVertex& in, const TexScale& tex_scale, OutputVertex& out) {
    auto F = [&](u32 a) { return xf.as_float(a); };
    auto mul34 = [&](u32 base, const Vec3& v) {
        return Vec3{F(base) * v.x + F(base + 1) * v.y + F(base + 2) * v.z + F(base + 3),
                    F(base + 4) * v.x + F(base + 5) * v.y + F(base + 6) * v.z + F(base + 7),
                    F(base + 8) * v.x + F(base + 9) * v.y + F(base + 10) * v.z + F(base + 11)};
    };
    out = OutputVertex{};
    // position
    out.mv_pos = mul34(static_cast<u32>(in.pos_mtx) * 4U, in.pos);
    const Vec3& m = out.mv_pos;
    const u32 P = xf::PROJECTION;
    if (xf.word(P + 6) == 0U) {
        out.clip = {F(P) * m.x + F(P + 1) * m.z, F(P + 2) * m.y + F(P + 3) * m.z,
                    (F(P + 4) * m.z + F(P + 5)) * (1.0f - 1e-7f), -m.z};
    } else {
        out.clip = {F(P) * m.x + F(P + 1), F(P + 2) * m.y + F(P + 3), F(P + 4) * m.z + F(P + 5), 1.0f};
    }
    // normals
    const u32 nb = xf::NORMAL_MATRICES + static_cast<u32>(in.pos_mtx & 31U) * 3U;
    for (unsigned k = 0; k < 3; ++k) {
        const Vec3& v = in.normal[k];
        out.normal[k] = {F(nb) * v.x + F(nb + 1) * v.y + F(nb + 2) * v.z, F(nb + 3) * v.x + F(nb + 4) * v.y + F(nb + 5) * v.z,
                         F(nb + 6) * v.x + F(nb + 7) * v.y + F(nb + 8) * v.z};
    }
    out.normal[0] = normalized(out.normal[0]);

    // lighting
    auto light_vec = [&](unsigned l, u32 w) { const u32 a = xf::LIGHTS + l * 16U + w; return Vec3{F(a), F(a + 1), F(a + 2)}; };
    auto attenuation = [&](unsigned l, Vec3& ldir, u32 chan) {
        const u32 attn_fn = (chan >> 9) & 3U, diff_fn = (chan >> 7) & 3U;
        const Vec3 n = out.normal[0];
        float attn = 1.0f;
        if (attn_fn == 0U || attn_fn == 2U) {
            ldir = normalized(ldir);
            if (ldir.x == 0.0f && ldir.y == 0.0f && ldir.z == 0.0f) ldir = n;
        } else if (attn_fn == 1U) { // specular
            ldir = normalized(ldir);
            attn = dot(ldir, n) >= 0.0f ? std::max(0.0f, dot(light_vec(l, 13), n)) : 0.0f;
            const Vec3 len{1.0f, attn, attn * attn};
            Vec3 dist = light_vec(l, 7);
            if (diff_fn != 0U) dist = normalized(dist);
            const float num = std::max(0.0f, dot(len, light_vec(l, 4))), den = dot(len, dist);
            attn = den == 0.0f ? (num > 0.0f ? 1.0f : 0.0f) : num / den;
        } else { // spot
            const float d2 = dot(ldir, ldir), d = std::sqrt(d2);
            ldir = {ldir.x / d, ldir.y / d, ldir.z / d};
            attn = std::max(0.0f, dot(ldir, light_vec(l, 13)));
            const Vec3 ca = light_vec(l, 4), da = light_vec(l, 7);
            const float cos_att = ca.x + ca.y * attn + ca.z * attn * attn;
            const float dist_att = da.x + da.y * d + da.z * d2;
            const float num = std::max(0.0f, cos_att);
            attn = dist_att == 0.0f ? (num > 0.0f ? 1.0f : 0.0f) : num / dist_att;
        }
        return attn;
    };
    auto diffuse = [&](const Vec3& ldir, u32 chan, float attn) {
        const float dif = dot(ldir, out.normal[0]);
        switch ((chan >> 7) & 3U) {
        case 0: return attn;
        case 1: return attn * dif;
        default: return attn * std::max(0.0f, dif);
        }
    };
    for (unsigned c = 0; c < 2; ++c) {
        const u32 cc = xf.word(xf::CHAN0_COLOR + c), ac = xf.word(xf::CHAN0_ALPHA + c);
        const u32 matreg = xf.word(xf::MAT_COLOR0 + c), ambreg = xf.word(xf::AMB_COLOR0 + c);
        const u32 vtx = in.color[c];
        auto byte = [](u32 v, unsigned sh) { return (v >> sh) & 0xFFU; };
        u32 mat = (cc & 1U) != 0U ? vtx : matreg;
        u32 res_rgb = mat & 0xFFFFFF00U;
        if ((cc & 2U) != 0U) {
            const u32 amb = ((cc >> 6) & 1U) != 0U ? vtx : ambreg;
            float lc[3] = {static_cast<float>(byte(amb, 24)), static_cast<float>(byte(amb, 16)), static_cast<float>(byte(amb, 8))};
            const u32 lmask = ((cc >> 2) & 15U) | (((cc >> 11) & 15U) << 4);
            for (unsigned l = 0; l < 8; ++l) {
                if ((lmask & (1U << l)) == 0U) continue;
                Vec3 ldir = sub(light_vec(l, 10), out.mv_pos);
                const float a = attenuation(l, ldir, cc);
                const float s = diffuse(ldir, cc, a);
                const u32 lcol = xf.word(xf::LIGHTS + l * 16U + 3U);
                lc[0] += static_cast<float>(byte(lcol, 24)) * s;
                lc[1] += static_cast<float>(byte(lcol, 16)) * s;
                lc[2] += static_cast<float>(byte(lcol, 8)) * s;
            }
            res_rgb = 0;
            for (unsigned k = 0; k < 3; ++k) {
                const int li = std::clamp(static_cast<int>(lc[k]), 0, 255);
                const u32 mc = byte(mat, 24U - 8U * k);
                res_rgb |= ((mc * static_cast<u32>(li + (li >> 7))) >> 8) << (24U - 8U * k);
            }
        }
        const u32 mat_a = (ac & 1U) != 0U ? (vtx & 0xFFU) : (matreg & 0xFFU);
        u32 res_a = mat_a;
        if ((ac & 2U) != 0U) {
            float la = static_cast<float>(((ac >> 6) & 1U) != 0U ? (vtx & 0xFFU) : (ambreg & 0xFFU));
            const u32 lmask = ((ac >> 2) & 15U) | (((ac >> 11) & 15U) << 4);
            for (unsigned l = 0; l < 8; ++l) {
                if ((lmask & (1U << l)) == 0U) continue;
                Vec3 ldir = sub(light_vec(l, 10), out.mv_pos);
                const float a = attenuation(l, ldir, ac);
                la += static_cast<float>(xf.word(xf::LIGHTS + l * 16U + 3U) & 0xFFU) * diffuse(ldir, ac, a);
            }
            const int li = std::clamp(static_cast<int>(la), 0, 255);
            res_a = (mat_a * static_cast<u32>(li + (li >> 7))) >> 8;
        }
        out.color[c] = res_rgb | res_a;
    }

    // texture coordinate generation
    const u32 ngen = xf.word(xf::NUM_TEXGENS) & 15U;
    const bool dual = (xf.word(xf::DUAL_TEX) & 1U) != 0U;
    for (u32 g = 0; g < ngen && g < 8U; ++g) {
        const u32 info = xf.word(xf::TEX_MTX_INFO + g);
        const u32 type = (info >> 4) & 7U;
        Vec3& d = out.tex[g];
        if (type == 0U) { // regular
            const u32 row = (info >> 7) & 31U;
            Vec3 s{};
            if (row == 0U) s = in.pos;
            else if (row == 1U) s = in.normal[0];
            else if (row == 3U) s = in.normal[1];
            else if (row == 4U) s = in.normal[2];
            else if (row >= 5U && row <= 12U) s = {in.tex[row - 5U][0], in.tex[row - 5U][1], 1.0f};
            if (std::isnan(s.x)) s.x = 1;
            if (std::isnan(s.y)) s.y = 1;
            if (std::isnan(s.z)) s.z = 1;
            const u32 mb = static_cast<u32>(in.tex_mtx[g]) * 4U;
            const bool stq = ((info >> 1) & 1U) != 0U, abc1 = ((info >> 2) & 1U) != 0U;
            const float w = abc1 ? s.z : 1.0f; // AB11: the third input is 1
            d.x = F(mb) * s.x + F(mb + 1) * s.y + F(mb + 2) * w + F(mb + 3);
            d.y = F(mb + 4) * s.x + F(mb + 5) * s.y + F(mb + 6) * w + F(mb + 7);
            d.z = stq ? F(mb + 8) * s.x + F(mb + 9) * s.y + F(mb + 10) * w + F(mb + 11) : 1.0f;
            if (dual) {
                const u32 pinfo = xf.word(xf::POST_MTX_INFO + g);
                const Vec3 t = ((pinfo >> 8) & 1U) != 0U ? normalized(d) : d;
                d = mul34(xf::POST_MATRICES + (pinfo & 63U) * 4U, t);
            }
            if (d.z == 0.0f) {
                d.x = std::clamp(d.x / 2.0f, -1.0f, 1.0f);
                d.y = std::clamp(d.y / 2.0f, -1.0f, 1.0f);
            }
        } else if (type == 1U) { // emboss
            const unsigned src = (info >> 12) & 7U, light = (info >> 15) & 7U;
            const Vec3 ldir = normalized(sub(light_vec(light, 10), out.mv_pos));
            d.x = out.tex[src].x + dot(ldir, out.normal[1]);
            d.y = out.tex[src].y + dot(ldir, out.normal[2]);
            d.z = out.tex[src].z;
        } else if (type == 2U || type == 3U) { // colour 0 / 1: s = red, t = green
            const u32 col = out.color[type - 2U];
            d = {static_cast<float>(col >> 24) / 255.0f, static_cast<float>((col >> 16) & 0xFFU) / 255.0f, 1.0f};
        }
    }
    for (u32 g = 0; g < ngen && g < 8U; ++g) {
        out.tex[g].x *= tex_scale[g][0];
        out.tex[g].y *= tex_scale[g][1];
    }
}

} // namespace orca::gpu::sw
