// Orca Core — GLSL ES 3.20 shaders of the OpenGL ES backend.
//
// The fragment shader is a port of the software renderer's per-pixel stage
// (sw/renderer.cpp: Renderer::pixel, sample, sample_mip, calc_lod) driven by the BP
// registers in a uniform array ("ubershader": one program for every GPU state, so there is
// no shader compilation while a game runs). The EFB colour (24-bit value, same encoding as
// sw::Efb) and depth live in two R32UI attachments read and written with
// EXT_shader_framebuffer_fetch, so the Z test, blending, logic ops, dither and the EFB
// formats use the same integer arithmetic as the software renderer.
#include "gles_shaders.hpp"

namespace orca::gpu::gles {

const char* kVertexShader = R"GLSL(#version 320 es
precision highp float;
layout(location = 0) in vec2 a_pos;   // screen x, y (before the scissor offset)
uniform vec2 u_off;                   // scissor offset of this pass (EFB = screen - offset)
void main() {
    // same snapping as the software rasterizer: round(16 * x) - 9 in 1/16 pixel; the sample of
    // EFB pixel p is then at p + 0.5 in GL window coordinates
    vec2 e = (floor((a_pos - u_off) * 16.0 + 0.5) - 9.0) / 16.0 + 0.5;
    gl_Position = vec4(e / vec2(320.0, 264.0) - 1.0, 0.0, 1.0);
}
)GLSL";

// The backend prepends "#version 320 es" and "#define ORCA_STATS 0|1".
const char* kFragmentShader = R"GLSL(
#extension GL_EXT_shader_framebuffer_fetch : require
precision highp float;
precision highp int;
precision highp usampler2D;
layout(location = 0) inout highp uvec4 o_color;   // .x = 24-bit EFB colour
layout(location = 1) inout highp uvec4 o_depth;   // .x = 24-bit depth
#if ORCA_STATS
// Pixel Engine statistics (see gpu/pixel_stats.hpp): bounding box and quad counters.
// u_quad_ids layer c holds, per 2x2 quad, the id of the last triangle that counted it
// for counter c; imageAtomicExchange returns the previous id, so exactly one pixel of
// each (quad, triangle) pair adds 1.
layout(std430, binding = 0) buffer Stats { uint s_bbox[4]; uint s_quads[5]; };
layout(r32ui, binding = 0) uniform highp uimage2DArray u_quad_ids;
uniform uvec2 u_stats_on;        // x: bounding box armed, y: counters armed
uniform uint u_tri_base;         // id of the first triangle of this draw
void reach(uint c, int x, int y) {
    if (u_stats_on.y == 0u) return;
    uint id = u_tri_base + uint(gl_PrimitiveID);
    if (imageAtomicExchange(u_quad_ids, ivec3(x >> 1, y >> 1, int(c)), id) != id) atomicAdd(s_quads[c], 1u);
}
void widen_bbox(int x, int y) {
    if (u_stats_on.x == 0u) return;
    atomicMin(s_bbox[0], uint(x & ~1)); atomicMax(s_bbox[1], uint(x | 1));
    atomicMin(s_bbox[2], uint(y & ~1)); atomicMax(s_bbox[3], uint(y | 1));
}
#else
void reach(uint c, int x, int y) {}
void widen_bbox(int x, int y) {}
#endif
// Per-triangle attribute slopes (gl_PrimitiveID * 35 texels): 0 = xoff, yoff, x0, y0;
// then (dfdx, dfdy, f0) of z, 1/w, colour 0 RGBA, colour 1 RGBA, and tex * (1/w) for 8 x STQ
uniform highp samplerBuffer u_tri;
uniform uvec4 u_bp[64];          // BP registers 0x00-0xFF
uniform ivec4 u_reg[4];          // TEV registers (R, G, B, A)
uniform ivec4 u_kon[4];          // TEV constants
uniform float u_vp_wd;           // XF viewport width (fog range)
uniform vec4 u_zs0;              // z freeze slope: dfdx, dfdy, f0, xoff
uniform vec4 u_zs1;              // yoff, x0, y0, -
uniform usampler2D u_tex[8];     // decoded texture maps, mip levels stacked vertically
uniform ivec2 u_lvl[8 * 16];     // per map and level: y offset in the atlas, levels decoded

