// Orca Core — OpenGL ES 3.2 backend. See include/orca/gpu/gles/backend.hpp.
#ifndef EGL_NO_X11
#define EGL_NO_X11
#endif
#include "orca/gpu/gles/backend.hpp"
#include "orca/gpu/texture_decoder.hpp"
#include "orca/gpu/sw/efb_copy.hpp"
#include "gles_shaders.hpp"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <array>

namespace orca::gpu::gles {
namespace {

constexpr int kW = static_cast<int>(sw::kEfbWidth), kH = static_cast<int>(sw::kEfbHeight);
constexpr std::size_t kTexelsPerTriangle = 35;

int iround(float x) {
    const int t = static_cast<int>(x);
    return (x - static_cast<float>(t)) >= 0.5f ? t + 1 : t;
}

// The software rasterizer's slope context and attribute slopes (sw/renderer.cpp SlopeCtx /
// Slope) for the scissor offset (xo, yo), written as 35 vec4: context, then (dfdx, dfdy, f0, 0)
// for z, 1/w, colour 0 RGBA, colour 1 RGBA and tex * (1/w) for 8 x STQ.
void triangle_slopes(const sw::ScreenVertex& v0, const sw::ScreenVertex& v1, const sw::ScreenVertex& v2, s32 xo, s32 yo,
                     std::vector<float>& out) {
    const s32 X1 = iround(16.0f * (v0.screen[0] - static_cast<float>(xo))) - 9;
    const s32 Y1 = iround(16.0f * (v0.screen[1] - static_cast<float>(yo))) - 9;
    const s32 x0 = (X1 + 0xF) >> 4, y0 = (Y1 + 0xF) >> 4;
    const float xoff = (static_cast<float>(x0) - (v0.screen[0] - static_cast<float>(xo))) + 0.495f;
    const float yoff = (static_cast<float>(y0) - (v0.screen[1] - static_cast<float>(yo))) + 0.495f;
    const float dx10 = v1.screen[0] - v0.screen[0], dx20 = v2.screen[0] - v0.screen[0];
    const float dy10 = v1.screen[1] - v0.screen[1], dy20 = v2.screen[1] - v0.screen[1];
    out.insert(out.end(), {xoff, yoff, static_cast<float>(x0), static_cast<float>(y0)});
    auto slope = [&](float a, float b, float c) {
        const float d20 = c - a, d10 = b - a;
        const float A = d20 * dy10 - d10 * dy20;
        const float B = dx20 * d10 - dx10 * d20;
        const float C = dx20 * dy10 - dx10 * dy20;
        out.insert(out.end(), {A / C, B / C, a, 0.0f});
    };
    slope(v0.screen[2], v1.screen[2], v2.screen[2]);
    const float w[3] = {1.0f / v0.clip[3], 1.0f / v1.clip[3], 1.0f / v2.clip[3]};
    slope(w[0], w[1], w[2]);
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned c = 0; c < 4; ++c) slope(v0.color[i][c], v1.color[i][c], v2.color[i][c]);
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned c = 0; c < 3; ++c) slope(v0.tex[i][c] * w[0], v1.tex[i][c] * w[1], v2.tex[i][c] * w[2]);
}

u32 bits(u32 v, unsigned sh, unsigned n) { return (v >> sh) & ((1U << n) - 1U); }

u64 fnv(u64 h, const u8* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ULL; }
    return h;
}
u64 fnv32(u64 h, u32 v) {
    const u8 b[4] = {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
    return fnv(h, b, 4);
}

GLuint compile(GLenum type, const char* src, std::string& error) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

u32 tex_reg(const BpMemory& bp, u32 unit, u32 r) {
    return bp.reg(static_cast<u8>(0x80U | (unit & 3U) | (r << 2) | ((unit & 4U) << 3)));
}
u32 nibbles(TexFormat f) {
    switch (f) {
    case TexFormat::I4: case TexFormat::C4: case TexFormat::CMPR: return 1;
    case TexFormat::I8: case TexFormat::IA4: case TexFormat::C8: return 2;
    case TexFormat::RGBA8: return 8;
    default: return 4;
    }
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

// ------------------------------------------------------------------ headless context

HeadlessContext::~HeadlessContext() {
    if (display_ != nullptr) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != nullptr) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
}

bool HeadlessContext::create(std::string& error) {
    if (std::getenv("EGL_PLATFORM") == nullptr) setenv("EGL_PLATFORM", "surfaceless", 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || eglInitialize(d, nullptr, nullptr) == EGL_FALSE) {
        error = "EGL: no display";
        return false;
    }
    display_ = d;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, static_cast<EGLConfig>(nullptr), EGL_NO_CONTEXT, attrs);
    if (c == EGL_NO_CONTEXT) {
        char b[64];
        std::snprintf(b, sizeof b, "EGL: cannot create a GLES 3.2 context (0x%X)", static_cast<unsigned>(eglGetError()));
        error = b;
        return false;
    }
    context_ = c;
    if (eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c) == EGL_FALSE) {
        error = "EGL: cannot make the context current (surfaceless contexts are needed)";
        return false;
    }
    return true;
}

std::string HeadlessContext::renderer_name() const {
    const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return std::string(v != nullptr ? v : "?") + " / " + (r != nullptr ? r : "?");
}

// ------------------------------------------------------------------ backend

// RAM bytes a texture map reads (all levels); `ram` false for TMEM textures.
struct TexRange {
    bool ram{false};
    u32 lo{0}, size{0};
};

// The drawing program: the ubershader, or a copy of it specialized for one
// pipeline configuration (see Impl::pick_program).
struct DrawProg {
    GLuint id{0};
    GLint u_off{-1}, u_bp{-1}, u_reg{-1}, u_kon{-1}, u_vp{-1}, u_zs0{-1}, u_zs1{-1}, u_tex{-1}, u_lvl{-1}, u_tri{-1};
    GLint u_stats_on{-1}, u_tri_base{-1};
    void locate() {
        u_off = glGetUniformLocation(id, "u_off");
        u_bp = glGetUniformLocation(id, "u_bp");
        u_reg = glGetUniformLocation(id, "u_reg");
        u_kon = glGetUniformLocation(id, "u_kon");
        u_vp = glGetUniformLocation(id, "u_vp_wd");
        u_zs0 = glGetUniformLocation(id, "u_zs0");
        u_zs1 = glGetUniformLocation(id, "u_zs1");
        u_tex = glGetUniformLocation(id, "u_tex");
        u_lvl = glGetUniformLocation(id, "u_lvl");
        u_tri = glGetUniformLocation(id, "u_tri");
        u_stats_on = glGetUniformLocation(id, "u_stats_on");
        u_tri_base = glGetUniformLocation(id, "u_tri_base");
    }
};

// BP bits that decide the structure of the pixel pipeline (Dolphin's shader
// UID: stage count, TEV combiners and orders, swap tables and konst selects,
// indirect stages, alpha test functions, z modes, blending, fog type, z
// texture). A specialized program has them as constants; everything else
// stays a uniform.
struct SpecMask { u8 reg; u32 mask; };
constexpr u32 kAll = 0xFFFFFFFFU;
const std::array<SpecMask, 74> kSpecMasks = [] {
    std::array<SpecMask, 74> m{};
    std::size_t n = 0;
    m[n++] = {0x00, kAll};
    for (u32 r = 0x10; r <= 0x1F; ++r) m[n++] = {static_cast<u8>(r), kAll};
    m[n++] = {0x27, kAll};
    for (u32 r = 0x28; r <= 0x2F; ++r) m[n++] = {static_cast<u8>(r), kAll};
    m[n++] = {0x40, kAll};
    m[n++] = {0x41, kAll};
    m[n++] = {0x42, 0x100U};
    m[n++] = {0x43, kAll};
    for (u32 r = 0xC0; r <= 0xDF; ++r) m[n++] = {static_cast<u8>(r), kAll};
    m[n++] = {0xE8, 0x400U};
    m[n++] = {0xF1, 0xF00000U};
    m[n++] = {0xF3, 0xFF0000U};
    m[n++] = {0xF5, kAll};
    for (u32 r = 0xF6; r <= 0xFD; ++r) m[n++] = {static_cast<u8>(r), kAll};
    return m;
}();

struct Backend::Impl {
    DrawProg main_prog;            // the ubershader
    struct Spec {
        std::array<u32, 74> values{};
        DrawProg prog;
        u32 seen{0};
        int state{0};              // 0 not built, 1 compiling (parallel), 2 ready, -1 failed
    };
    std::unordered_map<u64, std::vector<Spec>> specs;
    int spec_threshold{-1};        // draws of a configuration before it is specialized; -1 = never
    bool parallel_compile{false};
    GLuint vs_shader{0};
    std::string frag_prefix;
    GLuint prog{0}, vao{0}, vbo{0}, fbo{0}, efb[2]{0, 0}, dummy{0}, tri_buf{0}, tri_tex{0};
    // Display (XFB) copies encoded on the GPU (see encode_xfb): 0 = not set up yet
    // encoded copies stay in a ring of textures (Present shows them from there)
    static constexpr int kXfbSlots = 4;
    GLuint xfb_prog{0}, xfb_aux{0}, xfb_vao{0};
    std::array<GLuint, kXfbSlots> xfb_fbo{}, xfb_tex{};
    struct SlotUse { u32 handle{0}, width{0}, lines{0}; };
    std::array<SlotUse, kXfbSlots> xfb_slot{};
    int xfb_next_slot{0};
    // texture copies: the RAM bytes of the rows of blocks, 4 per texel
    static constexpr int kTexSlots = 8, kTexMaxW = 2560, kTexMaxH = 132;
    GLuint tc_prog{0};
    std::array<GLuint, kTexSlots> tc_fbo{}, tc_tex{};
    struct TexSlotUse {
        u32 handle{0}, dest{0}, stride{0}, row{0}, lines{0};
        u64 epoch{0};
        bool placed{false}; // epoch known: the rows were registered in RAM then
    };
    // RGBA6 clears of only the colour or only the alpha bits (fetch shader)
    GLuint mclear_prog{0};
    GLint u_mclear{-1};
    int mclear_state{0};
    // textures sampled straight from a texture copy on the GPU (decode_prog)
    GLuint dec_prog{0}, dec_fbo{0};
    GLint u_dec_src{-1}, u_dec_geo{-1};
    int dec_state{0};
    std::array<TexSlotUse, kTexSlots> tc_slot{};
    int tc_next_slot{0};
    GLint u_tc_efb{-1}, u_tc_aux{-1}, u_tc_rect{-1}, u_tc_w{-1}, u_tc_mode{-1}, u_tc_flags{-1};
    int tc_state{0};
    // presentation: YUYV texture -> RGBA (show_prog) and RGBA frames from the CPU (rgba_prog)
    GLuint show_prog{0}, rgba_prog{0}, rgba_tex{0};
    GLint u_show_tex{-1}, u_show_geo{-1}, u_rgba_tex{-1}, u_rgba_geo{-1};
    u32 rgba_w{0}, rgba_h{0};
    int show_state{0};
    GLint u_xfb_efb{-1}, u_xfb_aux{-1}, u_xfb_rect{-1}, u_xfb_w{-1}, u_xfb_flags{-1};
    int xfb_state{0}; // 0 untried, 1 ready, -1 unavailable
    struct PendingXfb {
        GLuint pbo{0};
        GLsync fence{nullptr};
        u32 row_bytes{0}, lines{0}, stride{0};
    };
    std::unordered_map<u32, PendingXfb> xfb_pending;
    std::vector<GLuint> pbo_pool;
    u32 xfb_next{1};
    static constexpr int kXfbMaxLines = 1024;
    // EFB readback through an RGBA8 target (see download): 0 = not set up yet
    GLuint pack_prog{0}, pack_fbo{0}, pack_tex{0}, pack_vao{0};
    GLint u_pack_src{-1}, u_pack_org{-1};
    int pack_state{0}; // 0 untried, 1 ready, -1 unavailable (plain integer reads)
    std::array<GLuint, 8> tex{};
    std::array<u64, 8> tex_key{};   // parameters + palette + texels of the uploaded texture
    std::array<u64, 8> tex_pkey{};  // parameters + palette only
    std::array<u64, 8> tex_epoch{}; // bus write epoch when the texels were last known equal
    std::array<bool, 8> tex_epoch_valid{};
    std::array<std::array<GLint, 2>, 8 * 16> lvl{};
    std::vector<u8> scratch;
    // Pixel Engine statistics (see pixel_stats.hpp and the shader)
    bool stats_supported{false};
    GLuint stats_buf{0}, quad_ids{0};
    PixelStats* stats{nullptr};
    u32 uploaded_epoch{~0U};
    bool gpu_dirty{false};
    u32 tri_base{0};

