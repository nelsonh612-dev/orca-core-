// Orca Core — OpenGL ES 3.2 backend. See include/orca/gpu/gles/backend.hpp.
#ifndef EGL_NO_X11
#define EGL_NO_X11
#endif
#include "orca/gpu/gles/backend.hpp"
#include "orca/gpu/texture_decoder.hpp"
#include "gles_shaders.hpp"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

struct Backend::Impl {
    GLuint prog{0}, vao{0}, vbo{0}, fbo{0}, efb[2]{0, 0}, dummy{0}, tri_buf{0}, tri_tex{0};
    std::array<GLuint, 8> tex{};
    std::array<u64, 8> tex_key{};
    std::array<std::array<GLint, 2>, 8 * 16> lvl{};
    GLint u_off{-1}, u_bp{-1}, u_reg{-1}, u_kon{-1}, u_vp{-1}, u_zs0{-1}, u_zs1{-1}, u_tex{-1}, u_lvl{-1}, u_tri{-1};
    std::vector<u8> scratch;
    // Pixel Engine statistics (see pixel_stats.hpp and the shader)
    bool stats_supported{false};
    GLuint stats_buf{0}, quad_ids{0};
    GLint u_stats_on{-1}, u_tri_base{-1};
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
    }

    // Decodes the mip levels a map can use into an RGBA8 atlas (levels stacked vertically).
    bool decode_map(const BpMemory& bp, const sw::TexSource& src, u32 map, std::vector<u8>& atlas, u32& aw, u32& ah,
                    std::array<std::array<GLint, 2>, 16>& levels, u64& key, std::string& error) {
        const u32 m0 = tex_reg(bp, map, 0), m1 = tex_reg(bp, map, 1), i0 = tex_reg(bp, map, 2), i1 = tex_reg(bp, map, 3),
                  i2 = tex_reg(bp, map, 4), i3 = tex_reg(bp, map, 5), tl = tex_reg(bp, map, 6);
        char b[128];
        const u32 fmt = bits(i0, 20, 4);
        if (!tex_format_valid(fmt)) { std::snprintf(b, sizeof b, "texture map %u uses invalid format %u", map, fmt); error = b; return false; }
        const TexFormat f = static_cast<TexFormat>(fmt);
        const bool manual = bits(i1, 21, 1) != 0U;
        const bool tmem_rgba8 = manual && f == TexFormat::RGBA8;
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
        atlas.assign(static_cast<std::size_t>(aw) * ah * 4U, 0);
        u32 offset = 0, mw = wm1 + 1U, mh = hm1 + 1U, y0 = 0;
        for (u32 m = 0; m < nlev; ++m) {
            const u32 w = (wm1 >> m) + 1U, h = (hm1 >> m) + 1U;
            levels[m] = {static_cast<GLint>(y0), static_cast<GLint>(nlev)};
            std::vector<u8> img;
            if (tmem_rgba8) {
                const std::size_t ar = static_cast<std::size_t>(bits(i1, 0, 15)) * 32U, gb = static_cast<std::size_t>(bits(i2, 0, 15)) * 32U;
                img.assign(static_cast<std::size_t>(w) * h * 4U, 0);
                auto rd = [&](std::size_t a) { return a < src.tmem.size() ? src.tmem[a] : u8{0}; };
                const std::size_t bw = (static_cast<std::size_t>(wm1) >> 2) + 1U;
                for (u32 y = 0; y < h; ++y)
                    for (u32 x = 0; x < w; ++x) {
                        const std::size_t blk = (static_cast<std::size_t>(y >> 2) * bw + (x >> 2)) << 4;
                        const std::size_t off = (blk + ((y & 3U) << 2) + (x & 3U)) << 1;
                        u8* o = &img[(static_cast<std::size_t>(y) * w + x) * 4U];
                        o[3] = rd(ar + off); o[0] = rd(ar + off + 1U); o[1] = rd(gb + off); o[2] = rd(gb + off + 1U);
                    }
                key = fnv(key, src.tmem.data() + std::min(ar, src.tmem.size()), std::min<std::size_t>(bw * (((hm1 >> 2) + 1U)) * 32U, src.tmem.size() - std::min(ar, src.tmem.size())));
                key = fnv(key, src.tmem.data() + std::min(gb, src.tmem.size()), std::min<std::size_t>(bw * (((hm1 >> 2) + 1U)) * 32U, src.tmem.size() - std::min(gb, src.tmem.size())));
            } else {
                const u32 bytes = tex_size_bytes(f, w, h);
                std::span<const u8> data;
                if (manual) {
                    const std::size_t a = static_cast<std::size_t>(bits(i1, 0, 15)) * 32U + offset;
                    if (a + bytes > src.tmem.size()) { error = "texture in TMEM runs past its end"; return false; }
                    data = src.tmem.subspan(a, bytes);
                } else {
                    const u32 addr = ((bits(i3, 0, 24) << 5) + offset) & src.phys_mask;
                    const u8* p = src.bus->ram_pointer(addr, bytes);
                    if (p == nullptr) {
                        if (m == 0U) { std::snprintf(b, sizeof b, "texture at %08X (%u bytes) is outside RAM", addr, bytes); error = b; return false; }
                        // a level the game may never sample: keep it black (the software renderer
                        // would stop only when sampling it)
                        img.assign(static_cast<std::size_t>(w) * h * 4U, 0);
                    } else {
                        data = std::span<const u8>(p, bytes);
                    }
                }
                if (!data.empty()) {
                    key = fnv(key, data.data(), data.size());
                    if (!decode_texture(f, data, w, h, tlut, tf, img, error)) { error = "texture: " + error; return false; }
                }
            }
            for (u32 y = 0; y < h; ++y)
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
    const std::string fsrc = std::string("#version 320 es\n#define ORCA_STATS ") + (g.stats_supported ? "1" : "0") +
                             "\n" + kFragmentShader;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc.c_str(), error);
    if (fs == 0) { glDeleteShader(vs); return false; }
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        char log[4096];
        glGetProgramInfoLog(g.prog, sizeof log, nullptr, log);
        error = std::string("link: ") + log;
        return false;
    }
    g.u_off = glGetUniformLocation(g.prog, "u_off");
    g.u_bp = glGetUniformLocation(g.prog, "u_bp");
    g.u_reg = glGetUniformLocation(g.prog, "u_reg");
    g.u_kon = glGetUniformLocation(g.prog, "u_kon");
    g.u_vp = glGetUniformLocation(g.prog, "u_vp_wd");
    g.u_zs0 = glGetUniformLocation(g.prog, "u_zs0");
    g.u_zs1 = glGetUniformLocation(g.prog, "u_zs1");
    g.u_tex = glGetUniformLocation(g.prog, "u_tex");
    g.u_lvl = glGetUniformLocation(g.prog, "u_lvl");
    g.u_tri = glGetUniformLocation(g.prog, "u_tri");
    g.u_stats_on = glGetUniformLocation(g.prog, "u_stats_on");
    g.u_tri_base = glGetUniformLocation(g.prog, "u_tri_base");
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
        u64 key = 0;
        if (!g.decode_map(bp, tex, m, atlas, aw, ah, lv, key, error)) { tris_.clear(); return false; }
        for (u32 l = 0; l < 16; ++l) g.lvl[m * 16U + l] = lv[l];
        glBindTexture(GL_TEXTURE_2D, g.tex[m]);
        if (key != g.tex_key[m]) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, static_cast<GLsizei>(aw), static_cast<GLsizei>(ah), 0, GL_RGBA_INTEGER,
                         GL_UNSIGNED_BYTE, atlas.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            g.tex_key[m] = key;
            ++tex_uploads_;
        }
    }
    glUseProgram(g.prog);
    std::array<GLuint, 256> regs{};
    for (u32 r = 0; r < 256; ++r) regs[r] = bp.reg(static_cast<u8>(r));
    glUniform4uiv(g.u_bp, 64, regs.data());
    std::array<GLint, 16> rv{}, kv{};
    for (u32 i = 0; i < 4; ++i)
        for (u32 c = 0; c < 4; ++c) { rv[i * 4U + c] = tev.reg[i][c]; kv[i * 4U + c] = tev.konst[i][c]; }
    glUniform4iv(g.u_reg, 4, rv.data());
    glUniform4iv(g.u_kon, 4, kv.data());
    glUniform1f(g.u_vp, xf.as_float(xf::VIEWPORT));
    const bool zfreeze = bits(gen, 19, 1) != 0U;
    glUniform4f(g.u_zs0, zslope.dfdx, zslope.dfdy, zslope.f0, zslope.xoff);
    glUniform4f(g.u_zs1, zslope.yoff, static_cast<float>(zslope.x0), static_cast<float>(zslope.y0), zfreeze ? 1.0f : 0.0f);
    const GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(g.u_tex, 8, units);
    glUniform2iv(g.u_lvl, 8 * 16, &g.lvl[0][0]);
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
        glUniform1i(g.u_tri, 8);
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
            glUniform2ui(g.u_stats_on, bbox_on ? 1U : 0U, quads_on ? 1U : 0U);
            glUniform1ui(g.u_tri_base, g.tri_base);
            if (bbox_on || quads_on) g.gpu_dirty = true;
        }
        for (const Range& rx : xr)
            for (const Range& ry : yr) {
                glUniform2f(g.u_off, static_cast<float>(rx.off), static_cast<float>(ry.off));
                glScissor(rx.start, ry.start, rx.end - rx.start, ry.end - ry.start);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size() / 2U));
                ++draws_;
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

void Backend::download(sw::Efb& efb) {
    Impl& g = *impl_;
    std::vector<GLuint> px(static_cast<std::size_t>(kW) * kH * 4U);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    for (int a = 0; a < 2; ++a) {
        glReadBuffer(static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + a));
        glReadPixels(0, 0, kW, kH, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px.data());
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const u32 v = px[(static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4U] & 0xFFFFFFU;
                if (a == 0) efb.set_color24(static_cast<u32>(x), static_cast<u32>(y), v);
                else efb.set_depth24(static_cast<u32>(x), static_cast<u32>(y), v);
            }
    }
}

void Backend::upload(const sw::Efb& efb) {
    Impl& g = *impl_;
    std::vector<GLuint> px(static_cast<std::size_t>(kW) * kH);
    for (int a = 0; a < 2; ++a) {
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] =
                    a == 0 ? efb.color24(static_cast<u32>(x), static_cast<u32>(y)) : efb.depth24(static_cast<u32>(x), static_cast<u32>(y));
        glBindTexture(GL_TEXTURE_2D, g.efb[a]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RED_INTEGER, GL_UNSIGNED_INT, px.data());
    }
}

} // namespace orca::gpu::gles
