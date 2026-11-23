// Orca Core — software rasterizer. See renderer.hpp for sources.
#include "orca/gpu/sw/renderer.hpp"
#include "orca/gpu/texture_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace orca::gpu::sw {
namespace {

u32 bits(u32 v, unsigned sh, unsigned n) { return (v >> sh) & ((1U << n) - 1U); }
s32 sext(u32 v, unsigned n) { const u32 m = 1U << (n - 1U); return static_cast<s32>((v ^ m) - m); }
u8 x6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }

int iround(float x) {
    const int t = static_cast<int>(x);
    return (x - static_cast<float>(t)) >= 0.5f ? t + 1 : t;
}

struct SlopeCtx {
    s32 x0, y0;
    float xoff, yoff, dx10, dx20, dy10, dy20;
};
struct Slope {
    float dfdx{0}, dfdy{0}, f0{1};
    s32 x0{0}, y0{0};
    float xoff{0}, yoff{0};
    Slope() = default;
    Slope(float a, float b, float c, const SlopeCtx& k) : f0(a), x0(k.x0), y0(k.y0), xoff(k.xoff), yoff(k.yoff) {
        const float d20 = c - a, d10 = b - a;
        const float A = d20 * k.dy10 - d10 * k.dy20;
        const float B = k.dx20 * d10 - k.dx10 * d20;
        const float C = k.dx20 * k.dy10 - k.dx10 * k.dy20;
        dfdx = A / C;
        dfdy = B / C;
    }
    float at(s32 x, s32 y) const {
        return f0 + dfdx * (xoff + static_cast<float>(x - x0)) + dfdy * (yoff + static_cast<float>(y - y0));
    }
};

template <typename V>
SlopeCtx slope_ctx(const V& v0, const V& v1, const V& v2, s32 xo, s32 yo) {
    const s32 X1 = iround(16.0f * (v0.screen[0] - static_cast<float>(xo))) - 9;
    const s32 Y1 = iround(16.0f * (v0.screen[1] - static_cast<float>(yo))) - 9;
    return {(X1 + 0xF) >> 4, (Y1 + 0xF) >> 4,
            (static_cast<float>((X1 + 0xF) >> 4) - (v0.screen[0] - static_cast<float>(xo))) + 0.495f,
            (static_cast<float>((Y1 + 0xF) >> 4) - (v0.screen[1] - static_cast<float>(yo))) + 0.495f,
            v1.screen[0] - v0.screen[0], v2.screen[0] - v0.screen[0],
            v1.screen[1] - v0.screen[1], v2.screen[1] - v0.screen[1]};
}
SlopeState to_state(const Slope& z) { return {z.dfdx, z.dfdy, z.f0, z.x0, z.y0, z.xoff, z.yoff}; }
Slope from_state(const SlopeState& z) {
    Slope r;
    r.dfdx = z.dfdx; r.dfdy = z.dfdy; r.f0 = z.f0; r.x0 = z.x0; r.y0 = z.y0; r.xoff = z.xoff; r.yoff = z.yoff;
    return r;
}

struct Range { s32 off, start, end; };
std::vector<Range> scissor_ranges(s32 start, s32 end, s32 offset, s32 dim) {
    std::vector<Range> r;
    for (s32 extra = -4096; extra <= 4096; extra += 1024) {
        const s32 o = offset + extra;
        const s32 s = std::clamp(start - o, 0, dim), e = std::clamp(end - o + 1, 0, dim);
        if (s < e) r.push_back({o, s, e});
    }
    return r;
}

} // namespace

void Renderer::efb_color_rgba(u32 c, u32 fmt, u8 o[4]) {
    if (fmt == 1U) { o[0] = x6(bits(c, 18, 6)); o[1] = x6(bits(c, 12, 6)); o[2] = x6(bits(c, 6, 6)); o[3] = x6(bits(c, 0, 6)); }
    else { o[0] = static_cast<u8>(c >> 16); o[1] = static_cast<u8>(c >> 8); o[2] = static_cast<u8>(c); o[3] = 255; }
}

bool Renderer::draw(Primitive prim, std::span<const OutputVertex> v, const BpMemory& bp, const XfMemory& xf,
                    const TevState& tev, const TexSource& tex, std::string& error) {
    bp_ = &bp; xf_ = &xf; tev_ = &tev; tex_ = &tex;
    err_.clear();
    const u32 fmt = bits(bp.reg(bp::ZCOMPARE), 0, 3);
    if (fmt > 3U) {
        char b[64];
        std::snprintf(b, sizeof b, "EFB pixel format %u is not implemented yet", fmt);
        error = b;
        return false;
    }
    if (bits(bp.reg(bp::FOG_PARAM3), 21, 3) != 0U) { error = "fog is not implemented yet"; return false; }
    if ((prim == Primitive::Lines || prim == Primitive::LineStrip || prim == Primitive::Points) &&
        bits(bp.reg(bp::LINE_PT_WIDTH), 22, 1) != 0U) {
        error = "lines and points with the aspect ratio adjustment (BP 22 bit 22) are not implemented yet";
        return false;
    }
    auto conv = [&](const OutputVertex& o) {
        Vtx t{};
        t.clip = o.clip;
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned k = 0; k < 4; ++k) t.color[c][k] = static_cast<float>((o.color[c] >> (24U - 8U * k)) & 0xFFU);
        for (unsigned i = 0; i < 8; ++i) t.tex[i] = {o.tex[i].x, o.tex[i].y, o.tex[i].z};
        return t;
    };
    auto tri = [&](std::size_t a, std::size_t b, std::size_t c) { return triangle(conv(v[a]), conv(v[b]), conv(v[c])); };
    const std::size_t n = v.size();
    bool ok = true;
    switch (prim) {
    case Primitive::Quads: case Primitive::Quads2:
        for (std::size_t i = 0; ok && i + 3 < n; i += 4) ok = tri(i, i + 1, i + 2) && tri(i, i + 2, i + 3);
        break;
    case Primitive::Triangles:
        for (std::size_t i = 0; ok && i + 2 < n; i += 3) ok = tri(i, i + 1, i + 2);
        break;
    case Primitive::TriangleStrip:
        for (std::size_t i = 0; ok && i + 2 < n; ++i) ok = (i & 1U) == 0U ? tri(i, i + 1, i + 2) : tri(i, i + 2, i + 1);
        break;
    case Primitive::TriangleFan:
        for (std::size_t i = 2; ok && i < n; ++i) ok = tri(0, i - 1, i);
        break;
    case Primitive::Lines:
        for (std::size_t i = 0; ok && i + 1 < n; i += 2) ok = line(conv(v[i]), conv(v[i + 1]));
        break;
    case Primitive::LineStrip:
        for (std::size_t i = 0; ok && i + 1 < n; ++i) ok = line(conv(v[i]), conv(v[i + 1]));
        break;
    case Primitive::Points:
        for (std::size_t i = 0; ok && i < n; ++i) ok = point(conv(v[i]));
        break;
    }
    if (!ok) error = err_;
    return ok;
}