    ~Impl() {
        if (prog != 0) glDeleteProgram(prog);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (efb[0] != 0) glDeleteTextures(2, efb);
        for (GLuint t : tex) if (t != 0) glDeleteTextures(1, &t);
        if (dummy != 0) glDeleteTextures(1, &dummy);
        if (tri_tex != 0) glDeleteTextures(1, &tri_tex);
        if (tri_buf != 0) glDeleteBuffers(1, &tri_buf);
        if (stats_buf != 0) glDeleteBuffers(1, &stats_buf);
        if (quad_ids != 0) glDeleteTextures(1, &quad_ids);
        for (auto& [h, x] : xfb_pending) {
            if (x.fence != nullptr) glDeleteSync(x.fence);
            if (x.pbo != 0) glDeleteBuffers(1, &x.pbo);
        }
        for (GLuint b : pbo_pool) glDeleteBuffers(1, &b);
        if (xfb_prog != 0) glDeleteProgram(xfb_prog);
        glDeleteFramebuffers(kXfbSlots, xfb_fbo.data());
        glDeleteTextures(kXfbSlots, xfb_tex.data());
        for (auto& [k, list] : specs)
            for (auto& sp : list)
                if (sp.prog.id != 0) glDeleteProgram(sp.prog.id);
        if (vs_shader != 0) glDeleteShader(vs_shader);
        if (tc_prog != 0) glDeleteProgram(tc_prog);
        if (dec_prog != 0) glDeleteProgram(dec_prog);
        if (mclear_prog != 0) glDeleteProgram(mclear_prog);
        if (dec_fbo != 0) glDeleteFramebuffers(1, &dec_fbo);
        glDeleteFramebuffers(kTexSlots, tc_fbo.data());
        glDeleteTextures(kTexSlots, tc_tex.data());
        if (show_prog != 0) glDeleteProgram(show_prog);
        if (rgba_prog != 0) glDeleteProgram(rgba_prog);
        if (rgba_tex != 0) glDeleteTextures(1, &rgba_tex);
        if (xfb_aux != 0) glDeleteTextures(1, &xfb_aux);
        if (xfb_vao != 0) glDeleteVertexArrays(1, &xfb_vao);
        if (pack_prog != 0) glDeleteProgram(pack_prog);
        if (pack_fbo != 0) glDeleteFramebuffers(1, &pack_fbo);
        if (pack_tex != 0) glDeleteTextures(1, &pack_tex);
        if (pack_vao != 0) glDeleteVertexArrays(1, &pack_vao);
    }

