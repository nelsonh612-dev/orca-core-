// Orca Core — software rasterizer (the accuracy reference for the GPU backends).
//
// Behaviour taken from Dolphin's software renderer, read as information only:
// SetupUnit.cpp (primitive assembly), Clipper.cpp (clip planes, culling,
// perspective divide + viewport), Rasterizer.cpp (fixed-point edge functions,
// 2x2 blocks, slope interpolation), Tev.cpp (combiner arithmetic, konst and
// swap tables, indirect texturing, alpha test, z texture), SWEfbInterface.cpp
// (EFB layout, z compare, blending, logic ops, dither); BPFunctions.cpp
// (scissor rectangles: nine 1024-pixel shifts of the offset, product of the
// x and y ranges). The code is Orca's own.
// Textures (TextureSampler.cpp): wrap clamp/repeat/mirror (3 = clamp, per the
// hardware note in BPMemory.h), nearest/bilinear with 7-bit fractions, LOD from
// the 2x2 block (FixedLog2, bias, min/max), point/linear mip selection, images
// in main memory (base << 5) or TMEM (manually managed cache), palettes in TMEM;
// RGBA8 in TMEM keeps AR and GB halves in two banks (TX_SETIMAGE1/2 lines), read as in
// TextureDecoder_Common.cpp TexDecoder_DecodeTexelRGBA8FromTmem.
// Indirect texture order in samples: S = alpha, T = blue, U = green (the
// decoder outputs R, G, B, A).
// Lines and points (Clipper.cpp ProcessLine/ProcessPoint): quads widened by
// BP 22 sizes (1/6 pixel), caps horizontal or vertical by the line's major
// axis, texture coordinate offsets per BP 30+2i bits 18/19; points are not
// clipped (as in Dolphin's software renderer, which notes this as a TODO).
// Z freeze (GENMODE bit 19): the depth of every pixel comes from the slope of
// the last triangle drawn or culled before freezing (also lines/points), as in
// Rasterizer.cpp UpdateZSlope and Clipper.cpp.
// The line/point aspect ratio bit (BP 22 bit 22) is ignored, as in Dolphin (the SDK sets it
// with the field mode; its effect is not documented). Fog follows Tev.cpp with A and C decoded
// as in BPMemory.cpp (11-bit mantissa floats, the infinite/NaN table).
// Not implemented yet (each stops with a message): fog with B_SHF >= 32 or a zero denominator,
// mipmapped RGBA8 textures in TMEM, pixel formats other than RGB8_Z24 / RGBA6_Z24 / RGB565_Z16 /
// Z24.
// Like Dolphin's software renderer, RGB565_Z16 keeps 8-bit colour and 24-bit
// depth (not exact hardware behaviour).
#pragma once
#include "orca/gpu/pixel_stats.hpp"
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/command_stream.hpp"
#include "orca/gpu/sw/vertex.hpp"
#include "orca/gpu/xf_memory.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace orca::gpu::sw {

inline constexpr u32 kEfbWidth = 640, kEfbHeight = 528;

// TEV registers and constants as set through BP 0xE0-0xE7 (11-bit signed), RGBA.
struct TevState {
    std::array<std::array<s16, 4>, 4> reg{};   // PREV, C0, C1, C2
    std::array<std::array<s16, 4>, 4> konst{}; // K0-K3
};

class Efb {
public:
    Efb() : mem_(static_cast<std::size_t>(kEfbWidth) * kEfbHeight * 6U, 0) {}
    u32 color24(u32 x, u32 y) const { return get(off(x, y)); }
    u32 depth24(u32 x, u32 y) const { return get(off(x, y) + kDepth); }
    void set_color24(u32 x, u32 y, u32 v) { put(off(x, y), v); }
    void set_depth24(u32 x, u32 y, u32 v) { put(off(x, y) + kDepth, v); }

private:
    static constexpr std::size_t kDepth = static_cast<std::size_t>(kEfbWidth) * kEfbHeight * 3U;
    static std::size_t off(u32 x, u32 y) { return (static_cast<std::size_t>(y) * kEfbWidth + x) * 3U; }
    u32 get(std::size_t o) const { return mem_[o] | (u32{mem_[o + 1]} << 8) | (u32{mem_[o + 2]} << 16); }
    void put(std::size_t o, u32 v) {
        mem_[o] = static_cast<u8>(v); mem_[o + 1] = static_cast<u8>(v >> 8); mem_[o + 2] = static_cast<u8>(v >> 16);
    }
    std::vector<u8> mem_;
};