bool Renderer::triangle(const Vtx& a, const Vtx& b, const Vtx& c) {
    const XfMemory& xf = *xf_;
    auto mask = [](const Vtx& v) {
        const auto& p = v.clip;
        int m = 0;
        if (p[3] - p[0] < 0) m |= 0x01;
        if (p[0] + p[3] < 0) m |= 0x02;
        if (p[3] - p[1] < 0) m |= 0x04;
        if (p[1] + p[3] < 0) m |= 0x08;
        if (p[3] * p[2] > 0) m |= 0x10;
        if (p[2] + p[3] < 0) m |= 0x20;
        return m;
    };
    if ((mask(a) & mask(b) & mask(c)) != 0) return true; // trivially rejected
    // backface test on the projected positions
    const float x0 = a.clip[0], x1 = b.clip[0], x2 = c.clip[0], y0 = a.clip[1], y1 = b.clip[1], y2 = c.clip[1];
    const float w0 = a.clip[3], w1 = b.clip[3], w2 = c.clip[3];
    const float nz = (x0 * w2 - x2 * w0) * y1 + (x2 * y0 - x0 * y2) * w1 + (y2 * w0 - y0 * w2) * x1;
    bool back = nz <= 0.0f;
    if (xf.as_float(xf::VIEWPORT + 1) > 0.0f) back = !back;
    const u32 cull = bits(bp_->reg(bp::GENMODE), 14, 2);
    if ((!back && (cull == 1U || cull == 3U)) || (back && (cull == 2U || cull == 3U))) {
        // culled triangles still set the z slope used by z freeze (Clipper.cpp),
        // with the scissor offset as the screen offset
        if (bits(bp_->reg(bp::GENMODE), 19, 1) == 0U) {
            Vtx p0 = a, p1 = back ? c : b, p2 = back ? b : c;
            divide(p0); divide(p1); divide(p2);
            const u32 so = bp_->reg(bp::SCISSOR_OFFSET);
            const SlopeCtx k = slope_ctx(p0, p1, p2, static_cast<s32>(bits(so, 0, 9)) << 1, static_cast<s32>(bits(so, 10, 9)) << 1);
            zslope_ = to_state(Slope(p0.screen[2], p1.screen[2], p2.screen[2], k));
        }
        return true;
    }
    std::vector<Vtx> V = {a, back ? c : b, back ? b : c};
    std::vector<int> idx = {0, 1, 2};
    const bool skip_clip = (xf.word(xf::CLIP_DISABLE) & 1U) != 0U && V[0].clip[3] >= 0 && V[1].clip[3] >= 0 && V[2].clip[3] >= 0;
    const int m = mask(V[0]) | mask(V[1]) | mask(V[2]);
    if (!skip_clip && m != 0) {
        std::vector<int> in = {0, 1, 2}, out;
        auto lerp = [&](float t, int o, int i) {
            const Vtx& A = V[static_cast<std::size_t>(o)];
            const Vtx& B = V[static_cast<std::size_t>(i)];
            Vtx r{};
            for (unsigned k = 0; k < 4; ++k) r.clip[k] = A.clip[k] + (B.clip[k] - A.clip[k]) * t;
            for (unsigned ch = 0; ch < 2; ++ch)
                for (unsigned k = 0; k < 4; ++k) r.color[ch][k] = A.color[ch][k] + (B.color[ch][k] - A.color[ch][k]) * t;
            for (unsigned i2 = 0; i2 < 8; ++i2)
                for (unsigned k = 0; k < 3; ++k) r.tex[i2][k] = A.tex[i2][k] + (B.tex[i2][k] - A.tex[i2][k]) * t;
            V.push_back(r);
            return static_cast<int>(V.size()) - 1;
        };
        struct Plane { int bit; float A, B, C, D; };
        const Plane planes[6] = {{0x01, -1, 0, 0, 1}, {0x02, 1, 0, 0, 1}, {0x04, 0, -1, 0, 1},
                                 {0x08, 0, 1, 0, 1},  {0x10, 0, 0, 0, 1}, {0x20, 0, 0, 1, 1}};
        for (const Plane& pl : planes) {
            if ((m & pl.bit) == 0) continue;
            auto dp = [&](int i) {
                const auto& p = V[static_cast<std::size_t>(i)].clip;
                return p[0] * pl.A + p[1] * pl.B + p[2] * pl.C + p[3] * pl.D;
            };
            out.clear();
            int prev = in[0];
            float dprev = dp(prev);
            const std::size_t cnt = in.size();
            for (std::size_t j = 1; j <= cnt; ++j) {
                const int cur = in[j % cnt];
                const float d = dp(cur);
                if (dprev >= 0) out.push_back(prev);
                const bool diff = (d <= 0 && dprev > 0) || (d > 0 && dprev <= 0);
                if (diff) out.push_back(d < 0 ? lerp(d / (d - dprev), cur, prev) : lerp(dprev / (dprev - d), prev, cur));
                prev = cur;
                dprev = d;
            }
            if (out.size() < 3) return true; // clipped away (Dolphin leaves the triangle unused)
            in.swap(out);
        }
        idx.clear();
        for (std::size_t j = 2; j < in.size(); ++j) { idx.push_back(in[0]); idx.push_back(in[j - 1]); idx.push_back(in[j]); }
    }
    for (Vtx& t : V) divide(t);
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
        if (!raster(V[static_cast<std::size_t>(idx[i])], V[static_cast<std::size_t>(idx[i + 1])], V[static_cast<std::size_t>(idx[i + 2])])) return false;
    return true;
}

// Perspective divide + viewport (Clipper.cpp PerspectiveDivide).
void Renderer::divide(Vtx& t) const {
    const XfMemory& xf = *xf_;
    const float wd = xf.as_float(xf::VIEWPORT), ht = xf.as_float(xf::VIEWPORT + 1), zr = xf.as_float(xf::VIEWPORT + 2);
    const float xo = xf.as_float(xf::VIEWPORT + 3), yo = xf.as_float(xf::VIEWPORT + 4), fz = xf.as_float(xf::VIEWPORT + 5);
    const float iw = 1.0f / t.clip[3];
    t.screen = {t.clip[0] * iw * wd + xo, t.clip[1] * iw * ht + yo, t.clip[2] * iw * zr + fz};
}

namespace {
// Texture coordinate offsets for lines and points (BP 22 bits 16-18 / 19-21).
constexpr float kLinePtTexOffsets[8] = {0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 1.0f, 1.0f};
} // namespace

