// OpenGL ES backend against the software reference: every scene is fed as real command bytes
// to two GPUs (software rasterizer / GLES backend) and the whole EFB (colour and depth) is
// compared. Exits with 77 (skipped) when no GLES 3.2 context with framebuffer fetch exists.
#include "orca/gpu/efb_access.hpp"
#include "orca/gpu/gles/backend.hpp"
#include "orca/gpu/gpu.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/system/system.hpp"
#include "orca/video/xfb.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

using namespace orca;
using namespace orca::gpu;

namespace {

gles::HeadlessContext* g_ctx = nullptr;

struct Side {
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    Gpu gpu{bus, pe};
    std::unique_ptr<gles::Backend> be;
    explicit Side(bool gl, bool gpu_copies = false) {
        CHECK(bus.map_ram("MEM1", 0, 0x200000U));
        if (gl) {
            be = std::make_unique<gles::Backend>();
            std::string e;
            if (!be->init(e)) std::printf("    backend init: %s\n", e.c_str());
            gles::Backend* b = be.get();
            gpu.set_backend(b, [b](sw::Efb& f, const sw::EfbRect& r, bool c, bool d) { b->download(f, r, c, d); }, [b](const sw::Efb& f, const sw::EfbRect& r) { b->upload(f, r); });
            if (gpu_copies) gpu.set_gpu_copies(gles::gpu_copies(*b));
        }
    }
    ~Side() {
        if (be) gpu.set_backend(nullptr, nullptr, nullptr); // reads pending GPU copies back first
    }
    Side(const Side&) = delete;
    Side& operator=(const Side&) = delete;
};

// Diff summary between the two EFBs.
struct Diff {
    unsigned color{0}, depth{0}, max_channel{0};
    u32 max_depth{0};
    int fx{-1}, fy{-1};
    u32 sw_c{0}, gl_c{0};
};

struct Twin {
    Side sw{false}, gl;
    explicit Twin(bool gpu_copies = false) : gl(true, gpu_copies) {}
    std::vector<u8> cmd;
    void be32(u32 v) { for (int i = 3; i >= 0; --i) cmd.push_back(static_cast<u8>(v >> (8 * i))); }
    void xfw(u32 a, u32 v) { cmd.push_back(0x10); be32(a); be32(v); }
    void xff(u32 a, std::initializer_list<float> v) {
        cmd.push_back(0x10); be32(((static_cast<u32>(v.size()) - 1U) << 16) | a);
        for (float f : v) be32(std::bit_cast<u32>(f));
    }
    void bpw(u32 r, u32 v) { cmd.push_back(0x61); be32((r << 24) | (v & 0xFFFFFFU)); }
    void cpl(u8 r, u32 v) { cmd.push_back(0x08); cmd.push_back(r); be32(v); }
    void ram(u32 addr, const std::vector<u8>& d) {
        CHECK(sw.bus.dma_write(addr, d) == mem::BusStatus::Ok);
        CHECK(gl.bus.dma_write(addr, d) == mem::BusStatus::Ok);
    }
    struct V { float x, y, z; u32 rgba; };
    void prim(u8 op, std::initializer_list<V> vs) {
        cmd.push_back(op); cmd.push_back(0); cmd.push_back(static_cast<u8>(vs.size()));
        for (const V& v : vs) { be32(std::bit_cast<u32>(v.x)); be32(std::bit_cast<u32>(v.y)); be32(std::bit_cast<u32>(v.z)); be32(v.rgba); }
    }
    struct TV { float x, y, z; u32 rgba; float s, t; };
    void tprim(u8 op, std::initializer_list<TV> vs) {
        cmd.push_back(op); cmd.push_back(0); cmd.push_back(static_cast<u8>(vs.size()));
        for (const TV& v : vs) {
            for (float f : {v.x, v.y, v.z}) be32(std::bit_cast<u32>(f));
            be32(v.rgba); be32(std::bit_cast<u32>(v.s)); be32(std::bit_cast<u32>(v.t));
        }
    }
    void quad(float x, float y, float w, float h, float z, u32 c) { prim(0x80, {{x, y, z, c}, {x + w, y, z, c}, {x + w, y + h, z, c}, {x, y + h, z, c}}); }
    // GX-like 640x528 orthographic scene: EFB (x, y) = model (x, y)
    void scene() {
        xff(0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
        xff(xf::PROJECTION, {1.0f / 320.0f, -1.0f, 1.0f / 264.0f, -1.0f, 1.0f, 0.0f});
        xfw(xf::PROJECTION + 6, 1);
        xff(xf::VIEWPORT, {320.0f, 264.0f, 16777215.0f, 662.0f, 606.0f, 16777215.0f});
        xfw(xf::CHAN0_COLOR, 1); xfw(xf::CHAN0_ALPHA, 1);
        bpw(bp::GENMODE, 1U << 4);
        bpw(bp::SCISSOR_TL, (342U << 12) | 342U);
        bpw(bp::SCISSOR_BR, ((342U + 639U) << 12) | (342U + 527U));
        bpw(bp::SCISSOR_OFFSET, 171U | (171U << 10));
        bpw(bp::BLENDMODE, 0x18);
        bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
        bpw(bp::TEV_ALPHA_ENV, (5U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19));
        bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));
        bpw(bp::TEV_KSEL, 1U << 2);
        bpw(bp::TEV_KSEL + 1, 2U | (3U << 2));
        cpl(0x50, (vcf::DIRECT << 9) | (vcf::DIRECT << 13));
        cpl(0x70, 1U | (4U << 1) | (5U << 14));
    }
    // texture coordinate 0 from the vertex (ST float), stage 0 = texture colour of map 0
    void tex_scene(u32 fmt, u32 w, u32 h, u32 addr) {
        cpl(0x50, (vcf::DIRECT << 9) | (vcf::DIRECT << 13));
        cpl(0x60, vcf::DIRECT);
        cpl(0x70, 1U | (4U << 1) | (5U << 14) | (1U << 21) | (4U << 22));
        xfw(xf::NUM_TEXGENS, 1);
        xfw(xf::TEX_MTX_INFO, 5U << 7);
        bpw(bp::GENMODE, (1U << 4) | 1U);
        bpw(bp::SU_SSIZE, w - 1U); bpw(bp::SU_TSIZE, h - 1U);
        bpw(0x28, 1U << 6);
        bpw(bp::TEV_COLOR_ENV, 8U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
        bpw(bp::TEV_ALPHA_ENV, (4U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19));
        bpw(0x80, 0);
        bpw(0x88, (w - 1U) | ((h - 1U) << 10) | (fmt << 20));
        bpw(0x94, addr >> 5);
    }
    bool run() {
        const bool a = sw.gpu.feed(cmd), b = gl.gpu.feed(cmd);
        if (!a) std::printf("    software: %s\n", sw.gpu.error().c_str());
        if (!b) std::printf("    gles: %s\n", gl.gpu.error().c_str());
        cmd.clear();
        return a && b;
    }
    Diff diff() {
        sw::Efb e;
        gl.be->download(e);
        const sw::Efb& r = sw.gpu.renderer().efb();
        Diff d;
        for (u32 y = 0; y < sw::kEfbHeight; ++y)
            for (u32 x = 0; x < sw::kEfbWidth; ++x) {
                const u32 a = r.color24(x, y), b = e.color24(x, y);
                if (a != b) {
                    if (d.color == 0U) { d.fx = static_cast<int>(x); d.fy = static_cast<int>(y); d.sw_c = a; d.gl_c = b; }
                    ++d.color;
                    for (unsigned sh = 0; sh < 24; sh += 8) {
                        const int ca = static_cast<int>((a >> sh) & 0xFFU), cb = static_cast<int>((b >> sh) & 0xFFU);
                        d.max_channel = std::max(d.max_channel, static_cast<unsigned>(std::abs(ca - cb)));
                    }
                }
                const u32 za = r.depth24(x, y), zb = e.depth24(x, y);
                if (za != zb) { ++d.depth; d.max_depth = std::max(d.max_depth, za > zb ? za - zb : zb - za); }
            }
        return d;
    }
};

void report(const char* name, const Diff& d) {
    std::printf("    %-28s colour diffs %6u (max channel %3u)  depth diffs %6u (max %u)", name, d.color, d.max_channel, d.depth, d.max_depth);
    if (d.color != 0U) std::printf("  first (%d,%d) sw %06X gl %06X", d.fx, d.fy, d.sw_c, d.gl_c);
    std::printf("\n");
}

std::vector<u8> tile565() {
    std::vector<u8> t;
    for (u32 i = 0; i < 16; ++i) { const u32 v = (i << 11) | ((2U * i) << 5) | (31U - i); t.push_back(static_cast<u8>(v >> 8)); t.push_back(static_cast<u8>(v)); }
    return t;
}

} // namespace