uint R(uint r) { return u_bp[r >> 2u][r & 3u]; }
uint B(uint v, int sh, int n) { return bitfieldExtract(v, sh, n); }
int sext(uint v, int n) { return bitfieldExtract(int(v), 0, n); }
int u8(int v) { return v & 255; }

// ---------------------------------------------------------------- attributes
vec4 g_ctx;
int g_base;
// value of attribute `a` at EFB pixel (x, y), in the software rasterizer's operation order
float attr(int a, int x, int y) {
    vec4 s = texelFetch(u_tri, g_base + 1 + a);
    precise float dx = g_ctx.x + float(x - int(g_ctx.z));
    precise float dy = g_ctx.y + float(y - int(g_ctx.w));
    precise float v = s.z + s.x * dx;
    v = v + s.y * dy;
    return v;
}
const int A_Z = 0, A_W = 1, A_COL = 2, A_TEX = 10;
vec2 tex_uv(int i, int x, int y) {
    float iw = 1.0 / attr(A_W, x, y);
    float proj = iw;
    float q = attr(A_TEX + 3 * i + 2, x, y) * iw;
    if (q != 0.0) proj = iw / q;
    return vec2(attr(A_TEX + 3 * i, x, y) * proj, attr(A_TEX + 3 * i + 1, x, y) * proj);
}

// ---------------------------------------------------------------- textures
uint texreg(uint unit, uint r) { return R(0x80u | (unit & 3u) | (r << 2u) | ((unit & 4u) << 3u)); }

void wrap_coord(inout int c, uint mode, int size) {
    if (mode == 1u) c &= size - 1;
    else if (mode == 2u) { if ((c & size) != 0) c = ~c; c &= size - 1; }
    else c = clamp(c, 0, size - 1);
}

uvec4 fetch(uint map, int x, int y, int mip) {
    ivec2 l = u_lvl[int(map) * 16 + min(mip, 15)];
    return texelFetch(u_tex[map], ivec2(x, y + l.x), 0);
}

uvec4 sample_mip(int s, int t, int mip, bool linear, uint map) {
    uint m0 = texreg(map, 0u), i0 = texreg(map, 2u);
    int wm1 = int(B(i0, 0, 10)) >> mip, hm1 = int(B(i0, 10, 10)) >> mip;
    if (mip != 0) { s >>= mip; t >>= mip; }
    int width = wm1 + 1, height = hm1 + 1;
    uint ws = B(m0, 0, 2), wt = B(m0, 2, 2);
    if (linear) {
        s -= 64; t -= 64;
        int is = s >> 7, it = t >> 7, is1 = is + 1, it1 = it + 1;
        uint fs = uint(s & 0x7F), ft = uint(t & 0x7F);
        wrap_coord(is, ws, width); wrap_coord(it, wt, height);
        wrap_coord(is1, ws, width); wrap_coord(it1, wt, height);
        uvec4 acc = fetch(map, is, it, mip) * ((128u - fs) * (128u - ft)) + fetch(map, is1, it, mip) * (fs * (128u - ft)) +
                    fetch(map, is, it1, mip) * ((128u - fs) * ft) + fetch(map, is1, it1, mip) * (fs * ft);
        return (acc >> 14u) & 255u;
    }
    int is = s >> 7, it = t >> 7;
    wrap_coord(is, ws, width); wrap_coord(it, wt, height);
    return fetch(map, is, it, mip);
}

int fixed_log2(float f) {
    uint x = floatBitsToUint(f);
    return int((x & 0x7F800000u) >> 19) - 2032 + int((x & 0x007FFFFFu) >> 19);
}

ivec2 calc_lod(uint map, vec2 p00, vec2 p10, vec2 p01) {   // (lod, linear)
    uint m0 = texreg(map, 0u), m1 = texreg(map, 1u);
    float dudx = abs(p00.x - p10.x), dvdx = abs(p00.y - p10.y);
    float dudy = abs(p00.x - p01.x), dvdy = abs(p00.y - p01.y);
    float sd, td;
    if (B(m0, 8, 1) != 0u) { sd = dudx + dudy; td = dvdx + dvdy; }
    else { sd = max(dudx, dudy); td = max(dvdx, dvdy); }
    int lod = fixed_log2(max(sd, td));
    lod += sext(B(m0, 9, 8), 8) >> 1;
    bool lin = (lod > 0 && B(m0, 7, 1) != 0u) || (lod <= 0 && B(m0, 4, 1) != 0u);
    int mx = int(B(m1, 8, 8)), mn = int(B(m1, 0, 8));
    if (lod > mx) lod = mx; else if (lod < mn) lod = mn;
    return ivec2(lod, lin ? 1 : 0);
}

