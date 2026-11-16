// Software GPU reference (M014): vertex decoding and XF transform. Expected
// values worked out by hand from the rules in vertex.hpp.
#include "orca/gpu/sw/vertex.hpp"
#include "orca/gpu/gpu.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "test_framework.hpp"

#include <array>
#include <bit>
#include <functional>
#include <cmath>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::gpu;
using namespace orca::gpu::sw;

namespace {

void xf_set(XfMemory& xf, u32 addr, std::initializer_list<u32> words) {
    std::vector<u8> b;
    for (u32 w : words) { b.push_back(static_cast<u8>(w >> 24)); b.push_back(static_cast<u8>(w >> 16)); b.push_back(static_cast<u8>(w >> 8)); b.push_back(static_cast<u8>(w)); }
    std::string e;
    CHECK(xf.load(addr, b, e));
}
void xf_f(XfMemory& xf, u32 addr, std::initializer_list<float> v) {
    for (float f : v) xf_set(xf, addr++, {std::bit_cast<u32>(f)});
}
bool near(float a, float b) { return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::fabs(b)); }

struct Rig {
    mem::Bus bus;
    CpState cp;
    XfMemory xf;
    VertexDecoder dec;
    Rig() { CHECK(bus.map_ram("MEM1", 0, 0x100000U)); }
    bool run(unsigned vat, const std::vector<u8>& d, InputVertex& v, std::string& e) {
        return dec.decode(cp, xf, bus, 0x03FFFFFFU, vat, d, v, e);
    }
    InputVertex ok(unsigned vat, const std::vector<u8>& d) {
        InputVertex v;
        std::string e;
        CHECK(run(vat, d, v, e));
        if (!e.empty()) std::printf("    decode error: %s\n", e.c_str());
        return v;
    }
};

} // namespace

TEST_CASE(sw_decode_positions) {
    Rig t;
    // direct s16 xyz, frac 4: 16, -32, 48 -> 1, -2, 3
    t.cp.vcd_lo = vcf::DIRECT << 9;
    t.cp.vat[0][0] = 1U | (3U << 1) | (4U << 4);
    InputVertex v = t.ok(0, {0x00, 0x10, 0xFF, 0xE0, 0x00, 0x30});
    CHECK(v.pos.x == 1.0f); CHECK(v.pos.y == -2.0f); CHECK(v.pos.z == 3.0f);
    // u8 xy: ByteDequant clear -> frac ignored; set -> applied; z = 0
    t.cp.vat[1][0] = 0U | (0U << 1) | (4U << 4);
    v = t.ok(1, {16, 32});
    CHECK(v.pos.x == 16.0f); CHECK(v.pos.y == 32.0f); CHECK(v.pos.z == 0.0f);
    t.cp.vat[1][0] |= 1U << 30;
    v = t.ok(1, {16, 32});
    CHECK(v.pos.x == 1.0f); CHECK(v.pos.y == 2.0f);
    // floats ignore frac
    t.cp.vat[2][0] = 1U | (4U << 1) | (31U << 4);
    v = t.ok(2, {0x3F, 0xC0, 0, 0, 0xC0, 0x20, 0, 0, 0x42, 0xC8, 0, 0});
    CHECK(v.pos.x == 1.5f); CHECK(v.pos.y == -2.5f); CHECK(v.pos.z == 100.0f);
    // indexed from array 0: base 0x1000, stride 12, index 2
    const std::vector<u8> arr = {0x40, 0x00, 0, 0, 0x40, 0x40, 0, 0, 0x40, 0x80, 0, 0}; // 2, 3, 4
    CHECK(t.bus.dma_write(0x1000 + 24, arr) == mem::BusStatus::Ok);
    t.cp.array_base[0] = 0x80001000U; // a virtual-looking base is masked to physical
    t.cp.array_stride[0] = 12;
    t.cp.vcd_lo = vcf::INDEX8 << 9;
    v = t.ok(2, {2});
    CHECK(v.pos.x == 2.0f); CHECK(v.pos.z == 4.0f); CHECK(!v.skip);
    v = t.ok(2, {0xFF});
    CHECK(v.skip);
    t.cp.vcd_lo = vcf::INDEX16 << 9;
    v = t.ok(2, {0x00, 0xFF});
    CHECK(!v.skip);
    v = t.ok(2, {0xFF, 0xFF});
    CHECK(v.skip);
    // index outside RAM and wrong vertex size are errors
    InputVertex w;
    std::string e;
    t.cp.array_base[0] = 0x00FFFFF0U;
    CHECK(!t.run(2, {0x00, 0x05}, w, e));
    CHECK(e.find("outside RAM") != std::string::npos);
    t.cp.vcd_lo = vcf::DIRECT << 9;
    CHECK(!t.run(0, {0, 0, 0, 0, 0, 0, 0}, w, e));
    CHECK(e.find("6 of 7") != std::string::npos);
}

TEST_CASE(sw_decode_matrix_indices) {
    Rig t;
    // defaults from XF 0x1018/0x1019
    xf_set(t.xf, xf::MATRIX_INDEX_A, {5U | (6U << 6) | (7U << 12) | (8U << 18) | (9U << 24)});
    xf_set(t.xf, xf::MATRIX_INDEX_B, {10U | (11U << 6) | (12U << 12) | (13U << 18)});
    t.cp.vcd_lo = vcf::DIRECT << 9;
    t.cp.vat[0][0] = 0U | (1U << 1); // xy s8
    InputVertex v = t.ok(0, {1, 2});
    CHECK_EQ(v.pos_mtx, 5U); CHECK_EQ(v.tex_mtx[0], 6U); CHECK_EQ(v.tex_mtx[3], 9U);
    CHECK_EQ(v.tex_mtx[4], 10U); CHECK_EQ(v.tex_mtx[7], 13U);
    // per-vertex bytes: position index, then tex 1 and tex 7
    t.cp.vcd_lo = 1U | (1U << 2) | (1U << 8) | (vcf::DIRECT << 9);
    v = t.ok(0, {33, 40, 63, 1, 2});
    CHECK_EQ(v.pos_mtx, 33U); CHECK_EQ(v.tex_mtx[1], 40U); CHECK_EQ(v.tex_mtx[7], 63U); CHECK_EQ(v.tex_mtx[0], 6U);
    InputVertex w;
    std::string e;
    CHECK(!t.run(0, {64, 40, 63, 1, 2}, w, e));
    CHECK(e.find("above 63") != std::string::npos);
}

TEST_CASE(sw_decode_normals) {
    Rig t;
    t.cp.vcd_lo = (vcf::DIRECT << 9) | (vcf::DIRECT << 11);
    // position xy u8, normal NBT s8: 64 -> 1, -64 -> -1, 32 -> 0.5
    t.cp.vat[0][0] = (1U << 9) | (1U << 10);
    InputVertex v = t.ok(0, {0, 0, 64, 0xC0, 32, 0, 0, 64, 32, 32, 32});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[0].y == -1.0f); CHECK(v.normal[0].z == 0.5f);
    CHECK(v.normal[1].z == 1.0f); CHECK(v.normal[2].x == 0.5f);
    // single normal: u8 /128, u16 /32768, s16 /16384
    t.cp.vat[1][0] = 0U << 10;
    v = t.ok(1, {0, 0, 128, 64, 0});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[0].y == 0.5f);
    t.cp.vat[1][0] = 2U << 10;
    v = t.ok(1, {0, 0, 0x80, 0x00, 0x40, 0x00, 0, 0});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[0].y == 0.5f);
    t.cp.vat[1][0] = 3U << 10;
    v = t.ok(1, {0, 0, 0x40, 0x00, 0xC0, 0x00, 0, 0});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[0].y == -1.0f);
    // no normal in the format: the last loaded normal is kept
    t.cp.vcd_lo = vcf::DIRECT << 9;
    v = t.ok(1, {0, 0});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[0].y == -1.0f);
    // NBT with three 8-bit indices into array 1 (s16, base 0x2000, stride 18):
    // vector k of index i is at base + i*stride + k*6
    t.cp.vcd_lo = (vcf::DIRECT << 9) | (vcf::INDEX8 << 11);
    t.cp.vat[2][0] = (1U << 9) | (3U << 10) | (1U << 31);
    t.cp.array_base[1] = 0x2000;
    t.cp.array_stride[1] = 18;
    std::vector<u8> nrm(18 * 4, 0);
    nrm[1 * 18 + 0] = 0x40;      // index 1, normal.x = 1
    nrm[2 * 18 + 6 + 2] = 0x20;  // index 2, tangent.y = 0.5
    nrm[3 * 18 + 12 + 4] = 0xC0; // index 3, binormal.z = -1
    CHECK(t.bus.dma_write(0x2000, nrm) == mem::BusStatus::Ok);
    v = t.ok(2, {0, 0, 1, 2, 3});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[1].y == 0.5f); CHECK(v.normal[2].z == -1.0f);
    // without index3 one index fetches all nine values
    t.cp.vat[2][0] &= ~(1U << 31);
    nrm[1 * 18 + 6 + 2] = 0x40;
    CHECK(t.bus.dma_write(0x2000, nrm) == mem::BusStatus::Ok);
    v = t.ok(2, {0, 0, 1});
    CHECK(v.normal[0].x == 1.0f); CHECK(v.normal[1].y == 1.0f);
}

TEST_CASE(sw_decode_colors_and_texcoords) {
    Rig t;
    t.cp.vcd_lo = (vcf::DIRECT << 9) | (vcf::DIRECT << 13) | (vcf::DIRECT << 15);
    auto both = [&](u32 f0, u32 f1, const std::vector<u8>& d) {
        t.cp.vat[0][0] = (f0 << 14) | (f1 << 18);
        std::vector<u8> all = {0, 0};
        all.insert(all.end(), d.begin(), d.end());
        return t.ok(0, all);
    };
    InputVertex v = both(0, 3, {0xF8, 0x00, 0x12, 0x34});          // RGB565 red, RGBA4444
    CHECK_EQ(v.color[0], 0xFF0000FFU); CHECK_EQ(v.color[1], 0x11223344U);
    v = both(1, 2, {1, 2, 3, 4, 5, 6, 7});                           // RGB888, RGB888x
    CHECK_EQ(v.color[0], 0x010203FFU); CHECK_EQ(v.color[1], 0x040506FFU);
    v = both(4, 5, {0xFC, 0x00, 0x3F, 9, 8, 7, 6});                  // RGBA6666 (R 63, A 63), RGBA8888
    CHECK_EQ(v.color[0], 0xFF0000FFU); CHECK_EQ(v.color[1], 0x09080706U);
    v = both(0, 0, {0x84, 0x10, 0x07, 0xE0});                        // 565 replicate: 16 -> 0x84, 32 -> 0x82
    CHECK_EQ(v.color[0], 0x848284FFU); CHECK_EQ(v.color[1], 0x00FF00FFU);
    // only colour 1 present: it feeds channel 0; channel 1 is white
    t.cp.vcd_lo = (vcf::DIRECT << 9) | (vcf::DIRECT << 15);
    t.cp.vat[0][0] = 5U << 18;
    v = t.ok(0, {0, 0, 1, 2, 3, 4});
    CHECK_EQ(v.color[0], 0x01020304U); CHECK_EQ(v.color[1], 0xFFFFFFFFU);
    // texture coordinates: tex0 st s16 frac 8; tex1 s u8 (no dequant); tex5 st float
    t.cp.vcd_lo = vcf::DIRECT << 9;
    t.cp.vcd_hi = (vcf::DIRECT << 0) | (vcf::DIRECT << 2) | (vcf::DIRECT << 10);
    t.cp.vat[3][0] = (1U << 21) | (3U << 22) | (8U << 25);
    t.cp.vat[3][1] = (0U << 0) | (0U << 1) | (3U << 4);
    t.cp.vat[3][2] = (1U << 5) | (4U << 6);
    v = t.ok(3, {0, 0, 0x01, 0x00, 0xFF, 0x80, 200, 0x3F, 0x80, 0, 0, 0x40, 0, 0, 0});
    CHECK(v.tex[0][0] == 1.0f); CHECK(v.tex[0][1] == -0.5f);
    CHECK(v.tex[1][0] == 200.0f); CHECK(v.tex[1][1] == 0.0f);
    CHECK(v.tex[5][0] == 1.0f); CHECK(v.tex[5][1] == 2.0f);
}