    // The EFB planes are 32-bit integer textures holding 24-bit values. Reading
    // them back as RGBA_INTEGER moves 16 bytes per pixel through a path GL ES
    // implementations do not optimise; packing the 24 bits into an RGBA8 target
    // first moves 4 bytes per pixel through the standard RGBA / UNSIGNED_BYTE
    // read. Exact: each byte b becomes b / 255 and converts back to b.
    bool setup_pack() {
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* fs = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
                                "uniform highp usampler2D u_src;\nuniform ivec2 u_org;\nout vec4 o;\n"
                                "void main() {\n  uint v = texelFetch(u_src, ivec2(gl_FragCoord.xy) + u_org, 0).r;\n"
                                "  o = vec4(float(v & 255u), float((v >> 8u) & 255u), float((v >> 16u) & 255u), 255.0) / 255.0;\n}\n";
        std::string e;
        const GLuint v = compile(GL_VERTEX_SHADER, vs, e);
        if (v == 0) return false;
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs, e);
        if (f == 0) { glDeleteShader(v); return false; }
        pack_prog = glCreateProgram();
        glAttachShader(pack_prog, v);
        glAttachShader(pack_prog, f);
        glLinkProgram(pack_prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(pack_prog, GL_LINK_STATUS, &ok);
        if (ok == 0) return false;
        u_pack_src = glGetUniformLocation(pack_prog, "u_src");
        u_pack_org = glGetUniformLocation(pack_prog, "u_org");
        glGenTextures(1, &pack_tex);
        glBindTexture(GL_TEXTURE_2D, pack_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kW, kH);
        glGenFramebuffers(1, &pack_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, pack_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pack_tex, 0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glGenVertexArrays(1, &pack_vao);
        // integer textures must not use linear filtering to be sampled
        for (GLuint t : efb) {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        return complete && glGetError() == GL_NO_ERROR;
    }

    // The XFB encoder: one fragment per output texel (two pixels as Y0 U Y1 V),
    // computing what sw::efb_copy computes for a display copy: the EFB pixel as the
    // copy unit reads it (RGBA6 expanded), the vertical copy filter with clamped
    // rows, the 9-bit wrap, min(255), gamma (table), BT.601 with rounding and the
    // 1-2-1 chroma filter between neighbouring pixels. Integer arithmetic only, so
    // the bytes are exact. aux row 0: source row of each output line; row 1: gamma.
    bool setup_xfb() {
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* fs = R"GLSL(#version 320 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D u_efb;
uniform usampler2D u_aux;
uniform ivec4 u_rect;   // left, top, clamp top, clamp bottom
uniform uvec3 u_w;      // filter weights: previous, current, next row
uniform ivec2 u_flags;  // wrap, six
out vec4 o;
uvec3 raw(int x, int y) {
    uint c = texelFetch(u_efb, ivec2(min(x, 639), min(y, 527)), 0).r;
    if (u_flags.y != 0) {
        uvec3 v = uvec3((c >> 18u) & 63u, (c >> 12u) & 63u, (c >> 6u) & 63u);
        return ((v << 2u) | (v >> 4u)) & 255u;
    }
    return uvec3((c >> 16u) & 255u, (c >> 8u) & 255u, c & 255u);
}
int crow(int y) { return clamp(y, u_rect.z, u_rect.w); }
uint gam(uint v) { return texelFetch(u_aux, ivec2(int(v), 1), 0).r; }
ivec3 yuv(int x, int y) {
    uvec3 cur = raw(x, crow(y));
    uvec3 prev = u_w.x != 0u ? raw(x, crow(y - 1)) : uvec3(0u);
    uvec3 next = u_w.z != 0u ? raw(x, crow(y + 1)) : uvec3(0u);
    uvec3 v = (prev * u_w.x + cur * u_w.y + next * u_w.z) >> 6u;
    if (u_flags.x != 0) v &= 511u;
    v = min(v, uvec3(255u));
    int r = int(gam(v.r)), g = int(gam(v.g)), b = int(gam(v.b));
    int yy = (66 * r + 129 * g + 25 * b) & 0xFFFF;
    int uu = bitfieldExtract(-38 * r - 74 * g + 112 * b, 0, 16);
    int vv = bitfieldExtract(112 * r - 94 * g - 18 * b, 0, 16);
    int Y = ((yy >> 8) + ((yy >> 7) & 1)) & 255;
    int U = bitfieldExtract((uu >> 8) + ((uu >> 7) & 1), 0, 8);
    int V = bitfieldExtract((vv >> 8) + ((vv >> 7) & 1), 0, 8);
    return ivec3(Y, U, V);
}
void main() {
    int p = int(gl_FragCoord.x), line = int(gl_FragCoord.y);
    int y = u_rect.y + int(texelFetch(u_aux, ivec2(line, 0), 0).r);
    int x1 = u_rect.x + 2 * p;
    ivec3 a = yuv(p == 0 ? x1 : x1 - 1, y), b = yuv(x1, y), c = yuv(x1 + 1, y);
    int y0 = (b.x + 16) & 255, y1 = (c.x + 16) & 255;
    int u = (128 + ((a.y + 2 * b.y + c.y) >> 2)) & 255;
    int v = (128 + ((a.z + 2 * b.z + c.z) >> 2)) & 255;
    o = vec4(float(y0), float(u), float(y1), float(v)) / 255.0;
}
)GLSL";
        std::string e;
        const GLuint v = compile(GL_VERTEX_SHADER, vs, e);
        if (v == 0) return false;
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs, e);
        if (f == 0) { glDeleteShader(v); std::fprintf(stderr, "XFB encoder: %s\n", e.c_str()); return false; }
        xfb_prog = glCreateProgram();
        glAttachShader(xfb_prog, v);
        glAttachShader(xfb_prog, f);
        glLinkProgram(xfb_prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(xfb_prog, GL_LINK_STATUS, &ok);
        if (ok == 0) return false;
        u_xfb_efb = glGetUniformLocation(xfb_prog, "u_efb");
        u_xfb_aux = glGetUniformLocation(xfb_prog, "u_aux");
        u_xfb_rect = glGetUniformLocation(xfb_prog, "u_rect");
        u_xfb_w = glGetUniformLocation(xfb_prog, "u_w");
        u_xfb_flags = glGetUniformLocation(xfb_prog, "u_flags");
        glGenTextures(kXfbSlots, xfb_tex.data());
        for (GLuint t : xfb_tex) {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kW / 2, kXfbMaxLines);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glGenTextures(1, &xfb_aux);
        glBindTexture(GL_TEXTURE_2D, xfb_aux);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, kXfbMaxLines, 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(kXfbSlots, xfb_fbo.data());
        bool complete = true;
        for (int k = 0; k < kXfbSlots; ++k) {
            glBindFramebuffer(GL_FRAMEBUFFER, xfb_fbo[static_cast<std::size_t>(k)]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xfb_tex[static_cast<std::size_t>(k)], 0);
            complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        glGenVertexArrays(1, &xfb_vao);
        for (GLuint t : efb) {
            glBindTexture(GL_TEXTURE_2D, t);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        return complete && glGetError() == GL_NO_ERROR;
    }

    GLuint link(const char* vs, const char* fs, std::string& e) {
        const GLuint v = compile(GL_VERTEX_SHADER, vs, e);
        if (v == 0) return 0;
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs, e);
        if (f == 0) { glDeleteShader(v); return 0; }
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok == 0) { glDeleteProgram(p); e = "presentation program does not link"; return 0; }
        return p;
    }

    // Presentation of a frame (video::decode_xfb on the GPU, same integer
    // arithmetic): output pixel (x, y) of a width x height frame; flip puts row 0
    // at the top of a window (GL rows count from the bottom).
    bool setup_show(std::string& e) {
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* show_fs = R"GLSL(#version 320 es
precision highp float;
precision highp int;
uniform highp sampler2D u_tex;
uniform ivec4 u_geo; // first line, line step, frame height, flip
out vec4 o;
void main() {
    int x = int(gl_FragCoord.x), r = int(gl_FragCoord.y);
    int y = u_geo.w != 0 ? u_geo.z - 1 - r : r;
    ivec4 t = ivec4(round(texelFetch(u_tex, ivec2(x >> 1, u_geo.x + u_geo.y * y), 0) * 255.0));
    int yy = ((x & 1) != 0 ? t.z : t.x) - 16, u = t.y - 128, v = t.w - 128;
    ivec3 c = clamp(ivec3(1192 * yy + 1634 * v, 1192 * yy - 401 * u - 833 * v, 1192 * yy + 2065 * u) >> 10, 0, 255);
    o = vec4(vec3(c), 255.0) / 255.0;
}
)GLSL";
        static const char* rgba_fs = R"GLSL(#version 320 es
precision highp float;
precision highp int;
uniform highp sampler2D u_tex;
uniform ivec4 u_geo; // unused, unused, frame height, flip
out vec4 o;
void main() {
    int x = int(gl_FragCoord.x), r = int(gl_FragCoord.y);
    int y = u_geo.w != 0 ? u_geo.z - 1 - r : r;
    o = vec4(texelFetch(u_tex, ivec2(x, y), 0).rgb, 1.0);
}
)GLSL";
        show_prog = link(vs, show_fs, e);
        if (show_prog == 0) return false;
        rgba_prog = link(vs, rgba_fs, e);
        if (rgba_prog == 0) return false;
        u_show_tex = glGetUniformLocation(show_prog, "u_tex");
        u_show_geo = glGetUniformLocation(show_prog, "u_geo");
        u_rgba_tex = glGetUniformLocation(rgba_prog, "u_tex");
        u_rgba_geo = glGetUniformLocation(rgba_prog, "u_geo");
        return glGetError() == GL_NO_ERROR;
    }

    // Draws with `program` into framebuffer `target` (0 = the window surface),
    // a width x height viewport, then puts the drawing state back.
    void draw_frame(GLuint program, GLint loc_tex, GLint loc_geo, GLuint texture, GLuint target, const std::array<GLint, 4>& geo,
                    u32 width, u32 height) {
        GLint unit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        const GLboolean dither = glIsEnabled(GL_DITHER), blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE), stencil = glIsEnabled(GL_STENCIL_TEST);
        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        if (target == 0) { const GLenum back = GL_BACK; glDrawBuffers(1, &back); }
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glUseProgram(program);
        glBindVertexArray(xfb_vao);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(loc_tex, 9);
        glUniform4i(loc_geo, geo[0], geo[1], geo[2], geo[3]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(static_cast<GLenum>(unit));
        if (dither == GL_TRUE) glEnable(GL_DITHER);
        if (blend == GL_TRUE) glEnable(GL_BLEND);
        if (scissor == GL_TRUE) glEnable(GL_SCISSOR_TEST);
        if (depth == GL_TRUE) glEnable(GL_DEPTH_TEST);
        if (cull == GL_TRUE) glEnable(GL_CULL_FACE);
        if (stencil == GL_TRUE) glEnable(GL_STENCIL_TEST);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kW, kH);
        glUseProgram(prog);
        glBindVertexArray(vao);
    }

    // The texture copy encoder: each output texel is 4 consecutive bytes of a
    // row of blocks, computed as sw::efb_copy does (PixelReader + packing).
    bool setup_texture_copy(std::string& e) {
        GLint max = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
        if (max < kTexMaxW) { e = "GL_MAX_TEXTURE_SIZE below 2560"; return false; }
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* fs = R"GLSL(#version 320 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D u_efb;
uniform usampler2D u_aux;
uniform ivec4 u_rect;   // left, top, clamp top, clamp bottom
uniform uvec3 u_w;      // filter weights
uniform ivec4 u_mode;   // format, block width log2, block height log2, block bytes
uniform ivec4 u_flags;  // wrap, six, half, yuv
out vec4 o;
uvec4 raw(int x, int y) {
    uint c = texelFetch(u_efb, ivec2(min(x, 639), min(y, 527)), 0).r;
    if (u_flags.y != 0) return uvec4((c >> 18u) & 63u, (c >> 12u) & 63u, (c >> 6u) & 63u, c & 63u);
    return uvec4((c >> 16u) & 255u, (c >> 8u) & 255u, c & 255u, 255u);
}
uvec4 sample_px(int x, int y0, int y1) {
    if (u_flags.z == 0) {
        uvec4 p = raw(x, y0);
        return u_flags.y != 0 ? ((p << 2u) | (p >> 4u)) & 255u : p;
    }
    uvec4 s = raw(x, y0) + raw(x + 1, y0) + raw(x, y1) + raw(x + 1, y1);
    return u_flags.y != 0 ? (s + (s >> 6u)) & 255u : s >> 2u;
}
int crow(int y) { return clamp(y, u_rect.z, u_rect.w); }
uvec4 texel(int x, int y) {
    int st = u_flags.z != 0 ? 2 : 1;
    uvec4 cur = sample_px(x, crow(y), crow(y + 1));
    uvec4 prev = u_w.x != 0u ? sample_px(x, crow(y - st), crow(y - 1)) : uvec4(0u);
    uvec4 next = u_w.z != 0u ? sample_px(x, crow(y + st), crow(y + st + 1)) : uvec4(0u);
    uvec3 v = (prev.rgb * u_w.x + cur.rgb * u_w.y + next.rgb * u_w.z) >> 6u;
    if (u_flags.x != 0) v &= 511u;
    v = min(v, uvec3(255u));
    uvec4 c = uvec4(texelFetch(u_aux, ivec2(int(v.r), 1), 0).r, texelFetch(u_aux, ivec2(int(v.g), 1), 0).r,
                    texelFetch(u_aux, ivec2(int(v.b), 1), 0).r, cur.a);
    if (u_flags.w != 0) {
        ivec3 i = ivec3(c.rgb);
        ivec3 w = ivec3(66 * i.r + 129 * i.g + 25 * i.b + 4096, -38 * i.r - 74 * i.g + 112 * i.b + 32768,
                        112 * i.r - 94 * i.g - 18 * i.b + 32768);
        c.rgb = uvec3(((w >> 8) + ((w >> 7) & 1)) & 255);
    }
    return c;
}
// texel i (row-major inside the block) of block sb in the current row of blocks
uvec4 block_texel(int sb, int i) {
    int bw = 1 << u_mode.y;
    int px = sb * bw + (i & (bw - 1)), py = int(gl_FragCoord.y) * (1 << u_mode.z) + (i >> u_mode.y);
    int st = u_flags.z != 0 ? 2 : 1;
    return texel(u_rect.x + px * st, u_rect.y + py * st);
}
uint byte_at(int k) {
    int sb = k / u_mode.w, j = k - sb * u_mode.w, f = u_mode.x;
    if (f == 0) { // R4: two texels per byte
        uint hi = block_texel(sb, 2 * j).r, lo = block_texel(sb, 2 * j + 1).r;
        return (hi & 0xF0u) | (lo >> 4u);
    }
    if (f == 1 || f == 8) return block_texel(sb, j).r;
    if (f == 7) return block_texel(sb, j).a;
    if (f == 9) return block_texel(sb, j).g;
    if (f == 10) return block_texel(sb, j).b;
    if (f == 2) { uvec4 c = block_texel(sb, j); return (c.a & 0xF0u) | (c.r >> 4u); }
    if (f == 6) { // RGBA8: AR in the first 32 bytes, GB in the next 32
        bool gb = j >= 32;
        int jj = gb ? j - 32 : j;
        uvec4 c = block_texel(sb, jj >> 1);
        bool odd = (jj & 1) != 0;
        return gb ? (odd ? c.b : c.g) : (odd ? c.r : c.a);
    }
    uvec4 c = block_texel(sb, j >> 1);
    bool odd = (j & 1) != 0;
    if (f == 3) return odd ? c.r : c.a;
    if (f == 11) return odd ? c.r : c.g;
    if (f == 12) return odd ? c.g : c.b;
    uint v;
    if (f == 4) v = ((c.r << 8u) & 0xF800u) | ((c.g << 3u) & 0x07E0u) | (c.b >> 3u);
    else if (c.a >= 224u) v = 0x8000u | ((c.r << 7u) & 0x7C00u) | ((c.g << 2u) & 0x03E0u) | (c.b >> 3u);
    else v = ((c.a << 7u) & 0x7000u) | ((c.r << 4u) & 0x0F00u) | (c.g & 0xF0u) | (c.b >> 4u);
    return odd ? (v & 255u) : (v >> 8u);
}
void main() {
    int k = int(gl_FragCoord.x) * 4;
    o = vec4(float(byte_at(k)), float(byte_at(k + 1)), float(byte_at(k + 2)), float(byte_at(k + 3))) / 255.0;
}
)GLSL";
        tc_prog = link(vs, fs, e);
        if (tc_prog == 0) return false;
        u_tc_efb = glGetUniformLocation(tc_prog, "u_efb");
        u_tc_aux = glGetUniformLocation(tc_prog, "u_aux");
        u_tc_rect = glGetUniformLocation(tc_prog, "u_rect");
        u_tc_w = glGetUniformLocation(tc_prog, "u_w");
        u_tc_mode = glGetUniformLocation(tc_prog, "u_mode");
        u_tc_flags = glGetUniformLocation(tc_prog, "u_flags");
        glGenTextures(kTexSlots, tc_tex.data());
        glGenFramebuffers(kTexSlots, tc_fbo.data());
        bool complete = true;
        for (std::size_t k = 0; k < static_cast<std::size_t>(kTexSlots); ++k) {
            glBindTexture(GL_TEXTURE_2D, tc_tex[k]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kTexMaxW, kTexMaxH);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, tc_fbo[k]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tc_tex[k], 0);
            complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        return complete && glGetError() == GL_NO_ERROR;
    }

    // GX texture decoding (texture_decoder.cpp, formats I4..RGBA8) on the GPU,
    // from the RAM bytes of a texture copy held in a copy slot texture (4 bytes
    // per texel, row_bytes per row): one RGBA8UI output texel per fragment.
    bool setup_decode(std::string& e) {
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* fs = R"GLSL(#version 320 es
precision highp float;
precision highp int;
uniform highp sampler2D u_src;
uniform ivec4 u_geo; // format, width, byte offset in the copy, copy row bytes
out uvec4 o;
uint b(int off) {
    int row = off / u_geo.w, col = off - row * u_geo.w;
    vec4 t = texelFetch(u_src, ivec2(col >> 2, row), 0);
    return uint(round(t[col & 3] * 255.0));
}
uint x3(uint v) { return (v << 5u) | (v << 2u) | (v >> 1u); }
uint x4(uint v) { return (v << 4u) | v; }
uint x5(uint v) { return (v << 3u) | (v >> 2u); }
uint x6(uint v) { return (v << 2u) | (v >> 4u); }
void main() {
    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y), f = u_geo.x;
    int bw = f <= 2 ? 8 : 4, bh = f == 0 ? 8 : 4, bb = f == 6 ? 64 : 32;
    int base = u_geo.z + ((y / bh) * ((u_geo.y + bw - 1) / bw) + x / bw) * bb;
    int ix = x % bw, iy = y % bh;
    if (f == 0) {
        uint v = b(base + iy * 4 + ix / 2);
        uint i = x4((ix & 1) != 0 ? (v & 15u) : (v >> 4u));
        o = uvec4(i);
    } else if (f == 1) {
        o = uvec4(b(base + iy * 8 + ix));
    } else if (f == 2) {
        uint v = b(base + iy * 8 + ix), l = x4(v & 15u);
        o = uvec4(l, l, l, x4(v >> 4u));
    } else if (f == 6) {
        int i = (iy * 4 + ix) * 2;
        o = uvec4(b(base + i + 1), b(base + 32 + i), b(base + 33 + i), b(base + i));
    } else {
        int e = base + (iy * 4 + ix) * 2;
        uint hi = b(e), lo = b(e + 1), v = (hi << 8u) | lo;
        if (f == 3) o = uvec4(lo, lo, lo, hi);
        else if (f == 4) o = uvec4(x5((v >> 11u) & 31u), x6((v >> 5u) & 63u), x5(v & 31u), 255u);
        else if ((v & 0x8000u) != 0u) o = uvec4(x5((v >> 10u) & 31u), x5((v >> 5u) & 31u), x5(v & 31u), 255u);
        else o = uvec4(x4((v >> 8u) & 15u), x4((v >> 4u) & 15u), x4(v & 15u), x3((v >> 12u) & 7u));
    }
}
)GLSL";
        dec_prog = link(vs, fs, e);
        if (dec_prog == 0) return false;
        u_dec_src = glGetUniformLocation(dec_prog, "u_src");
        u_dec_geo = glGetUniformLocation(dec_prog, "u_geo");
        glGenFramebuffers(1, &dec_fbo);
        return glGetError() == GL_NO_ERROR;
    }

    // The program for a draw with these BP registers: a specialized one once
    // its configuration was drawn spec_threshold times and its build finished
    // (in the background with KHR_parallel_shader_compile), else the
    // ubershader. Both compute the same pixels: the specialized program is the
    // ubershader's source with the structural BP bits as constants.
    const DrawProg& pick_program(const BpMemory& bp) {
        if (spec_threshold < 0) return main_prog;
        std::array<u32, 74> v{};
        u64 h = 0xCBF29CE484222325ULL;
        for (std::size_t i = 0; i < kSpecMasks.size(); ++i) {
            v[i] = bp.reg(kSpecMasks[i].reg) & kSpecMasks[i].mask;
            h = (h ^ v[i]) * 0x100000001B3ULL;
        }
        auto& list = specs[h];
        Spec* sp = nullptr;
        for (auto& e : list)
            if (e.values == v) { sp = &e; break; }
        if (sp == nullptr) {
            list.push_back(Spec{});
            sp = &list.back();
            sp->values = v;
        }
        if (sp->state == 2) return sp->prog;
        if (sp->state == 1) {
            GLint done = 0;
            glGetProgramiv(sp->prog.id, 0x91B1 /* GL_COMPLETION_STATUS_KHR */, &done);
            if (done != 0) finish_spec(*sp);
            if (sp->state == 2) return sp->prog;
            return main_prog;
        }
        if (sp->state == 0 && ++sp->seen > static_cast<u32>(spec_threshold)) {
            start_spec(*sp);
            if (!parallel_compile && sp->state == 1) finish_spec(*sp);
            if (sp->state == 2) return sp->prog;
        }
        return main_prog;
    }
    void start_spec(Spec& sp) {
        std::string src(kFragmentShader);
        const std::string from = "uint R(uint r) { return u_bp[r >> 2u][r & 3u]; }";
        const std::size_t at = src.find(from);
        if (at == std::string::npos) { sp.state = -1; return; }
        std::array<u32, 256> km{}, kv{};
        for (std::size_t i = 0; i < kSpecMasks.size(); ++i) {
            km[kSpecMasks[i].reg] = kSpecMasks[i].mask;
            kv[kSpecMasks[i].reg] = sp.values[i];
        }
        std::string to = "const uint KM[256] = uint[256](";
        for (std::size_t r = 0; r < 256; ++r) to += std::to_string(km[r]) + (r + 1 < 256 ? "u," : "u);\n");
        to += "const uint KV[256] = uint[256](";
        for (std::size_t r = 0; r < 256; ++r) to += std::to_string(kv[r]) + (r + 1 < 256 ? "u," : "u);\n");
        to += "uint R(uint r) { return (u_bp[r >> 2u][r & 3u] & ~KM[r]) | KV[r]; }";
        src.replace(at, from.size(), to);
        const std::string full = frag_prefix + src;
        std::string e;
        const GLuint fs = compile(GL_FRAGMENT_SHADER, full.c_str(), e);
        if (fs == 0) { sp.state = -1; std::fprintf(stderr, "specialized shader: %s\n", e.c_str()); return; }
        sp.prog.id = glCreateProgram();
        glAttachShader(sp.prog.id, vs_shader);
        glAttachShader(sp.prog.id, fs);
        glLinkProgram(sp.prog.id);
        glDeleteShader(fs);
        sp.state = 1;
        ++spec_builds;
    }
    void finish_spec(Spec& sp) {
        GLint ok = 0;
        glGetProgramiv(sp.prog.id, GL_LINK_STATUS, &ok);
        if (ok == 0) {
            glDeleteProgram(sp.prog.id);
            sp.prog.id = 0;
            sp.state = -1;
            return;
        }
        sp.prog.locate();
        sp.state = 2;
    }
    u64 spec_draws{0}, spec_builds{0};

    // A texture map whose texels are all inside a texture copy still on the GPU
    // (its RAM unchanged since the copy): decoded there into the map's texture,
    // without reading the copy back. False: use RAM.
    bool texture_from_copy(const BpMemory& bp, u32 map, const TexRange& range, const mem::Bus* bus, u64& key) {
        if (!range.ram || range.size == 0U || bus == nullptr || tc_state != 1) return false;
        const u32 m0 = tex_reg(bp, map, 0), i0 = tex_reg(bp, map, 2);
        const u32 fmt = bits(i0, 20, 4);
        if (fmt > 6U || bits(m0, 5, 2) != 0U) return false; // one level (no mipmaps), I4..RGBA8
        const u32 w = bits(i0, 0, 10) + 1U, h = bits(i0, 10, 10) + 1U;
        for (std::size_t k = 0; k < tc_slot.size(); ++k) {
            const TexSlotUse& u = tc_slot[k];
            if (u.handle == 0U || !u.placed || u.stride != u.row) continue; // dense rows only
            const u64 end = static_cast<u64>(u.dest) + static_cast<u64>(u.row) * u.lines;
            if (range.lo < u.dest || static_cast<u64>(range.lo) + range.size > end) continue;
            if (!bus->unchanged_since(range.lo, range.size, u.epoch)) continue;
            if (dec_state == 0) {
                std::string e;
                dec_state = setup_decode(e) ? 1 : -1;
                if (dec_state != 1) std::fprintf(stderr, "GPU texture decoder: %s\n", e.c_str());
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (dec_state != 1) return false;
            GLint unit = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            const auto target = static_cast<GLuint>(bound);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA_INTEGER,
                         GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            const GLboolean dither = glIsEnabled(GL_DITHER), scissor = glIsEnabled(GL_SCISSOR_TEST);
            const GLboolean blend = glIsEnabled(GL_BLEND);
            glDisable(GL_DITHER);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, dec_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
            glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
            glUseProgram(dec_prog);
            glBindVertexArray(xfb_vao);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, tc_tex[k]);
            glUniform1i(u_dec_src, 9);
            glUniform4i(u_dec_geo, static_cast<GLint>(fmt), static_cast<GLint>(w), static_cast<GLint>(range.lo - u.dest),
                        static_cast<GLint>(u.row));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindTexture(GL_TEXTURE_2D, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glActiveTexture(static_cast<GLenum>(unit));
            glBindTexture(GL_TEXTURE_2D, target);
            if (dither == GL_TRUE) glEnable(GL_DITHER);
            if (scissor == GL_TRUE) glEnable(GL_SCISSOR_TEST);
            if (blend == GL_TRUE) glEnable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, kW, kH);
            glUseProgram(prog);
            glBindVertexArray(vao);
            if (glGetError() != GL_NO_ERROR) return false;
            key = 0x9E3779B97F4A7C15ULL ^ (static_cast<u64>(u.handle) << 32) ^ (static_cast<u64>(fmt) << 24) ^ range.lo;
            return true;
        }
        return false;
    }

    // Starts the asynchronous read-back of a w x h RGBA8 area of the bound
    // read framebuffer; returns its handle (0 on a GL error, `error` set).
    u32 start_readback(int w, int h, u32 row_bytes, u32 lines, u32 stride, std::string& error) {
        PendingXfb x;
        x.row_bytes = row_bytes;
        x.lines = lines;
        x.stride = stride;
        const auto bytes = static_cast<GLsizeiptr>(w) * h * 4;
        if (!pbo_pool.empty()) { x.pbo = pbo_pool.back(); pbo_pool.pop_back(); }
        else glGenBuffers(1, &x.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, x.pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        x.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        const GLenum e = glGetError();
        if (e != GL_NO_ERROR || x.fence == nullptr) {
            if (x.fence != nullptr) glDeleteSync(x.fence);
            pbo_pool.push_back(x.pbo);
            char b[64];
            std::snprintf(b, sizeof b, "GL error 0x%X in an EFB copy", e);
            error = b;
            return 0;
        }
        const u32 handle = xfb_next++;
        xfb_pending.emplace(handle, x);
        return handle;
    }

    bool ensure_show(std::string& e) {
        if (show_state == 0) {
            if (xfb_state == 0) {
                xfb_state = setup_xfb() ? 1 : -1;
                while (glGetError() != GL_NO_ERROR) {}
            }
            show_state = xfb_state == 1 && setup_show(e) ? 1 : -1;
            while (glGetError() != GL_NO_ERROR) {}
        }
        if (show_state != 1 && e.empty()) e = "the GPU presentation programs are not available";
        return show_state == 1;
    }

    // Decodes the mip levels a map can use into an RGBA8 atlas (levels stacked vertically).
    bool decode_map(const BpMemory& bp, const sw::TexSource& src, u32 map, std::vector<u8>& atlas, u32& aw, u32& ah,
                    std::array<std::array<GLint, 2>, 16>& levels, u64& key, bool hash_data, bool decode,
                    TexRange& range, std::string& error) {
        // Passes: (hash_data = false) key of the parameters and palette, level layout and the
        // RAM range read; (hash_data) + hash of the texel bytes; (decode) + the decoded atlas.
        range = {};
        const u32 m0 = tex_reg(bp, map, 0), m1 = tex_reg(bp, map, 1), i0 = tex_reg(bp, map, 2), i1 = tex_reg(bp, map, 3),
                  i2 = tex_reg(bp, map, 4), i3 = tex_reg(bp, map, 5), tl = tex_reg(bp, map, 6);
        char b[128];
        const u32 fmt = bits(i0, 20, 4);
        if (!tex_format_valid(fmt)) { std::snprintf(b, sizeof b, "texture map %u uses invalid format %u", map, fmt); error = b; return false; }
        const TexFormat f = static_cast<TexFormat>(fmt);
        const bool manual = bits(i1, 21, 1) != 0U;
        const bool tmem_rgba8 = manual && f == TexFormat::RGBA8;
        range.ram = !manual;
        if (tex_format_paletted(f) && bits(tl, 10, 2) > 2U) { error = "invalid TLUT format 3"; return false; }
        const u32 mipf = bits(m0, 5, 2), maxlod = bits(m1, 8, 8);
        const u32 nlev = mipf == 0U ? 1U : std::min(16U, (maxlod >> 4) + 2U);
        if (tmem_rgba8 && nlev > 1U) { error = "mipmapped RGBA8 textures in TMEM are not implemented"; return false; }
        const u32 wm1 = bits(i0, 0, 10), hm1 = bits(i0, 10, 10);
        const std::size_t taddr = static_cast<std::size_t>(bits(tl, 0, 10)) << 9;
        const std::span<const u8> tlut = taddr < src.tmem.size() ? src.tmem.subspan(taddr) : std::span<const u8>{};
        const TlutFormat tf = static_cast<TlutFormat>(bits(tl, 10, 2));
        key = 0xCBF29CE484222325ULL;
        for (u32 v : {i0, i1, i2, i3, tl, nlev}) key = fnv32(key, v);
        if (tex_format_paletted(f)) {
            const std::size_t pal = (f == TexFormat::C4 ? 16U : f == TexFormat::C8 ? 256U : 16384U) * 2U;
            key = fnv(key, tlut.data(), std::min(pal, tlut.size()));
        }
        aw = wm1 + 1U;
        ah = 0;
        for (u32 m = 0; m < nlev; ++m) ah += (hm1 >> m) + 1U;
        if (decode) atlas.assign(static_cast<std::size_t>(aw) * ah * 4U, 0);
        u32 offset = 0, mw = wm1 + 1U, mh = hm1 + 1U, y0 = 0;
        for (u32 m = 0; m < nlev; ++m) {
            const u32 w = (wm1 >> m) + 1U, h = (hm1 >> m) + 1U;
            levels[m] = {static_cast<GLint>(y0), static_cast<GLint>(nlev)};
            std::vector<u8> img;
            if (tmem_rgba8) {
                const std::size_t ar = static_cast<std::size_t>(bits(i1, 0, 15)) * 32U, gb = static_cast<std::size_t>(bits(i2, 0, 15)) * 32U;
                auto rd = [&](std::size_t a) { return a < src.tmem.size() ? src.tmem[a] : u8{0}; };
                const std::size_t bw = (static_cast<std::size_t>(wm1) >> 2) + 1U;
                if (decode) img.assign(static_cast<std::size_t>(w) * h * 4U, 0);
                for (u32 y = 0; decode && y < h; ++y)
                    for (u32 x = 0; x < w; ++x) {
                        const std::size_t blk = (static_cast<std::size_t>(y >> 2) * bw + (x >> 2)) << 4;
                        const std::size_t off = (blk + ((y & 3U) << 2) + (x & 3U)) << 1;
                        u8* o = &img[(static_cast<std::size_t>(y) * w + x) * 4U];
                        o[3] = rd(ar + off); o[0] = rd(ar + off + 1U); o[1] = rd(gb + off); o[2] = rd(gb + off + 1U);
                    }
                if (hash_data) {
                    key = fnv(key, src.tmem.data() + std::min(ar, src.tmem.size()), std::min<std::size_t>(bw * (((hm1 >> 2) + 1U)) * 32U, src.tmem.size() - std::min(ar, src.tmem.size())));
                    key = fnv(key, src.tmem.data() + std::min(gb, src.tmem.size()), std::min<std::size_t>(bw * (((hm1 >> 2) + 1U)) * 32U, src.tmem.size() - std::min(gb, src.tmem.size())));
                }
            } else {
                const u32 bytes = tex_size_bytes(f, w, h);
                std::span<const u8> data;
                if (manual) {
                    const std::size_t a = static_cast<std::size_t>(bits(i1, 0, 15)) * 32U + offset;
                    if (a + bytes > src.tmem.size()) { error = "texture in TMEM runs past its end"; return false; }
                    data = src.tmem.subspan(a, bytes);
                } else {
                    const u32 addr = ((bits(i3, 0, 24) << 5) + offset) & src.phys_mask;
                    // the first pass only needs the range: it must not read RAM (a
                    // deferred EFB copy there may be decoded on the GPU instead)
                    const bool read = hash_data || decode;
                    const u8* p = read ? src.bus->ram_pointer(addr, bytes) : nullptr;
                    if (read ? p == nullptr : !src.bus->is_ram(addr, bytes)) {
                        if (m == 0U) { std::snprintf(b, sizeof b, "texture at %08X (%u bytes) is outside RAM", addr, bytes); error = b; return false; }
                        // a level the game may never sample: keep it black (the software renderer
                        // would stop only when sampling it)
                        key = fnv32(key, 0xDEADu + m);
                        if (decode) img.assign(static_cast<std::size_t>(w) * h * 4U, 0);
                    } else {
                        if (read) data = std::span<const u8>(p, bytes);
                        if (range.size == 0U) range.lo = addr;
                        range.size = std::max(range.size, addr + bytes - range.lo); // levels are consecutive
                        if (addr < range.lo) range.ram = false; // wrapped around the mask: no epoch shortcut
                    }
                }
                if (!data.empty()) {
                    if (hash_data) key = fnv(key, data.data(), data.size());
                    if (decode && !decode_texture(f, data, w, h, tlut, tf, img, error)) { error = "texture: " + error; return false; }
                }
            }
            for (u32 y = 0; decode && y < h; ++y)
                std::memcpy(&atlas[(static_cast<std::size_t>(y0 + y) * aw) * 4U], &img[static_cast<std::size_t>(y) * w * 4U], static_cast<std::size_t>(w) * 4U);
            y0 += h;
            mw = std::max(mw, tex_block_width(f));
            mh = std::max(mh, tex_block_height(f));
            offset += (mw * mh * nibbles(f)) >> 1;
            mw >>= 1; mh >>= 1;
        }
        return true;
    }
};