uvec4 sample_tex(int s, int t, ivec2 l, uint map) {
    uint mipf = B(texreg(map, 0u), 5, 2);
    int base = 0;
    bool mip_linear = false;
    int frac = l.x & 0xF;
    if (l.x > 0 && mipf != 0u) {
        base = l.x >> 4;
        mip_linear = frac != 0 && mipf == 2u;
        if (mipf == 1u && frac >= 8) ++base;
    }
    if (!mip_linear) return sample_mip(s, t, base, l.y != 0, map);
    uvec4 a = sample_mip(s, t, base, l.y != 0, map), b = sample_mip(s, t, base + 1, l.y != 0, map);
    return (a * uint(16 - frac) + b * uint(frac)) >> 4u;
}

// ---------------------------------------------------------------- TEV helpers
const int kv[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);
uint swap_sel(uint id, uint ch) {
    uint rg = R(0xF6u + 2u * id), ba = R(0xF6u + 2u * id + 1u);
    if (ch == 0u) return B(rg, 0, 2);
    if (ch == 1u) return B(rg, 2, 2);
    if (ch == 2u) return B(ba, 0, 2);
    return B(ba, 2, 2);
}
int konst_val(uint sel, int ch) {
    if (sel < 8u) return kv[sel];
    if (sel < 12u) return 0;
    if (sel < 16u) return ch == 3 ? 0 : u_kon[sel - 12u][ch];
    int which = int(sel - 16u) / 4;
    return u_kon[(sel - 16u) & 3u][which];
}