namespace {
TexScale unit_scale() {
    TexScale s{};
    for (auto& p : s) p = {1.0f, 1.0f};
    return s;
}
void identity(XfMemory& xf, u32 base) { xf_f(xf, base, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}); }
} // namespace

TEST_CASE(sw_transform_position_and_projection) {
    XfMemory xf;
    xf_f(xf, 4, {2, 0, 0, 10, 0, 3, 0, 20, 0, 0, 4, 30}); // matrix 1: scale + translate
    InputVertex in;
    in.pos = {1, 2, 3};
    in.pos_mtx = 1;
    OutputVertex o;
    // orthographic: x' = p0*x + p1, ...
    xf_f(xf, xf::PROJECTION, {0.5f, 1, 0.25f, 2, 0.1f, 3});
    xf_set(xf, xf::PROJECTION + 6, {1});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK(o.mv_pos.x == 12.0f); CHECK(o.mv_pos.y == 26.0f); CHECK(o.mv_pos.z == 42.0f);
    CHECK(near(o.clip[0], 7.0f)); CHECK(near(o.clip[1], 8.5f)); CHECK(near(o.clip[2], 7.2f)); CHECK(o.clip[3] == 1.0f);
    // perspective: x' = p0*x + p1*z, y' = p2*y + p3*z, z' = (p4*z + p5)(1 - 1e-7), w = -z
    xf_set(xf, xf::PROJECTION + 6, {0});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK(near(o.clip[0], 0.5f * 12 + 1 * 42)); CHECK(near(o.clip[1], 0.25f * 26 + 2 * 42));
    CHECK(near(o.clip[2], (0.1f * 42 + 3) * (1.0f - 1e-7f))); CHECK(o.clip[3] == -42.0f);
    CHECK(o.clip[2] < 0.1f * 42 + 3); // the (1 - 1e-7) factor is applied (7.2 is 1.5 ulp lower)
}

TEST_CASE(sw_transform_normals_and_lighting) {
    XfMemory xf;
    identity(xf, 0);
    xf_f(xf, xf::NORMAL_MATRICES, {2, 0, 0, 0, 2, 0, 0, 0, 2}); // scaled: normal[0] renormalised
    InputVertex in;
    in.normal[0] = {0, 0, 1};
    in.normal[1] = {1, 0, 0};
    in.color = {0x204060FFU, 0x80808080U};
    OutputVertex o;
    transform_vertex(xf, in, unit_scale(), o);
    CHECK(o.normal[0].z == 1.0f); CHECK(o.normal[1].x == 2.0f);
    // lighting off, material from the vertex
    xf_set(xf, xf::CHAN0_COLOR, {1}); xf_set(xf, xf::CHAN0_ALPHA, {1});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0], 0x204060FFU);
    // material register, lighting off
    xf_set(xf, xf::MAT_COLOR0, {0x11223344U});
    xf_set(xf, xf::CHAN0_COLOR, {0}); xf_set(xf, xf::CHAN0_ALPHA, {0});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0], 0x11223344U);
    // light 0 at (0,0,10) colour (128,64,0,200); ambient (32,0,0,10); material white;
    // no attenuation, clamped diffuse: ldir = (0,0,1), dot = 1
    xf_set(xf, xf::MAT_COLOR0, {0xFFFFFFFFU});
    xf_set(xf, xf::AMB_COLOR0, {0x2000000AU});
    xf_set(xf, xf::LIGHTS + 3, {0x804000C8U});
    xf_f(xf, xf::LIGHTS + 10, {0, 0, 10});
    const u32 lit = 2U | (1U << 2) | (2U << 7) | (2U << 9); // enable, light 0, clamp, dir attenuation
    xf_set(xf, xf::CHAN0_COLOR, {lit}); xf_set(xf, xf::CHAN0_ALPHA, {lit});
    transform_vertex(xf, in, unit_scale(), o);
    // R: 32+128 = 160 -> 255*161>>8 = 160; G: 64 -> 255*64>>8 = 63; B: 0; A: 10+200 = 210 -> 255*211>>8 = 210
    CHECK_EQ(o.color[0], (160U << 24) | (63U << 16) | (0U << 8) | 210U);
    // the light behind the surface: clamp gives 0, sign subtracts
    xf_f(xf, xf::LIGHTS + 10, {0, 0, -10});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 31U); // 255*(32+0)>>8 = 31
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 2) | (1U << 7) | (2U << 9)});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 0U); // 32 - 128 < 0 -> clamped to 0 before the multiply
    // spot light: pos (0,0,2), dir (0,0,-1), cos (1,0,0), dist (1,0,0.25):
    // attn = max(0, dot((0,0,1),(0,0,-1))) = 0 -> cos 1, dist 1 + 0.25*4 = 2 -> 0.5
    xf_set(xf, xf::AMB_COLOR0, {0});
    xf_set(xf, xf::LIGHTS + 3, {0xC8000000U});
    xf_f(xf, xf::LIGHTS + 4, {1, 0, 0, 1, 0, 0.25f, 0, 0, 2, 0, 0, -1});
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 2) | (2U << 7) | (3U << 9)});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 99U); // 200*0.5 = 100 -> 255*100>>8 = 99
    // specular: half-angle (0,0,1), cos (0,0,1): attn = 1; dist (2,0,0) normalised -> (1,0,0) because diffuse != none
    xf_f(xf, xf::LIGHTS + 4, {0, 0, 1, 2, 0, 0, 0, 0, 10, 0, 0, 1});
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 2) | (2U << 7) | (1U << 9)});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 200U); // 200*1*dot 1 -> 255*(200+1)>>8 = 200
    // with diffuse "none" the distance attenuation is not normalised: 1/2
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 2) | (0U << 7) | (1U << 9)});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 99U);
    // lights 4-7 are enabled by bits 11-14: light 4 at (0,0,10) colour R 100, no attenuation
    xf_set(xf, xf::LIGHTS + 4U * 16U + 3U, {0x64000000U});
    xf_f(xf, xf::LIGHTS + 4U * 16U + 10U, {0, 0, 10});
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 11) | (2U << 7) | (2U << 9)});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 99U); // 255*100>>8
    // ambient from the vertex
    xf_set(xf, xf::CHAN0_COLOR, {2U | (1U << 6)}); // lighting on, no lights, ambient = vertex colour
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[0] >> 24, 31U); // ambient R 0x20 from the vertex, material 255: 255*32>>8 = 31
    // second channel is independent
    xf_set(xf, xf::CHAN0_COLOR + 1, {1}); xf_set(xf, xf::CHAN0_ALPHA + 1, {1});
    transform_vertex(xf, in, unit_scale(), o);
    CHECK_EQ(o.color[1], 0x80808080U);
}

TEST_CASE(sw_transform_emboss_and_binormals) {
    XfMemory xf;
    identity(xf, 0);
    xf_f(xf, xf::NORMAL_MATRICES, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    InputVertex in;
    in.pos = {4, 5, 6};
    in.normal[0] = {0, 0, 1};
    in.normal[1] = {1, 0, 0}; // tangent
    in.normal[2] = {0, 1, 0}; // binormal
    in.tex[0] = {0.25f, 0.5f};
    xf_set(xf, xf::NUM_TEXGENS, {4});
    xf_set(xf, xf::TEX_MTX_INFO, {5U << 7});                           // regular from tex0 (identity matrix 0)
    xf_set(xf, xf::TEX_MTX_INFO + 1, {(1U << 4) | (0U << 12) | (2U << 15)}); // emboss from texgen 0, light 2
    xf_set(xf, xf::TEX_MTX_INFO + 2, {3U << 7});                       // regular from row 3 (tangent)
    xf_set(xf, xf::TEX_MTX_INFO + 3, {4U << 7});                       // regular from row 4 (binormal)
    xf_f(xf, xf::LIGHTS + 2U * 16U + 10U, {7, 9, 6});                  // light 2: direction (3,4,0)/5
    OutputVertex o;
    transform_vertex(xf, in, unit_scale(), o);
    CHECK(near(o.tex[1].x, 0.25f + 0.6f)); CHECK(near(o.tex[1].y, 0.5f + 0.8f)); CHECK(o.tex[1].z == 1.0f);
    CHECK(o.tex[2].x == 1.0f); CHECK(o.tex[2].y == 0.0f);
    CHECK(o.tex[3].x == 0.0f); CHECK(o.tex[3].y == 1.0f);
}

TEST_CASE(sw_transform_texgen) {
    XfMemory xf;
    identity(xf, 0);
    // texture matrix 30 (words 120..131): s' = 2s + 1, t' = 3t + 0.5, q row (0,0,1,0)
    xf_f(xf, 120, {2, 0, 0, 1, 0, 3, 0, 0.5f, 0, 0, 1, 0});
    InputVertex in;
    in.tex[0] = {0.25f, 0.5f};
    in.tex_mtx[0] = 30;
    in.color = {0x80400000U, 0};
    xf_set(xf, xf::CHAN0_COLOR, {1}); xf_set(xf, xf::CHAN0_ALPHA, {1});
    xf_set(xf, xf::NUM_TEXGENS, {3});
    xf_set(xf, xf::TEX_MTX_INFO, {(0U << 1) | (0U << 2) | (0U << 4) | (5U << 7)});  // ST, AB11, regular, from tex0
    xf_set(xf, xf::TEX_MTX_INFO + 1, {2U << 4});                                      // colour 0
    xf_set(xf, xf::TEX_MTX_INFO + 2, {(1U << 1) | (1U << 2) | (0U << 7)});            // STQ, ABC1, from position
    in.pos = {4, 5, 6};
    in.tex_mtx[2] = 0;
    TexScale sc = unit_scale();
    sc[0] = {8.0f, 4.0f};
    OutputVertex o;
    transform_vertex(xf, in, sc, o);
    CHECK(near(o.tex[0].x, (2 * 0.25f + 1) * 8)); CHECK(near(o.tex[0].y, (3 * 0.5f + 0.5f) * 4)); CHECK(o.tex[0].z == 1.0f);
    CHECK(near(o.tex[1].x, 128.0f / 255.0f)); CHECK(near(o.tex[1].y, 64.0f / 255.0f));
    CHECK(o.tex[2].x == 4.0f); CHECK(o.tex[2].y == 5.0f); CHECK(o.tex[2].z == 6.0f);
    // q = 0 halves and clamps s/t
    in.pos = {4, -1, 0};
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 1.0f); CHECK(o.tex[2].y == -0.5f);
    in.pos = {1, -3, 0};
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 0.5f); CHECK(o.tex[2].y == -1.0f);
    // dual texture transform: post matrix 2 (words 0x508..) doubles, normalisation optional
    xf_set(xf, xf::DUAL_TEX, {1});
    xf_f(xf, xf::POST_MATRICES + 8, {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0});
    xf_set(xf, xf::POST_MTX_INFO + 2, {2});
    in.pos = {3, 4, 12};
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 6.0f); CHECK(o.tex[2].z == 24.0f);
    xf_set(xf, xf::POST_MTX_INFO + 2, {2U | (1U << 8)});
    transform_vertex(xf, in, sc, o);
    CHECK(near(o.tex[2].x, 6.0f / 13.0f)); CHECK(near(o.tex[2].z, 24.0f / 13.0f));
    // AB11 from the position: the third input is 1, not z (matrix 33: s' = third input)
    xf_f(xf, 132, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    in.tex_mtx[2] = 33;
    xf_set(xf, xf::DUAL_TEX, {0});
    xf_set(xf, xf::TEX_MTX_INFO + 2, {0U});
    in.pos = {3, 4, 12};
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 1.0f);
    xf_set(xf, xf::TEX_MTX_INFO + 2, {1U << 2});                       // ABC1: z
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 12.0f);
    xf_set(xf, xf::TEX_MTX_INFO + 2, {(1U << 1) | (1U << 2)});
    in.tex_mtx[2] = 0;
    // NaN inputs become 1
    xf_set(xf, xf::DUAL_TEX, {0});
    in.pos = {std::nanf(""), 2, 3};
    transform_vertex(xf, in, sc, o);
    CHECK(o.tex[2].x == 1.0f);
}