Backend::Backend() : impl_(std::make_unique<Impl>()) {}
Backend::~Backend() = default;

bool Backend::init(std::string& error) {
    const auto* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int major = 0, minor = 0;
    if (ver == nullptr || std::sscanf(ver, "OpenGL ES %d.%d", &major, &minor) != 2 || major * 10 + minor < 32) {
        error = std::string("OpenGL ES 3.2 is required (got ") + (ver != nullptr ? ver : "no context") + ")";
        return false;
    }
    GLint n = 0;
    bool fetch = false;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
        if (std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))), "GL_EXT_shader_framebuffer_fetch") == 0) fetch = true;
    if (!fetch) { error = "GL_EXT_shader_framebuffer_fetch is required"; return false; }
    Impl& g = *impl_;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, error);
    if (vs == 0) return false;
    // Bounding box / quad counters need one storage buffer and one image in the fragment
    // stage; without them the shader is built without that code and the PE reports it.
    GLint frag_ssbo = 0, frag_images = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &frag_ssbo);
    glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &frag_images);
    g.stats_supported = frag_ssbo >= 1 && frag_images >= 1;
    g.frag_prefix = std::string("#version 320 es\n#define ORCA_STATS ") + (g.stats_supported ? "1" : "0") + "\n";
    const std::string fsrc = g.frag_prefix + kFragmentShader;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc.c_str(), error);
    if (fs == 0) { glDeleteShader(vs); return false; }
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    g.vs_shader = vs; // kept for the specialized programs
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        char log[4096];
        glGetProgramInfoLog(g.prog, sizeof log, nullptr, log);
        error = std::string("link: ") + log;
        return false;
    }
    g.main_prog.id = g.prog;
    g.main_prog.locate();
    for (GLint i = 0; i < n; ++i)
        if (std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))),
                        "GL_KHR_parallel_shader_compile") == 0) {
            using MaxThreads = void (*)(GLuint);
            if (auto f = reinterpret_cast<MaxThreads>(eglGetProcAddress("glMaxShaderCompilerThreadsKHR"));
                f != nullptr && default_spec_background_) {
                f(0xFFFFFFFFU);
                g.parallel_compile = true;
            }
        }
    g.spec_threshold = default_spec_threshold_;
    if (g.stats_supported) {
        glGenBuffers(1, &g.stats_buf);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats_buf);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 9 * sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);
        glGenTextures(1, &g.quad_ids);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g.quad_ids);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R32UI, kW / 2, kH / 2, PixelStats::kCount);
        const std::vector<GLuint> none(static_cast<std::size_t>(kW / 2) * (kH / 2) * PixelStats::kCount, 0xFFFFFFFFU);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kW / 2, kH / 2, PixelStats::kCount, GL_RED_INTEGER,
                        GL_UNSIGNED_INT, none.data());
    }
    glGenBuffers(1, &g.tri_buf);
    glGenTextures(1, &g.tri_tex);
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenTextures(2, g.efb);
    for (GLuint t : g.efb) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, kW, kH);
    }
    glGenFramebuffers(1, &g.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.efb[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g.efb[1], 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { error = "EFB framebuffer incomplete"; return false; }
    const GLuint zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, zero);
    glClearBufferuiv(GL_COLOR, 1, zero);
    glGenTextures(1, &g.dummy);
    glBindTexture(GL_TEXTURE_2D, g.dummy);
    const u8 black[4] = {0, 0, 0, 0};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(8, g.tex.data());
    if (glGetError() != GL_NO_ERROR) { error = "GL error while creating the backend objects"; return false; }
    return true;
}