// Lines (Clipper.cpp ClipLine/ProcessLine): clipped against the six planes
// (+z plane: -z + w), drawn as two triangles widened by half the line width
// (BP 22 bits 0-7 in 1/6 pixel) across the minor axis, without culling.
bool Renderer::line(Vtx a, Vtx b) {
    const BpMemory& bp = *bp_;
    auto mask = [](const Vtx& v) {
        const auto& p = v.clip;
        int m = 0;
        if (p[3] - p[0] < 0) m |= 0x01;
        if (p[0] + p[3] < 0) m |= 0x02;
        if (p[3] - p[1] < 0) m |= 0x04;
        if (p[1] + p[3] < 0) m |= 0x08;
        if (p[3] * p[2] > 0) m |= 0x10;
        if (p[2] + p[3] < 0) m |= 0x20;
        return m;
    };
    const int m0 = mask(a), m1 = mask(b), m = m0 | m1;
    if (m != 0) {
        struct Plane { int bit; float A, B, C, D; };
        const Plane planes[6] = {{0x01, -1, 0, 0, 1}, {0x02, 1, 0, 0, 1}, {0x04, 0, -1, 0, 1},
                                 {0x08, 0, 1, 0, 1},  {0x10, 0, 0, -1, 1}, {0x20, 0, 0, 1, 1}};
        float t0 = 0.0f, t1 = 0.0f;
        for (const Plane& pl : planes) {
            if ((m & pl.bit) == 0) continue;
            auto dp = [&](const Vtx& v) { return v.clip[0] * pl.A + v.clip[1] * pl.B + v.clip[2] * pl.C + v.clip[3] * pl.D; };
            const float d0 = dp(a), d1 = dp(b);
            if (d0 < 0 && d1 < 0) return true; // fully clipped
            if (d1 < 0) t1 = std::max(t1, d1 / (d1 - d0));
            else if (d0 < 0) t0 = std::max(t0, d0 / (d0 - d1));
        }
        auto lerp = [](float t, const Vtx& A, const Vtx& B) {
            Vtx r{};
            for (unsigned k = 0; k < 4; ++k) r.clip[k] = A.clip[k] + (B.clip[k] - A.clip[k]) * t;
            for (unsigned ch = 0; ch < 2; ++ch)
                for (unsigned k = 0; k < 4; ++k) r.color[ch][k] = A.color[ch][k] + (B.color[ch][k] - A.color[ch][k]) * t;
            for (unsigned i = 0; i < 8; ++i)
                for (unsigned k = 0; k < 3; ++k) r.tex[i][k] = A.tex[i][k] + (B.tex[i][k] - A.tex[i][k]) * t;
            return r;
        };
        const Vtx oa = a, ob = b;
        if (m0 != 0) a = lerp(t0, oa, ob);
        if (m1 != 0) b = lerp(t1, ob, oa);
    }
    divide(a);
    divide(b);
    const float dx = b.screen[0] - a.screen[0], dy = b.screen[1] - a.screen[1];
    int px = 0, py = 0;
    if (std::fabs(dx) > std::fabs(dy)) py = dx > 0 ? -1 : 1;
    else px = dy > 0 ? 1 : -1;
    const u32 lp = bp.reg(bp::LINE_PT_WIDTH);
    const float half = static_cast<float>(bits(lp, 0, 8)) / 12.0f;
    const float off = kLinePtTexOffsets[bits(lp, 16, 3)];
    const u32 ngen = std::min<u32>(xf_->word(xf::NUM_TEXGENS) & 0xFU, 8U);
    auto make = [&](const Vtx& src, int sx, int sy, bool tex_off) {
        Vtx d = src;
        d.screen[0] = src.screen[0] + static_cast<float>(sx) * half;
        d.screen[1] = src.screen[1] + static_cast<float>(sy) * half;
        if (tex_off && off != 0.0f)
            for (u32 c = 0; c < ngen; ++c) {
                const u32 s = bp.reg(static_cast<u8>(bp::SU_SSIZE + 2U * c));
                if (bits(s, 18, 1) != 0U) d.tex[c][0] += static_cast<float>(bits(s, 0, 16) + 1U) * off;
            }
        return d;
    };
    const Vtx q0 = make(a, px, py, false), q1 = make(b, px, py, false), q2 = make(b, -px, -py, true);
    if (!raster(q2, q1, q0)) return false;
    return raster(q0, make(a, -px, -py, true), q2);
}

// Points (Clipper.cpp ProcessPoint): not clipped, a square of half size
// BP 22 bits 8-15 / 12 around the vertex, drawn as two triangles.
bool Renderer::point(Vtx c) {
    const BpMemory& bp = *bp_;
    divide(c);
    const u32 lp = bp.reg(bp::LINE_PT_WIDTH);
    const float r = static_cast<float>(bits(lp, 8, 8)) / 12.0f;
    const float off = kLinePtTexOffsets[bits(lp, 19, 3)];
    const u32 ngen = std::min<u32>(xf_->word(xf::NUM_TEXGENS) & 0xFU, 8U);
    auto make = [&](bool px, bool py) {
        Vtx d = c;
        d.screen[0] = c.screen[0] + (px ? 1.0f : -1.0f) * r;
        d.screen[1] = c.screen[1] + (py ? 1.0f : -1.0f) * r;
        if (off != 0.0f)
            for (u32 i = 0; i < ngen; ++i) {
                const u32 s = bp.reg(static_cast<u8>(bp::SU_SSIZE + 2U * i)), t = bp.reg(static_cast<u8>(bp::SU_TSIZE + 2U * i));
                if (bits(s, 19, 1) == 0U) continue;
                if (px) d.tex[i][0] += static_cast<float>(bits(s, 0, 16) + 1U) * off;
                if (py) d.tex[i][1] += static_cast<float>(bits(t, 0, 16) + 1U) * off;
            }
        return d;
    };
    const Vtx ll = make(false, false), lr = make(true, false), ur = make(true, true), ul = make(false, true);
    if (!raster(ll, ul, lr)) return false;
    return raster(ur, lr, ul);
}