namespace {
// A GPU fed with real command bytes, set up the way GX sets up a 640x528
// orthographic scene: EFB pixel (x, y) = model (x, y); depth = (1 + z) * 0xFFFFFF.
struct Scene {
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    Gpu gpu{bus, pe};
    std::vector<u8> cmd;
    Scene() {
        CHECK(bus.map_ram("MEM1", 0, 0x100000U));
        xff(0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
        xff(xf::PROJECTION, {1.0f / 320.0f, -1.0f, 1.0f / 264.0f, -1.0f, 1.0f, 0.0f});
        xfw(xf::PROJECTION + 6, 1);
        xff(xf::VIEWPORT, {320.0f, 264.0f, 16777215.0f, 662.0f, 606.0f, 16777215.0f});
        xfw(xf::CHAN0_COLOR, 1); xfw(xf::CHAN0_ALPHA, 1);
        bpw(bp::GENMODE, 1U << 4);                               // 1 colour channel, 1 stage, no texgen, no culling
        bpw(bp::SCISSOR_TL, (342U << 12) | 342U);
        bpw(bp::SCISSOR_BR, ((342U + 639U) << 12) | (342U + 527U));
        bpw(bp::SCISSOR_OFFSET, 171U | (171U << 10));
        bpw(bp::BLENDMODE, 0x18);                                 // colour + alpha update
        bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19)); // = rasc
        bpw(bp::TEV_ALPHA_ENV, (5U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19)); // = rasa
        bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));          // always pass
        bpw(bp::TEV_KSEL, 1U << 2);                               // swap table 0 = R G B A
        bpw(bp::TEV_KSEL + 1, 2U | (3U << 2));
        cpl(0x50, (vcf::DIRECT << 9) | (vcf::DIRECT << 13));       // position + colour 0
        cpl(0x70, 1U | (4U << 1) | (5U << 14));                    // xyz float, RGBA8888
        flush();
    }
    void be32(u32 v) { for (int i = 3; i >= 0; --i) cmd.push_back(static_cast<u8>(v >> (8 * i))); }
    void xfw(u32 a, u32 v) { cmd.push_back(0x10); be32(a); be32(v); }
    void xff(u32 a, std::initializer_list<float> v) {
        cmd.push_back(0x10); be32(((static_cast<u32>(v.size()) - 1U) << 16) | a);
        for (float f : v) be32(std::bit_cast<u32>(f));
    }
    void bpw(u32 r, u32 v) { cmd.push_back(0x61); be32((r << 24) | (v & 0xFFFFFFU)); }
    void cpl(u8 r, u32 v) { cmd.push_back(0x08); cmd.push_back(r); be32(v); }
    struct V { float x, y, z; u32 rgba; };
    void prim(u8 op, std::initializer_list<V> vs) {
        cmd.push_back(op); cmd.push_back(0); cmd.push_back(static_cast<u8>(vs.size()));
        for (const V& v : vs) { be32(std::bit_cast<u32>(v.x)); be32(std::bit_cast<u32>(v.y)); be32(std::bit_cast<u32>(v.z)); be32(v.rgba); }
    }
    bool flush() {
        const bool ok = gpu.feed(cmd);
        cmd.clear();
        if (!ok) std::printf("    gpu stopped: %s\n", gpu.error().c_str());
        return ok;
    }
    u32 rgb(u32 x, u32 y) const { return gpu.bp().reg(bp::ZCOMPARE) == 0 ? const_cast<Gpu&>(gpu).renderer().efb().color24(x, y) : 0; }
    u32 c24(u32 x, u32 y) { return gpu.renderer().efb().color24(x, y); }
    u32 z24(u32 x, u32 y) { return gpu.renderer().efb().depth24(x, y); }
};
unsigned count_color(Scene& s, u32 w, u32 h, u32 color) {
    unsigned n = 0;
    for (u32 y = 0; y < h; ++y) for (u32 x = 0; x < w; ++x) n += s.c24(x, y) == color ? 1U : 0U;
    return n;
}
} // namespace

TEST_CASE(sw_raster_flat_triangle_coverage) {
    Scene s;
    s.prim(0x90, {{0, 0, -0.5f, 0x204080FFU}, {8, 0, -0.5f, 0x204080FFU}, {0, 8, -0.5f, 0x204080FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x204080U), 28U);
    unsigned wrong = 0;
    for (u32 y = 0; y < 10; ++y)
        for (u32 x = 0; x < 10; ++x) wrong += (s.c24(x, y) == 0x204080U) != (x + y <= 6U) ? 1U : 0U;
    CHECK_EQ(wrong, 0U);
    CHECK_EQ(s.z24(0, 0), 0U); // z test off: depth untouched
    CHECK_EQ(s.gpu.renderer().pixels_written(), 28U);
    // a quad covers (0..4) x (10..14): 4x4 = 16 pixels
    s.prim(0x80, {{0, 10, -0.5f, 0x00FF00FFU}, {4, 10, -0.5f, 0x00FF00FFU}, {4, 14, -0.5f, 0x00FF00FFU}, {0, 14, -0.5f, 0x00FF00FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x00FF00U), 16U);
    CHECK_EQ(s.c24(0, 10), 0x00FF00U); CHECK_EQ(s.c24(3, 13), 0x00FF00U); CHECK_EQ(s.c24(4, 13), 0U); CHECK_EQ(s.c24(3, 14), 0U);
}

TEST_CASE(sw_raster_fill_rule) {
    // a quad whose edges lie exactly on sample positions (x + 9/16, y + 9/16): with a
    // consistent fill rule each sample is drawn once -> exactly 8 x 4 = 32 pixels
    Scene s;
    const float o = 9.0f / 16.0f;
    s.prim(0x80, {{o, 2 + o, -0.5f, 0x112233FFU}, {8 + o, 2 + o, -0.5f, 0x112233FFU},
                  {8 + o, 6 + o, -0.5f, 0x112233FFU}, {o, 6 + o, -0.5f, 0x112233FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x112233U), 32U);
    CHECK_EQ(s.c24(0, 2), 0x112233U); CHECK_EQ(s.c24(7, 5), 0x112233U);
    CHECK_EQ(s.c24(8, 2), 0U); CHECK_EQ(s.c24(0, 6), 0U);
    // every starting corner and both windings give the same 32 pixels
    const float cx[4] = {0, 8, 8, 0}, cy[4] = {0, 0, 4, 4};
    unsigned bad = 0;
    for (unsigned start = 0; start < 4; ++start)
        for (unsigned dir = 0; dir < 2; ++dir) {
            Scene q;
            Scene::V v[4];
            for (unsigned k = 0; k < 4; ++k) {
                const unsigned c = dir == 0 ? (start + k) % 4 : (start + 4 - k) % 4;
                v[k] = {cx[c] + 20 + o, cy[c] + 20 + o, -0.5f, 0x445566FFU};
            }
            q.prim(0x80, {v[0], v[1], v[2], v[3]});
            if (!q.flush()) { ++bad; continue; }
            if (count_color(q, 40, 40, 0x445566U) != 32U || q.c24(20, 20) != 0x445566U || q.c24(27, 23) != 0x445566U) ++bad;
        }
    CHECK_EQ(bad, 0U);
}

TEST_CASE(sw_skipped_vertices) {
    // positions indexed (8 bits) from array 0 at 0x4000: 0 (0,0), 1 (4,0), 2 (0,4), 3 (4,4);
    // index 0xFF drops the vertex and the primitive uses the remaining ones
    Scene s;
    std::vector<u8> arr;
    const float pts[4][3] = {{0, 0, -0.5f}, {4, 0, -0.5f}, {0, 4, -0.5f}, {4, 4, -0.5f}};
    for (const auto& p : pts)
        for (float f : p) { const u32 v = std::bit_cast<u32>(f); for (int k = 3; k >= 0; --k) arr.push_back(static_cast<u8>(v >> (8 * k))); }
    CHECK(s.bus.dma_write(0x4000, arr) == mem::BusStatus::Ok);
    s.cpl(0x50, (vcf::INDEX8 << 9) | (vcf::DIRECT << 13));
    s.cpl(0xA0, 0x4000);
    s.cpl(0xB0, 12);
    auto prim = [&](u8 op, std::initializer_list<u8> idx, u32 rgba) {
        s.cmd.push_back(op); s.cmd.push_back(0); s.cmd.push_back(static_cast<u8>(idx.size()));
        for (u8 i : idx) { s.cmd.push_back(i); s.be32(rgba); }
    };
    prim(0x98, {0, 1, 0xFF, 2, 3}, 0x10FF10FFU);   // strip 0,1,2,3: the 4x4 square
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x10FF10U), 16U);
    prim(0x90, {0, 0xFF, 1, 2}, 0x2020F0FFU);       // triangle 0,1,2: x + y <= 2 -> 6 pixels
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x2020F0U), 6U);
    prim(0x90, {0xFF, 0xFF, 0xFF}, 0x777777FFU);    // all dropped: nothing, no error
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0x777777U), 0U);
}