void Backend::triangle(const sw::ScreenVertex& a, const sw::ScreenVertex& b, const sw::ScreenVertex& c) {
    tris_.push_back({a, b, c});
}

bool Backend::flush(const BpMemory& bp, const XfMemory& xf, const sw::TevState& tev, const sw::TexSource& tex,
                    const sw::SlopeState& zslope, std::string& error) {
    if (tris_.empty()) return true;
    Impl& g = *impl_;
    char eb[128];
    const u32 gen = bp.reg(bp::GENMODE);
    const u32 ngen = bits(gen, 0, 4), nstages = bits(gen, 10, 4) + 1U, nind = bits(gen, 16, 3);
    // state the software renderer stops on (checked here: the shader cannot report errors)
    std::array<bool, 8> used{};
    for (u32 st = 0; st < nstages; ++st) {
        const u32 order = bp.reg(static_cast<u8>(0x28U + st / 2U));
        const bool odd = (st & 1U) != 0U;
        const u32 ind = bp.reg(static_cast<u8>(bp::IND_CMD + st));
        const u32 mi = bits(ind, 9, 2), mid = bits(ind, 11, 2), sw = bits(ind, 13, 3), tw = bits(ind, 16, 3);
        if (mid > 2U || sw > 6U || tw > 6U) { std::snprintf(eb, sizeof eb, "invalid indirect stage %u setup %06X", st, ind); error = eb; tris_.clear(); return false; }
        if (mi == 0U && mid != 0U) { std::snprintf(eb, sizeof eb, "indirect stage %u: matrix id without matrix", st); error = eb; tris_.clear(); return false; }
        const u32 chan = bits(order, odd ? 19 : 7, 3);
        if (chan >= 2U && chan <= 4U) { std::snprintf(eb, sizeof eb, "TEV stage %u uses invalid colour channel %u", st, chan); error = eb; tris_.clear(); return false; }
        if (bits(order, odd ? 18 : 6, 1) != 0U && ngen > 0U) used[bits(order, odd ? 12 : 0, 3)] = true;
    }
    for (u32 i = 0; i < nind && i < 4U; ++i) used[bits(bp.reg(bp::IREF), 6U * i, 3)] = true;
    const u32 zt2 = bp.reg(bp::ZTEX2);
    if (bits(zt2, 2, 2) != 0U && bits(zt2, 0, 2) == 3U) { error = "invalid z texture format"; tris_.clear(); return false; }
    const u32 p3 = bp.reg(bp::FOG_PARAM3);
    if (bits(p3, 21, 3) != 0U && bits(p3, 20, 1) == 0U && bp.reg(bp::FOG_B_EXPONENT) >= 32U) {
        std::snprintf(eb, sizeof eb, "fog b shift %u is not handled", bp.reg(bp::FOG_B_EXPONENT));
        error = eb; tris_.clear(); return false;
    }
    // textures
    for (u32 m = 0; m < 8; ++m) {
        glActiveTexture(GL_TEXTURE0 + m);
        if (!used[m]) { glBindTexture(GL_TEXTURE_2D, g.dummy); continue; }
        std::vector<u8> atlas;
        u32 aw = 0, ah = 0;
        std::array<std::array<GLint, 2>, 16> lv{};
        u64 pkey = 0, key = 0;
        TexRange range;
        if (!g.decode_map(bp, tex, m, atlas, aw, ah, lv, pkey, false, false, range, error)) { tris_.clear(); return false; }
        for (u32 l = 0; l < 16; ++l) g.lvl[m * 16U + l] = lv[l];
        glBindTexture(GL_TEXTURE_2D, g.tex[m]);
        // Same parameters and palette, and no write to its RAM pages since it was checked:
        // the uploaded texture is still right, without hashing the texels.
        const bool same_memory = range.ram && g.tex_epoch_valid[m] && pkey == g.tex_pkey[m] && tex.bus != nullptr &&
                                 tex.bus->unchanged_since(range.lo, range.size, g.tex_epoch[m]);
        bool from_copy = false;
        if (!same_memory && g.texture_from_copy(bp, m, range, tex.bus, key)) {
            from_copy = true;
            g.tex_key[m] = key;
            ++tex_from_copies_;
        }
        if (!same_memory && !from_copy) {
            ++tex_hashes_;
            if (!g.decode_map(bp, tex, m, atlas, aw, ah, lv, key, true, false, range, error)) { tris_.clear(); return false; }
        }
        g.tex_pkey[m] = pkey;
        g.tex_epoch_valid[m] = range.ram && tex.bus != nullptr;
        if (g.tex_epoch_valid[m]) g.tex_epoch[m] = tex.bus->write_epoch();
        if (!same_memory && !from_copy && key != g.tex_key[m]) {
            if (!g.decode_map(bp, tex, m, atlas, aw, ah, lv, key, true, true, range, error)) { tris_.clear(); return false; }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, static_cast<GLsizei>(aw), static_cast<GLsizei>(ah), 0, GL_RGBA_INTEGER,
                         GL_UNSIGNED_BYTE, atlas.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            g.tex_key[m] = key;
            ++tex_uploads_;
        }
    }
    const DrawProg& P = g.pick_program(bp);
    glUseProgram(P.id);
    std::array<GLuint, 256> regs{};
    for (u32 r = 0; r < 256; ++r) regs[r] = bp.reg(static_cast<u8>(r));
    glUniform4uiv(P.u_bp, 64, regs.data());
    std::array<GLint, 16> rv{}, kv{};
    for (u32 i = 0; i < 4; ++i)
        for (u32 c = 0; c < 4; ++c) { rv[i * 4U + c] = tev.reg[i][c]; kv[i * 4U + c] = tev.konst[i][c]; }
    glUniform4iv(P.u_reg, 4, rv.data());
    glUniform4iv(P.u_kon, 4, kv.data());
    glUniform1f(P.u_vp, xf.as_float(xf::VIEWPORT));
    const bool zfreeze = bits(gen, 19, 1) != 0U;
    glUniform4f(P.u_zs0, zslope.dfdx, zslope.dfdy, zslope.f0, zslope.xoff);
    glUniform4f(P.u_zs1, zslope.yoff, static_cast<float>(zslope.x0), static_cast<float>(zslope.y0), zfreeze ? 1.0f : 0.0f);
    const GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(P.u_tex, 8, units);
    glUniform2iv(P.u_lvl, 8 * 16, &g.lvl[0][0]);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, kW, kH);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glEnable(GL_SCISSOR_TEST);
    const u32 tl = bp.reg(bp::SCISSOR_TL), br = bp.reg(bp::SCISSOR_BR), so = bp.reg(bp::SCISSOR_OFFSET);
    const s32 left = static_cast<s32>(bits(tl, 12, 11)), top = static_cast<s32>(bits(tl, 0, 11));
    const s32 right = static_cast<s32>(bits(br, 12, 11)), bottom = static_cast<s32>(bits(br, 0, 11));
    if (left <= right && top <= bottom) {
        const auto xr = scissor_ranges(left, right, static_cast<s32>(bits(so, 0, 9)) << 1, kW);
        const auto yr = scissor_ranges(top, bottom, static_cast<s32>(bits(so, 10, 9)) << 1, kH);
        // positions, and the attribute slopes of the principal scissor range per triangle
        const s32 ox = static_cast<s32>(bits(so, 0, 9)) << 1, oy = static_cast<s32>(bits(so, 10, 9)) << 1;
        verts_.clear();
        slopes_.clear();
        verts_.reserve(tris_.size() * 6U);
        slopes_.reserve(tris_.size() * kTexelsPerTriangle * 4U);
        for (const auto& t : tris_) {
            for (const sw::ScreenVertex& v : t) verts_.insert(verts_.end(), {v.screen[0], v.screen[1]});
            triangle_slopes(t[0], t[1], t[2], ox, oy, slopes_);
        }
        glBindBuffer(GL_TEXTURE_BUFFER, g.tri_buf);
        glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(slopes_.size() * sizeof(float)), slopes_.data(), GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_BUFFER, g.tri_tex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, g.tri_buf);
        glUniform1i(P.u_tri, 8);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(float)), verts_.data(), GL_STREAM_DRAW);
        // statistics: upload what the CPU changed (BP 0x55-0x57) and arm the shader
        const bool bbox_on = g.stats != nullptr && g.stats_supported && g.stats->bbox_armed;
        const bool quads_on = g.stats != nullptr && g.stats_supported && g.stats->quads_armed;
        if (g.stats_supported) {
            if ((bbox_on || quads_on) && g.stats->epoch != g.uploaded_epoch) {
                GLuint v[9];
                for (unsigned i = 0; i < 4; ++i) v[i] = g.stats->bbox[i];
                for (unsigned i = 0; i < PixelStats::kCount; ++i) v[4 + i] = g.stats->quads[i];
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats_buf);
                glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof v, v);
                g.uploaded_epoch = g.stats->epoch;
            }
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g.stats_buf);
            glBindImageTexture(0, g.quad_ids, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32UI);
            glUniform2ui(P.u_stats_on, bbox_on ? 1U : 0U, quads_on ? 1U : 0U);
            glUniform1ui(P.u_tri_base, g.tri_base);
            if (bbox_on || quads_on) g.gpu_dirty = true;
        }
        for (const Range& rx : xr)
            for (const Range& ry : yr) {
                glUniform2f(P.u_off, static_cast<float>(rx.off), static_cast<float>(ry.off));
                glScissor(rx.start, ry.start, rx.end - rx.start, ry.end - ry.start);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size() / 2U));
                ++draws_;
                if (P.id != g.main_prog.id) ++g.spec_draws;
            }
    }
    glDisable(GL_SCISSOR_TEST);
    g.tri_base += static_cast<u32>(tris_.size()); // triangle ids for the quad counters (wrap after 2^32)
    tris_.clear();
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) { std::snprintf(eb, sizeof eb, "GL error 0x%X while drawing", e); error = eb; return false; }
    return true;
}