void main() {
    int x = int(floor(gl_FragCoord.x)), y = int(floor(gl_FragCoord.y));
    uvec4 old_color = o_color, old_depth = o_depth;
    uint gen = R(0x00u), zmode = R(0x40u), zc = R(0x43u);
    uint fmt = B(zc, 0, 3);
    g_base = gl_PrimitiveID * 35;
    g_ctx = texelFetch(u_tri, g_base);
    float zf;
    if (u_zs1.w != 0.0) {   // z freeze: the frozen slope
        precise float zx = u_zs0.w + float(x - int(u_zs1.y));
        precise float zy = u_zs1.x + float(y - int(u_zs1.z));
        precise float zv = u_zs0.z + u_zs0.x * zx;
        zf = zv + u_zs0.y * zy;
    } else {
        zf = attr(A_Z, x, y);
    }
    int z = int(clamp(zf, 0.0, 16777215.0));
    bool ztest = (zmode & 1u) != 0u, early = ztest && (zc & 0x40u) != 0u, late = ztest && !early;
    uint dz = o_depth.x;
    // depth result: written on every exit (a program specialized with no depth
    // write must still store the fetched value back)
    uint depth_val = dz;
    bool zpass = true;
    bool zupd = (zmode & 0x10u) != 0u;

    // early z
    if (early) {
        reach(0u, x, y);
        uint zv = uint(z), d = dz;
        uint f = B(zmode, 1, 3);
        zpass = f == 1u ? zv < d : f == 2u ? zv == d : f == 3u ? zv <= d : f == 4u ? zv > d :
                f == 5u ? zv != d : f == 6u ? zv >= d : f == 7u;
        if (zpass && zupd) depth_val = zv & 0xFFFFFFu;
        if (!zpass) { o_color = old_color; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
        reach(1u, x, y);
    }

    // ---- TEV ----
    ivec4 reg[4] = ivec4[4](u_reg[0], u_reg[1], u_reg[2], u_reg[3]);
    ivec4 color[2] = ivec4[2](ivec4(0), ivec4(0));
    uint ncol = B(gen, 4, 3), ngen = B(gen, 0, 4), nstages = B(gen, 10, 4) + 1u, nind = B(gen, 16, 3);
    for (uint i = 0u; i < ncol && i < 2u; ++i)
        for (int c = 0; c < 4; ++c) color[i][c] = int(clamp(attr(A_COL + 4 * int(i) + c, x, y), 0.0, 255.0));
    // texture coordinates of this pixel and of the 2x2 block's pixels (x, y), (x+1, y), (x, y+1)
    // for the LOD, as the software rasterizer does
    vec2 uv[8], uvb[8], uvx[8], uvy[8];
    int us[8], vt[8];
    int bx = x & ~1, by = y & ~1;
    for (int i = 0; i < 8; ++i) {
        uv[i] = vec2(0.0); uvb[i] = vec2(0.0); uvx[i] = vec2(0.0); uvy[i] = vec2(0.0);
        us[i] = 0; vt[i] = 0;
        if (uint(i) < ngen) {
            uv[i] = tex_uv(i, x, y);
            uvb[i] = tex_uv(i, bx, by);
            uvx[i] = tex_uv(i, bx + 1, by);
            uvy[i] = tex_uv(i, bx, by + 1);
            us[i] = int(uv[i].x * 128.0); vt[i] = int(uv[i].y * 128.0);
        }
    }
    ivec4 indtex[4] = ivec4[4](ivec4(0), ivec4(0), ivec4(0), ivec4(0));
    uint iref = R(0x27u);
    for (uint i = 0u; i < nind && i < 4u; ++i) {
        uint tc = B(iref, int(6u * i + 3u), 3);
        uint map = B(iref, int(6u * i), 3);
        uint lodtc = tc;
        if (tc >= ngen) tc = 0u;
        uint sc = R(0x25u + i / 2u);
        int ss = int(B(sc, (i & 1u) != 0u ? 8 : 0, 4)), tsc = int(B(sc, (i & 1u) != 0u ? 12 : 4, 4));
        ivec2 l = calc_lod(map, uvb[lodtc & 7u], uvx[lodtc & 7u], uvy[lodtc & 7u]);
        indtex[i] = ivec4(sample_tex(us[tc] >> ss, vt[tc] >> tsc, l, map));
    }
    int tc_s = 0, tc_t = 0;
    int alpha_bump = 0;
    ivec4 tex = ivec4(0), rawtex = ivec4(0);
    for (uint st = 0u; st < nstages; ++st) {
        uint order = R(0x28u + st / 2u);
        bool odd = (st & 1u) != 0u;
        uint texmap = B(order, odd ? 12 : 0, 3);
        uint tcsel = B(order, odd ? 15 : 3, 3);
        uint lodtc = tcsel;
        bool tex_on = B(order, odd ? 18 : 6, 1) != 0u;
        uint chan = B(order, odd ? 19 : 7, 3);
        uint cc = R(0xC0u + 2u * st), ac = R(0xC1u + 2u * st);
        if (tcsel >= ngen) tcsel = 0u;
        {
            uint ind = R(0x10u + st);
            uint bt = B(ind, 0, 2), ifmt = B(ind, 2, 2), bs = B(ind, 7, 2), mi = B(ind, 9, 2), mid = B(ind, 11, 2);
            uint sw = B(ind, 13, 3), tw = B(ind, 16, 3);
            if (mid > 2u || sw > 6u || tw > 6u) { o_color = old_color; depth_val = old_depth.x; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
            ivec4 im = indtex[bt];   // R, G, B, A of the indirect map
            alpha_bump = bs == 0u ? 0 : im[bs == 1u ? 3 : (bs == 2u ? 2 : 1)];
            int bv = ifmt == 0u ? -128 : 1;
            int bias_s = B(ind, 4, 1) != 0u ? bv : 0, bias_t = B(ind, 5, 1) != 0u ? bv : 0, bias_u = B(ind, 6, 1) != 0u ? bv : 0;
            int sh = ifmt == 0u ? 0 : (ifmt == 1u ? 3 : (ifmt == 2u ? 4 : 5));
            ivec3 ic = ivec3((im[3] >> sh) + bias_s, (im[2] >> sh) + bias_t, (im[1] >> sh) + bias_u);
            alpha_bump = ifmt == 0u ? (alpha_bump & 0xF8) : u8(alpha_bump << (ifmt == 1u ? 5 : (ifmt == 2u ? 4 : 3)));
            ivec2 tr = ivec2(0);
            int s_in = us[tcsel], t_in = vt[tcsel];
            if (mi != 0u) {
                uint a0 = R(0x06u + 3u * (mi - 1u)), a1 = R(0x07u + 3u * (mi - 1u)), a2 = R(0x08u + 3u * (mi - 1u));
                int shift = 17 - int(B(a0, 22, 2) | (B(a1, 22, 2) << 2) | (B(a2, 22, 1) << 4));
                if (mid == 0u) {
                    tr.x = (sext(B(a0, 0, 11), 11) * ic.x + sext(B(a1, 0, 11), 11) * ic.y + sext(B(a2, 0, 11), 11) * ic.z) >> 3;
                    tr.y = (sext(B(a0, 11, 11), 11) * ic.x + sext(B(a1, 11, 11), 11) * ic.y + sext(B(a2, 11, 11), 11) * ic.z) >> 3;
                } else if (mid == 1u) { tr.x = s_in * ic.x / 256; tr.y = t_in * ic.x / 256; }
                else { tr.x = s_in * ic.y / 256; tr.y = t_in * ic.y / 256; }
                tr = shift >= 0 ? tr >> shift : tr * (1 << -shift);
            } else if (mid != 0u) { o_color = old_color; depth_val = old_depth.x; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
            const int m[7] = int[7](0, 256, 128, 64, 32, 16, 0);
            int ws = sw == 0u ? s_in : (sw == 6u ? 0 : (s_in & ((m[sw] << 7) - 1)));
            int wt = tw == 0u ? t_in : (tw == 6u ? 0 : (t_in & ((m[tw] << 7) - 1)));
            int ns = ws + tr.x, nt = wt + tr.y;
            if (B(ind, 20, 1) != 0u) { tc_s += ns; tc_t += nt; } else { tc_s = ns; tc_t = nt; }
        }
        if (tex_on) {
            ivec4 tx = ivec4(0);
            if (ngen > 0u) tx = ivec4(sample_tex(tc_s, tc_t, calc_lod(texmap, uvb[lodtc & 7u], uvx[lodtc & 7u], uvy[lodtc & 7u]), texmap));
            rawtex = tx;
            uint tsw = B(ac, 2, 2);
            for (uint c = 0u; c < 4u; ++c) tex[c] = tx[swap_sel(tsw, c)];
        }
        uint ks = R(0xF6u + st / 2u);
        uint kc = B(ks, odd ? 14 : 4, 5), ka = B(ks, odd ? 19 : 9, 5);
        ivec4 kon = ivec4(konst_val(kc, 0), konst_val(kc, 1), konst_val(kc, 2), (ka >= 8u && ka < 16u) ? 0 : konst_val(ka, 3));
        ivec4 ras = ivec4(0);
        uint rsw = B(ac, 0, 2);
        if (chan == 0u || chan == 1u) { for (uint c = 0u; c < 4u; ++c) ras[c] = color[chan][swap_sel(rsw, c)]; }
        else if (chan == 5u) ras = ivec4(alpha_bump);
        else if (chan == 6u) ras = ivec4(alpha_bump | (alpha_bump >> 5));
        else if (chan == 7u) ras = ivec4(0);
        else { o_color = old_color; depth_val = old_depth.x; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
        // inputs a, b, c, d per channel (R, G, B, A)
        ivec4 ia, ib, icc, id;
        for (int ch = 0; ch < 3; ++ch) {
            ivec4 sel = ivec4(int(B(cc, 12, 4)), int(B(cc, 8, 4)), int(B(cc, 4, 4)), int(B(cc, 0, 4)));
            ivec4 v;
            for (int k = 0; k < 4; ++k) {
                int a = sel[k], r;
                if (a == 0) r = reg[0][ch]; else if (a == 1) r = reg[0][3]; else if (a == 2) r = reg[1][ch]; else if (a == 3) r = reg[1][3];
                else if (a == 4) r = reg[2][ch]; else if (a == 5) r = reg[2][3]; else if (a == 6) r = reg[3][ch]; else if (a == 7) r = reg[3][3];
                else if (a == 8) r = tex[ch]; else if (a == 9) r = tex[3]; else if (a == 10) r = ras[ch]; else if (a == 11) r = ras[3];
                else if (a == 12) r = 255; else if (a == 13) r = 128; else if (a == 14) r = kon[ch]; else r = 0;
                v[k] = r;
            }
            ia[ch] = u8(v.x); ib[ch] = u8(v.y); icc[ch] = u8(v.z); id[ch] = sext(uint(v.w) & 0x7FFu, 11);
        }
        {
            ivec4 sel = ivec4(int(B(ac, 13, 3)), int(B(ac, 10, 3)), int(B(ac, 7, 3)), int(B(ac, 4, 3)));
            ivec4 v;
            for (int k = 0; k < 4; ++k) {
                int a = sel[k], r;
                if (a == 0) r = reg[0][3]; else if (a == 1) r = reg[1][3]; else if (a == 2) r = reg[2][3]; else if (a == 3) r = reg[3][3];
                else if (a == 4) r = tex[3]; else if (a == 5) r = ras[3]; else if (a == 6) r = kon[3]; else r = 0;
                v[k] = r;
            }
            ia[3] = u8(v.x); ib[3] = u8(v.y); icc[3] = u8(v.z); id[3] = sext(uint(v.w) & 0x7FFu, 11);
        }
        const int kBias[4] = int[4](0, 128, -128, 0);
        const int kL[4] = int[4](0, 1, 2, 0), kR[4] = int[4](0, 0, 0, 1);
        // colour
        uint cdest = B(cc, 22, 2), cbias = B(cc, 16, 2), cscale = B(cc, 20, 2);
        bool csub = B(cc, 18, 1) != 0u;
        for (int ch = 0; ch < 3; ++ch) {
            int res;
            if (cbias != 3u) {
                int c = icc[ch] + (icc[ch] >> 7);
                int t = ia[ch] * (256 - c) + ib[ch] * c;
                t <<= kL[cscale];
                t += cscale == 3u ? 0 : (csub ? 127 : 128);
                t >>= 8;
                t = csub ? -t : t;
                res = ((id[ch] + kBias[cbias]) << kL[cscale]) + t;
                res >>= kR[cscale];
            } else {
                uint a, b;
                if (cscale == 3u) { a = uint(ia[ch]); b = uint(ib[ch]); }
                else if (cscale == 0u) { a = uint(ia.r); b = uint(ib.r); }
                else if (cscale == 1u) { a = (uint(ia.g) << 8) | uint(ia.r); b = (uint(ib.g) << 8) | uint(ib.r); }
                else { a = (uint(ia.b) << 16) | (uint(ia.g) << 8) | uint(ia.r); b = (uint(ib.b) << 16) | (uint(ib.g) << 8) | uint(ib.r); }
                res = id[ch] + ((B(cc, 18, 1) == 0u ? a > b : a == b) ? icc[ch] : 0);
            }
            reg[cdest][ch] = B(cc, 19, 1) != 0u ? clamp(res, 0, 255) : clamp(res, -1024, 1023);
        }
        // alpha
        {
            uint adest = B(ac, 22, 2), abias = B(ac, 16, 2), ascale = B(ac, 20, 2);
            bool asub = B(ac, 18, 1) != 0u;
            int res;
            if (abias != 3u) {
                int c = icc.a + (icc.a >> 7);
                int t = ia.a * (256 - c) + ib.a * c;
                t <<= kL[ascale];
                t += ascale == 3u ? 0 : (asub ? 127 : 128);
                t = asub ? (-t >> 8) : (t >> 8);
                res = ((id.a + kBias[abias]) << kL[ascale]) + t;
                res >>= kR[ascale];
            } else {
                uint a, b;
                if (ascale == 0u) { a = uint(ia.r); b = uint(ib.r); }
                else if (ascale == 1u) { a = (uint(ia.g) << 8) | uint(ia.r); b = (uint(ib.g) << 8) | uint(ib.r); }
                else if (ascale == 2u) { a = (uint(ia.b) << 16) | (uint(ia.g) << 8) | uint(ia.r); b = (uint(ib.b) << 16) | (uint(ib.g) << 8) | uint(ib.r); }
                else { a = uint(ia.a); b = uint(ib.a); }
                res = id.a + ((B(ac, 18, 1) == 0u ? a > b : a == b) ? icc.a : 0);
            }
            reg[adest][3] = B(ac, 19, 1) != 0u ? clamp(res, 0, 255) : clamp(res, -1024, 1023);
        }
    }
    uint last_cc = R(0xC0u + 2u * (nstages - 1u)), last_ac = R(0xC1u + 2u * (nstages - 1u));
    ivec4 outc = ivec4(u8(reg[B(last_cc, 22, 2)][0]), u8(reg[B(last_cc, 22, 2)][1]), u8(reg[B(last_cc, 22, 2)][2]),
                       u8(reg[B(last_ac, 22, 2)][3]));
    // alpha test (a failing pixel keeps the depth written by early z)
    {
        uint at = R(0xF3u);
        int a = outc.a;
        bool c0, c1;
        for (int k = 0; k < 2; ++k) {
            int ref = int(B(at, 8 * k, 8));
            uint mode = B(at, 16 + 3 * k, 3);
            bool r = mode == 1u ? a < ref : mode == 2u ? a == ref : mode == 3u ? a <= ref : mode == 4u ? a > ref :
                     mode == 5u ? a != ref : mode == 6u ? a >= ref : mode == 7u;
            if (k == 0) c0 = r; else c1 = r;
        }
        uint op = B(at, 22, 2);
        bool pass = op == 0u ? (c0 && c1) : op == 1u ? (c0 || c1) : op == 2u ? (c0 != c1) : (c0 == c1);
        if (!pass) { o_color = old_color; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
    }
    // z texture
    {
        uint zt2 = R(0xF5u);
        uint op = B(zt2, 2, 2);
        if (op != 0u) {
            uint zt = B(R(0xF4u), 0, 24);
            uint ty = B(zt2, 0, 2);
            if (ty == 0u) zt += uint(u8(rawtex[3]));
            else if (ty == 1u) zt += (uint(u8(rawtex[3])) << 8) | uint(u8(rawtex[0]));
            else if (ty == 2u) zt += (uint(u8(rawtex[0])) << 16) | (uint(u8(rawtex[1])) << 8) | uint(u8(rawtex[2]));
            else { o_color = old_color; depth_val = old_depth.x; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
            if (op == 1u) zt += uint(z);
            z = int(zt & 0xFFFFFFu);
        }
    }
    // fog
    {
        uint p3 = R(0xF1u), p0 = R(0xEEu);
        uint fsel = B(p3, 21, 3);
        if (fsel != 0u) {
            float fa = uintBitsToFloat((B(p0, 19, 1) << 31) | (B(p0, 11, 8) << 23) | (B(p0, 0, 11) << 12));
            float fc = uintBitsToFloat((B(p3, 19, 1) << 31) | (B(p3, 11, 8) << 23) | (B(p3, 0, 11) << 12));
            if (B(p0, 11, 8) == 255u && B(p3, 11, 8) == 255u) {
                fa = 0.0;
                fc = (B(p0, 19, 1) == 0u && B(p3, 19, 1) == 0u) ? uintBitsToFloat(0xFF800000u) : uintBitsToFloat(0x7F800000u);
            }
            float ze;
            if (B(p3, 20, 1) == 0u) {
                uint shift = R(0xF0u);
                int denom = int(R(0xEFu)) - (z >> int(min(shift, 31u)));
                ze = (fa * 16777215.0) / float(denom);
            } else {
                ze = fa * (float(z) / 16777215.0);
            }
            uint range = R(0xE8u);
            if (B(range, 10, 1) != 0u) {
                float offset = float(x - (int(B(range, 0, 10)) - 342)) / u_vp_wd;
                float fi = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
                int lo = int(fi), hi = lo + 1;
                uint rl = R(0xE9u + uint(lo / 2)), rh = R(0xE9u + uint(hi / 2));
                float kl = float((lo % 2) != 0 ? B(rl, 0, 12) : B(rl, 12, 12)) / 256.0 * 4.0;
                float kh = float((hi % 2) != 0 ? B(rh, 0, 12) : B(rh, 12, 12)) / 256.0 * 4.0;
                float factor = float(hi) - fi;
                float k = kl * factor + kh * (1.0 - factor);
                ze *= sqrt(offset * offset + k * k) / k;
            }
            ze -= fc;
            float fog = clamp(ze, 0.0, 1.0);
            if (fsel == 4u) fog = 1.0 - exp2(-8.0 * fog);
            else if (fsel == 5u) fog = 1.0 - exp2(-8.0 * fog * fog);
            else if (fsel == 6u) { fog = 1.0 - fog; fog = exp2(-8.0 * fog); }
            else if (fsel == 7u) { fog = 1.0 - fog; fog = exp2(-8.0 * fog * fog); }
            uint fi8 = uint(fog * 256.0), inv = 256u - fi8;
            uint fcol = R(0xF2u);
            outc.r = int((uint(outc.r) * inv + fi8 * B(fcol, 16, 8)) >> 8);
            outc.g = int((uint(outc.g) * inv + fi8 * B(fcol, 8, 8)) >> 8);
            outc.b = int((uint(outc.b) * inv + fi8 * B(fcol, 0, 8)) >> 8);
        }
    }
    // late z
    if (late) {
        reach(2u, x, y);
        uint zv = uint(z), d = dz;
        uint f = B(zmode, 1, 3);
        bool pass = f == 1u ? zv < d : f == 2u ? zv == d : f == 3u ? zv <= d : f == 4u ? zv > d :
                    f == 5u ? zv != d : f == 6u ? zv >= d : f == 7u;
        if (pass && zupd) depth_val = zv & 0xFFFFFFu;
        if (!pass) { o_color = old_color; o_depth = uvec4(depth_val, 0u, 0u, 0u); return; }
        reach(3u, x, y);
    }
    reach(4u, x, y);
    widen_bbox(x, y);
    // ---- blending into the EFB ----
    uint bm = R(0x41u);
    uint cur = old_color.x;
    ivec4 dst;
    if (fmt == 1u) {
        uvec4 d6 = uvec4(B(cur, 18, 6), B(cur, 12, 6), B(cur, 6, 6), B(cur, 0, 6));
        dst = ivec4((d6 << 2) | (d6 >> 4));
    } else {
        dst = ivec4(int(B(cur, 16, 8)), int(B(cur, 8, 8)), int(B(cur, 0, 8)), 255);
    }
    ivec4 res = outc;
    if ((bm & 1u) != 0u) {
        if ((bm & 0x800u) != 0u) {
            res = max(dst - outc, ivec4(0));
        } else {
            for (int i = 0; i < 4; ++i) {
                uint fsrc, fdst;
                for (int k = 0; k < 2; ++k) {
                    uint mode = k == 0 ? B(bm, 8, 3) : B(bm, 5, 3);
                    bool is_src = k == 0;
                    uint f;
                    if (mode == 0u) f = 0u;
                    else if (mode == 1u) f = 255u;
                    else if (mode == 2u) f = uint(is_src ? dst[i] : outc[i]);
                    else if (mode == 3u) f = 255u - uint(is_src ? dst[i] : outc[i]);
                    else if (mode == 4u) f = uint(outc[3]);
                    else if (mode == 5u) f = 255u - uint(outc[3]);
                    else if (mode == 6u) f = uint(dst[3]);
                    else f = 255u - uint(dst[3]);
                    f += f >> 7;
                    if (k == 0) fsrc = f; else fdst = f;
                }
                uint c = (uint(outc[i]) * fsrc + uint(dst[i]) * fdst) >> 8;
                res[i] = int(min(c, 255u));
            }
        }
    } else if ((bm & 2u) != 0u) {
        uint op = B(bm, 12, 4);
        for (int i = 0; i < 4; ++i) {
            uint s = uint(outc[i]), d = uint(dst[i]), r;
            if (op == 0u) r = 0u; else if (op == 1u) r = s & d; else if (op == 2u) r = s & ~d; else if (op == 3u) r = s;
            else if (op == 4u) r = ~s & d; else if (op == 5u) r = d; else if (op == 6u) r = s ^ d; else if (op == 7u) r = s | d;
            else if (op == 8u) r = ~(s | d); else if (op == 9u) r = ~(s ^ d); else if (op == 10u) r = ~d; else if (op == 11u) r = s | ~d;
            else if (op == 12u) r = ~s; else if (op == 13u) r = ~s | d; else if (op == 14u) r = ~(s & d); else r = 0xFFu;
            res[i] = int(r & 255u);
        }
    }
    uint da = R(0x42u);
    if ((da & 0x100u) != 0u) res[3] = int(da & 255u);
    bool cupd = (bm & 8u) != 0u, aupd = (bm & 0x10u) != 0u;
    if (cupd) {
        if ((bm & 4u) != 0u && fmt == 1u) {
            const int dither[4] = int[4](0, 2, 3, 1);
            int dv = dither[(y & 1) * 2 + (x & 1)];
            for (int i = 0; i < 3; ++i) res[i] = ((res[i] - (res[i] >> 6)) + dv) & 0xFC;
        }
        if (fmt == 1u) {
            uint rgb = (uint(res[0] >> 2) << 18) | (uint(res[1] >> 2) << 12) | (uint(res[2] >> 2) << 6);
            cur = aupd ? (rgb | uint(res[3] >> 2)) : (rgb | (cur & 0x3Fu));
        } else {
            cur = (uint(res[0]) << 16) | (uint(res[1]) << 8) | uint(res[2]);
        }
    } else if (aupd && fmt == 1u) {
        cur = (cur & 0xFFFFC0u) | uint(res[3] >> 2);
    }
    o_color = uvec4(cur, 0u, 0u, 0u);
    o_depth = uvec4(depth_val, 0u, 0u, 0u);
}
)GLSL";

} // namespace orca::gpu::gles