TEST_CASE(sw_raster_strip_fan_and_culling) {
    Scene s;
    // strip of 4 vertices = the same 4x4 square as two triangles; fan likewise
    s.prim(0x98, {{0, 0, -0.5f, 0xFF0000FFU}, {4, 0, -0.5f, 0xFF0000FFU}, {0, 4, -0.5f, 0xFF0000FFU}, {4, 4, -0.5f, 0xFF0000FFU}});
    s.prim(0xA0, {{10, 0, -0.5f, 0x0000FFFFU}, {14, 0, -0.5f, 0x0000FFFFU}, {14, 4, -0.5f, 0x0000FFFFU}, {10, 4, -0.5f, 0x0000FFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 20, 8, 0xFF0000U), 16U);
    CHECK_EQ(count_color(s, 20, 8, 0x0000FFU), 16U);
    // culling (Dolphin's rule): IsBackface = normalZDir <= 0, inverted when the viewport height
    // is positive; a triangle that is not "backface" is removed by mode 1 (back) or 3, a
    // "backface" one by mode 2 (front) or 3. (30,0),(38,0),(30,8): normalZDir = (8/320)(8/264) > 0
    // -> not backface -> inverted (height 264 > 0) -> backface.
    s.bpw(bp::GENMODE, (1U << 4) | (2U << 14)); // mode 2 removes it
    s.prim(0x90, {{30, 0, -0.5f, 0x111111FFU}, {38, 0, -0.5f, 0x111111FFU}, {30, 8, -0.5f, 0x111111FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 48, 10, 0x111111U), 0U);
    s.bpw(bp::GENMODE, (1U << 4) | (1U << 14)); // mode 1 keeps it
    s.prim(0x90, {{30, 0, -0.5f, 0x222222FFU}, {38, 0, -0.5f, 0x222222FFU}, {30, 8, -0.5f, 0x222222FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 48, 10, 0x222222U), 28U);
    // the other winding is removed by mode 1
    s.prim(0x90, {{30, 0, -0.5f, 0x333333FFU}, {30, 8, -0.5f, 0x333333FFU}, {38, 0, -0.5f, 0x333333FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 48, 10, 0x333333U), 0U);
    s.bpw(bp::GENMODE, (1U << 4) | (3U << 14)); // cull all
    s.prim(0x90, {{30, 0, -0.5f, 0x444444FFU}, {30, 8, -0.5f, 0x444444FFU}, {38, 0, -0.5f, 0x444444FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 48, 10, 0x444444U), 0U);
}

TEST_CASE(sw_raster_depth_test) {
    Scene s;
    s.bpw(bp::ZMODE, 1U | (3U << 1) | (1U << 4)); // test, LEQUAL, update
    s.bpw(bp::ZCOMPARE, 0x40);                    // early z
    // clear depth to far (0xFFFFFF) by drawing with ALWAYS
    s.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));
    s.prim(0x80, {{0, 0, 0.0f, 0x000000FFU}, {8, 0, 0.0f, 0x000000FFU}, {8, 8, 0.0f, 0x000000FFU}, {0, 8, 0.0f, 0x000000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.z24(3, 3), 0xFFFFFFU);
    s.bpw(bp::ZMODE, 1U | (3U << 1) | (1U << 4));
    // near square on the left half, then a farther square over everything
    s.prim(0x80, {{0, 0, -0.75f, 0x0000FFFFU}, {4, 0, -0.75f, 0x0000FFFFU}, {4, 8, -0.75f, 0x0000FFFFU}, {0, 8, -0.75f, 0x0000FFFFU}});
    s.prim(0x80, {{0, 0, -0.5f, 0xFF0000FFU}, {8, 0, -0.5f, 0xFF0000FFU}, {8, 8, -0.5f, 0xFF0000FFU}, {0, 8, -0.5f, 0xFF0000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0x0000FFU); CHECK_EQ(s.c24(6, 6), 0xFF0000U);
    // depth = z * 16777215 + 16777215 in float: -0.75 * 16777215 = -12582911.25 rounds to
    // -12582911 (24-bit mantissa) -> 4194304 (0x400000); -0.5 gives exactly 8388607.5, truncated to 8388607 (0x7FFFFF)
    CHECK_EQ(s.z24(1, 1), 0x400000U); CHECK_EQ(s.z24(6, 6), 0x7FFFFFU);
    // late z gives the same result for opaque pixels; without update the depth stays
    s.bpw(bp::ZCOMPARE, 0);
    s.bpw(bp::ZMODE, 1U | (7U << 1));
    s.prim(0x80, {{0, 0, -0.9f, 0x00FF00FFU}, {2, 0, -0.9f, 0x00FF00FFU}, {2, 2, -0.9f, 0x00FF00FFU}, {0, 2, -0.9f, 0x00FF00FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0x00FF00U); CHECK_EQ(s.z24(1, 1), 0x400000U);
    // LESS at equal depth fails, LEQUAL passes
    s.bpw(bp::ZMODE, 1U | (1U << 1) | (1U << 4));
    s.prim(0x80, {{0, 0, -0.75f, 0x123456FFU}, {2, 0, -0.75f, 0x123456FFU}, {2, 2, -0.75f, 0x123456FFU}, {0, 2, -0.75f, 0x123456FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0x00FF00U);
    // NEVER draws nothing
    s.bpw(bp::ZMODE, 1U | (0U << 1) | (1U << 4));
    s.prim(0x80, {{0, 0, -1.0f, 0xFFFFFFFFU}, {8, 0, -1.0f, 0xFFFFFFFFU}, {8, 8, -1.0f, 0xFFFFFFFFU}, {0, 8, -1.0f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 8, 8, 0xFFFFFFU), 0U);
}

TEST_CASE(sw_raster_clipping_and_scissor) {
    Scene s;
    // x from 600 to 700: clip x = 600/320-1 = 0.875 .. 1.1875; clipped at x = 640
    s.prim(0x80, {{600, 0, -0.5f, 0xABCDEFFFU}, {700, 0, -0.5f, 0xABCDEFFFU}, {700, 4, -0.5f, 0xABCDEFFFU}, {600, 4, -0.5f, 0xABCDEFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 640, 8, 0xABCDEFU), 40U * 4U);
    // fully outside: nothing; behind the camera (z > 0): trivially rejected
    s.prim(0x90, {{700, 0, -0.5f, 0x123456FFU}, {720, 0, -0.5f, 0x123456FFU}, {700, 20, -0.5f, 0x123456FFU}});
    s.prim(0x90, {{0, 20, 0.5f, 0x123456FFU}, {20, 20, 0.5f, 0x123456FFU}, {0, 40, 0.5f, 0x123456FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 640, 64, 0x123456U), 0U);
    // scissor right/bottom are inclusive: x 342..345, y 342..343 -> 4x2 pixels
    s.bpw(bp::SCISSOR_BR, ((342U + 3U) << 12) | (342U + 1U));
    s.prim(0x80, {{0, 0, -0.5f, 0x0F0F0FFFU}, {100, 0, -0.5f, 0x0F0F0FFFU}, {100, 100, -0.5f, 0x0F0F0FFFU}, {0, 100, -0.5f, 0x0F0F0FFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 640, 128, 0x0F0F0FU), 8U);
    // the scissor offset moves the EFB position: offset 175 (350) moves everything 8 pixels left/up
    s.bpw(bp::SCISSOR_BR, ((342U + 639U) << 12) | (342U + 527U));
    s.bpw(bp::SCISSOR_OFFSET, 175U | (175U << 10));
    s.prim(0x80, {{20, 20, -0.5f, 0x777777FFU}, {22, 20, -0.5f, 0x777777FFU}, {22, 22, -0.5f, 0x777777FFU}, {20, 22, -0.5f, 0x777777FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(12, 12), 0x777777U); CHECK_EQ(s.c24(13, 13), 0x777777U); CHECK_EQ(s.c24(20, 20), 0U);
}

TEST_CASE(sw_raster_color_interpolation) {
    Scene s;
    // red 0 at x=0, 255 at x=8 along the bottom edge; pixel (0,0) is sampled at
    // (0.495, 0.495) from vertex 0: 255/8 * 0.495 = 15.78 -> 15; pixel (3,0): 3.495*31.875 = 111.4 -> 111
    s.prim(0x90, {{0, 0, -0.5f, 0x000000FFU}, {8, 0, -0.5f, 0xFF0000FFU}, {0, 8, -0.5f, 0x000000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(0, 0) >> 16, 15U);
    CHECK_EQ(s.c24(3, 0) >> 16, 111U);
    CHECK_EQ(s.c24(3, 2) >> 16, 111U); // no change along y
}

TEST_CASE(sw_tev_arithmetic) {
    Scene s;
    // stage 0: prev = lerp(a=C0, b=K0, c=half) -> per channel (a*127 + b*129 + 128) >> 8 with c = 128 + 1
    s.bpw(bp::TEV_COLOR_RA, (100U) | (40U << 12));           // C? no: E0 is PREV. use C0 = E2/E3
    s.bpw(0xE2, 200U | (60U << 12));                          // C0: R 200, A 60
    s.bpw(0xE3, 50U | (10U << 12));                           // C0: B 50, G 10
    s.bpw(0xE0, 0U | (1U << 23));                             // K0: R 0, A 0 (constant)
    s.bpw(0xE1, 255U | (255U << 12) | (1U << 23));            // K0: B 255, G 255
    s.bpw(bp::TEV_KSEL, (1U << 2) | (12U << 4));               // stage 0 konst colour = K0
    s.bpw(bp::TEV_COLOR_ENV, 15U | (13U << 4) | (14U << 8) | (2U << 12) | (1U << 19)); // d 0, c half, b konst, a c0
    s.prim(0x90, {{0, 0, -0.5f, 0xFFFFFFFFU}, {8, 0, -0.5f, 0xFFFFFFFFU}, {0, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    // R: (200*127 + 0*129 + 128) >> 8 = 99; G: (10*127 + 255*129 + 128) >> 8 = 133; B: (50*127 + 255*129 + 128) >> 8 = 153
    CHECK_EQ(s.c24(1, 1), (99U << 16) | (133U << 8) | 153U);
    // subtract with bias and scale 2, unclamped then clamped: d - (a*(256-c')+b*c' + 127)>>8 with c = 0 -> d - a
    s.bpw(bp::TEV_COLOR_ENV, 2U | (15U << 4) | (15U << 8) | (12U << 12) | (1U << 16) | (1U << 18) | (1U << 19) | (1U << 20));
    // a = one (255), d = C0: ((d + 128) << 1) - ((255*256 << 1) + 127 >> 8) = 2d + 256 - 510 -> R 200: 146, G 10: <0 -> 0, B 50: -154 -> 0
    s.prim(0x90, {{20, 0, -0.5f, 0xFFFFFFFFU}, {28, 0, -0.5f, 0xFFFFFFFFU}, {20, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(21, 1), (146U << 16));
    // compare R8 GT: d + (a.r > b.r ? c : 0): a = C0 (R 200), b = K0 (R 0), c = one, d = zero -> 255
    s.bpw(bp::TEV_COLOR_ENV, 15U | (12U << 4) | (14U << 8) | (2U << 12) | (3U << 16) | (0U << 18) | (1U << 19) | (0U << 20));
    s.prim(0x90, {{40, 0, -0.5f, 0xFFFFFFFFU}, {48, 0, -0.5f, 0xFFFFFFFFU}, {40, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(41, 1), 0xFFFFFFU);
    // compare EQ fails -> 0
    s.bpw(bp::TEV_COLOR_ENV, 15U | (12U << 4) | (14U << 8) | (2U << 12) | (3U << 16) | (1U << 18) | (1U << 19) | (0U << 20));
    s.prim(0x90, {{60, 0, -0.5f, 0xFFFFFFFFU}, {68, 0, -0.5f, 0xFFFFFFFFU}, {60, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(61, 1), 0U);
    // compare GT with a == b fails (C0 R 200 against itself)
    s.bpw(bp::TEV_COLOR_ENV, 15U | (12U << 4) | (2U << 8) | (2U << 12) | (3U << 16) | (0U << 18) | (1U << 19));
    s.prim(0x90, {{70, 0, -0.5f, 0xFFFFFFFFU}, {78, 0, -0.5f, 0xFFFFFFFFU}, {70, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(71, 1), 0U);
    // scale 1/2 adds no rounding: b = C0 (R 3), c = half (128 + 1): (3*129) >> 8 = 1, >> 1 = 0
    s.bpw(0xE2, 3U | (60U << 12));
    s.bpw(bp::TEV_COLOR_ENV, 15U | (13U << 4) | (2U << 8) | (15U << 12) | (1U << 19) | (3U << 20));
    s.prim(0x90, {{100, 0, -0.5f, 0xFFFFFFFFU}, {108, 0, -0.5f, 0xFFFFFFFFU}, {100, 8, -0.5f, 0xFFFFFFFFU}});
    // bias -128: d = C0 (R 3) -> 3 - 128 < 0 -> 0; with R 200 -> 72
    s.bpw(bp::TEV_COLOR_ENV + 0, 2U | (15U << 4) | (15U << 8) | (15U << 12) | (2U << 16) | (1U << 19));
    s.bpw(0xE2, 200U | (60U << 12));
    s.prim(0x90, {{110, 0, -0.5f, 0xFFFFFFFFU}, {118, 0, -0.5f, 0xFFFFFFFFU}, {110, 8, -0.5f, 0xFFFFFFFFU}});
    // konst selection 9 is invalid and outputs 0; 7 is 1/8 (32)
    s.bpw(bp::TEV_COLOR_ENV, 14U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    s.bpw(bp::TEV_KSEL, (1U << 2) | (9U << 4));
    s.prim(0x90, {{120, 0, -0.5f, 0xFFFFFFFFU}, {128, 0, -0.5f, 0xFFFFFFFFU}, {120, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(101, 1) >> 16, 0U);
    CHECK_EQ(s.c24(111, 1) >> 16, 72U);
    CHECK_EQ(s.c24(121, 1), 0U);
    s.bpw(bp::TEV_KSEL, (1U << 2) | (7U << 4));
    s.prim(0x90, {{130, 0, -0.5f, 0xFFFFFFFFU}, {138, 0, -0.5f, 0xFFFFFFFFU}, {130, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(131, 1), 0x202020U);
    s.bpw(bp::TEV_KSEL, (1U << 2) | (12U << 4));
    // two stages: stage 0 writes C1 = ras, stage 1 outputs C1 * 1/2 via konst 1/2 (sel 4)
    s.bpw(bp::GENMODE, (1U << 4) | (1U << 10));
    s.bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19) | (2U << 22)); // C1 = rasc
    s.bpw(bp::TEV_COLOR_ENV + 2, 15U | (14U << 4) | (4U << 8) | (15U << 12) | (1U << 19));            // prev = C1 * konst
    s.bpw(bp::TEV_ALPHA_ENV + 2, (5U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19));
    s.bpw(bp::TEV_KSEL, (1U << 2) | (4U << 14));               // stage 1 konst colour = 1/2 (128)
    s.prim(0x90, {{80, 0, -0.5f, 0xC86432FFU}, {88, 0, -0.5f, 0xC86432FFU}, {80, 8, -0.5f, 0xC86432FFU}});
    CHECK(s.flush());
    // b*c with c = 128 + 1: (200*129 + 128) >> 8 = 101, (100*129+128)>>8 = 50, (50*129+128)>>8 = 25
    CHECK_EQ(s.c24(81, 1), (101U << 16) | (50U << 8) | 25U);
}

TEST_CASE(sw_alpha_test_blend_and_formats) {
    Scene s;
    // alpha test: pass only if alpha > 100 (comp0 GREATER, comp1 NEVER, OR)
    s.bpw(bp::ALPHA_COMPARE, 100U | (4U << 16) | (0U << 19) | (1U << 22));
    s.prim(0x90, {{0, 0, -0.5f, 0xFF000064U}, {8, 0, -0.5f, 0xFF000064U}, {0, 8, -0.5f, 0xFF000064U}}); // alpha 100
    s.prim(0x90, {{10, 0, -0.5f, 0xFF000065U}, {18, 0, -0.5f, 0xFF000065U}, {10, 8, -0.5f, 0xFF000065U}}); // alpha 101
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0U); CHECK_EQ(s.c24(11, 1), 0xFF0000U);
    // XOR: alpha > 100 xor alpha < 200 -> 150 fails (both true), 50 passes
    s.bpw(bp::ALPHA_COMPARE, 100U | (200U << 8) | (4U << 16) | (1U << 19) | (2U << 22));
    s.prim(0x90, {{20, 0, -0.5f, 0x00FF0096U}, {28, 0, -0.5f, 0x00FF0096U}, {20, 8, -0.5f, 0x00FF0096U}});
    s.prim(0x90, {{30, 20, -0.5f, 0x00FF0032U}, {38, 20, -0.5f, 0x00FF0032U}, {30, 28, -0.5f, 0x00FF0032U}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(21, 1), 0U); CHECK_EQ(s.c24(31, 21), 0x00FF00U);
    s.bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));
    // blend: src alpha / inv src alpha over red: src blue, alpha 64 -> sf 64, df 191+1 = 192
    s.bpw(bp::BLENDMODE, 0x18U | 1U | (4U << 8) | (5U << 5));
    s.prim(0x90, {{10, 0, -0.5f, 0x0000FF40U}, {18, 0, -0.5f, 0x0000FF40U}, {10, 8, -0.5f, 0x0000FF40U}});
    CHECK(s.flush());
    // R: (0*64 + 255*192) >> 8 = 191; B: (255*64 + 0) >> 8 = 63
    CHECK_EQ(s.c24(11, 1), (191U << 16) | 63U);
    // factors of 128 or more get +1: blue with alpha 192 over red -> sf 193, df 63:
    // R (255*63) >> 8 = 62, B (255*193) >> 8 = 192 (191 without the +1)
    const u32 blend = s.gpu.bp().reg(bp::BLENDMODE);
    s.bpw(bp::BLENDMODE, 0x18U);
    s.prim(0x90, {{50, 0, -0.5f, 0xFF0000FFU}, {58, 0, -0.5f, 0xFF0000FFU}, {50, 8, -0.5f, 0xFF0000FFU}});
    s.bpw(bp::BLENDMODE, blend);
    s.prim(0x90, {{50, 0, -0.5f, 0x0000FFC0U}, {58, 0, -0.5f, 0x0000FFC0U}, {50, 8, -0.5f, 0x0000FFC0U}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(51, 1), (62U << 16) | 192U);
    // subtract: dst - src
    s.bpw(bp::BLENDMODE, 0x18U | 1U | 0x800U);
    s.prim(0x90, {{10, 0, -0.5f, 0x100000FFU}, {18, 0, -0.5f, 0x100000FFU}, {10, 8, -0.5f, 0x100000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(11, 1), (175U << 16) | 63U);
    // logic XOR
    s.bpw(bp::BLENDMODE, 0x18U | 2U | (6U << 12));
    s.prim(0x90, {{10, 0, -0.5f, 0xFF00FFFFU}, {18, 0, -0.5f, 0xFF00FFFFU}, {10, 8, -0.5f, 0xFF00FFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(11, 1), ((175U ^ 255U) << 16) | (63U ^ 255U));
    // colour update off: nothing changes
    s.bpw(bp::BLENDMODE, 0x10U);
    s.prim(0x90, {{10, 0, -0.5f, 0x123456FFU}, {18, 0, -0.5f, 0x123456FFU}, {10, 8, -0.5f, 0x123456FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(11, 1), ((175U ^ 255U) << 16) | (63U ^ 255U));
    // RGBA6_Z24: 6-bit channels incl. alpha; destination alpha constant replaces alpha
    s.bpw(bp::ZCOMPARE, 1);
    s.bpw(bp::BLENDMODE, 0x18U);
    s.prim(0x90, {{30, 0, -0.5f, 0xFF8040C0U}, {38, 0, -0.5f, 0xFF8040C0U}, {30, 8, -0.5f, 0xFF8040C0U}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(31, 1), (63U << 18) | (32U << 12) | (16U << 6) | 48U);
    u8 rgba[4];
    sw::Renderer::efb_color_rgba(s.c24(31, 1), 1, rgba);
    CHECK_EQ(rgba[1], 0x82U); CHECK_EQ(rgba[3], 0xC3U);
    s.bpw(bp::CONSTANT_ALPHA, 0x100U | 0x04U);
    s.prim(0x90, {{40, 0, -0.5f, 0xFF8040C0U}, {48, 0, -0.5f, 0xFF8040C0U}, {40, 8, -0.5f, 0xFF8040C0U}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(41, 1) & 0x3FU, 1U);
    // alpha-only update keeps the colour
    s.bpw(bp::BLENDMODE, 0x10U);
    s.bpw(bp::CONSTANT_ALPHA, 0);
    s.prim(0x90, {{30, 0, -0.5f, 0x000000FCU}, {38, 0, -0.5f, 0x000000FCU}, {30, 8, -0.5f, 0x000000FCU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(31, 1), (63U << 18) | (32U << 12) | (16U << 6) | 63U);
    // dither (RGBA6 only): pixel (31,1) uses dither[1][1] = 1: ((128 - 2) + 1) & 0xFC = 124 -> 6-bit 31
    s.bpw(bp::BLENDMODE, 0x18U | 4U);
    s.prim(0x90, {{30, 0, -0.5f, 0x808080FFU}, {38, 0, -0.5f, 0x808080FFU}, {30, 8, -0.5f, 0x808080FFU}});
    CHECK(s.flush());
    CHECK_EQ((s.c24(31, 1) >> 18) & 63U, 31U);
    // pixel (30,0): dither[0][0] = 0: (128-2) & 0xFC = 124 -> 31 as well; (31,0): dither[0][1] = 2 -> 128 & 0xFC = 128 -> 32
    CHECK_EQ((s.c24(31, 0) >> 18) & 63U, 32U);
    // 131 - 2 = 129: (30,1) uses dither[y 1][x 0] = 3 -> 132 -> 33; (31,0) uses dither[0][1] = 2 -> 128 -> 32
    s.prim(0x90, {{30, 0, -0.5f, 0x838383FFU}, {38, 0, -0.5f, 0x838383FFU}, {30, 8, -0.5f, 0x838383FFU}});
    CHECK(s.flush());
    CHECK_EQ((s.c24(30, 1) >> 18) & 63U, 33U);
    CHECK_EQ((s.c24(31, 0) >> 18) & 63U, 32U);
}

namespace {
// Scene with one texture coordinate (tex0 ST float, passed through matrix 0 = identity)
// and stage 0 = texture colour. Texture unit 0 reads `fmt` at 0x2000, size w x h.
struct TexScene : Scene {
    TexScene(u32 fmt, u32 w, u32 h) {
        cpl(0x50, (vcf::DIRECT << 9) | (vcf::DIRECT << 13));
        cpl(0x60, vcf::DIRECT);
        cpl(0x70, 1U | (4U << 1) | (5U << 14) | (1U << 21) | (4U << 22));
        xfw(xf::NUM_TEXGENS, 1);
        xfw(xf::TEX_MTX_INFO, 5U << 7);                          // regular, AB11, ST, from tex0
        bpw(bp::GENMODE, (1U << 4) | 1U);
        bpw(bp::SU_SSIZE, w - 1U); bpw(bp::SU_TSIZE, h - 1U);
        bpw(0x28, 1U << 6);                                       // stage 0: texmap 0, coord 0, enabled
        bpw(bp::TEV_COLOR_ENV, 8U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19)); // texc
        bpw(bp::TEV_ALPHA_ENV, (4U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19)); // texa
        bpw(0x80, 0);                                             // clamp, nearest
        bpw(0x88, (w - 1U) | ((h - 1U) << 10) | (fmt << 20));
        bpw(0x94, 0x2000U >> 5);
        flush();
    }
    struct TV { float x, y, s, t; };
    void quad(std::initializer_list<TV> vs) {
        cmd.push_back(0x80); cmd.push_back(0); cmd.push_back(4);
        for (const TV& v : vs) {
            for (float f : {v.x, v.y, -0.5f}) be32(std::bit_cast<u32>(f));
            be32(0xFFFFFFFFU);
            be32(std::bit_cast<u32>(v.s)); be32(std::bit_cast<u32>(v.t));
        }
    }
    void square(float x0, float y0, float size, float u1, float v1) {
        quad({{x0, y0, 0, 0}, {x0 + size, y0, u1, 0}, {x0 + size, y0 + size, u1, v1}, {x0, y0 + size, 0, v1}});
    }
};
std::vector<u8> rgb565_tile(u32 seed) { // 4x4 tile: texel i -> R5 = i, G6 = 2i, B5 = 31 - i
    std::vector<u8> t;
    for (u32 i = 0; i < 16; ++i) { const u32 v = (((i + seed) & 31U) << 11) | ((2U * i) << 5) | (31U - i); t.push_back(static_cast<u8>(v >> 8)); t.push_back(static_cast<u8>(v)); }
    return t;
}
u32 rgb_of565(u32 v) {
    auto x5 = [](u32 c) { return (c << 3) | (c >> 2); };
    auto x6 = [](u32 c) { return (c << 2) | (c >> 4); };
    return (x5(v >> 11) << 16) | (x6((v >> 5) & 63U) << 8) | x5(v & 31U);
}
} // namespace

TEST_CASE(sw_texture_nearest_wrap_and_bilinear) {
    TexScene s(4, 4, 4);
    CHECK(s.bus.dma_write(0x2000, rgb565_tile(0)) == mem::BusStatus::Ok);
    s.square(0, 0, 4, 1, 1);
    CHECK(s.flush());
    unsigned bad = 0;
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const u32 i = y * 4 + x;
            bad += s.c24(x, y) != rgb_of565((i << 11) | ((2U * i) << 5) | (31U - i)) ? 1U : 0U;
        }
    CHECK_EQ(bad, 0U);
    // repeat: 8 pixels over u 0..2 -> texel x mod 4; mirror -> 0 1 2 3 3 2 1 0
    s.bpw(0x80, 1U | (1U << 2));
    s.square(10, 0, 8, 2, 2);
    CHECK(s.flush());
    CHECK_EQ(s.c24(15, 0), s.c24(1, 0)); CHECK_EQ(s.c24(17, 7), s.c24(3, 3));
    s.bpw(0x80, 2U | (2U << 2));
    s.square(20, 0, 8, 2, 2);
    CHECK(s.flush());
    CHECK_EQ(s.c24(24, 0), s.c24(3, 0)); CHECK_EQ(s.c24(27, 0), s.c24(0, 0)); CHECK_EQ(s.c24(25, 6), s.c24(2, 1));
    // clamp: beyond the edge repeats the last texel
    s.bpw(0x80, 0);
    s.square(30, 0, 8, 2, 2);
    CHECK(s.flush());
    CHECK_EQ(s.c24(37, 7), s.c24(3, 3));
    // bilinear (mag filter linear): pixel (1,1) samples s = t = 128 + 63 - 64 = 127 ->
    // texels (0,0) w 1, (1,0) w 127, (0,1) w 127, (1,1) w 127*127, sum >> 14
    s.bpw(0x80, 1U << 4);
    s.square(40, 0, 4, 1, 1);
    CHECK(s.flush());
    auto ch = [](u32 c, unsigned sh) { return (c >> sh) & 0xFFU; };
    const u32 t00 = s.c24(0, 0), t10 = s.c24(1, 0), t01 = s.c24(0, 1), t11 = s.c24(1, 1);
    u32 expect = 0;
    for (unsigned sh : {16U, 8U, 0U}) expect |= ((ch(t00, sh) * 1U + ch(t10, sh) * 127U + ch(t01, sh) * 127U + ch(t11, sh) * 16129U) >> 14) << sh;
    CHECK_EQ(s.c24(41, 1), expect);
}

TEST_CASE(sw_texture_palette_and_mipmaps) {
    TexScene s(9, 8, 4); // C8, 8x4 = one tile
    std::vector<u8> idx(32);
    for (u32 i = 0; i < 32; ++i) idx[i] = static_cast<u8>(i % 3U);
    CHECK(s.bus.dma_write(0x2000, idx) == mem::BusStatus::Ok);
    const std::vector<u8> pal = {0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F}; // red, green, blue
    std::vector<u8> pal32(32, 0);
    std::copy(pal.begin(), pal.end(), pal32.begin());
    CHECK(s.bus.dma_write(0x3000, pal32) == mem::BusStatus::Ok);
    s.bpw(bp::LOAD_TLUT0, 0x3000U >> 5);
    s.bpw(bp::LOAD_TLUT1, 0x100U | (1U << 10));   // TMEM 0x20000, 1 line
    s.bpw(0x98, 0x100U | (1U << 10));            // palette at 0x100 << 9, RGB565
    s.quad({{0, 0, 0, 0}, {8, 0, 1, 0}, {8, 4, 1, 1}, {0, 4, 0, 1}});
    CHECK(s.flush());
    CHECK_EQ(s.gpu.tmem()[0x20000], 0xF8U);
    CHECK_EQ(s.c24(0, 0), 0xFF0000U); CHECK_EQ(s.c24(1, 0), 0x00FF00U); CHECK_EQ(s.c24(2, 0), 0x0000FFU);
    CHECK_EQ(s.c24(0, 1), 0x0000FFU); // texel (0,1) has index 8: 8 % 3 = 2 -> blue
}

TEST_CASE(sw_texture_mip_selection) {
    TexScene s(4, 8, 8); // RGB565 8x8 + 4x4 mip at +128 bytes
    std::vector<u8> img(128 + 32);
    for (std::size_t i = 0; i < 128; i += 2) { img[i] = 0xF8; img[i + 1] = 0x00; }        // mip 0 red
    for (std::size_t i = 128; i < 160; i += 2) { img[i] = 0x00; img[i + 1] = 0x1F; }      // mip 1 blue
    CHECK(s.bus.dma_write(0x2000, img) == mem::BusStatus::Ok);
    // 8x8 texels on 4x4 pixels: du/dx = 2 texels -> FixedLog2(2.0) = 16 (1.0 in 1/16 steps)
    s.bpw(0x84, 16U << 8);                    // max lod 16
    s.square(0, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0xFF0000U);          // mip filter none: level 0
    s.bpw(0x80, 1U << 5);                      // mip point
    s.square(10, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(11, 1), 0x0000FFU);
    s.bpw(0x84, 8U << 8);                      // max lod 8: lod 8 -> point rounds up (frac >= 8) -> level 1
    s.square(20, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(21, 1), 0x0000FFU);
    s.bpw(0x84, 7U << 8);                      // lod 7 -> stays at level 0
    s.square(30, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(31, 1), 0xFF0000U);
    // LOD bias is in 1/32 steps (register >> 1): 4 texels on 4 pixels -> lod 0;
    // bias 14 -> +7 -> level 0 with mip point; bias 16 -> +8 -> rounds up to level 1
    s.bpw(0x84, 16U << 8);
    s.bpw(0x80, (1U << 5) | (14U << 9));
    s.square(50, 0, 4, 0.5f, 0.5f);
    s.bpw(0x80, (1U << 5) | (16U << 9));
    s.square(60, 0, 4, 0.5f, 0.5f);
    CHECK(s.flush());
    CHECK_EQ(s.c24(51, 1), 0xFF0000U); CHECK_EQ(s.c24(61, 1), 0x0000FFU);
    s.bpw(0x84, 7U << 8);
    s.bpw(0x80, 2U << 5);                      // mip linear at lod 7: (red*9 + blue*7) >> 4
    s.square(40, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(41, 1), ((255U * 9U >> 4) << 16) | (255U * 7U >> 4));
}

TEST_CASE(sw_indirect_alpha_bump) {
    TexScene s(1, 8, 4); // I8 texture used as the indirect map
    std::vector<u8> img(32, 0xAB);
    CHECK(s.bus.dma_write(0x2000, img) == mem::BusStatus::Ok);
    s.bpw(bp::GENMODE, (1U << 4) | 1U | (1U << 16));         // one indirect stage
    s.bpw(bp::IREF, 0);                                        // indirect stage 0: map 0, coord 0
    s.bpw(bp::IND_CMD, 1U << 7);                               // stage 0: bump alpha from S, 8-bit
    s.bpw(0x28, (5U << 7));                                    // stage 0: no texture, ras = alpha bump
    s.bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    s.quad({{0, 0, 0, 0}, {8, 0, 1, 0}, {8, 4, 1, 1}, {0, 4, 0, 1}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(1, 1), 0xA8A8A8U); // 0xAB & 0xF8
    // 5-bit format: alpha bump = (0xAB << 5) & 0xFF = 0x60
    s.bpw(bp::IND_CMD, (1U << 7) | (1U << 2));
    s.quad({{10, 0, 0, 0}, {18, 0, 1, 0}, {18, 4, 1, 1}, {10, 4, 0, 1}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(11, 1), 0x606060U);
}

TEST_CASE(sw_texture_rgba8_in_tmem) {
    // RGBA8 4x4 preloaded into TMEM: AR half to the even bank (line 0x10), GB half to the odd
    // bank (line 0x800); texel i: A = 16i, R = i, G = 100 + i, B = 200 - i
    TexScene s(6, 4, 4);
    std::vector<u8> img(64);
    for (u32 i = 0; i < 16; ++i) {
        img[2 * i] = static_cast<u8>(16U * i); img[2 * i + 1] = static_cast<u8>(i);
        img[32 + 2 * i] = static_cast<u8>(100U + i); img[33 + 2 * i] = static_cast<u8>(200U - i);
    }
    CHECK(s.bus.dma_write(0x5000, img) == mem::BusStatus::Ok);
    s.bpw(bp::PRELOAD_ADDR, 0x5000U >> 5);
    s.bpw(bp::PRELOAD_TMEM_EVEN, 0x10);
    s.bpw(bp::PRELOAD_TMEM_ODD, 0x800);
    s.bpw(bp::PRELOAD_MODE, (3U << 15) | 1U);
    s.bpw(0x8C, 0x10U | (1U << 21));   // image1: even line, manually managed
    s.bpw(0x90, 0x800U);                // image2: odd line
    s.bpw(0x94, 0x7F000U >> 5);         // RAM base points elsewhere (must not be used)
    s.square(0, 0, 4, 1, 1);
    CHECK(s.flush());
    unsigned bad = 0;
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const u32 i = y * 4 + x;
            bad += s.c24(x, y) != ((i << 16) | ((100U + i) << 8) | (200U - i)) ? 1U : 0U;
        }
    CHECK_EQ(bad, 0U);
    // 8x8: 2x2 blocks, AR/GB lines alternate in RAM per block (count 4); block b adds 16b to
    // R and G and subtracts it from B. Pixel (25,5) = texel (5,5): block 3, i = 5
    std::vector<u8> img2(256);
    for (u32 b = 0; b < 4; ++b)
        for (u32 i = 0; i < 16; ++i) {
            img2[64 * b + 2 * i] = static_cast<u8>(16U * i); img2[64 * b + 2 * i + 1] = static_cast<u8>(i + 16U * b);
            img2[64 * b + 32 + 2 * i] = static_cast<u8>(100U + i + 16U * b); img2[64 * b + 33 + 2 * i] = static_cast<u8>(200U - i - 16U * b);
        }
    CHECK(s.bus.dma_write(0x6000, img2) == mem::BusStatus::Ok);
    s.bpw(bp::PRELOAD_ADDR, 0x6000U >> 5);
    s.bpw(bp::PRELOAD_MODE, (3U << 15) | 4U);
    s.bpw(0x88, 7U | (7U << 10) | (6U << 20));
    s.bpw(bp::SU_SSIZE, 7); s.bpw(bp::SU_TSIZE, 7);
    s.square(20, 0, 8, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(25, 5), (53U << 16) | (153U << 8) | 147U);
    CHECK_EQ(s.c24(25, 1), (21U << 16) | (121U << 8) | 179U);   // block 1
    CHECK_EQ(s.c24(21, 5), (37U << 16) | (137U << 8) | 163U);   // block 2
    // a second level would be needed: stops
    s.bpw(0x80, 1U << 5);
    s.bpw(0x84, 16U | (16U << 8));
    s.square(10, 0, 4, 1, 1);
    CHECK(!s.gpu.feed(s.cmd));
    CHECK(s.gpu.error().find("mipmapped RGBA8 textures in TMEM") != std::string::npos);
}

TEST_CASE(sw_fog) {
    // red pixels at z = -0.5 (depth 8388607), fog colour blue. Orthographic: ze = A * z / 16777215.
    // A = 1.5 (exp 127, mant 0x400), C = (1 + 409/2048) / 4 = 0.29992676 -> ze - C = 0.45007318
    Scene s;
    s.bpw(bp::FOG_COLOR, 0x0000FF);
    s.bpw(bp::FOG_PARAM0, (127U << 11) | 0x400U);
    const u32 c3 = (125U << 11) | 409U | (1U << 20);
    // fsel -> fog * 256 (worked out with float32): linear 115, exp 234, exp2 172, back exp 12, back exp2 47
    const u32 fsel[5] = {2, 4, 5, 6, 7}, f8[5] = {115, 234, 172, 12, 47};
    for (unsigned i = 0; i < 5; ++i) {
        s.bpw(bp::FOG_PARAM3, c3 | (fsel[i] << 21));
        const float x0 = static_cast<float>(10 * i);
        s.prim(0x90, {{x0, 0, -0.5f, 0xFF0000FFU}, {x0 + 8, 0, -0.5f, 0xFF0000FFU}, {x0, 8, -0.5f, 0xFF0000FFU}});
    }
    CHECK(s.flush());
    for (unsigned i = 0; i < 5; ++i)
        CHECK_EQ(s.c24(10 * i + 1, 1), (((255U * (256U - f8[i])) >> 8) << 16) | ((f8[i] * 255U) >> 8));
    // perspective: A = 1/32, B_MAG 0x100000, B_SHF 4: ze = A * 16777215 / (1048576 - 524287) = 0.99999803;
    // C = 0.25 -> 191 (191.9995)
    s.bpw(bp::FOG_PARAM0, 122U << 11);
    s.bpw(bp::FOG_B_MAGNITUDE, 0x100000);
    s.bpw(bp::FOG_B_EXPONENT, 4);
    s.bpw(bp::FOG_PARAM3, (125U << 11) | (2U << 21));
    s.prim(0x90, {{60, 0, -0.5f, 0xFF0000FFU}, {68, 0, -0.5f, 0xFF0000FFU}, {60, 8, -0.5f, 0xFF0000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(61, 1), (((255U * 65U) >> 8) << 16) | ((191U * 255U) >> 8));
    // range adjustment: centre at EFB x 0 (342 - 342), all K = 256/256 * 4 = 4; pixel x 301:
    // offset 301/320, ze 0.25 * sqrt(offset^2 + 16) / 4 = 0.25681928 -> 65 (63 without range)
    s.bpw(bp::FOG_PARAM0, 126U << 11);
    s.bpw(bp::FOG_PARAM3, (1U << 20) | (2U << 21));
    s.bpw(bp::FOG_RANGE + 1, (256U << 12) | 256U);
    s.prim(0x90, {{300, 0, -0.5f, 0xFF0000FFU}, {308, 0, -0.5f, 0xFF0000FFU}, {300, 8, -0.5f, 0xFF0000FFU}});
    s.bpw(bp::FOG_RANGE, 342U | (1U << 10));
    s.prim(0x90, {{300, 20, -0.5f, 0xFF0000FFU}, {308, 20, -0.5f, 0xFF0000FFU}, {300, 28, -0.5f, 0xFF0000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(301, 1) & 0xFFU, (63U * 255U) >> 8);
    CHECK_EQ(s.c24(301, 21) & 0xFFU, (65U * 255U) >> 8);
    s.bpw(bp::FOG_RANGE, 0);
    // A and C both infinite (exponent 255): same signs -> fogged, otherwise unfogged
    s.bpw(bp::FOG_PARAM0, 255U << 11);
    s.bpw(bp::FOG_PARAM3, (255U << 11) | (1U << 20) | (2U << 21));
    s.prim(0x90, {{80, 0, -0.5f, 0xFF0000FFU}, {88, 0, -0.5f, 0xFF0000FFU}, {80, 8, -0.5f, 0xFF0000FFU}});
    s.bpw(bp::FOG_PARAM3, (255U << 11) | (1U << 19) | (1U << 20) | (2U << 21));
    s.prim(0x90, {{90, 0, -0.5f, 0xFF0000FFU}, {98, 0, -0.5f, 0xFF0000FFU}, {90, 8, -0.5f, 0xFF0000FFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(81, 1), 0x0000FFU);
    CHECK_EQ(s.c24(91, 1), 0xFF0000U);
    // stops: shift of 32, zero denominator
    s.bpw(bp::FOG_PARAM3, (125U << 11) | (2U << 21));
    s.bpw(bp::FOG_PARAM0, 122U << 11);
    s.bpw(bp::FOG_B_EXPONENT, 32);
    s.prim(0x90, {{100, 0, -0.5f, 0xFF0000FFU}, {108, 0, -0.5f, 0xFF0000FFU}, {100, 8, -0.5f, 0xFF0000FFU}});
    CHECK(!s.gpu.feed(s.cmd));
    CHECK(s.gpu.error().find("fog b shift 32") != std::string::npos);
    Scene t;
    t.bpw(bp::FOG_PARAM3, (125U << 11) | (2U << 21));
    t.bpw(bp::FOG_B_MAGNITUDE, 524287);
    t.bpw(bp::FOG_B_EXPONENT, 4);
    t.prim(0x90, {{0, 0, -0.5f, 0xFF0000FFU}, {8, 0, -0.5f, 0xFF0000FFU}, {0, 8, -0.5f, 0xFF0000FFU}});
    CHECK(!t.gpu.feed(t.cmd));
    CHECK(t.gpu.error().find("zero denominator") != std::string::npos);
}

TEST_CASE(sw_z_texture) {
    TexScene s(6, 4, 4); // RGBA8: every texel A 12, R 34, G 56, B 78
    std::vector<u8> img(64);
    for (u32 i = 0; i < 16; ++i) { img[2 * i] = 0x12; img[2 * i + 1] = 0x34; img[32 + 2 * i] = 0x56; img[33 + 2 * i] = 0x78; }
    CHECK(s.bus.dma_write(0x2000, img) == mem::BusStatus::Ok);
    s.bpw(bp::ZMODE, 0x1F);
    s.bpw(bp::BIAS, 0x100000);
    const u32 fmts[3] = {0x100012, 0x101234, 0x445678};
    for (u32 f = 0; f < 3; ++f) {
        s.bpw(bp::ZTEX2, f | (2U << 2)); // replace
        s.square(static_cast<float>(10 * f), 0, 4, 1, 1);
        CHECK(s.flush());
        CHECK_EQ(s.z24(10 * f + 1, 1), fmts[f]);
    }
    // add: z (0x7FFFFF at -0.5) + bias + A
    s.bpw(bp::ZTEX2, 0U | (1U << 2));
    s.square(40, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.z24(41, 1), (0x7FFFFFU + 0x100012U) & 0xFFFFFFU);
}

TEST_CASE(sw_indirect_matrix_offset) {
    // texmap 0: RGB565 4x4 tile, repeat; texmap 1: I8 8x4 all 0xAB used as indirect map.
    // Matrix 0: m00 = 512, scale exponent 17 (shift 0): offset s = (512 * ic0) >> 3 = 64 * ic0.
    TexScene s(4, 4, 4);
    CHECK(s.bus.dma_write(0x2000, rgb565_tile(0)) == mem::BusStatus::Ok);
    CHECK(s.bus.dma_write(0x3000, std::vector<u8>(32, 0xAB)) == mem::BusStatus::Ok);
    s.bpw(0x80, 1U | (1U << 2));                                  // map 0 repeat
    s.bpw(0x89, 7U | (3U << 10) | (1U << 20));                    // map 1: I8 8x4
    s.bpw(0x95, 0x3000U >> 5);
    s.bpw(bp::GENMODE, (1U << 4) | 1U | (1U << 16));
    s.bpw(bp::IREF, 1U);                                          // indirect 0: map 1, coord 0
    s.bpw(bp::IND_MTXA, 512U | (1U << 22));                       // scale bits 0-1 = 1
    s.bpw(bp::IND_MTXA + 1, 0);
    s.bpw(bp::IND_MTXA + 2, 1U << 22);                            // scale bit 4 -> 17
    auto texel = [](u32 x, u32 y) { const u32 i = y * 4 + x; return rgb_of565((i << 11) | ((2U * i) << 5) | (31U - i)); };
    // 8-bit format: ic0 = 0xAB = 171 -> offset 10944 = 85.5 texels; pixel 0 s = 63 -> 11007 >> 7 = 85 -> texel 1
    s.bpw(bp::IND_CMD, 1U << 9);
    s.square(0, 0, 4, 1, 1);
    // 5-bit format: ic0 = 0xAB >> 3 = 21 -> 1344 -> s = 1407 -> 10 -> texel 2
    s.bpw(bp::IND_CMD, (1U << 9) | (1U << 2));
    s.square(10, 0, 4, 1, 1);
    // 4-bit: 0xAB >> 4 = 10 -> 640 -> 703 -> 5 -> texel 1; 3-bit: >> 5 = 5 -> 320 -> 383 -> 2 -> texel 2
    s.bpw(bp::IND_CMD, (1U << 9) | (2U << 2));
    s.square(20, 0, 4, 1, 1);
    s.bpw(bp::IND_CMD, (1U << 9) | (3U << 2));
    s.square(30, 0, 4, 1, 1);
    CHECK(s.flush());
    CHECK_EQ(s.c24(0, 0), texel(1, 0));
    CHECK_EQ(s.c24(10, 0), texel(2, 0));
    CHECK_EQ(s.c24(20, 0), texel(1, 0));
    CHECK_EQ(s.c24(30, 0), texel(2, 0));
}

TEST_CASE(sw_lines_and_points) {
    Scene s;
    // horizontal line (2,5)-(10,5), width 6/6 px: rectangle y 4.5..5.5 -> row 4, x 2..9
    s.bpw(bp::LINE_PT_WIDTH, 6U);
    s.prim(0xA8, {{2, 5, -0.5f, 0xFF0000FFU}, {10, 5, -0.5f, 0xFF0000FFU}, {2, 7, -0.5f, 0x00FF00FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 16, 16, 0xFF0000U), 8U);
    CHECK_EQ(s.c24(2, 4), 0xFF0000U); CHECK_EQ(s.c24(9, 4), 0xFF0000U); CHECK_EQ(s.c24(10, 4), 0U);
    CHECK_EQ(s.c24(5, 5), 0U); CHECK_EQ(s.c24(5, 3), 0U);
    CHECK_EQ(count_color(s, 16, 16, 0x00FF00U), 0U); // odd vertex of a line list is unused
    // vertical line (20,0)-(20,8), width 12/6: x 19..21 -> columns 19, 20, rows 0..7
    s.bpw(bp::LINE_PT_WIDTH, 12U);
    s.prim(0xA8, {{20, 0, -0.5f, 0x0000FFFFU}, {20, 8, -0.5f, 0x0000FFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 32, 16, 0x0000FFU), 16U);
    CHECK_EQ(s.c24(19, 0), 0x0000FFU); CHECK_EQ(s.c24(20, 7), 0x0000FFU); CHECK_EQ(s.c24(21, 3), 0U); CHECK_EQ(s.c24(18, 3), 0U);
    // line strip (40,10)-(48,10)-(48,18), width 6: row 9 x 40..47, then column 47 rows 10..17
    s.bpw(bp::LINE_PT_WIDTH, 6U);
    s.prim(0xB0, {{40, 10, -0.5f, 0x123456FFU}, {48, 10, -0.5f, 0x123456FFU}, {48, 18, -0.5f, 0x123456FFU}});
    CHECK(s.flush());
    unsigned n = 0;
    for (u32 y = 0; y < 30; ++y) for (u32 x = 30; x < 60; ++x) n += s.c24(x, y) == 0x123456U ? 1U : 0U;
    CHECK_EQ(n, 16U);
    CHECK_EQ(s.c24(40, 9), 0x123456U); CHECK_EQ(s.c24(47, 17), 0x123456U); CHECK_EQ(s.c24(47, 18), 0U);
    // clipped at x = 0 (clip x = -1): (-100,50)-(10,50) -> row 49, x 0..9
    s.prim(0xA8, {{-100, 50, -0.5f, 0x00FFFFFFU}, {10, 50, -0.5f, 0x00FFFFFFU}});
    // fully outside: nothing
    s.prim(0xA8, {{-100, 60, -0.5f, 0x00FFFFFFU}, {-50, 60, -0.5f, 0x00FFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 64, 64, 0x00FFFFU), 10U);
    CHECK_EQ(s.c24(0, 49), 0x00FFFFU); CHECK_EQ(s.c24(9, 49), 0x00FFFFU);
    // clipped at the second vertex: (10,70)-(-100,70) -> row 69, x 0..9
    s.prim(0xA8, {{10, 70, -0.5f, 0x0F0F0FFFU}, {-100, 70, -0.5f, 0x0F0F0FFFU}});
    // the clipped end keeps the interpolated colour: red 220 -> 0 over (−100..10),
    // at x = 0 about 20 (same as an unclipped line from (0, R 20) to (10, R 0))
    s.prim(0xA8, {{10, 100, -0.5f, 0x000001FFU}, {-100, 100, -0.5f, 0xDC0001FFU}});
    s.prim(0xA8, {{10, 102, -0.5f, 0x000001FFU}, {0, 102, -0.5f, 0x140001FFU}});
    CHECK(s.flush());
    for (u32 x : {0U, 4U, 9U}) {
        const int d = static_cast<int>(s.c24(x, 99) >> 16) - static_cast<int>(s.c24(x, 101) >> 16);
        CHECK(d >= -1 && d <= 1);
    }
    CHECK((s.c24(0, 99) >> 16) >= 17U);
    // a long line whose far end is just outside: (300,110)-(-10,110) still covers x 0..299
    s.prim(0xA8, {{300, 110, -0.5f, 0x0F0F0FFFU}, {-10, 110, -0.5f, 0x0F0F0FFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(5, 109), 0x0F0F0FU); CHECK_EQ(s.c24(299, 109), 0x0F0F0FU); CHECK_EQ(s.c24(300, 109), 0U);
    // the line +z plane is -z + w (Clipper.cpp LINE_CLIP): z = 1.5 at both ends is
    // fully clipped; z = 0.5 is not clipped by it (row 89, x 2..9)
    s.prim(0xA8, {{2, 80, 1.5f, 0x0F0F0FFFU}, {10, 80, 1.5f, 0x0F0F0FFFU}});
    s.prim(0xA8, {{2, 90, 0.5f, 0x0F0F0FFFU}, {10, 90, 0.5f, 0x0F0F0FFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 64, 100, 0x0F0F0FU), 18U);
    CHECK_EQ(s.c24(0, 69), 0x0F0F0FU); CHECK_EQ(s.c24(9, 69), 0x0F0F0FU);
    CHECK_EQ(s.c24(2, 89), 0x0F0F0FU); CHECK_EQ(s.c24(5, 79), 0U);
    // 45 degrees (|dx| == |dy|): caps are horizontal (offset along x), width 36/6
    s.bpw(bp::LINE_PT_WIDTH, 36U);
    s.prim(0xA8, {{0, 140, -0.5f, 0x0A0B0CFFU}, {8, 148, -0.5f, 0x0A0B0CFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(9, 147), 0x0A0B0CU); CHECK_EQ(s.c24(0, 138), 0U);
    s.bpw(bp::LINE_PT_WIDTH, 6U);
    // points: size 12/6 -> 2x2 square around (30,30); size 6 -> one pixel
    s.bpw(bp::LINE_PT_WIDTH, 12U << 8);
    s.prim(0xB8, {{30, 30, -0.5f, 0xAA0000FFU}});
    s.bpw(bp::LINE_PT_WIDTH, 6U << 8);
    s.prim(0xB8, {{40, 30, -0.5f, 0x00AA00FFU}, {44, 30, -0.5f, 0x00AA00FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 64, 64, 0xAA0000U), 4U);
    CHECK_EQ(s.c24(29, 29), 0xAA0000U); CHECK_EQ(s.c24(30, 30), 0xAA0000U);
    CHECK_EQ(count_color(s, 64, 64, 0x00AA00U), 2U);
    CHECK_EQ(s.c24(39, 29), 0x00AA00U); CHECK_EQ(s.c24(43, 29), 0x00AA00U);
    // the aspect ratio bit is ignored (as in Dolphin): same single pixel
    s.bpw(bp::LINE_PT_WIDTH, (6U << 8) | (1U << 22));
    s.prim(0xB8, {{40, 40, -0.5f, 0x00AA00FFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 64, 64, 0x00AA00U), 3U);
    CHECK_EQ(s.c24(39, 39), 0x00AA00U);
}

TEST_CASE(sw_line_point_texture_offsets) {
    TexScene s(4, 4, 4);
    CHECK(s.bus.dma_write(0x2000, rgb565_tile(0)) == mem::BusStatus::Ok);
    auto texel = [](u32 x, u32 y) { const u32 i = y * 4 + x; return rgb_of565((i << 11) | ((2U * i) << 5) | (31U - i)); };
    auto tprim = [&](u8 op, std::initializer_list<std::array<float, 2>> pos) {
        s.cmd.push_back(op); s.cmd.push_back(0); s.cmd.push_back(static_cast<u8>(pos.size()));
        for (const auto& p : pos) {
            for (float f : {p[0], p[1], -0.5f}) s.be32(std::bit_cast<u32>(f));
            s.be32(0xFFFFFFFFU);
            s.be32(0); s.be32(0); // s = t = 0
        }
    };
    // point offset 1.0 (index 5) on coordinate 0: +x corners get s + 4, +y corners t + 4.
    // Pixel (49,49) samples 0.495 into the 2-pixel square -> s, t = 0.99 -> texel 0;
    // pixel 50 -> 2.99 -> texel 2.
    s.bpw(bp::SU_SSIZE, 3U | (1U << 19));
    s.bpw(bp::LINE_PT_WIDTH, (12U << 8) | (5U << 19));
    tprim(0xB8, {{50, 50}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(49, 49), texel(0, 0)); CHECK_EQ(s.c24(50, 49), texel(2, 0));
    CHECK_EQ(s.c24(49, 50), texel(0, 2)); CHECK_EQ(s.c24(50, 50), texel(2, 2));
    // without the enable bit the offset is not applied
    s.bpw(bp::SU_SSIZE, 3U);
    tprim(0xB8, {{60, 50}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(60, 50), texel(0, 0));
    // line offset 1.0 on a horizontal line of width 12/6: the lower edge gets s + 4
    s.bpw(bp::SU_SSIZE, 3U | (1U << 18));
    s.bpw(bp::LINE_PT_WIDTH, 12U | (5U << 16));
    tprim(0xA8, {{60, 60}, {68, 60}});
    CHECK(s.flush());
    CHECK_EQ(s.c24(62, 59), texel(0, 0)); CHECK_EQ(s.c24(62, 60), texel(2, 0));
}

TEST_CASE(sw_z_freeze) {
    const u32 frozen = (1U << 4) | (1U << 19);
    {
        // before any primitive the slope is Dolphin's default: constant 1.0 -> depth 1
        Scene s;
        s.bpw(bp::ZMODE, 0x1F); // test on, always, update
        s.bpw(bp::GENMODE, frozen);
        s.prim(0x80, {{0, 0, -0.5f, 0xFFFFFFFFU}, {4, 0, -0.5f, 0xFFFFFFFFU}, {4, 4, -0.5f, 0xFFFFFFFFU}, {0, 4, -0.5f, 0xFFFFFFFFU}});
        CHECK(s.flush());
        CHECK_EQ(s.z24(1, 1), 1U);
    }
    Scene s;
    s.bpw(bp::ZMODE, 0x1F);
    // reference depths for z = -0.5 and -0.25 without freezing
    s.prim(0x80, {{40, 0, -0.25f, 0xFFFFFFFFU}, {44, 0, -0.25f, 0xFFFFFFFFU}, {44, 4, -0.25f, 0xFFFFFFFFU}, {40, 4, -0.25f, 0xFFFFFFFFU}});
    // slope source: z from -0.5 at x = 0 to -0.25 at x = 64
    s.prim(0x90, {{0, 0, -0.5f, 0xFFFFFFFFU}, {64, 0, -0.25f, 0xFFFFFFFFU}, {0, 8, -0.5f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    const u32 z_quarter = s.z24(41, 1), z_left = s.z24(1, 1), z_right = s.z24(5, 1);
    CHECK(z_right > z_left);
    // frozen: a flat quad at z = -0.25 gets the frozen slope's depths at its own pixels
    s.bpw(bp::GENMODE, frozen);
    s.prim(0x80, {{0, 20, -0.25f, 0xFFFFFFFFU}, {8, 20, -0.25f, 0xFFFFFFFFU}, {8, 24, -0.25f, 0xFFFFFFFFU}, {0, 24, -0.25f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK(s.z24(1, 21) != z_quarter);
    CHECK(s.z24(5, 21) > s.z24(1, 21));
    CHECK_EQ(s.z24(1, 21) - s.z24(1, 20), s.z24(1, 1) - s.z24(1, 0)); // same dz/dy (0)
    CHECK_EQ(s.z24(5, 21) - s.z24(1, 21), z_right - z_left);         // same dz/dx
    // a culled triangle still sets the slope (when not frozen)
    s.bpw(bp::GENMODE, (1U << 4) | (3U << 14));                      // cull all
    s.prim(0x90, {{0, 30, -0.75f, 0xFFFFFFFFU}, {8, 30, -0.75f, 0xFFFFFFFFU}, {0, 38, -0.75f, 0xFFFFFFFFU}});
    s.bpw(bp::GENMODE, frozen);
    s.prim(0x80, {{20, 30, -0.25f, 0xFFFFFFFFU}, {24, 30, -0.25f, 0xFFFFFFFFU}, {24, 34, -0.25f, 0xFFFFFFFFU}, {20, 34, -0.25f, 0xFFFFFFFFU}});
    s.bpw(bp::GENMODE, 1U << 4);
    s.prim(0x80, {{50, 30, -0.75f, 0xFFFFFFFFU}, {54, 30, -0.75f, 0xFFFFFFFFU}, {54, 34, -0.75f, 0xFFFFFFFFU}, {50, 34, -0.75f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.z24(21, 31), s.z24(51, 31));
    // a triangle inside the clip volume but outside the scissor rectangle still sets the slope
    s.bpw(bp::GENMODE, 1U << 4);
    s.prim(0x80, {{60, 40, -0.6f, 0xFFFFFFFFU}, {64, 40, -0.6f, 0xFFFFFFFFU}, {64, 44, -0.6f, 0xFFFFFFFFU}, {60, 44, -0.6f, 0xFFFFFFFFU}});
    s.prim(0x90, {{70, 40, -0.3f, 0xFFFFFFFFU}, {78, 40, -0.3f, 0xFFFFFFFFU}, {70, 48, -0.3f, 0xFFFFFFFFU}}); // slope -0.3
    s.bpw(bp::SCISSOR_BR, ((342U + 99U) << 12) | (342U + 527U));  // x 0..99 only
    s.prim(0x90, {{200, 0, -0.6f, 0xFFFFFFFFU}, {208, 0, -0.6f, 0xFFFFFFFFU}, {200, 8, -0.6f, 0xFFFFFFFFU}});
    s.bpw(bp::SCISSOR_BR, ((342U + 639U) << 12) | (342U + 527U));
    s.bpw(bp::GENMODE, frozen);
    s.prim(0x80, {{40, 30, 0.0f, 0xFFFFFFFFU}, {44, 30, 0.0f, 0xFFFFFFFFU}, {44, 34, 0.0f, 0xFFFFFFFFU}, {40, 34, 0.0f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(count_color(s, 640, 20, 0xFFFFFFU) - count_color(s, 100, 20, 0xFFFFFFU), 0U); // nothing drawn at x 200
    CHECK_EQ(s.z24(41, 31), s.z24(61, 41));
    s.bpw(bp::GENMODE, 1U << 4);
    s.prim(0x90, {{0, 30, -0.75f, 0xFFFFFFFFU}, {8, 30, -0.75f, 0xFFFFFFFFU}, {0, 38, -0.75f, 0xFFFFFFFFU}}); // slope back to -0.75
    // while frozen, drawing does not change the slope
    s.bpw(bp::GENMODE, frozen);
    s.prim(0x90, {{0, 50, -0.1f, 0xFFFFFFFFU}, {8, 50, -0.1f, 0xFFFFFFFFU}, {0, 58, -0.1f, 0xFFFFFFFFU}});
    s.prim(0x80, {{30, 50, -0.25f, 0xFFFFFFFFU}, {34, 50, -0.25f, 0xFFFFFFFFU}, {34, 54, -0.25f, 0xFFFFFFFFU}, {30, 54, -0.25f, 0xFFFFFFFFU}});
    CHECK(s.flush());
    CHECK_EQ(s.z24(1, 51), s.z24(51, 31)); CHECK_EQ(s.z24(31, 51), s.z24(51, 31));
}

TEST_CASE(sw_raster_stops) {
    const std::vector<std::pair<std::function<void(Scene&)>, const char*>> cases = {
        {[](Scene& s) { s.bpw(bp::GENMODE, (1U << 4) | 1U); s.bpw(0x28, 1U << 6); s.bpw(0x94, 0x100000U); }, "outside RAM"},
        {[](Scene& s) { s.bpw(bp::ZCOMPARE, 4); }, "pixel format 4"},
    };
    unsigned bad = 0;
    for (const auto& [setup, what] : cases) {
        Scene s;
        setup(s);
        s.prim(0x90, {{0, 0, -0.5f, 0xFFFFFFFFU}, {8, 0, -0.5f, 0xFFFFFFFFU}, {0, 8, -0.5f, 0xFFFFFFFFU}});
        if (s.gpu.feed(s.cmd) || s.gpu.error().find(what) == std::string::npos) { ++bad; std::printf("    case %s: %s\n", what, s.gpu.error().c_str()); }
    }
    CHECK_EQ(bad, 0U);
}

TEST_MAIN("gpu_sw")