bool Backend::attach_stats(PixelStats* stats) {
    impl_->stats = stats;
    return impl_->stats_supported;
}

bool Backend::sync_stats(bool bbox, std::string& error) {
    Impl& g = *impl_;
    if (!g.stats_supported || g.stats == nullptr) { error = "the GL implementation has no fragment storage buffers/images"; return false; }
    if (!(bbox ? g.stats->bbox_armed : g.stats->quads_armed)) {
        error = bbox ? "bounding box read before the game set it (BP 0x55/0x56): the GLES backend only tracks it from then"
                     : "quad counters read before the game cleared them (BP 0x57): the GLES backend only counts from then";
        return false;
    }
    if (!g.gpu_dirty) return true;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats_buf);
    const auto* v = static_cast<const GLuint*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 9 * sizeof(GLuint), GL_MAP_READ_BIT));
    if (v == nullptr) { error = "could not map the statistics buffer"; return false; }
    for (unsigned i = 0; i < 4; ++i) g.stats->bbox[i] = static_cast<u16>(v[i]);
    for (unsigned i = 0; i < PixelStats::kCount; ++i) g.stats->quads[i] = v[4 + i];
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    g.gpu_dirty = false;
    return true;
}

bool Backend::peek_pixel(u32 x, u32 y, u32& color, u32& depth, std::string& error) {
    Impl& g = *impl_;
    GLuint px[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
    color = px[0] & 0xFFFFFFU;
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
    depth = px[0] & 0xFFFFFFU;
    if (glGetError() != GL_NO_ERROR) { error = "GL error while reading an EFB pixel"; return false; }
    return true;
}

bool Backend::poke_pixel(u32 x, u32 y, bool depth, u32 value, std::string& error) {
    Impl& g = *impl_;
    const GLuint v = value & 0xFFFFFFU;
    glBindTexture(GL_TEXTURE_2D, g.efb[depth ? 1 : 0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(y), 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &v);
    if (glGetError() != GL_NO_ERROR) { error = "GL error while writing an EFB pixel"; return false; }
    return true;
}

void Backend::download(sw::Efb& efb, const sw::EfbRect& area, bool color, bool depth) {
    Impl& g = *impl_;
    const int x0 = static_cast<int>(area.x0), y0 = static_cast<int>(area.y0);
    const int w = static_cast<int>(area.x1) - x0, h = static_cast<int>(area.y1) - y0;
    if (w <= 0 || h <= 0) return;
    if (g.pack_state == 0) {
        GLint unit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        g.pack_state = g.setup_pack() ? 1 : -1;
        glActiveTexture(static_cast<GLenum>(unit));
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (g.pack_state == 1) {
        GLint unit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        const GLboolean dither = glIsEnabled(GL_DITHER);
        glDisable(GL_DITHER);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, g.pack_fbo);
        glViewport(0, 0, w, h);
        glUseProgram(g.pack_prog);
        glBindVertexArray(g.pack_vao);
        glActiveTexture(GL_TEXTURE9);
        glUniform1i(g.u_pack_src, 9);
        glUniform2i(g.u_pack_org, x0, y0);
        std::vector<u8> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        for (int a = 0; a < 2; ++a) {
            if ((a == 0 && !color) || (a == 1 && !depth)) continue;
            glBindTexture(GL_TEXTURE_2D, g.efb[a]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
                    const u32 v = u32{px[i]} | u32{px[i + 1]} << 8 | u32{px[i + 2]} << 16;
                    const auto ex = static_cast<u32>(x0 + x), ey = static_cast<u32>(y0 + y);
                    if (a == 0) efb.set_color24(ex, ey, v);
                    else efb.set_depth24(ex, ey, v);
                }
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(static_cast<GLenum>(unit));
        if (dither == GL_TRUE) glEnable(GL_DITHER);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
        glViewport(0, 0, kW, kH);
        glUseProgram(g.prog);
        glBindVertexArray(g.vao);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    for (int a = 0; a < 2; ++a) {
        if ((a == 0 && !color) || (a == 1 && !depth)) continue;
        glReadBuffer(static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + a));
        // GL ES always accepts RGBA_INTEGER; the implementation may also offer
        // RED_INTEGER for an R32UI attachment (a quarter of the data to move)
        GLint fmt = 0, type = 0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
        const bool red = fmt == GL_RED_INTEGER && type == GL_UNSIGNED_INT;
        const std::size_t comps = red ? 1U : 4U;
        std::vector<GLuint> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * comps);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x0, y0, w, h, red ? GL_RED_INTEGER : GL_RGBA_INTEGER, GL_UNSIGNED_INT, px.data());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u32 v = px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * comps] & 0xFFFFFFU;
                const auto ex = static_cast<u32>(x0 + x), ey = static_cast<u32>(y0 + y);
                if (a == 0) efb.set_color24(ex, ey, v);
                else efb.set_depth24(ex, ey, v);
            }
    }
}