TEST_CASE(gles_flat_coverage_and_depth) {
    Twin t;
    t.scene();
    t.prim(0x90, {{0, 0, -0.5f, 0x204080FFU}, {8, 0, -0.5f, 0x204080FFU}, {0, 8, -0.5f, 0x204080FFU}});
    t.quad(20, 0, 4, 4, -0.5f, 0x00FF00FFU);
    t.prim(0x98, {{40, 0, -0.5f, 0xFF0000FFU}, {44, 0, -0.5f, 0xFF0000FFU}, {40, 4, -0.5f, 0xFF0000FFU}, {44, 4, -0.5f, 0xFF0000FFU}});
    t.prim(0xA0, {{50, 0, -0.5f, 0x0000FFFFU}, {54, 0, -0.5f, 0x0000FFFFU}, {54, 4, -0.5f, 0x0000FFFFU}, {50, 4, -0.5f, 0x0000FFFFU}});
    // fill rule: a rectangle with edges on sample positions, every corner order
    const float o = 9.0f / 16.0f;
    t.quad(60 + o, 2 + o, 8, 4, -0.5f, 0x112233FFU);
    t.prim(0x80, {{80 + o, 6 + o, -0.5f, 0x445566FFU}, {80 + o, 2 + o, -0.5f, 0x445566FFU}, {88 + o, 2 + o, -0.5f, 0x445566FFU}, {88 + o, 6 + o, -0.5f, 0x445566FFU}});
    // big triangles with odd slopes
    t.prim(0x90, {{100.3f, 20.7f, -0.5f, 0xABCDEFFFU}, {400.9f, 33.1f, -0.5f, 0xABCDEFFFU}, {170.2f, 300.6f, -0.5f, 0xABCDEFFFU}});
    t.prim(0x90, {{639.0f, 527.0f, -0.5f, 0x123456FFU}, {300.5f, 527.9f, -0.5f, 0x123456FFU}, {500.25f, 200.75f, -0.5f, 0x123456FFU}});
    // depth: sloped z, LEQUAL with update, then an intersecting plane
    t.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));
    t.prim(0x90, {{10, 100, -0.9f, 0xFF00FFFFU}, {200, 110, -0.1f, 0xFF00FFFFU}, {30, 250, -0.5f, 0xFF00FFFFU}});
    t.bpw(bp::ZMODE, 1U | (3U << 1) | (1U << 4));
    t.prim(0x90, {{0, 120, -0.5f, 0x00FFFFFFU}, {220, 130, -0.5f, 0x00FFFFFFU}, {20, 260, -0.5f, 0x00FFFFFFU}});
    CHECK(t.run());
    const Diff d = t.diff();
    report("flat + fill rule + depth", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_interpolation_and_tev) {
    Twin t;
    t.scene();
    t.prim(0x90, {{0, 0, -0.5f, 0xFF0000FFU}, {200, 0, -0.5f, 0x00FF00FFU}, {0, 150, -0.5f, 0x0000FF80U}});
    // TEV: lerp C0/K0, subtract with bias/scale, compare
    t.bpw(0xE2, 200U | (60U << 12));
    t.bpw(0xE3, 50U | (10U << 12));
    t.bpw(0xE0, 0U | (1U << 23));
    t.bpw(0xE1, 255U | (255U << 12) | (1U << 23));
    t.bpw(bp::TEV_KSEL, (1U << 2) | (12U << 4));
    t.bpw(bp::TEV_COLOR_ENV, 15U | (13U << 4) | (14U << 8) | (2U << 12) | (1U << 19));
    t.quad(300, 0, 20, 20, -0.5f, 0xFFFFFFFFU);
    t.bpw(bp::TEV_COLOR_ENV, 2U | (15U << 4) | (15U << 8) | (12U << 12) | (1U << 16) | (1U << 18) | (1U << 19) | (1U << 20));
    t.quad(330, 0, 20, 20, -0.5f, 0xFFFFFFFFU);
    t.bpw(bp::TEV_COLOR_ENV, 15U | (12U << 4) | (14U << 8) | (2U << 12) | (3U << 16) | (1U << 19));
    t.quad(360, 0, 20, 20, -0.5f, 0xFFFFFFFFU);
    // two stages with rasterized colour * vertex-colour gradient
    t.bpw(bp::GENMODE, (1U << 4) | (1U << 10));
    t.bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19) | (2U << 22));
    t.bpw(bp::TEV_COLOR_ENV + 2, 15U | (14U << 4) | (4U << 8) | (15U << 12) | (1U << 19));
    t.bpw(bp::TEV_ALPHA_ENV + 2, (5U << 4) | (7U << 7) | (7U << 10) | (7U << 13) | (1U << 19));
    t.bpw(bp::TEV_KSEL, (1U << 2) | (4U << 14));
    t.prim(0x90, {{400, 0, -0.5f, 0xC86432FFU}, {600, 50, -0.5f, 0x3264C8FFU}, {420, 200, -0.5f, 0x808080FFU}});
    CHECK(t.run());
    const Diff d = t.diff();
    report("gradients + TEV", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_blend_alpha_formats) {
    Twin t;
    t.scene();
    t.bpw(bp::ALPHA_COMPARE, 100U | (4U << 16) | (0U << 19) | (1U << 22));
    t.quad(0, 0, 10, 10, -0.5f, 0xFF000064U);
    t.quad(12, 0, 10, 10, -0.5f, 0xFF000065U);
    t.bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));
    t.quad(30, 0, 10, 10, -0.5f, 0xFF0000FFU);
    t.bpw(bp::BLENDMODE, 0x18U | 1U | (4U << 8) | (5U << 5));
    t.quad(30, 0, 10, 10, -0.5f, 0x0000FFC0U);
    t.bpw(bp::BLENDMODE, 0x18U | 1U | 0x800U);
    t.quad(30, 0, 10, 5, -0.5f, 0x100000FFU);
    t.bpw(bp::BLENDMODE, 0x18U | 2U | (6U << 12));
    t.quad(30, 5, 10, 5, -0.5f, 0xFF00FFFFU);
    // RGBA6 with constant alpha and dither on a gradient
    t.bpw(bp::ZCOMPARE, 1);
    t.bpw(bp::BLENDMODE, 0x18U | 4U);
    t.prim(0x90, {{100, 0, -0.5f, 0xFF8040C0U}, {300, 0, -0.5f, 0x10305080U}, {100, 100, -0.5f, 0x808080FFU}});
    t.bpw(bp::CONSTANT_ALPHA, 0x100U | 0x04U);
    t.bpw(bp::BLENDMODE, 0x18U | 1U | (6U << 8) | (7U << 5));
    t.quad(150, 20, 40, 40, -0.5f, 0x20E020FFU);
    CHECK(t.run());
    const Diff d = t.diff();
    report("alpha test + blend + RGBA6", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_textures) {
    Twin t;
    t.scene();
    t.ram(0x2000, tile565());
    t.tex_scene(4, 4, 4, 0x2000);
    // nearest, repeat, mirror, bilinear, big magnified quad
    t.tprim(0x80, {{0, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {4, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {4, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {0, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    t.bpw(0x80, 1U | (1U << 2));
    t.tprim(0x80, {{10, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {18, 0, -0.5f, 0xFFFFFFFFU, 2, 0}, {18, 8, -0.5f, 0xFFFFFFFFU, 2, 2}, {10, 8, -0.5f, 0xFFFFFFFFU, 0, 2}});
    t.bpw(0x80, 2U | (2U << 2));
    t.tprim(0x80, {{20, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {28, 0, -0.5f, 0xFFFFFFFFU, 2, 0}, {28, 8, -0.5f, 0xFFFFFFFFU, 2, 2}, {20, 8, -0.5f, 0xFFFFFFFFU, 0, 2}});
    t.bpw(0x80, 1U << 4);
    t.tprim(0x80, {{40, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {104, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {104, 64, -0.5f, 0xFFFFFFFFU, 1, 1}, {40, 64, -0.5f, 0xFFFFFFFFU, 0, 1}});
    // mipmaps: 8x8 red level 0 + 4x4 blue level 1, point and linear
    std::vector<u8> img(160);
    for (std::size_t i = 0; i < 128; i += 2) { img[i] = 0xF8; img[i + 1] = 0x00; }
    for (std::size_t i = 128; i < 160; i += 2) { img[i] = 0x00; img[i + 1] = 0x1F; }
    t.ram(0x3000, img);
    t.bpw(0x88, 7U | (7U << 10) | (4U << 20));
    t.bpw(0x94, 0x3000U >> 5);
    t.bpw(bp::SU_SSIZE, 7); t.bpw(bp::SU_TSIZE, 7);
    t.bpw(0x84, 16U << 8);
    t.bpw(0x80, 1U << 5);
    t.tprim(0x80, {{120, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {124, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {124, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {120, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    t.bpw(0x84, 7U << 8);
    t.bpw(0x80, 2U << 5);
    t.tprim(0x80, {{130, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {134, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {134, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {130, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    // a minified, rotated quad: LOD changes across it
    t.bpw(0x84, 32U << 8);
    t.bpw(0x80, (1U << 4) | (1U << 7) | (2U << 5) | 1U | (1U << 2));
    t.tprim(0x80, {{200, 100, -0.5f, 0xFFFFFFFFU, 0, 0}, {300, 120, -0.5f, 0xFFFFFFFFU, 8, 0}, {280, 220, -0.5f, 0xFFFFFFFFU, 8, 8}, {190, 200, -0.5f, 0xFFFFFFFFU, 0, 8}});
    CHECK(t.run());
    const Diff d = t.diff();
    report("textures", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_fog_lines_points_zfreeze) {
    Twin t;
    t.scene();
    t.bpw(bp::FOG_COLOR, 0x0000FF);
    t.bpw(bp::FOG_PARAM0, (127U << 11) | 0x400U);
    t.bpw(bp::FOG_PARAM3, ((125U << 11) | 409U | (1U << 20)) | (4U << 21));
    t.prim(0x90, {{0, 0, -0.9f, 0xFF0000FFU}, {100, 0, -0.1f, 0xFF0000FFU}, {0, 100, -0.5f, 0xFF0000FFU}});
    t.bpw(bp::FOG_PARAM3, 0);
    t.bpw(bp::LINE_PT_WIDTH, 6U | (12U << 8));
    t.prim(0xA8, {{150, 10, -0.5f, 0x00FF00FFU}, {300, 60, -0.5f, 0x00FF00FFU}, {160, 20, -0.5f, 0xFF00FFFFU}, {165, 200, -0.5f, 0xFF00FFFFU}});
    t.prim(0xB8, {{320, 30, -0.5f, 0xFFFF00FFU}, {330.4f, 40.6f, -0.5f, 0xFFFF00FFU}});
    t.bpw(bp::ZMODE, 0x1F);
    t.prim(0x90, {{400, 0, -0.5f, 0xFFFFFFFFU}, {464, 0, -0.25f, 0xFFFFFFFFU}, {400, 8, -0.5f, 0xFFFFFFFFU}});
    t.bpw(bp::GENMODE, (1U << 4) | (1U << 19));
    t.quad(400, 20, 16, 8, -0.25f, 0x808080FFU);
    CHECK(t.run());
    const Diff d = t.diff();
    report("fog + lines + points + zfreeze", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_perspective_indirect_misc) {
    Twin t;
    t.scene();
    // perspective projection: colours (screen-linear) and texture coordinates (perspective)
    t.xff(xf::PROJECTION, {1.2f, 0.0f, 1.6f, 0.0f, -1.0f / 9.0f, -10.0f / 9.0f});   // GX perspective, near 1, far 10
    t.xfw(xf::PROJECTION + 6, 0);
    t.xff(0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    t.ram(0x2000, tile565());
    t.tex_scene(4, 4, 4, 0x2000);
    t.bpw(0x80, 1U | (1U << 2) | (1U << 4));
    t.bpw(bp::TEV_COLOR_ENV, 15U | (8U << 4) | (10U << 8) | (15U << 12) | (1U << 19)); // ras * tex
    t.tprim(0x80, {{-1.0f, -0.6f, -1.5f, 0xFF8000FFU, 0, 0}, {1.0f, -0.6f, -1.5f, 0x00FF80FFU, 4, 0},
                   {1.0f, 0.6f, -6.0f, 0x8000FFFFU, 4, 4}, {-1.0f, 0.6f, -6.0f, 0xFFFFFFFFU, 0, 4}});
    t.bpw(bp::ZMODE, 0x1F);
    t.tprim(0x90, {{-0.5f, -0.5f, -2.0f, 0xFFFFFFFFU, 0, 0}, {0.8f, -0.2f, -3.0f, 0xFFFFFFFFU, 2, 0}, {0.0f, 0.7f, -2.5f, 0xFFFFFFFFU, 0, 3}});
    // fog with range adjustment on the perspective scene
    t.bpw(bp::FOG_COLOR, 0x40C020);
    t.bpw(bp::FOG_PARAM0, 122U << 11);
    t.bpw(bp::FOG_B_MAGNITUDE, 0x100000); t.bpw(bp::FOG_B_EXPONENT, 4);
    t.bpw(bp::FOG_PARAM3, (124U << 11) | (5U << 21));
    t.bpw(bp::FOG_RANGE, 342U + 320U | (1U << 10));
    for (u32 k = 1; k <= 5; ++k) t.bpw(bp::FOG_RANGE + k, ((300U - 10U * k) << 12) | (280U + 7U * k));
    t.tprim(0x90, {{-0.9f, 0.2f, -1.8f, 0xFFFFFFFFU, 0, 0}, {0.9f, 0.3f, -2.2f, 0xFFFFFFFFU, 3, 0}, {0.1f, 0.9f, -4.0f, 0xFFFFFFFFU, 0, 3}});
    t.bpw(bp::FOG_PARAM3, 0);
    t.bpw(bp::FOG_RANGE, 0);
    // back to ortho: indirect texture offset (map 1 = I8 0xAB), alpha bump, z texture
    t.xff(xf::PROJECTION, {1.0f / 320.0f, -1.0f, 1.0f / 264.0f, -1.0f, 1.0f, 0.0f});
    t.xfw(xf::PROJECTION + 6, 1);
    t.ram(0x3000, std::vector<u8>(32, 0xAB));
    t.bpw(bp::TEV_COLOR_ENV, 8U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    t.bpw(0x89, 7U | (3U << 10) | (1U << 20));
    t.bpw(0x95, 0x3000U >> 5);
    t.bpw(bp::GENMODE, (1U << 4) | 1U | (1U << 16));
    t.bpw(bp::IREF, 1U);
    t.bpw(bp::IND_MTXA, 512U | (1U << 22)); t.bpw(bp::IND_MTXA + 1, 0); t.bpw(bp::IND_MTXA + 2, 1U << 22);
    for (u32 f = 0; f < 4; ++f) {
        t.bpw(bp::IND_CMD, (1U << 9) | (f << 2) | (1U << 7));
        const float x0 = 300.0f + 20.0f * static_cast<float>(f);
        t.tprim(0x80, {{x0, 300, -0.5f, 0xFFFFFFFFU, 0, 0}, {x0 + 16, 300, -0.5f, 0xFFFFFFFFU, 1, 0}, {x0 + 16, 316, -0.5f, 0xFFFFFFFFU, 1, 1}, {x0, 316, -0.5f, 0xFFFFFFFFU, 0, 1}});
    }
    t.bpw(bp::IND_CMD, 0);
    t.bpw(bp::GENMODE, (1U << 4) | 1U);
    t.bpw(bp::ZTEX2, 1U | (2U << 2));
    t.bpw(bp::BIAS, 0x100000);
    t.tprim(0x80, {{400, 400, -0.5f, 0xFFFFFFFFU, 0, 0}, {420, 400, -0.5f, 0xFFFFFFFFU, 1, 0}, {420, 420, -0.5f, 0xFFFFFFFFU, 1, 1}, {400, 420, -0.5f, 0xFFFFFFFFU, 0, 1}});
    t.bpw(bp::ZTEX2, 0);
    // a different scissor offset and rectangle
    t.bpw(bp::SCISSOR_OFFSET, 150U | (160U << 10));
    t.bpw(bp::SCISSOR_TL, (310U << 12) | 330U);
    t.bpw(bp::SCISSOR_BR, (500U << 12) | 450U);
    t.bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    t.bpw(bp::GENMODE, 1U << 4);
    t.cpl(0x50, (vcf::DIRECT << 9) | (vcf::DIRECT << 13));
    t.cpl(0x70, 1U | (4U << 1) | (5U << 14));
    t.prim(0x90, {{0, 0, -0.5f, 0xF0F0F0FFU}, {639, 0, -0.5f, 0x0F0F0FFFU}, {0, 527, -0.5f, 0x8040C0FFU}});
    CHECK(t.run());
    const Diff d = t.diff();
    report("perspective + indirect + ztex", d);
    // every part drew something (EFB coordinates of each part)
    const sw::Efb& r = t.sw.gpu.renderer().efb();
    auto drawn = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
        unsigned n = 0;
        for (u32 y = y0; y < y1; ++y) for (u32 x = x0; x < x1; ++x) n += r.color24(x, y) != 0U ? 1U : 0U;
        return n;
    };
    const unsigned persp = drawn(0, 0, 640, 290), ind = drawn(300, 300, 380, 316), zt = drawn(400, 400, 420, 420);
    std::printf("    drawn pixels: perspective part %u, indirect %u, ztex %u, depth at (410,410) %06X\n", persp, ind, zt, r.depth24(410, 410));
    CHECK(persp > 20000U); CHECK(ind > 900U); CHECK(zt > 300U);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
    sw::Efb g;
    t.gl.be->download(g);
    {   // an EFB copy reads back only the plane it uses (colour or depth)
        sw::Efb c, z;
        c.set_depth24(410, 410, 0x123456U);
        z.set_color24(410, 410, 0x654321U);
        t.gl.be->download(c, {0, 0, sw::kEfbWidth, sw::kEfbHeight}, true, false);
        t.gl.be->download(z, {0, 0, sw::kEfbWidth, sw::kEfbHeight}, false, true);
        CHECK_EQ(c.color24(410, 410), g.color24(410, 410));
        CHECK_EQ(c.depth24(410, 410), 0x123456U);
        CHECK_EQ(z.depth24(410, 410), g.depth24(410, 410));
        CHECK_EQ(z.color24(410, 410), 0x654321U);
    }
    CHECK(t.gl.be->packed_readback()); // the reads above went through the RGBA8 target
    unsigned ortho = 0;
    auto cmp = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
        unsigned n = 0;
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x)
                if (g.color24(x, y) != r.color24(x, y)) {
                    if (n == 0U) std::printf("    region (%u,%u): first diff (%u,%u) sw %06X gl %06X\n", x0, y0, x, y, r.color24(x, y), g.color24(x, y));
                    ++n;
                }
        ortho += n;
    };
    cmp(300, 300, 380, 316);   // indirect quads
    cmp(400, 400, 420, 420);   // z texture quad
    cmp(0, 400, 300, 528);     // untouched by the perspective part
    std::printf("    orthographic parts: %u colour diffs\n", ortho);
    CHECK_EQ(ortho, 0U);
}

TEST_CASE(gles_more_state) {
    Twin t;
    t.scene();
    // TEV: scale 1/2 without rounding, bias -128, invalid konst selection
    t.bpw(0xE2, 3U | (60U << 12));
    t.bpw(bp::TEV_COLOR_ENV, 15U | (13U << 4) | (2U << 8) | (15U << 12) | (1U << 19) | (3U << 20));
    t.quad(0, 0, 8, 8, -0.5f, 0xFFFFFFFFU);
    t.bpw(0xE2, 200U | (60U << 12));
    t.bpw(bp::TEV_COLOR_ENV, 2U | (15U << 4) | (15U << 8) | (15U << 12) | (2U << 16) | (1U << 19));
    t.quad(10, 0, 8, 8, -0.5f, 0xFFFFFFFFU);
    t.bpw(bp::TEV_COLOR_ENV, 14U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    t.bpw(bp::TEV_KSEL, (1U << 2) | (9U << 4));
    t.quad(20, 0, 8, 8, -0.5f, 0xFFFFFFFFU);
    t.bpw(bp::TEV_KSEL, 1U << 2);
    t.bpw(bp::TEV_COLOR_ENV, 10U | (15U << 4) | (15U << 8) | (15U << 12) | (1U << 19));
    // early z with update, then a failing alpha test: depth written, colour kept
    t.bpw(bp::ZCOMPARE, 0x40);
    t.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));
    t.bpw(bp::ALPHA_COMPARE, 200U | (4U << 16) | (0U << 19) | (1U << 22));
    t.quad(40, 0, 8, 8, -0.75f, 0x00FF0010U);
    t.bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));
    // early z test without update over the previous quad
    t.bpw(bp::ZMODE, 1U | (7U << 1));
    t.quad(40, 4, 8, 8, -0.1f, 0x808000FFU);
    // z test without update
    t.bpw(bp::ZCOMPARE, 0);
    t.bpw(bp::ZMODE, 1U | (7U << 1));
    t.quad(50, 0, 8, 8, -0.25f, 0x0000FFFFU);
    t.bpw(bp::ZMODE, 0);
    // z texture 8 and 24 bits
    std::vector<u8> img(64);
    for (u32 i = 0; i < 16; ++i) { img[2 * i] = 0x12; img[2 * i + 1] = 0x34; img[32 + 2 * i] = 0x56; img[33 + 2 * i] = 0x78; }
    t.ram(0x2000, img);
    t.tex_scene(6, 4, 4, 0x2000);
    t.bpw(bp::ZMODE, 0x1F);
    t.bpw(bp::BIAS, 0x100000);
    for (u32 f = 0; f < 3; ++f) {
        t.bpw(bp::ZTEX2, f | (2U << 2));
        const float x0 = 60.0f + 10.0f * static_cast<float>(f);
        t.tprim(0x80, {{x0, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {x0 + 4, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {x0 + 4, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {x0, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    }
    t.bpw(bp::ZTEX2, 0);
    t.bpw(bp::ZMODE, 0);
    // LOD bias and per-block LOD on a perspective mipmapped texture (8x8 red / 4x4 blue / 2x2 green)
    std::vector<u8> mip(128 + 32 + 32);
    for (std::size_t i = 0; i < 128; i += 2) { mip[i] = 0xF8; mip[i + 1] = 0x00; }
    for (std::size_t i = 128; i < 160; i += 2) { mip[i] = 0x00; mip[i + 1] = 0x1F; }
    for (std::size_t i = 160; i < 192; i += 2) { mip[i] = 0x07; mip[i + 1] = 0xE0; }
    t.ram(0x3000, mip);
    t.bpw(0x88, 7U | (7U << 10) | (4U << 20));
    t.bpw(0x94, 0x3000U >> 5);
    t.bpw(bp::SU_SSIZE, 7); t.bpw(bp::SU_TSIZE, 7);
    t.bpw(0x84, 32U << 8);
    t.bpw(0x80, 1U | (1U << 2) | (1U << 5) | (14U << 9));
    t.tprim(0x80, {{100, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {104, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {104, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {100, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    t.bpw(0x80, 1U | (1U << 2) | (1U << 5) | (16U << 9));
    t.tprim(0x80, {{110, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {114, 0, -0.5f, 0xFFFFFFFFU, 1, 0}, {114, 4, -0.5f, 0xFFFFFFFFU, 1, 1}, {110, 4, -0.5f, 0xFFFFFFFFU, 0, 1}});
    t.xff(xf::PROJECTION, {1.2f, 0.0f, 1.6f, 0.0f, -1.0f / 9.0f, -10.0f / 9.0f});
    t.xfw(xf::PROJECTION + 6, 0);
    t.bpw(0x80, 1U | (1U << 2) | (2U << 5) | (1U << 4) | (1U << 7));
    t.tprim(0x80, {{-1.0f, 0.0f, -1.5f, 0xFFFFFFFFU, 0, 0}, {1.0f, 0.0f, -1.5f, 0xFFFFFFFFU, 6, 0},
                   {1.0f, 0.6f, -8.0f, 0xFFFFFFFFU, 6, 12}, {-1.0f, 0.6f, -8.0f, 0xFFFFFFFFU, 0, 12}});
    CHECK(t.run());
    const Diff d = t.diff();
    report("TEV scale/bias, z, ztex, LOD", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_efb_copy_and_clear) {
    Twin t;
    t.scene();
    t.quad(0, 0, 32, 16, -0.5f, 0x3366CCFFU);
    t.bpw(bp::COPY_FILTER0, (21U << 12) | (22U << 18));
    t.bpw(bp::COPY_FILTER1, 21U);
    t.bpw(bp::EFB_TL, 0); t.bpw(bp::EFB_WH, 31U | (15U << 10));
    t.bpw(bp::EFB_ADDR, 0x100000U >> 5); t.bpw(bp::EFB_STRIDE, 4);
    t.bpw(bp::CLEAR_AR, 0x0011); t.bpw(bp::CLEAR_GB, 0x2233); t.bpw(bp::CLEAR_Z, 0x00ABCD);
    t.bpw(bp::ZMODE, 0x10);
    t.bpw(bp::TRIGGER_EFB_COPY, (12U << 3) | 0x800U);   // RGBA8 copy + clear
    t.quad(4, 4, 8, 8, -0.5f, 0xFF0000FFU);            // drawn over the clear on the GPU
    CHECK(t.run());
    std::vector<u8> a(1024), b(1024);
    CHECK(t.sw.bus.dma_read(0x100000, a) == mem::BusStatus::Ok);
    CHECK(t.gl.bus.dma_read(0x100000, b) == mem::BusStatus::Ok);
    CHECK(a == b);
    const Diff d = t.diff();
    report("copy + clear + redraw", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

// Display copies encoded on the GPU (GpuCopies): the RAM bytes, read back only
// when touched, must be exactly the software XFB encoder's, for every filter,
// gamma, scale, clamp and pixel format path; the GPU clear must match too.
TEST_CASE(gles_xfb_copies_on_the_gpu) {
    struct Case { const char* name; u32 fmt, trigger_bits, yscale, f0, f1, top, w, h, stride; };
    const Case cases[] = {
        {"plain 640x480", 0, 0, 256, (21U << 12) | (22U << 18), 21U, 0, 640, 480, 1280},
        {"filter + clamps", 0, 3U, 256, 8U | (8U << 6) | (10U << 12) | (12U << 18), 10U | (8U << 6) | (8U << 12), 10, 320, 200, 640},
        {"wrapping filter", 0, 0, 256, 63U | (63U << 6) | (63U << 12), 0U, 0, 64, 64, 128},
        {"gamma 1.7", 0, 1U << 7, 256, (21U << 12) | (22U << 18), 21U, 0, 200, 100, 416},
        {"gamma 2.2", 0, 2U << 7, 256, (21U << 12) | (22U << 18), 21U, 0, 200, 100, 416},
        {"y scale up", 0, 0, 128, (21U << 12) | (22U << 18), 21U, 0, 320, 200, 640},
        {"y scale invert", 0, 1U << 10, 512, (21U << 12) | (22U << 18), 21U, 0, 320, 200, 640},
        {"rows with gaps", 0, 0, 256, (21U << 12) | (22U << 18), 21U, 4, 100, 50, 1024},
        {"RGBA6", 1, 0, 256, (21U << 12) | (22U << 18), 21U, 0, 320, 240, 640},
        {"RGB565", 2, 0, 256, (21U << 12) | (22U << 18), 21U, 0, 320, 240, 640},
    };
    for (const Case& c : cases) {
        Twin t(true);
        t.scene();
        t.bpw(bp::ZCOMPARE, c.fmt);
        t.prim(0x80, {{0, 0, -0.5f, 0xFF2010FFU}, {640, 0, -0.5f, 0x10F040FFU}, {640, 528, -0.5f, 0x2040FFFFU}, {0, 528, -0.5f, 0xF0F0F0FFU}});
        t.quad(100, 60, 50, 30, -0.4f, 0x00FF80FFU);
        t.bpw(bp::COPY_FILTER0, c.f0);
        t.bpw(bp::COPY_FILTER1, c.f1);
        t.bpw(bp::COPY_YSCALE, c.yscale);
        t.bpw(bp::EFB_TL, c.top << 10); t.bpw(bp::EFB_WH, (c.w - 1U) | ((c.h - 1U) << 10));
        t.bpw(bp::EFB_ADDR, 0x100000U >> 5); t.bpw(bp::EFB_STRIDE, c.stride >> 5);
        t.bpw(bp::CLEAR_AR, 0x0011); t.bpw(bp::CLEAR_GB, 0x2233); t.bpw(bp::CLEAR_Z, 0x00ABCD);
        t.bpw(bp::ZMODE, 0x10);
        t.bpw(bp::TRIGGER_EFB_COPY, 0x4000U | 0x800U | c.trigger_bits);
        t.quad(4, 4, 8, 8, -0.5f, 0xFF0000FFU);
        CHECK(t.run());
        if (t.gl.gpu.gpu_xfb_copies() != 1U) std::printf("    %s: CPU path: %s\n", c.name, t.gl.gpu.last_fallback().c_str());
        CHECK_EQ(t.gl.gpu.gpu_xfb_copies(), 1U);
        CHECK(t.gl.bus.lazy_ranges() == 1U);
        std::vector<u8> a(0x100000), b(0x100000);
        CHECK(t.sw.bus.dma_read(0x100000, a) == mem::BusStatus::Ok);
        CHECK(t.gl.bus.dma_read(0x100000, b) == mem::BusStatus::Ok);
        CHECK(t.gl.bus.lazy_ranges() == 0U);
        unsigned diffs = 0;
        for (std::size_t i = 0; i < a.size(); ++i) diffs += a[i] != b[i] ? 1U : 0U;
        std::printf("    %s: XFB bytes that differ %u\n", c.name, diffs);
        CHECK_EQ(diffs, 0U);
        const Diff d = t.diff();
        report(c.name, d);
        CHECK_EQ(d.color, 0U);
        CHECK_EQ(d.depth, 0U);
    }
    {   // a newer copy over the same rows drops the older one without reading it back
        Twin t(true);
        t.scene();
        t.quad(0, 0, 64, 32, -0.5f, 0x3366CCFFU);
        for (int k = 0; k < 2; ++k) {
            t.bpw(bp::COPY_YSCALE, 256);
            t.bpw(bp::EFB_TL, 0); t.bpw(bp::EFB_WH, 63U | (31U << 10));
            t.bpw(bp::EFB_ADDR, 0x100000U >> 5); t.bpw(bp::EFB_STRIDE, 128U >> 5);
            t.bpw(bp::TRIGGER_EFB_COPY, 0x4000U);
            t.quad(0, 0, 64, 32, -0.5f, 0x80FF20FFU);
        }
        CHECK(t.run());
        CHECK(t.gl.bus.lazy_ranges() == 1U);
        const u64 fills = t.gl.bus.lazy_fills();
        std::vector<u8> a(128U * 32U), b(128U * 32U);
        CHECK(t.sw.bus.dma_read(0x100000, a) == mem::BusStatus::Ok);
        CHECK(t.gl.bus.dma_read(0x100000, b) == mem::BusStatus::Ok);
        CHECK(t.gl.bus.lazy_fills() == fills + 1U);
        CHECK(a == b);
    }
    {   // a CPU write inside a pending copy: the copy appears first, then the write
        Twin t(true);
        t.scene();
        t.quad(0, 0, 64, 32, -0.5f, 0x3366CCFFU);
        t.bpw(bp::COPY_YSCALE, 256);
        t.bpw(bp::EFB_TL, 0); t.bpw(bp::EFB_WH, 63U | (31U << 10));
        t.bpw(bp::EFB_ADDR, 0x100000U >> 5); t.bpw(bp::EFB_STRIDE, 128U >> 5);
        t.bpw(bp::TRIGGER_EFB_COPY, 0x4000U);
        CHECK(t.run());
        const std::vector<u8> w{1, 2, 3, 4};
        t.ram(0x100040, w);
        std::vector<u8> a(128U * 32U), b(128U * 32U);
        CHECK(t.sw.bus.dma_read(0x100000, a) == mem::BusStatus::Ok);
        CHECK(t.gl.bus.dma_read(0x100000, b) == mem::BusStatus::Ok);
        CHECK(a == b);
    }
}

// Bounding box and quad counters: the GLES backend (storage buffer + quad id image)
// must report exactly what the software renderer counts.
TEST_CASE(gles_bounding_box_and_counters) {
    Twin t;
    auto pe = [](Side& s, u32 off, bool& ok) { u64 v = 0; ok = s.pe.mmio_read(off, 2, v) && ok; return static_cast<u32>(v); };
    auto compare = [&](const char* what) {
        bool ok = true;
        unsigned diffs = 0;
        for (u32 off = 0x10; off <= 0x2A; off += 2) {
            const u32 a = pe(t.sw, off, ok), b = pe(t.gl, off, ok);
            if (a != b) { if (diffs++ < 4) std::printf("    %s: PE %02X sw %u gl %u\n", what, off, a, b); }
        }
        CHECK(ok);
        CHECK_EQ(diffs, 0U);
    };
    t.scene();
    CHECK(t.run());
    {   // the GLES backend has not tracked anything yet: a read before BP 0x55-0x57 stops
        u64 v = 0;
        CHECK(!t.gl.pe.mmio_read(0x10, 2, v));
        CHECK(t.gl.pe.mmio_error().find("BP 0x55") != std::string::npos);
        CHECK(!t.gl.pe.mmio_read(0x18, 2, v));
    }
    t.bpw(bp::CLEAR_BBOX1, 0x3FFU); t.bpw(bp::CLEAR_BBOX2, 0x3FFU); t.bpw(bp::CLEAR_PIXEL_PERF, 0);
    // z off: blend input only; a triangle and quads
    t.prim(0x90, {{0, 0, -0.5f, 0x204080FFU}, {8, 0, -0.5f, 0x204080FFU}, {0, 8, -0.5f, 0x204080FFU}});
    t.quad(101, 51, 7, 9, -0.5f, 0x00FF00FFU);
    CHECK(t.run());
    compare("z off");
    {
        bool ok = true;
        CHECK_EQ(pe(t.gl, 0x10, ok), 0U); CHECK_EQ(pe(t.gl, 0x12, ok), 107U);
        CHECK_EQ(pe(t.gl, 0x14, ok), 0U); CHECK_EQ(pe(t.gl, 0x16, ok), 59U);
        CHECK(ok);
    }
    // late z, LEQUAL with update; intersecting planes, odd slopes
    t.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));
    t.prim(0x90, {{10, 100, -0.9f, 0xFF00FFFFU}, {200, 110, -0.1f, 0xFF00FFFFU}, {30, 250, -0.5f, 0xFF00FFFFU}});
    t.bpw(bp::ZMODE, 1U | (3U << 1) | (1U << 4));
    t.prim(0x90, {{0, 120, -0.5f, 0x00FFFFFFU}, {220, 130, -0.5f, 0x00FFFFFFU}, {20, 260, -0.5f, 0x00FFFFFFU}});
    CHECK(t.run());
    compare("late z");
    // early z + alpha test that rejects half of a gradient; BP 0x55 rewritten in between
    t.bpw(bp::ZCOMPARE, 0x40);
    t.bpw(bp::CLEAR_BBOX1, 300U | (310U << 10));
    t.bpw(bp::ALPHA_COMPARE, 0x80U | (4U << 16) | (0U << 22) | (7U << 19)); // a > 128 OR never -> a > 128
    t.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));   // ALWAYS with update: early z passes
    t.prim(0x90, {{300.3f, 300.2f, -0.6f, 0xFFFFFF00U}, {500.7f, 310.9f, -0.6f, 0xFFFFFFFFU}, {320.1f, 480.4f, -0.6f, 0xFFFFFF80U}});
    t.bpw(bp::ZMODE, 1U | (1U << 1) | (1U << 4));   // LESS: equal depth fails
    t.prim(0x90, {{300.3f, 300.2f, -0.6f, 0xFFFFFFFFU}, {500.7f, 310.9f, -0.6f, 0xFFFFFFFFU}, {320.1f, 480.4f, -0.6f, 0xFFFFFFFFU}});
    CHECK(t.run());
    compare("early z + alpha test");
    {
        bool ok = true;
        std::printf("    counters after early z + alpha test: zloc in %u out %u, z in %u out %u, blend %u; box %u..%u x %u..%u\n",
                    pe(t.sw, 0x18, ok), pe(t.sw, 0x1C, ok), pe(t.sw, 0x20, ok), pe(t.sw, 0x24, ok), pe(t.sw, 0x28, ok),
                    pe(t.sw, 0x10, ok), pe(t.sw, 0x12, ok), pe(t.sw, 0x14, ok), pe(t.sw, 0x16, ok));
        CHECK(pe(t.sw, 0x18, ok) > 0U && pe(t.sw, 0x1C, ok) > 0U && pe(t.sw, 0x1C, ok) < pe(t.sw, 0x18, ok));
        CHECK(pe(t.sw, 0x20, ok) > 0U && pe(t.sw, 0x28, ok) > pe(t.sw, 0x24, ok));
        CHECK(pe(t.sw, 0x12, ok) > 310U); // the box grew past the value written by BP 0x55
        CHECK(ok);
    }
    // counters cleared again, then more draws across a scissor offset change (several passes)
    t.bpw(bp::CLEAR_PIXEL_PERF, 0);
    t.bpw(bp::ALPHA_COMPARE, (7U << 16) | (7U << 19));
    t.bpw(bp::ZMODE, 1U | (7U << 1));
    t.bpw(bp::SCISSOR_OFFSET, 100U | (90U << 10));
    t.prim(0x90, {{150.5f, 140.25f, -0.4f, 0x808080FFU}, {420.75f, 150.5f, -0.4f, 0x808080FFU}, {200.0f, 400.5f, -0.4f, 0x808080FFU}});
    CHECK(t.run());
    compare("after clear, scissor offset");
    // BP 0x56 right after a draw (no PE read in between): left/right must keep what the
    // GPU accumulated, so the backend has to be synchronised before the CPU write.
    t.bpw(bp::SCISSOR_OFFSET, 171U | (171U << 10)); // back to EFB = model coordinates
    t.bpw(bp::CLEAR_BBOX1, 0x3FFU);
    t.quad(601, 11, 5, 3, -0.4f, 0x123456FFU);
    t.bpw(bp::CLEAR_BBOX2, 7U | (8U << 10));
    CHECK(t.run());
    compare("BP 0x56 after a draw");
    {
        bool ok = true;
        CHECK_EQ(pe(t.gl, 0x10, ok), 600U); CHECK_EQ(pe(t.gl, 0x12, ok), 605U);
        CHECK_EQ(pe(t.gl, 0x14, ok), 7U); CHECK_EQ(pe(t.gl, 0x16, ok), 8U);
        CHECK(ok);
    }
    const Diff d = t.diff();
    report("bbox/counter scenes", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

// CPU EFB peeks/pokes through the GLES backend give the software results.
TEST_CASE(gles_efb_cpu_access) {
    Twin t;
    t.scene();
    t.bpw(bp::ZMODE, 1U | (7U << 1) | (1U << 4));
    t.prim(0x90, {{10, 100, -0.9f, 0xFF00FFFFU}, {200, 110, -0.1f, 0x00FF80FFU}, {30, 250, -0.5f, 0x8040C0FFU}});
    CHECK(t.run());
    EfbAccess a_sw(t.sw.gpu, t.sw.pe), a_gl(t.gl.gpu, t.gl.pe);
    auto addr = [](u32 x, u32 y, bool z) { return (y << 12) | (x << 2) | (z ? 0x400000U : 0U); };
    unsigned diffs = 0, nonzero = 0;
    for (u32 y = 90; y < 260; y += 7)
        for (u32 x = 0; x < 220; x += 9)
            for (bool z : {false, true}) {
                u64 a = 0, b = 0;
                CHECK(a_sw.mmio_read(addr(x, y, z), 4, a));
                CHECK(a_gl.mmio_read(addr(x, y, z), 4, b));
                diffs += a != b ? 1U : 0U;
                nonzero += (z && a != 0U) ? 1U : 0U;
            }
    CHECK_EQ(diffs, 0U);
    CHECK(nonzero > 50U); // the probes really hit the triangle
    // pokes on both sides, then the whole EFB is compared
    for (u32 i = 0; i < 40; ++i) {
        CHECK(a_sw.mmio_write(addr(300 + i, 400 - i, false), 4, 0xA1B2C3D4U + i * 0x010203U));
        CHECK(a_gl.mmio_write(addr(300 + i, 400 - i, false), 4, 0xA1B2C3D4U + i * 0x010203U));
        CHECK(a_sw.mmio_write(addr(300 + i, 400 - i, true), 4, 0x00123456U * i));
        CHECK(a_gl.mmio_write(addr(300 + i, 400 - i, true), 4, 0x00123456U * i));
    }
    const Diff d = t.diff();
    report("EFB peek/poke", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_timing_report) {
    // not a check: time of 64 textured full-screen quads (2 triangles each) on both paths.
    // llvmpipe runs the "GPU" on this machine's CPU, so this says nothing about a phone.
    Twin t;
    t.scene();
    t.ram(0x2000, tile565());
    t.tex_scene(4, 4, 4, 0x2000);
    t.bpw(0x80, 1U | (1U << 2) | (1U << 4));
    for (int i = 0; i < 64; ++i)
        t.tprim(0x80, {{0, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {640, 0, -0.5f, 0xFFFFFFFFU, 40, 0},
                       {640, 528, -0.5f, 0xFFFFFFFFU, 40, 33}, {0, 528, -0.5f, 0xFFFFFFFFU, 0, 33}});
    const std::vector<u8> c = t.cmd;
    t.cmd.clear();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(t.sw.gpu.feed(c));
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(t.gl.gpu.feed(c));
    sw::Efb e;
    t.gl.be->download(e);   // waits for the GPU
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("    64 textured full-screen quads (21.6 M pixels): software %.1f ms, GLES (llvmpipe) %.1f ms\n", ms(t0, t1), ms(t1, t2));
    const Diff d = t.diff();
    CHECK_EQ(d.color, 0U);
}

// Texture cache invalidation: the GLES backend skips hashing while no write touched the
// texture's RAM pages (bus write epochs); every kind of write must still be seen.
TEST_CASE(gles_texture_cache_invalidation) {
    Twin t;
    t.scene();
    std::vector<u8> img(64U * 64U * 2U);
    for (std::size_t i = 0; i < img.size(); ++i) img[i] = static_cast<u8>(i * 13U + 7U);
    t.ram(0x30000, img);                       // 8 KiB: pages 0x30 and 0x31
    t.tex_scene(4, 64, 64, 0x30000);
    t.bpw(0x80, 1U | (1U << 2) | (1U << 4));
    auto draw = [&](float x) {
        t.tprim(0x80, {{x, 0, -0.5f, 0xFFFFFFFFU, 0, 0}, {x + 64, 0, -0.5f, 0xFFFFFFFFU, 1, 0},
                       {x + 64, 64, -0.5f, 0xFFFFFFFFU, 1, 1}, {x, 64, -0.5f, 0xFFFFFFFFU, 0, 1}});
        CHECK(t.run());
    };
    auto uploads = [&] { return t.gl.be->texture_uploads(); };
    draw(0);
    const u64 u0 = uploads();
    draw(70);                                  // unchanged: no upload
    CHECK_EQ(uploads(), u0);
    // 1. CPU-style store into the second page of the texture
    for (Side* s : {&t.sw, &t.gl}) CHECK(s->bus.write32(0x31F00, 0x12345678U) == mem::BusStatus::Ok);
    draw(140);
    CHECK_EQ(uploads(), u0 + 1U);
    // 2. same bytes written again: the pages are stamped, the hash finds the same texels;
    //    the next draw needs no hashing at all
    for (Side* s : {&t.sw, &t.gl}) CHECK(s->bus.write32(0x31F00, 0x12345678U) == mem::BusStatus::Ok);
    const u64 h0 = t.gl.be->texture_hashes();
    draw(210);
    CHECK_EQ(uploads(), u0 + 1U);
    CHECK_EQ(t.gl.be->texture_hashes(), h0 + 1U);
    draw(215);
    CHECK_EQ(t.gl.be->texture_hashes(), h0 + 1U);
    // 3. a write next to the texture (another page): no upload
    for (Side* s : {&t.sw, &t.gl}) CHECK(s->bus.write32(0x32000, 0xFFFFFFFFU) == mem::BusStatus::Ok);
    draw(280);
    CHECK_EQ(uploads(), u0 + 1U);
    // 4. external writer through ram_pointer + notify_external_write (as DMA engines do)
    for (Side* s : {&t.sw, &t.gl}) {
        u8* p = s->bus.ram_pointer(0x30010, 16);
        CHECK(p != nullptr);
        for (int i = 0; i < 16; ++i) p[i] = static_cast<u8>(0xF0 + i);
        s->bus.notify_external_write(0x30010, 16, mem::AccessSource::Dma);
    }
    draw(350);
    CHECK_EQ(uploads(), u0 + 2U);
    // 5. DMA block write
    const std::vector<u8> blk(256, 0x5A);
    for (Side* s : {&t.sw, &t.gl}) CHECK(s->bus.dma_write(0x30800, blk) == mem::BusStatus::Ok);
    draw(420);
    CHECK_EQ(uploads(), u0 + 3U);
    const Diff d = t.diff();
    report("texture cache invalidation", d);
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

TEST_CASE(gles_texture_cost_report) {
    // not a check of speed: CPU time of the GLES backend for 400 small draws that all
    // sample the same 512x512 RGB565 texture (the cost is the per-draw texture work,
    // not pixels). Used to compare texture-cache changes on the same machine.
    Twin t;
    t.scene();
    std::vector<u8> big(512U * 512U * 2U);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 37U + (i >> 9));
    t.ram(0x10000, big);
    t.tex_scene(4, 512, 512, 0x10000);
    t.bpw(0x80, 1U | (1U << 2) | (1U << 4));
    for (int i = 0; i < 400; ++i) {
        const float x = static_cast<float>((i % 20) * 30), y = static_cast<float>((i / 20) * 25);
        t.tprim(0x90, {{x, y, -0.5f, 0xFFFFFFFFU, 0, 0}, {x + 20, y, -0.5f, 0xFFFFFFFFU, 1, 0}, {x, y + 20, -0.5f, 0xFFFFFFFFU, 0, 1}});
    }
    const std::vector<u8> c = t.cmd;
    t.cmd.clear();
    CHECK(t.sw.gpu.feed(c));
    const u64 uploads_before = t.gl.be->texture_uploads();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(t.gl.gpu.feed(c));
    sw::Efb e;
    t.gl.be->download(e);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("    400 draws with one 512x512 RGB565 texture: GLES backend %.1f ms, %llu texture upload(s)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                static_cast<unsigned long long>(t.gl.be->texture_uploads() - uploads_before));
    const Diff d = t.diff();
    CHECK_EQ(d.color, 0U);
    CHECK_EQ(d.depth, 0U);
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(gles_gpu_draw_program) {
    // the PowerPC drawing program on the whole System, software vs GLES: same XFB bytes
    std::ifstream in(ORCA_PROGRAMS_DIR "/gpu_draw.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    std::vector<u8> xfb[3];
    for (int side = 0; side < 3; ++side) {
        system::System sys({});
        test::TestPort port;
        port.cpu = &sys.cpu();
        sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
        gles::Backend be;
        if (side >= 1) {
            std::string e;
            CHECK(be.init(e));
            sys.gpu().set_backend(&be, [&be](sw::Efb& f, const sw::EfbRect& r, bool c, bool d) { be.download(f, r, c, d); }, [&be](const sw::Efb& f, const sw::EfbRect& r) { be.upload(f, r); });
            if (side == 2) sys.gpu().set_gpu_copies(gles::gpu_copies(be));
        }
        std::string err;
        CHECK(sys.load_executable(file, err));
        const auto r = sys.run(200000000ULL);
        CHECK(r.stop == ppc::StepResult::Halted);
        if (side >= 1) CHECK(be.draws() >= 2U);
        if (side == 2) CHECK(sys.gpu().gpu_xfb_copies() >= 1U);
        xfb[side].resize(1280U * 480U);
        CHECK(sys.bus().dma_read(0x00400000, xfb[side]) == mem::BusStatus::Ok);
        if (side >= 1) sys.gpu().set_backend(nullptr, nullptr, nullptr);
    }
    for (int side = 1; side < 3; ++side) {
        unsigned diffs = 0;
        for (std::size_t i = 0; i < xfb[0].size(); ++i) diffs += xfb[0][i] != xfb[side][i] ? 1U : 0U;
        std::printf("    gpu_draw XFB bytes that differ (%s): %u of %zu\n", side == 1 ? "CPU copies" : "GPU copies", diffs,
                    xfb[0].size());
        CHECK_EQ(diffs, 0U);
    }
}
#endif

int main() {
    gles::HeadlessContext ctx;
    std::string e;
    if (!ctx.create(e)) {
        std::printf("[gles] skipped: %s\n", e.c_str());
        return 77;
    }
    std::printf("[gles] %s\n", ctx.renderer_name().c_str());
    gles::Backend probe;
    if (!probe.init(e)) {
        std::printf("[gles] skipped: %s\n", e.c_str());
        return 77;
    }
    g_ctx = &ctx;
    return ::orca::test::run_all("gles");
}