bool Renderer::raster(const Vtx& v0, const Vtx& v1, const Vtx& v2) {
    const BpMemory& bp = *bp_;
    const u32 tl = bp.reg(bp::SCISSOR_TL), br = bp.reg(bp::SCISSOR_BR), so = bp.reg(bp::SCISSOR_OFFSET);
    const s32 left = static_cast<s32>(bits(tl, 12, 11)), top = static_cast<s32>(bits(tl, 0, 11));
    const s32 right = static_cast<s32>(bits(br, 12, 11)), bottom = static_cast<s32>(bits(br, 0, 11));
    if (left > right || top > bottom) return true;
    const auto xr = scissor_ranges(left, right, static_cast<s32>(bits(so, 0, 9)) << 1, static_cast<s32>(kEfbWidth));
    const auto yr = scissor_ranges(top, bottom, static_cast<s32>(bits(so, 10, 9)) << 1, static_cast<s32>(kEfbHeight));
    const u32 gen = bp.reg(bp::GENMODE);
    const u32 ncol = bits(gen, 4, 3), ngen = bits(gen, 0, 4);
    const bool zfreeze = bits(gen, 19, 1) != 0U;
    for (const Range& rx : xr) {
        for (const Range& ry : yr) {
            const s32 xo = rx.off, yo = ry.off;
            const s32 X1 = iround(16.0f * (v0.screen[0] - static_cast<float>(xo))) - 9, Y1 = iround(16.0f * (v0.screen[1] - static_cast<float>(yo))) - 9;
            const s32 X2 = iround(16.0f * (v1.screen[0] - static_cast<float>(xo))) - 9, Y2 = iround(16.0f * (v1.screen[1] - static_cast<float>(yo))) - 9;
            const s32 X3 = iround(16.0f * (v2.screen[0] - static_cast<float>(xo))) - 9, Y3 = iround(16.0f * (v2.screen[1] - static_cast<float>(yo))) - 9;
            const s32 DX12 = X1 - X2, DX23 = X2 - X3, DX31 = X3 - X1, DY12 = Y1 - Y2, DY23 = Y2 - Y3, DY31 = Y3 - Y1;
            s32 minx = (std::min({X1, X2, X3}) + 0xF) >> 4, maxx = (std::max({X1, X2, X3}) + 0xF) >> 4;
            s32 miny = (std::min({Y1, Y2, Y3}) + 0xF) >> 4, maxy = (std::max({Y1, Y2, Y3}) + 0xF) >> 4;
            minx = std::max(minx, rx.start); maxx = std::min(maxx, rx.end);
            miny = std::max(miny, ry.start); maxy = std::min(maxy, ry.end);
            const SlopeCtx k = slope_ctx(v0, v1, v2, xo, yo);
            // the z slope is updated before the scissor rejection (z freeze depends on it)
            if (!zfreeze) zslope_ = to_state(Slope(v0.screen[2], v1.screen[2], v2.screen[2], k));
            if (minx >= maxx || miny >= maxy) continue;
            const Slope zs = from_state(zslope_);
            const float w[3] = {1.0f / v0.clip[3], 1.0f / v1.clip[3], 1.0f / v2.clip[3]};
            const Slope ws(w[0], w[1], w[2], k);
            Slope cs[2][4];
            for (unsigned i = 0; i < ncol && i < 2U; ++i)
                for (unsigned c = 0; c < 4; ++c) cs[i][c] = Slope(v0.color[i][c], v1.color[i][c], v2.color[i][c], k);
            Slope ts[8][3];
            for (unsigned i = 0; i < ngen && i < 8U; ++i)
                for (unsigned c = 0; c < 3; ++c) ts[i][c] = Slope(v0.tex[i][c] * w[0], v1.tex[i][c] * w[1], v2.tex[i][c] * w[2], k);
            s32 C1 = DY12 * X1 - DX12 * Y1, C2 = DY23 * X2 - DX23 * Y2, C3 = DY31 * X3 - DX31 * Y3;
            if (DY12 < 0 || (DY12 == 0 && DX12 > 0)) C1++;
            if (DY23 < 0 || (DY23 == 0 && DX23 > 0)) C2++;
            if (DY31 < 0 || (DY31 == 0 && DX31 > 0)) C3++;
            xoff_ = xo; yoff_ = yo;
            const u32 nstages = bits(gen, 10, 4) + 1U, nind = bits(gen, 16, 3);
            for (s32 y = miny & ~1; y < maxy; y += 2) {
                for (s32 x = minx & ~1; x < maxx; x += 2) {
                    bool cov[2][2];
                    bool any = false;
                    for (s32 iy = 0; iy < 2; ++iy)
                        for (s32 ix = 0; ix < 2; ++ix) {
                            const s32 px = x + ix, py = y + iy;
                            const s32 cx1 = C1 + DX12 * (py << 4) - DY12 * (px << 4);
                            const s32 cx2 = C2 + DX23 * (py << 4) - DY23 * (px << 4);
                            const s32 cx3 = C3 + DX31 * (py << 4) - DY31 * (px << 4);
                            cov[iy][ix] = cx1 > 0 && cx2 > 0 && cx3 > 0 && px >= minx && px < maxx && py >= miny && py < maxy;
                            any = any || (cx1 > 0 && cx2 > 0 && cx3 > 0);
                        }
                    if (!any) continue;
                    Uv uv[2][2]{};
                    for (s32 iy = 0; iy < 2; ++iy)
                        for (s32 ix = 0; ix < 2; ++ix) {
                            const s32 px = x + ix, py = y + iy;
                            const float iw = 1.0f / ws.at(px, py);
                            for (unsigned i = 0; i < ngen && i < 8U; ++i) {
                                float proj = iw;
                                const float q = ts[i][2].at(px, py) * iw;
                                if (q != 0.0f) proj = iw / q;
                                uv[iy][ix][i] = {ts[i][0].at(px, py) * proj, ts[i][1].at(px, py) * proj};
                            }
                        }
                    std::array<Lod, 4> ilod{};
                    const u32 iref = bp.reg(bp::IREF);
                    for (unsigned i = 0; i < nind && i < 4U; ++i)
                        ilod[i] = calc_lod(bits(iref, 6U * i, 3), bits(iref, 6U * i + 3U, 3), uv[0][0], uv[0][1], uv[1][0]);
                    std::array<Lod, 16> tlod{};
                    for (unsigned st = 0; st < nstages; ++st) {
                        const u32 order = bp.reg(static_cast<u8>(0x28U + st / 2U));
                        const bool odd = (st & 1U) != 0U;
                        if (bits(order, odd ? 18 : 6, 1) != 0U)
                            tlod[st] = calc_lod(bits(order, odd ? 12 : 0, 3), bits(order, odd ? 15 : 3, 3), uv[0][0], uv[0][1], uv[1][0]);
                    }
                    for (s32 iy = 0; iy < 2; ++iy)
                        for (s32 ix = 0; ix < 2; ++ix) {
                            if (!cov[iy][ix]) continue;
                            const s32 px = x + ix, py = y + iy;
                            std::array<std::array<float, 4>, 2> col{};
                            for (unsigned i = 0; i < ncol && i < 2U; ++i)
                                for (unsigned c = 0; c < 4; ++c) col[i][c] = cs[i][c].at(px, py);
                            if (!pixel(px, py, zs.at(px, py), col, uv[iy][ix], tlod, ilod)) return false;
                        }
                }
            }
        }
    }
    return true;
}