bool Backend::packed_readback() const noexcept { return impl_->pack_state == 1; }

bool Backend::encode_xfb(const sw::XfbPlan& plan, u32& handle, std::string& error) {
    Impl& g = *impl_;
    const int w = static_cast<int>(plan.width / 2U), h = static_cast<int>(plan.lines);
    if (w <= 0 || h <= 0 || w > kW / 2 || h > Impl::kXfbMaxLines || plan.rows.size() != plan.lines) {
        error = "XFB copy size not handled on the GPU";
        return false;
    }
    GLint unit = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    if (g.xfb_state == 0) {
        g.xfb_state = g.setup_xfb() ? 1 : -1;
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (g.xfb_state != 1) {
        glActiveTexture(static_cast<GLenum>(unit));
        error = "the GPU XFB encoder is not available";
        return false;
    }
    // aux: source rows and gamma
    std::vector<GLuint> row(plan.rows.begin(), plan.rows.end());
    std::array<GLuint, 256> gam{};
    for (std::size_t k = 0; k < 256; ++k) gam[k] = plan.gamma[k];
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, g.xfb_aux);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, h, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, row.data());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 1, 256, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, gam.data());
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, g.efb[0]);
    const GLboolean dither = glIsEnabled(GL_DITHER);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    const int slot = g.xfb_next_slot;
    g.xfb_next_slot = (slot + 1) % Impl::kXfbSlots;
    glBindFramebuffer(GL_FRAMEBUFFER, g.xfb_fbo[static_cast<std::size_t>(slot)]);
    glViewport(0, 0, w, h);
    glUseProgram(g.xfb_prog);
    glBindVertexArray(g.xfb_vao);
    glUniform1i(g.u_xfb_efb, 9);
    glUniform1i(g.u_xfb_aux, 10);
    glUniform4i(g.u_xfb_rect, static_cast<GLint>(plan.left), static_cast<GLint>(plan.top),
                static_cast<GLint>(plan.clamp_top), static_cast<GLint>(plan.clamp_bottom));
    glUniform3ui(g.u_xfb_w, plan.weights[0], plan.weights[1], plan.weights[2]);
    glUniform2i(g.u_xfb_flags, plan.wrap ? 1 : 0, plan.six ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // asynchronous read-back into a pixel pack buffer, waited for in read_xfb
    Impl::PendingXfb x;
    x.row_bytes = plan.width * 2U;
    x.lines = plan.lines;
    x.stride = plan.stride;
    const auto bytes = static_cast<GLsizeiptr>(x.row_bytes) * h;
    if (!g.pbo_pool.empty()) { x.pbo = g.pbo_pool.back(); g.pbo_pool.pop_back(); }
    else glGenBuffers(1, &x.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, x.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    x.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(static_cast<GLenum>(unit));
    if (dither == GL_TRUE) glEnable(GL_DITHER);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, kW, kH);
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR || x.fence == nullptr) {
        g.xfb_slot[static_cast<std::size_t>(slot)] = Impl::SlotUse{};
        if (x.fence != nullptr) glDeleteSync(x.fence);
        g.pbo_pool.push_back(x.pbo);
        char b[64];
        std::snprintf(b, sizeof b, "GL error 0x%X in the XFB encoder", e);
        error = b;
        return false;
    }
    handle = g.xfb_next++;
    g.xfb_pending.emplace(handle, x);
    g.xfb_slot[static_cast<std::size_t>(slot)] = Impl::SlotUse{handle, plan.width, plan.lines};
    ++xfb_encodes_;
    return true;
}

void Backend::read_xfb(u32 handle, u8* dst) {
    Impl& g = *impl_;
    const auto it = g.xfb_pending.find(handle);
    if (it == g.xfb_pending.end()) return;
    const Impl::PendingXfb x = it->second;
    g.xfb_pending.erase(it);
    while (glClientWaitSync(x.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL) == GL_TIMEOUT_EXPIRED) {}
    glDeleteSync(x.fence);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, x.pbo);
    const auto bytes = static_cast<GLsizeiptr>(x.row_bytes) * x.lines;
    if (const auto* src = static_cast<const u8*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT))) {
        for (u32 y = 0; y < x.lines; ++y)
            std::memcpy(dst + static_cast<std::size_t>(y) * x.stride, src + static_cast<std::size_t>(y) * x.row_bytes, x.row_bytes);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g.pbo_pool.push_back(x.pbo);
}

void Backend::drop_xfb(u32 handle) {
    Impl& g = *impl_;
    const auto it = g.xfb_pending.find(handle);
    if (it == g.xfb_pending.end()) return;
    glDeleteSync(it->second.fence);
    g.pbo_pool.push_back(it->second.pbo);
    g.xfb_pending.erase(it);
}

