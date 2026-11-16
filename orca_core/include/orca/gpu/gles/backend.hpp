// Orca Core — OpenGL ES 3.2 backend for the GPU (the renderer for phones).
//
// Division of work:
//  * The reference geometry stage of sw::Renderer (vertex transform, clipping, culling,
//    perspective divide, lines/points, z slope for z freeze) runs on the CPU and hands
//    screen-space triangles to this backend through sw::TriangleSink.
//  * The GPU rasterizes them: vertices are snapped to 1/16 pixel exactly like the software
//    rasterizer and shifted so that GL's pixel-centre sample is the software sample point
//    (p + 9/16).
//  * The fragment shader (gles/shaders.cpp) is a port of the software per-pixel stage driven
//    by the BP registers in uniforms (one program for every state: no shader compilation
//    while a game runs). The EFB colour and depth are two R32UI attachments holding the same
//    24-bit values as sw::Efb, read and written in the shader through
//    EXT_shader_framebuffer_fetch, so Z, blending, logic ops, dither and the EFB formats are
//    integer-exact. On tile-based GPUs (Mali, Adreno, PowerVR) the fetch reads the tile memory.
//  * Textures are decoded on the CPU to RGBA8 (all mip levels stacked in one RGBA8UI texture
//    per map, with the level sizes the hardware uses: (size - 1) >> level + 1) and sampled
//    with texelFetch using the software renderer's integer filtering.
//  * EFB copies and clears use the reference CPU code: the EFB is downloaded before and
//    uploaded after (slow; to be moved to the GPU).
//
//  * Attributes: the GPU only rasterizes (coverage). For every triangle the CPU writes the
//    software rasterizer's slope context and the (dfdx, dfdy, f0) of z, 1/w, both colours and
//    the 8 texture coordinates to a texture buffer; the fragment shader reads them with
//    gl_PrimitiveID and evaluates them in the same order with `precise` (no fused
//    multiply-add), including the 2x2-block texture coordinates for the LOD. On llvmpipe the
//    result is bit-identical to the software reference in every test scene (tests/test_gles.cpp).
//
// Known differences from the software reference:
//  * GLSL ES allows division and exp2 with a few ULP of error: GPUs that use them can differ
//    in the last bit of a texture coordinate or fog factor;
//  * fog with a zero denominator or a NaN factor follows IEEE float instead of stopping;
//  * with several scissor ranges (scissor offsets beyond the EFB) the attribute slopes of the
//    principal range are used for all of them;
//  * pixels exactly on a triangle edge follow the GPU's fill rule (identical on llvmpipe).
// Requires OpenGL ES 3.2 with GL_EXT_shader_framebuffer_fetch; init() reports otherwise.
#pragma once
#include "orca/gpu/sw/renderer.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace orca::gpu::gles {

// An EGL context without a window (Mesa "surfaceless" platform), for tests and tools.
// On Android the app creates the context and makes it current before Backend::init.
class HeadlessContext {
public:
    HeadlessContext() = default;
    HeadlessContext(const HeadlessContext&) = delete;
    HeadlessContext& operator=(const HeadlessContext&) = delete;
    ~HeadlessContext();
    bool create(std::string& error);
    std::string renderer_name() const;

private:
    void* display_{nullptr};
    void* context_{nullptr};
};

class Backend final : public sw::TriangleSink {
public:
    Backend();
    ~Backend() override;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // Creates the GL objects; a GL ES 3.2 context must be current.
    bool init(std::string& error);

    void triangle(const sw::ScreenVertex& a, const sw::ScreenVertex& b, const sw::ScreenVertex& c) override;
    bool flush(const BpMemory& bp, const XfMemory& xf, const sw::TevState& tev, const sw::TexSource& tex,
               const sw::SlopeState& zslope, std::string& error) override;

    // EFB transfer (24-bit colour and depth values, same layout as sw::Efb).
    void download(sw::Efb& efb);
    void upload(const sw::Efb& efb);

    u64 draws() const noexcept { return draws_; }
    u64 texture_uploads() const noexcept { return tex_uploads_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::array<sw::ScreenVertex, 3>> tris_;
    std::vector<float> verts_;
    std::vector<float> slopes_;
    u64 draws_{0};
    u64 tex_uploads_{0};
};

} // namespace orca::gpu::gles