bool Renderer::pixel(s32 x, s32 y, float zf, const std::array<std::array<float, 4>, 2>& colf, const Uv& uvf,
                     const std::array<Lod, 16>& tex_lod, const std::array<Lod, 4>& ind_lod) {
    const BpMemory& bp = *bp_;
    const u32 gen = bp.reg(bp::GENMODE), zmode = bp.reg(bp::ZMODE), zc = bp.reg(bp::ZCOMPARE);
    const u32 fmt = bits(zc, 0, 3);
    const u32 ux = static_cast<u32>(x), uy = static_cast<u32>(y);
    s32 z = static_cast<s32>(std::clamp(zf, 0.0f, 16777215.0f));
    const bool ztest = (zmode & 1U) != 0U, early = ztest && (zc & 0x40U) != 0U, late = ztest && !early;
    auto zcompare = [&](u32 zv) {
        const u32 d = efb_.depth24(ux, uy);
        bool pass = false;
        switch (bits(zmode, 1, 3)) {
        case 0: pass = false; break;
        case 1: pass = zv < d; break;
        case 2: pass = zv == d; break;
        case 3: pass = zv <= d; break;
        case 4: pass = zv > d; break;
        case 5: pass = zv != d; break;
        case 6: pass = zv >= d; break;
        default: pass = true; break;
        }
        if (pass && (zmode & 0x10U) != 0U) efb_.set_depth24(ux, uy, zv & 0xFFFFFFU);
        return pass;
    };
    if (early && !zcompare(static_cast<u32>(z))) return true;

    // ---- TEV ----
    using C4 = std::array<s16, 4>; // R, G, B, A
    std::array<C4, 4> reg = tev_->reg;
    std::array<u8, 4> color[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    const u32 ncol = bits(gen, 4, 3), ngen = bits(gen, 0, 4), nstages = bits(gen, 10, 4) + 1U, nind = bits(gen, 16, 3);
    for (unsigned i = 0; i < ncol && i < 2U; ++i)
        for (unsigned c = 0; c < 4; ++c) color[i][c] = static_cast<u8>(std::clamp(colf[i][c], 0.0f, 255.0f));
    s32 us[8] = {}, vt[8] = {};
    for (unsigned i = 0; i < ngen && i < 8U; ++i) { us[i] = static_cast<s32>(uvf[i][0] * 128.0f); vt[i] = static_cast<s32>(uvf[i][1] * 128.0f); }
    u8 indtex[4][4] = {};
    {
        const u32 iref = bp.reg(bp::IREF);
        for (unsigned i = 0; i < nind && i < 4U; ++i) {
            u32 tc = bits(iref, 6U * i + 3U, 3);
            const u32 map = bits(iref, 6U * i, 3);
            if (tc >= ngen) tc = 0;
            const u32 sc = bp.reg(static_cast<u8>(bp::RAS1_SS0 + i / 2U));
            const u32 ss = bits(sc, (i & 1U) != 0U ? 8 : 0, 4), tsc = bits(sc, (i & 1U) != 0U ? 12 : 4, 4);
            if (!sample(us[tc] >> ss, vt[tc] >> tsc, ind_lod[i], map, indtex[i])) return false;
        }
    }
    s32 tc_s = 0, tc_t = 0;
    u8 alpha_bump = 0;
    C4 tex{}, rawtex{};
    static constexpr s16 kv[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    auto swap_table = [&](u32 id, u32 ch) {
        const u32 rg = bp.reg(static_cast<u8>(bp::TEV_KSEL + 2U * id)), ba = bp.reg(static_cast<u8>(bp::TEV_KSEL + 2U * id + 1U));
        switch (ch) { case 0: return bits(rg, 0, 2); case 1: return bits(rg, 2, 2); case 2: return bits(ba, 0, 2); default: return bits(ba, 2, 2); }
    };
    auto konst_val = [&](u32 sel, unsigned ch) -> s16 { // ch: 0 R 1 G 2 B 3 A
        if (sel < 8U) return kv[sel];
        if (sel < 12U) return 0;
        if (sel < 16U) return ch == 3U ? 0 : tev_->konst[sel - 12U][ch];
        const unsigned which = (sel - 16U) / 4U; // 0 R, 1 G, 2 B, 3 A
        return tev_->konst[(sel - 16U) & 3U][which];
    };
    char eb[96];
    for (unsigned st = 0; st < nstages; ++st) {
        const u32 order = bp.reg(static_cast<u8>(0x28U + st / 2U));
        const bool odd = (st & 1U) != 0U;
        const u32 texmap = bits(order, odd ? 12 : 0, 3);
        u32 tcsel = bits(order, odd ? 15 : 3, 3);
        const bool tex_on = bits(order, odd ? 18 : 6, 1) != 0U;
        const u32 chan = bits(order, odd ? 19 : 7, 3);
        const u32 cc = bp.reg(static_cast<u8>(bp::TEV_COLOR_ENV + 2U * st)), ac = bp.reg(static_cast<u8>(bp::TEV_ALPHA_ENV + 2U * st));
        if (tcsel >= ngen) tcsel = 0;
        // indirect stage (no indirect texture sampled yet: maps read as 0)
        {
            const u32 ind = bp.reg(static_cast<u8>(bp::IND_CMD + st));
            const u32 bt = bits(ind, 0, 2), ifmt = bits(ind, 2, 2), bs = bits(ind, 7, 2), mi = bits(ind, 9, 2), mid = bits(ind, 11, 2);
            const u32 sw = bits(ind, 13, 3), tw = bits(ind, 16, 3);
            if (mid > 2U || sw > 6U || tw > 6U) { std::snprintf(eb, sizeof eb, "invalid indirect stage %u setup %06X", st, ind); err_ = eb; return false; }
            const u8* im = indtex[bt];
            alpha_bump = bs == 0U ? 0 : im[bs == 1U ? 3 : (bs == 2U ? 2 : 1)];
            const s16 bv = ifmt == 0U ? -128 : 1;
            const s32 bias_s = bits(ind, 4, 1) ? bv : 0, bias_t = bits(ind, 5, 1) ? bv : 0, bias_u = bits(ind, 6, 1) ? bv : 0;
            const unsigned sh = ifmt == 0U ? 0 : (ifmt == 1U ? 3 : (ifmt == 2U ? 4 : 5));
            const s32 ic[3] = {(im[3] >> sh) + bias_s, (im[2] >> sh) + bias_t, (im[1] >> sh) + bias_u};
            alpha_bump = ifmt == 0U ? static_cast<u8>(alpha_bump & 0xF8U) : static_cast<u8>(alpha_bump << (ifmt == 1U ? 5 : (ifmt == 2U ? 4 : 3)));
            s32 tr[2] = {0, 0};
            const s32 s_in = us[tcsel], t_in = vt[tcsel];
            if (mi != 0U) {
                const u32 a0 = bp.reg(static_cast<u8>(bp::IND_MTXA + 3U * (mi - 1U))), a1 = bp.reg(static_cast<u8>(bp::IND_MTXA + 3U * (mi - 1U) + 1U)),
                          a2 = bp.reg(static_cast<u8>(bp::IND_MTXA + 3U * (mi - 1U) + 2U));
                const int shift = 17 - static_cast<int>(bits(a0, 22, 2) | (bits(a1, 22, 2) << 2) | (bits(a2, 22, 1) << 4));
                if (mid == 0U) {
                    tr[0] = (sext(bits(a0, 0, 11), 11) * ic[0] + sext(bits(a1, 0, 11), 11) * ic[1] + sext(bits(a2, 0, 11), 11) * ic[2]) >> 3;
                    tr[1] = (sext(bits(a0, 11, 11), 11) * ic[0] + sext(bits(a1, 11, 11), 11) * ic[1] + sext(bits(a2, 11, 11), 11) * ic[2]) >> 3;
                } else if (mid == 1U) { tr[0] = s_in * ic[0] / 256; tr[1] = t_in * ic[0] / 256; }
                else { tr[0] = s_in * ic[1] / 256; tr[1] = t_in * ic[1] / 256; }
                for (s32& v : tr) v = shift >= 0 ? v >> shift : v * (1 << -shift);
            } else if (mid != 0U) { std::snprintf(eb, sizeof eb, "indirect stage %u: matrix id without matrix", st); err_ = eb; return false; }
            auto wrap = [](s32 c, u32 mode) {
                static constexpr s32 m[7] = {0, 256, 128, 64, 32, 16, 0};
                if (mode == 0U) return c;
                if (mode == 6U) return 0;
                return c & ((m[mode] << 7) - 1);
            };
            const s32 ns = wrap(s_in, sw) + tr[0], nt = wrap(t_in, tw) + tr[1];
            if (bits(ind, 20, 1) != 0U) { tc_s += ns; tc_t += nt; } else { tc_s = ns; tc_t = nt; }
        }
        if (tex_on) {
            u8 tx[4] = {0, 0, 0, 0};
            if (ngen > 0U && !sample(tc_s, tc_t, tex_lod[st], texmap, tx)) return false;
            rawtex = {tx[0], tx[1], tx[2], tx[3]};
            const u32 tsw = bits(ac, 2, 2);
            for (unsigned c = 0; c < 4; ++c) tex[c] = tx[swap_table(tsw, c)];
        }
        const u32 kc = bits(bp.reg(static_cast<u8>(bp::TEV_KSEL + st / 2U)), odd ? 14 : 4, 5);
        const u32 ka = bits(bp.reg(static_cast<u8>(bp::TEV_KSEL + st / 2U)), odd ? 19 : 9, 5);
        const C4 kon = {konst_val(kc, 0), konst_val(kc, 1), konst_val(kc, 2), ka >= 8U && ka < 16U ? s16{0} : konst_val(ka, 3)};
        C4 ras{};
        const u32 rsw = bits(ac, 0, 2);
        switch (chan) {
        case 0: case 1: for (unsigned c = 0; c < 4; ++c) ras[c] = color[chan][swap_table(rsw, c)]; break;
        case 5: ras = {alpha_bump, alpha_bump, alpha_bump, alpha_bump}; break;
        case 6: { const s16 n = static_cast<s16>(alpha_bump | (alpha_bump >> 5)); ras = {n, n, n, n}; break; }
        case 7: ras = {0, 0, 0, 0}; break;
        default: std::snprintf(eb, sizeof eb, "TEV stage %u uses invalid colour channel %u", st, chan); err_ = eb; return false;
        }
        auto carg = [&](u32 a, unsigned ch) -> s16 {
            switch (a) {
            case 0: return reg[0][ch]; case 1: return reg[0][3]; case 2: return reg[1][ch]; case 3: return reg[1][3];
            case 4: return reg[2][ch]; case 5: return reg[2][3]; case 6: return reg[3][ch]; case 7: return reg[3][3];
            case 8: return tex[ch]; case 9: return tex[3]; case 10: return ras[ch]; case 11: return ras[3];
            case 12: return 255; case 13: return 128; case 14: return kon[ch]; default: return 0;
            }
        };
        auto aarg = [&](u32 a) -> s16 {
            switch (a) {
            case 0: return reg[0][3]; case 1: return reg[1][3]; case 2: return reg[2][3]; case 3: return reg[3][3];
            case 4: return tex[3]; case 5: return ras[3]; case 6: return kon[3]; default: return 0;
            }
        };
        struct In { s32 a, b, c, d; };
        auto mk = [](s16 a, s16 b, s16 c, s16 d) {
            return In{static_cast<u8>(a), static_cast<u8>(b), static_cast<u8>(c), sext(static_cast<u32>(d) & 0x7FFU, 11)};
        };
        In in[4]; // R G B A
        for (unsigned ch = 0; ch < 3; ++ch)
            in[ch] = mk(carg(bits(cc, 12, 4), ch), carg(bits(cc, 8, 4), ch), carg(bits(cc, 4, 4), ch), carg(bits(cc, 0, 4), ch));
        in[3] = mk(aarg(bits(ac, 13, 3)), aarg(bits(ac, 10, 3)), aarg(bits(ac, 7, 3)), aarg(bits(ac, 4, 3)));
        static constexpr s32 kBias[4] = {0, 128, -128, 0};
        static constexpr unsigned kL[4] = {0, 1, 2, 0}, kR[4] = {0, 0, 0, 1};
        auto cmpval = [&](u32 mode, bool b_side, bool alpha) -> u32 {
            auto g = [&](unsigned ch) { return static_cast<u32>(b_side ? in[ch].b : in[ch].a); };
            switch (mode) {
            case 0: return g(0);
            case 1: return (g(1) << 8) | g(0);
            case 2: return (g(2) << 16) | (g(1) << 8) | g(0);
            default: return alpha ? g(3) : 0U;
            }
        };
        // colour
        const u32 cdest = bits(cc, 22, 2), cbias = bits(cc, 16, 2), cscale = bits(cc, 20, 2);
        const bool csub = bits(cc, 18, 1) != 0U;
        for (unsigned ch = 0; ch < 3; ++ch) {
            const In& r = in[ch];
            s32 res;
            if (cbias != 3U) {
                const s32 c = r.c + (r.c >> 7);
                s32 t = r.a * (256 - c) + r.b * c;
                t <<= kL[cscale];
                t += cscale == 3U ? 0 : (csub ? 127 : 128);
                t >>= 8;
                t = csub ? -t : t;
                res = ((r.d + kBias[cbias]) << kL[cscale]) + t;
                res >>= kR[cscale];
            } else {
                u32 a, b;
                if (cscale == 3U) { a = static_cast<u32>(r.a); b = static_cast<u32>(r.b); }
                else { a = cmpval(cscale, false, false); b = cmpval(cscale, true, false); }
                res = r.d + ((bits(cc, 18, 1) == 0U ? a > b : a == b) ? r.c : 0);
            }
            reg[cdest][ch] = static_cast<s16>(bits(cc, 19, 1) != 0U ? std::clamp(res, 0, 255) : std::clamp(res, -1024, 1023));
        }
        // alpha
        {
            const u32 adest = bits(ac, 22, 2), abias = bits(ac, 16, 2), ascale = bits(ac, 20, 2);
            const bool asub = bits(ac, 18, 1) != 0U;
            const In& r = in[3];
            s32 res;
            if (abias != 3U) {
                const s32 c = r.c + (r.c >> 7);
                s32 t = r.a * (256 - c) + r.b * c;
                t <<= kL[ascale];
                t += ascale == 3U ? 0 : (asub ? 127 : 128);
                t = asub ? (-t >> 8) : (t >> 8);
                res = ((r.d + kBias[abias]) << kL[ascale]) + t;
                res >>= kR[ascale];
            } else {
                const u32 a = cmpval(ascale, false, true), b = cmpval(ascale, true, true);
                res = r.d + ((bits(ac, 18, 1) == 0U ? a > b : a == b) ? r.c : 0);
            }
            reg[adest][3] = static_cast<s16>(bits(ac, 19, 1) != 0U ? std::clamp(res, 0, 255) : std::clamp(res, -1024, 1023));
        }
    }
    const u32 last_cc = bp.reg(static_cast<u8>(bp::TEV_COLOR_ENV + 2U * (nstages - 1U)));
    const u32 last_ac = bp.reg(static_cast<u8>(bp::TEV_ALPHA_ENV + 2U * (nstages - 1U)));
    u8 out[4] = {static_cast<u8>(reg[bits(last_cc, 22, 2)][0]), static_cast<u8>(reg[bits(last_cc, 22, 2)][1]),
                 static_cast<u8>(reg[bits(last_cc, 22, 2)][2]), static_cast<u8>(reg[bits(last_ac, 22, 2)][3])};
    // alpha test
    {
        const u32 at = bp.reg(bp::ALPHA_COMPARE);
        auto cmp = [&](u32 ref, u32 mode) {
            const u32 a = out[3];
            switch (mode) { case 0: return false; case 1: return a < ref; case 2: return a == ref; case 3: return a <= ref;
                            case 4: return a > ref; case 5: return a != ref; case 6: return a >= ref; default: return true; }
        };
        const bool c0 = cmp(bits(at, 0, 8), bits(at, 16, 3)), c1 = cmp(bits(at, 8, 8), bits(at, 19, 3));
        bool pass = true;
        switch (bits(at, 22, 2)) { case 0: pass = c0 && c1; break; case 1: pass = c0 || c1; break; case 2: pass = c0 != c1; break; default: pass = c0 == c1; break; }
        if (!pass) return true;
    }
    // z texture
    {
        const u32 zt2 = bp.reg(bp::ZTEX2);
        const u32 op = bits(zt2, 2, 2);
        if (op != 0U) {
            u32 zt = bits(bp.reg(bp::BIAS), 0, 24);
            switch (bits(zt2, 0, 2)) {
            case 0: zt += static_cast<u32>(static_cast<u8>(rawtex[3])); break;
            case 1: zt += (static_cast<u32>(static_cast<u8>(rawtex[3])) << 8) | static_cast<u8>(rawtex[0]); break;
            case 2: zt += (static_cast<u32>(static_cast<u8>(rawtex[0])) << 16) | (static_cast<u32>(static_cast<u8>(rawtex[1])) << 8) | static_cast<u8>(rawtex[2]); break;
            default: err_ = "invalid z texture format"; return false;
            }
            if (op == 1U) zt += static_cast<u32>(z);
            z = static_cast<s32>(zt & 0xFFFFFFU);
        }
    }
    if (late && !zcompare(static_cast<u32>(z))) return true;

    // ---- blending into the EFB ----
    const u32 bm = bp.reg(bp::BLENDMODE);
    u8 dst[4];
    efb_color_rgba(efb_.color24(ux, uy), fmt, dst);
    u8 res[4] = {out[0], out[1], out[2], out[3]};
    if ((bm & 1U) != 0U) {
        if ((bm & 0x800U) != 0U) {
            for (unsigned i = 0; i < 4; ++i) { const int c = static_cast<int>(dst[i]) - out[i]; res[i] = static_cast<u8>(c < 0 ? 0 : c); }
        } else {
            auto factor = [&](u32 mode, bool is_src, unsigned ch) -> u32 {
                switch (mode) {
                case 0: return 0;
                case 1: return 255;
                case 2: return is_src ? dst[ch] : out[ch];
                case 3: return 255U - (is_src ? dst[ch] : out[ch]);
                case 4: return out[3];
                case 5: return 255U - out[3];
                case 6: return dst[3];
                default: return 255U - dst[3];
                }
            };
            for (unsigned i = 0; i < 4; ++i) {
                u32 sf = factor(bits(bm, 8, 3), true, i), df = factor(bits(bm, 5, 3), false, i);
                sf += sf >> 7;
                df += df >> 7;
                const u32 c = (out[i] * sf + dst[i] * df) >> 8;
                res[i] = static_cast<u8>(c > 255U ? 255U : c);
            }
        }
    } else if ((bm & 2U) != 0U) {
        for (unsigned i = 0; i < 4; ++i) {
            const u32 s = out[i], d = dst[i];
            u32 r = d;
            switch (bits(bm, 12, 4)) {
            case 0: r = 0; break; case 1: r = s & d; break; case 2: r = s & ~d; break; case 3: r = s; break;
            case 4: r = ~s & d; break; case 5: r = d; break; case 6: r = s ^ d; break; case 7: r = s | d; break;
            case 8: r = ~(s | d); break; case 9: r = ~(s ^ d); break; case 10: r = ~d; break; case 11: r = s | ~d; break;
            case 12: r = ~s; break; case 13: r = ~s | d; break; case 14: r = ~(s & d); break; default: r = 0xFF; break;
            }
            res[i] = static_cast<u8>(r);
        }
    }
    const u32 da = bp.reg(bp::CONSTANT_ALPHA);
    if ((da & 0x100U) != 0U) res[3] = static_cast<u8>(da);
    const bool cupd = (bm & 8U) != 0U, aupd = (bm & 0x10U) != 0U;
    u32 cur = efb_.color24(ux, uy);
    if (cupd) {
        if ((bm & 4U) != 0U && fmt == 1U) {
            static constexpr u8 dither[2][2] = {{0, 2}, {3, 1}};
            for (unsigned i = 0; i < 3; ++i) res[i] = static_cast<u8>(((res[i] - (res[i] >> 6)) + dither[uy & 1U][ux & 1U]) & 0xFC);
        }
        if (fmt == 1U) {
            const u32 rgb = (static_cast<u32>(res[0] >> 2) << 18) | (static_cast<u32>(res[1] >> 2) << 12) | (static_cast<u32>(res[2] >> 2) << 6);
            cur = aupd ? (rgb | (res[3] >> 2)) : (rgb | (cur & 0x3FU));
        } else {
            cur = (static_cast<u32>(res[0]) << 16) | (static_cast<u32>(res[1]) << 8) | res[2];
        }
    } else if (aupd && fmt == 1U) {
        cur = (cur & 0xFFFFC0U) | (res[3] >> 2);
    }
    efb_.set_color24(ux, uy, cur);
    ++pixels_;
    return true;
}

namespace {
s32 fixed_log2(float f) {
    u32 x;
    std::memcpy(&x, &f, sizeof x);
    return static_cast<s32>((x & 0x7F800000U) >> 19) - 2032 + static_cast<s32>((x & 0x007FFFFFU) >> 19);
}
u32 tex_reg(const BpMemory& bp, u32 unit, u32 r) {
    return bp.reg(static_cast<u8>(0x80U | (unit & 3U) | (r << 2) | ((unit & 4U) << 3)));
}
void wrap_coord(int& c, u32 mode, int size) {
    switch (mode) {
    case 1: c &= size - 1; break;
    case 2: if ((c & size) != 0) c = ~c; c &= size - 1; break;
    default: c = std::clamp(c, 0, size - 1); break; // 0 clamp, 3 behaves as clamp
    }
}
u32 nibbles(TexFormat f) {
    switch (f) {
    case TexFormat::I4: case TexFormat::C4: case TexFormat::CMPR: return 1;
    case TexFormat::I8: case TexFormat::IA4: case TexFormat::C8: return 2;
    case TexFormat::RGBA8: return 8;
    default: return 4;
    }
}
} // namespace

Renderer::Lod Renderer::calc_lod(u32 texmap, u32 tc, const Uv& p00, const Uv& p10, const Uv& p01) const {
    const u32 m0 = tex_reg(*bp_, texmap, 0), m1 = tex_reg(*bp_, texmap, 1);
    const float dudx = std::fabs(p00[tc][0] - p10[tc][0]), dvdx = std::fabs(p00[tc][1] - p10[tc][1]);
    const float dudy = std::fabs(p00[tc][0] - p01[tc][0]), dvdy = std::fabs(p00[tc][1] - p01[tc][1]);
    float sd, td;
    if (bits(m0, 8, 1) != 0U) { sd = dudx + dudy; td = dvdx + dvdy; }
    else { sd = std::max(dudx, dudy); td = std::max(dvdx, dvdy); }
    s32 lod = fixed_log2(std::max(sd, td));
    lod += sext(bits(m0, 9, 8), 8) >> 1;
    Lod r;
    r.linear = (lod > 0 && bits(m0, 7, 1) != 0U) || (lod <= 0 && bits(m0, 4, 1) != 0U);
    const s32 mx = static_cast<s32>(bits(m1, 8, 8)), mn = static_cast<s32>(bits(m1, 0, 8));
    if (lod > mx) lod = mx;
    else if (lod < mn) lod = mn;
    r.lod = lod;
    return r;
}

bool Renderer::sample(s32 s, s32 t, Lod l, u32 texmap, u8 out[4]) {
    const u32 m0 = tex_reg(*bp_, texmap, 0);
    const u32 mipf = bits(m0, 5, 2);
    s32 base = 0;
    bool mip_linear = false;
    const s32 frac = l.lod & 0xF;
    if (l.lod > 0 && mipf != 0U) {
        base = l.lod >> 4;
        mip_linear = frac != 0 && mipf == 2U;
        if (mipf == 1U && frac >= 8) ++base;
    }
    if (!mip_linear) return sample_mip(s, t, base, l.linear, texmap, out);
    u8 a[4], b[4];
    if (!sample_mip(s, t, base, l.linear, texmap, a) || !sample_mip(s, t, base + 1, l.linear, texmap, b)) return false;
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<u8>((static_cast<u32>(a[i]) * static_cast<u32>(16 - frac) + static_cast<u32>(b[i]) * static_cast<u32>(frac)) >> 4);
    return true;
}

bool Renderer::sample_mip(s32 s, s32 t, s32 mip, bool linear, u32 texmap, u8 out[4]) {
    const BpMemory& bp = *bp_;
    const u32 m0 = tex_reg(bp, texmap, 0), i0 = tex_reg(bp, texmap, 2), i1 = tex_reg(bp, texmap, 3),
              i3 = tex_reg(bp, texmap, 5), tl = tex_reg(bp, texmap, 6);
    const u32 fmt = bits(i0, 20, 4);
    char eb[96];
    if (!tex_format_valid(fmt)) { std::snprintf(eb, sizeof eb, "texture map %u uses invalid format %u", texmap, fmt); err_ = eb; return false; }
    const TexFormat f = static_cast<TexFormat>(fmt);
    const bool manual = bits(i1, 21, 1) != 0U;
    if (manual && f == TexFormat::RGBA8) { err_ = "RGBA8 textures in TMEM (preloaded) are not implemented yet"; return false; }
    int wm1 = static_cast<int>(bits(i0, 0, 10)), hm1 = static_cast<int>(bits(i0, 10, 10));
    // byte offset of the mip level
    u32 offset = 0;
    if (mip != 0) {
        u32 mw = static_cast<u32>(wm1) + 1U, mh = static_cast<u32>(hm1) + 1U;
        wm1 >>= mip; hm1 >>= mip; s >>= mip; t >>= mip;
        for (s32 m = mip; m > 0; --m) {
            mw = std::max(mw, tex_block_width(f));
            mh = std::max(mh, tex_block_height(f));
            offset += (mw * mh * nibbles(f)) >> 1;
            mw >>= 1; mh >>= 1;
        }
    }
    const u32 width = static_cast<u32>(wm1) + 1U, height = static_cast<u32>(hm1) + 1U;
    const u32 bytes = tex_size_bytes(f, width, height);
    std::span<const u8> src;
    if (manual) {
        const std::size_t a = static_cast<std::size_t>(bits(i1, 0, 15)) * 32U + offset;
        if (a + bytes > tex_->tmem.size()) { err_ = "texture in TMEM runs past its end"; return false; }
        src = tex_->tmem.subspan(a, bytes);
    } else {
        const u32 addr = ((bits(i3, 0, 24) << 5) + offset) & tex_->phys_mask;
        const u8* p = tex_->bus->ram_pointer(addr, bytes);
        if (p == nullptr) { std::snprintf(eb, sizeof eb, "texture at %08X (%u bytes) is outside RAM", addr, bytes); err_ = eb; return false; }
        src = std::span<const u8>(p, bytes);
    }
    const std::size_t taddr = static_cast<std::size_t>(bits(tl, 0, 10)) << 9;
    const std::span<const u8> tlut = taddr < tex_->tmem.size() ? tex_->tmem.subspan(taddr) : std::span<const u8>{};
    const TlutFormat tf = static_cast<TlutFormat>(bits(tl, 10, 2));
    if (tex_format_paletted(f) && bits(tl, 10, 2) > 2U) { err_ = "invalid TLUT format 3"; return false; }
    const u32 ws = bits(m0, 0, 2), wt = bits(m0, 2, 2);
    std::string e;
    if (linear) {
        s -= 64; t -= 64;
        int is = s >> 7, it = t >> 7, is1 = is + 1, it1 = it + 1;
        const u32 fs = static_cast<u32>(s & 0x7F), ft = static_cast<u32>(t & 0x7F);
        wrap_coord(is, ws, static_cast<int>(width)); wrap_coord(it, wt, static_cast<int>(height));
        wrap_coord(is1, ws, static_cast<int>(width)); wrap_coord(it1, wt, static_cast<int>(height));
        u8 c[4][4];
        const int xs[4] = {is, is1, is, is1}, ys[4] = {it, it, it1, it1};
        const u32 w[4] = {(128 - fs) * (128 - ft), fs * (128 - ft), (128 - fs) * ft, fs * ft};
        u32 acc[4] = {0, 0, 0, 0};
        for (unsigned k = 0; k < 4; ++k) {
            if (!decode_texel(f, src, static_cast<u32>(xs[k]), static_cast<u32>(ys[k]), width, tlut, tf, c[k], e)) { err_ = "texture: " + e; return false; }
            for (unsigned i = 0; i < 4; ++i) acc[i] += c[k][i] * w[k];
        }
        for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<u8>(acc[i] >> 14);
        return true;
    }
    int is = s >> 7, it = t >> 7;
    wrap_coord(is, ws, static_cast<int>(width)); wrap_coord(it, wt, static_cast<int>(height));
    if (!decode_texel(f, src, static_cast<u32>(is), static_cast<u32>(it), width, tlut, tf, out, e)) { err_ = "texture: " + e; return false; }
    return true;
}

} // namespace orca::gpu::sw