bool Backend::encode_texture_copy(const sw::TexCopyPlan& plan, u32& handle, std::string& error) {
    Impl& g = *impl_;
    const u32 row = plan.row_bytes();
    const int w = static_cast<int>(row / 4U), h = static_cast<int>(plan.blocks_y);
    if (w <= 0 || h <= 0 || w > Impl::kTexMaxW || h > Impl::kTexMaxH || (row & 3U) != 0U) {
        error = "texture copy size not handled on the GPU";
        return false;
    }
    if (g.xfb_state == 0) {
        g.xfb_state = g.setup_xfb() ? 1 : -1;
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (g.tc_state == 0) {
        std::string e;
        g.tc_state = g.xfb_state == 1 && g.setup_texture_copy(e) ? 1 : -1;
        if (g.tc_state != 1) std::fprintf(stderr, "texture copy encoder: %s\n", e.c_str());
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (g.tc_state != 1) { error = "the GPU texture copy encoder is not available"; return false; }
    GLint unit = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    std::array<GLuint, 256> gam{};
    for (std::size_t k = 0; k < 256; ++k) gam[k] = plan.gamma[k];
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, g.xfb_aux);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 1, 256, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, gam.data());
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, g.efb[plan.depth ? 1 : 0]);
    const GLboolean dither = glIsEnabled(GL_DITHER), scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    const int slot = g.tc_next_slot;
    g.tc_next_slot = (slot + 1) % Impl::kTexSlots;
    glBindFramebuffer(GL_FRAMEBUFFER, g.tc_fbo[static_cast<std::size_t>(slot)]);
    glViewport(0, 0, w, h);
    glUseProgram(g.tc_prog);
    glBindVertexArray(g.xfb_vao);
    glUniform1i(g.u_tc_efb, 9);
    glUniform1i(g.u_tc_aux, 10);
    glUniform4i(g.u_tc_rect, static_cast<GLint>(plan.left), static_cast<GLint>(plan.top),
                static_cast<GLint>(plan.clamp_top), static_cast<GLint>(plan.clamp_bottom));
    glUniform3ui(g.u_tc_w, plan.weights[0], plan.weights[1], plan.weights[2]);
    glUniform4i(g.u_tc_mode, static_cast<GLint>(plan.format), static_cast<GLint>(plan.block_w_log),
                static_cast<GLint>(plan.block_h_log), static_cast<GLint>(plan.block_bytes));
    glUniform4i(g.u_tc_flags, plan.wrap ? 1 : 0, plan.six ? 1 : 0, plan.half ? 1 : 0, plan.yuv ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    handle = g.start_readback(w, h, row, plan.blocks_y, plan.stride, error);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(static_cast<GLenum>(unit));
    if (dither == GL_TRUE) glEnable(GL_DITHER);
    if (scissor == GL_TRUE) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glViewport(0, 0, kW, kH);
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    if (handle == 0U) {
        g.tc_slot[static_cast<std::size_t>(slot)] = {};
        return false;
    }
    g.tc_slot[static_cast<std::size_t>(slot)] = Impl::TexSlotUse{handle, plan.dest, plan.stride, row, plan.blocks_y, 0, false};
    ++texture_copy_encodes_;
    return true;
}

int Backend::default_spec_threshold_ = 8;
bool Backend::default_spec_background_ = true;
void Backend::set_default_specialization(int threshold, bool background) noexcept {
    default_spec_threshold_ = threshold;
    default_spec_background_ = background;
}
void Backend::set_specialization(int threshold) noexcept { impl_->spec_threshold = threshold; }
u64 Backend::specialized_draws() const noexcept { return impl_->spec_draws; }
u64 Backend::specialized_builds() const noexcept { return impl_->spec_builds; }

void Backend::copy_placed(u32 handle, u64 epoch) noexcept {
    for (auto& u : impl_->tc_slot)
        if (u.handle == handle && handle != 0U) { u.epoch = epoch; u.placed = true; }
}

bool Backend::xfb_on_gpu(u32 handle) const noexcept {
    if (handle == 0U) return false;
    for (const auto& u : impl_->xfb_slot)
        if (u.handle == handle) return true;
    return false;
}

bool Backend::present_xfb(u32 handle, u32 first_line, u32 line_step, u32 width, u32 height, u32 target, bool flip,
                          std::string& error) {
    Impl& g = *impl_;
    if (!g.ensure_show(error)) return false;
    for (std::size_t k = 0; k < g.xfb_slot.size(); ++k) {
        const auto& u = g.xfb_slot[k];
        if (u.handle != handle || handle == 0U) continue;
        if (width == 0U || height == 0U || width > u.width || (width & 1U) != 0U ||
            first_line + static_cast<u64>(line_step) * (height - 1U) >= u.lines) {
            error = "the field is not inside the copy on the GPU";
            return false;
        }
        g.draw_frame(g.show_prog, g.u_show_tex, g.u_show_geo, g.xfb_tex[k], target,
                     {static_cast<GLint>(first_line), static_cast<GLint>(line_step), static_cast<GLint>(height), flip ? 1 : 0},
                     width, height);
        const GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            char b[64];
            std::snprintf(b, sizeof b, "GL error 0x%X while presenting", e);
            error = b;
            return false;
        }
        ++presented_;
        return true;
    }
    error = "the copy is no longer on the GPU";
    return false;
}

bool Backend::present_rgba(std::span<const u8> rgba, u32 width, u32 height, u32 target, bool flip, std::string& error) {
    Impl& g = *impl_;
    if (!g.ensure_show(error)) return false;
    if (width == 0U || height == 0U || rgba.size() < static_cast<std::size_t>(width) * height * 4U) {
        error = "empty frame";
        return false;
    }
    GLint unit = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE9);
    if (g.rgba_tex == 0 || g.rgba_w != width || g.rgba_h != height) {
        if (g.rgba_tex != 0) glDeleteTextures(1, &g.rgba_tex);
        glGenTextures(1, &g.rgba_tex);
        glBindTexture(GL_TEXTURE_2D, g.rgba_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g.rgba_w = width;
        g.rgba_h = height;
    }
    glBindTexture(GL_TEXTURE_2D, g.rgba_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(static_cast<GLenum>(unit));
    g.draw_frame(g.rgba_prog, g.u_rgba_tex, g.u_rgba_geo, g.rgba_tex, target,
                 {0, 1, static_cast<GLint>(height), flip ? 1 : 0}, width, height);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char b[64];
        std::snprintf(b, sizeof b, "GL error 0x%X while presenting", e);
        error = b;
        return false;
    }
    return true;
}

bool Backend::clear_efb(const sw::CopyArea& a, const sw::ClearPlan& c, std::string& error) {
    Impl& g = *impl_;
    if (c.color && !c.whole && g.xfb_state == 0) { // its vertex array
        g.xfb_state = g.setup_xfb() ? 1 : -1;
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (c.color && !c.whole && g.mclear_state == 0) {
        static const char* vs = "#version 320 es\nvoid main() {\n"
                                "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
        static const char* fs = "#version 320 es\n#extension GL_EXT_shader_framebuffer_fetch : require\n"
                                "precision highp int;\n"
                                "layout(location = 0) inout highp uvec4 o_color;\n"
                                "uniform uvec2 u_keep_value;\n"
                                "void main() { o_color = uvec4((o_color.x & u_keep_value.x) | u_keep_value.y, 0u, 0u, 0u); }\n";
        std::string e;
        g.mclear_prog = g.link(vs, fs, e);
        g.u_mclear = g.mclear_prog != 0 ? glGetUniformLocation(g.mclear_prog, "u_keep_value") : -1;
        g.mclear_state = g.mclear_prog != 0 ? 1 : -1;
        if (g.mclear_state != 1) std::fprintf(stderr, "partial clear program: %s\n", e.c_str());
        while (glGetError() != GL_NO_ERROR) {}
    }
    if (c.color && !c.whole && g.mclear_state != 1) { error = "the partial RGBA6 clear program is not available"; return false; }
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(a.left), static_cast<GLint>(a.top), static_cast<GLsizei>(a.right - a.left),
              static_cast<GLsizei>(a.bottom - a.top));
    if (c.color && c.whole) { const GLuint v[4] = {c.color_value, 0, 0, 0}; glClearBufferuiv(GL_COLOR, 0, v); }
    if (c.color && !c.whole) {
        // only the enabled bits: new = (old & keep) | value, on attachment 0 only
        const GLenum only0[2] = {GL_COLOR_ATTACHMENT0, GL_NONE}, both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        const GLboolean blend = glIsEnabled(GL_BLEND), dither = glIsEnabled(GL_DITHER);
        const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST), stencil = glIsEnabled(GL_STENCIL_TEST), cull = glIsEnabled(GL_CULL_FACE);
        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawBuffers(2, only0);
        glViewport(0, 0, kW, kH);
        glUseProgram(g.mclear_prog);
        glBindVertexArray(g.xfb_vao);
        glUniform2ui(g.u_mclear, c.color_keep, c.color_value);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDrawBuffers(2, both);
        if (blend == GL_TRUE) glEnable(GL_BLEND);
        if (dither == GL_TRUE) glEnable(GL_DITHER);
        if (depth_test == GL_TRUE) glEnable(GL_DEPTH_TEST);
        if (stencil == GL_TRUE) glEnable(GL_STENCIL_TEST);
        if (cull == GL_TRUE) glEnable(GL_CULL_FACE);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glUseProgram(g.prog);
        glBindVertexArray(g.vao);
    }
    if (c.depth) { const GLuint v[4] = {c.depth_value, 0, 0, 0}; glClearBufferuiv(GL_COLOR, 1, v); }
    glDisable(GL_SCISSOR_TEST);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char b[64];
        std::snprintf(b, sizeof b, "GL error 0x%X while clearing the EFB", e);
        error = b;
        return false;
    }
    return true;
}

void Backend::upload(const sw::Efb& efb, const sw::EfbRect& area) {
    Impl& g = *impl_;
    const int x0 = static_cast<int>(area.x0), y0 = static_cast<int>(area.y0);
    const int w = static_cast<int>(area.x1) - x0, h = static_cast<int>(area.y1) - y0;
    if (w <= 0 || h <= 0) return;
    std::vector<GLuint> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int a = 0; a < 2; ++a) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto ex = static_cast<u32>(x0 + x), ey = static_cast<u32>(y0 + y);
                px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                    a == 0 ? efb.color24(ex, ey) : efb.depth24(ex, ey);
            }
        glBindTexture(GL_TEXTURE_2D, g.efb[a]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, px.data());
    }
}

} // namespace orca::gpu::gles