// Where textures come from: main memory through the bus, palettes/caches in TMEM.
struct TexSource {
    mem::Bus* bus{nullptr};
    u32 phys_mask{0x03FFFFFFU};
    std::span<const u8> tmem;
};

// Depth slope kept between primitives for z freeze (Rasterizer.cpp ZSlope;
// the default before any primitive is Dolphin's guess: constant 1.0).
struct SlopeState {
    float dfdx{0}, dfdy{0}, f0{1};
    s32 x0{0}, y0{0};
    float xoff{0}, yoff{0};
};

// A vertex after clipping, perspective divide and viewport (screen coordinates before the
// scissor offset), as handed to the rasterizer.
struct ScreenVertex {
    std::array<float, 4> clip;
    std::array<float, 3> screen;
    std::array<std::array<float, 4>, 2> color; // RGBA, 0..255
    std::array<std::array<float, 3>, 8> tex;
};

// Receives the triangles of a draw instead of the software rasterizer (GPU backends). The
// geometry stage (clipping, culling, lines/points, z slope tracking) stays the reference one.
class TriangleSink {
public:
    virtual ~TriangleSink() = default;
    virtual void triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) = 0;
    // Called at the end of every draw with the state it used and the current z slope.
    virtual bool flush(const BpMemory& bp, const XfMemory& xf, const TevState& tev, const TexSource& tex,
                       const SlopeState& zslope, std::string& error) = 0;
    // Pixel Engine statistics (bounding box, quad counters). A sink that can track them
    // returns true from attach_stats and keeps `stats` current when sync_stats is called
    // (before the CPU reads a PE register). The default sink cannot.
    virtual bool attach_stats(PixelStats* /*stats*/) { return false; }
    virtual bool sync_stats(bool /*bbox*/, std::string& error) {
        error = "the GPU backend does not track the bounding box / counters";
        return false;
    }
};

class Renderer {
public:
    // With a sink, triangles go to it and the EFB of this renderer is not drawn.
    void set_sink(TriangleSink* sink) noexcept { sink_ = sink; }
    TriangleSink* sink() const noexcept { return sink_; }
    // Draws one primitive of already transformed vertices.
    bool draw(Primitive prim, std::span<const OutputVertex> verts, const BpMemory& bp, const XfMemory& xf,
              const TevState& tev, const TexSource& tex, std::string& error);
    Efb& efb() noexcept { return efb_; }
    const Efb& efb() const noexcept { return efb_; }
    u64 pixels_written() const noexcept { return pixels_; }
    // Bounding box and quad counters (owned by the Pixel Engine); nullptr = not tracked.
    void set_stats(PixelStats* stats) noexcept { stats_ = stats; }

    // EFB colour as RGBA8 for the current pixel format (exposed for copies/tests).
    static void efb_color_rgba(u32 c24, u32 pixel_format, u8 out[4]);

private:
    using Vtx = ScreenVertex;
    TriangleSink* sink_{nullptr};
    bool triangle(const Vtx& a, const Vtx& b, const Vtx& c);
    bool line(Vtx a, Vtx b);
    bool point(Vtx c);
    void divide(Vtx& t) const;
    bool raster(const Vtx& v0, const Vtx& v1, const Vtx& v2);
    using Uv = std::array<std::array<float, 2>, 8>;
    struct Lod { s32 lod{0}; bool linear{false}; };
    bool pixel(s32 x, s32 y, float z, const std::array<std::array<float, 4>, 2>& col, const Uv& uv,
               const std::array<Lod, 16>& tex_lod, const std::array<Lod, 4>& ind_lod);
    Lod calc_lod(u32 texmap, u32 texcoord, const Uv& p00, const Uv& p10, const Uv& p01) const;
    bool sample(s32 s, s32 t, Lod lod, u32 texmap, u8 out[4]);
    bool sample_mip(s32 s, s32 t, s32 mip, bool linear, u32 texmap, u8 out[4]);

    Efb efb_;
    const BpMemory* bp_{nullptr};
    const XfMemory* xf_{nullptr};
    const TevState* tev_{nullptr};
    const TexSource* tex_{nullptr};
    std::string err_;
    u64 pixels_{0};
    PixelStats* stats_{nullptr};
    u8 quad_stages_{0}; // PixelStats counters reached by the current 2x2 quad
    // per-triangle setup
    s32 xoff_{0}, yoff_{0};
    SlopeState zslope_;
};

} // namespace orca::gpu::sw
