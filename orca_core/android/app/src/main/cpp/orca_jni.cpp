#include <jni.h>
#include <GLES3/gl32.h>
#include <android/log.h>

#include "orca/disc/disc_image.hpp"
#include "orca/gpu/gles/backend.hpp"
#include "orca/system/system.hpp"
#include "orca/video/xfb.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr const char* kTag = "OrcaCore";
std::unique_ptr<orca::gpu::gles::Backend> g_backend;
std::unique_ptr<orca::system::System> g_system;
std::vector<orca::u8> g_rgba;
std::uint32_t g_frame_width = 0;
std::uint32_t g_frame_height = 0;
GLuint g_blit_program = 0;
GLuint g_blit_texture = 0;
GLuint g_blit_vbo = 0;
bool g_backend_attached = false;
bool g_frame_dirty = false;
std::string g_last_run_status = "Sin juego cargado";

std::string gl_string(GLenum name) {
    const auto* p = glGetString(name);
    return p != nullptr ? reinterpret_cast<const char*>(p) : "(no disponible)";
}

std::vector<std::string> extensions() {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    std::vector<std::string> out;
    out.reserve(count > 0 ? static_cast<std::size_t>(count) : 0U);
    for (GLint i = 0; i < count; ++i) {
        const auto* p = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (p != nullptr) out.emplace_back(reinterpret_cast<const char*>(p));
    }
    return out;
}

bool has(const std::vector<std::string>& list, const char* name) {
    for (const auto& item : list) if (item == name) return true;
    return false;
}

jstring java_string(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

GLuint compile_shader(GLenum type, const char* source, std::string& error) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    GLint n = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &n);
    std::string log(static_cast<std::size_t>(std::max(n, 1)), '\0');
    glGetShaderInfoLog(shader, n, nullptr, log.data());
    error = log;
    glDeleteShader(shader);
    return 0;
}

bool init_blitter(std::string& error) {
    static constexpr char vs[] = R"(#version 300 es
layout(location=0) in vec2 aPosition;
layout(location=1) in vec2 aUv;
out vec2 vUv;
void main(){ gl_Position=vec4(aPosition,0.0,1.0); vUv=aUv; }
)";
    static constexpr char fs[] = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uFrame;
out vec4 outColor;
void main(){ outColor=texture(uFrame,vUv); }
)";
    const GLuint v = compile_shader(GL_VERTEX_SHADER, vs, error);
    if (v == 0) return false;
    const GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, error);
    if (f == 0) { glDeleteShader(v); return false; }
    g_blit_program = glCreateProgram();
    glAttachShader(g_blit_program, v);
    glAttachShader(g_blit_program, f);
    glLinkProgram(g_blit_program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(g_blit_program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint n = 0;
        glGetProgramiv(g_blit_program, GL_INFO_LOG_LENGTH, &n);
        std::string log(static_cast<std::size_t>(std::max(n, 1)), '\0');
        glGetProgramInfoLog(g_blit_program, n, nullptr, log.data());
        error = log;
        glDeleteProgram(g_blit_program);
        g_blit_program = 0;
        return false;
    }
    glGenTextures(1, &g_blit_texture);
    glBindTexture(GL_TEXTURE_2D, g_blit_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &g_blit_vbo);
    return glGetError() == GL_NO_ERROR;
}

void capture_xfb(bool bottom) {
    if (!g_system) return;
    const auto field = g_system->vi().xfb_field(bottom);
    if (field.width == 0U || field.height == 0U || field.stride < field.width * 2U ||
        field.width > 1024U || field.height > 600U || field.stride > 4096U) return;
    std::vector<orca::u8> yuv(static_cast<std::size_t>(field.stride) * field.height);
    if (g_system->bus().dma_read(field.address, yuv) != orca::mem::BusStatus::Ok) return;
    std::vector<orca::u8> rgba;
    if (!orca::video::decode_xfb(yuv, field.width, field.height, field.stride, rgba)) return;
    g_rgba.swap(rgba);
    g_frame_width = field.width;
    g_frame_height = field.height;
    g_frame_dirty = true;
}

void draw_frame() {
    if (g_system) {
        if (g_backend && !g_backend_attached) {
            g_system->gpu().set_backend(g_backend.get(),
                [](orca::gpu::sw::Efb& efb) { g_backend->download(efb); },
                [](const orca::gpu::sw::Efb& efb) { g_backend->upload(efb); });
            g_backend_attached = true;
        }
        orca::u64 budget = 8100000ULL; // NTSC fallback until the guest programs VI timing.
        const auto timing = g_system->vi().timing();
        if (timing.valid) {
            const orca::u64 half_lines = timing.interlaced
                ? (g_system->vi().bottom_field() ? timing.even_half_lines : timing.odd_half_lines)
                : timing.odd_half_lines;
            budget = g_system->vi().cycles_per_line() * half_lines / 2U;
        }
        const auto result = g_system->run(budget);
        if (result.device_stop) g_last_run_status = "Dispositivo detenido: " + result.device_error;
        else if (result.stop != orca::ppc::StepResult::Executed && result.stop != orca::ppc::StepResult::Idle)
            g_last_run_status = std::string("CPU detenido en ") + orca::ppc::to_string(result.stop) +
                " PC=0x" + [&] { std::ostringstream s; s << std::hex << g_system->cpu().state().pc; return s.str(); }();
    }
    if (g_rgba.empty() || g_blit_program == 0U) return;
    glUseProgram(g_blit_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_blit_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (g_frame_dirty) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(g_frame_width),
                     static_cast<GLsizei>(g_frame_height), 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba.data());
        g_frame_dirty = false;
    }
    glUniform1i(glGetUniformLocation(g_blit_program, "uFrame"), 0);
    // Triangle strip: position.xy, uv.xy. Flip Y because XFB row zero is the top line.
    static constexpr GLfloat vertices[] = {
        -1.f,-1.f, 0.f,1.f,  1.f,-1.f, 1.f,1.f,
        -1.f, 1.f, 0.f,0.f,  1.f, 1.f, 1.f,0.f
    };
    glBindBuffer(GL_ARRAY_BUFFER, g_blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcacore_android_MainActivity_nativeInit(JNIEnv* env, jobject) {
    std::ostringstream report;
    const std::string version = gl_string(GL_VERSION);
    const std::string vendor = gl_string(GL_VENDOR);
    const std::string renderer = gl_string(GL_RENDERER);
    const std::string shading = gl_string(GL_SHADING_LANGUAGE_VERSION);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const auto ext = extensions();
    const bool ext_fetch = has(ext, "GL_EXT_shader_framebuffer_fetch");
    const bool arm_fetch = has(ext, "GL_ARM_shader_framebuffer_fetch");
    report << "Orca Core 0.18.0-dev — frontend Android\n\n"
           << "Fabricante: " << vendor << "\nGPU: " << renderer << "\n"
           << "OpenGL ES: " << version << " (" << major << "." << minor << ")\n"
           << "GLSL: " << shading << "\n"
           << "GL_EXT_shader_framebuffer_fetch: " << (ext_fetch ? "sí" : "no") << "\n"
           << "GL_ARM_shader_framebuffer_fetch: " << (arm_fetch ? "sí" : "no") << "\n"
           << "Extensiones GL notificadas: " << ext.size() << "\n\n";

    g_backend.reset();
    g_backend_attached = false;
    if (major < 3 || (major == 3 && minor < 2) || !ext_fetch) {
        report << "Backend GLES de rasterización: no disponible en este dispositivo.\n"
               << "La salida XFB de respaldo puede seguir presentándose cuando exista un fotograma.";
    } else {
        g_backend = std::make_unique<orca::gpu::gles::Backend>();
        std::string error;
        if (g_backend->init(error)) report << "Backend GLES: inicializado; pendiente validar con un juego real.\n";
        else { report << "Backend GLES no disponible: " << error << "\n"; g_backend.reset(); }
    }
    std::string blit_error;
    if (init_blitter(blit_error)) report << "Presentador XFB GLES: listo.\n";
    else report << "Presentador XFB GLES no disponible: " << blit_error << "\n";
    const std::string result = report.str();
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", result.c_str());
    return java_string(env, result);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcacore_android_MainActivity_nativeLoadDisc(JNIEnv* env, jobject, jstring path) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (chars == nullptr) return java_string(env, "No se pudo leer la ruta del archivo.");
    std::string error;
    auto disc = orca::disc::FileDisc::open(chars, error);
    env->ReleaseStringUTFChars(path, chars);
    if (!disc) return java_string(env, error);
    auto candidate = std::make_unique<orca::system::System>(orca::system::SystemConfig{orca::system::Console::GameCube});
    candidate->insert_disc(std::move(disc));
    if (!candidate->boot_disc(error)) return java_string(env, "No se pudo iniciar el disco: " + error);
    candidate->set_field_listener([](bool bottom) { capture_xfb(bottom); });
    g_system = std::move(candidate);
    g_rgba.clear();
    g_frame_width = g_frame_height = 0;
    g_frame_dirty = false;
    g_backend_attached = false;
    g_last_run_status = "Disco iniciado; ejecutando CPU PowerPC interpretada.";
    return java_string(env, "Disco GameCube cargado. " + g_last_run_status);
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcacore_android_MainActivity_nativeSetPadState(JNIEnv*, jobject, jint buttons, jint sx, jint sy,
                                                          jint csx, jint csy, jint tl, jint tr) {
    if (!g_system) return;
    auto& p = g_system->controller().state;
    p.buttons = static_cast<orca::u16>(buttons);
    p.stick_x = static_cast<orca::u8>(std::clamp(sx, 0, 255));
    p.stick_y = static_cast<orca::u8>(std::clamp(sy, 0, 255));
    p.cstick_x = static_cast<orca::u8>(std::clamp(csx, 0, 255));
    p.cstick_y = static_cast<orca::u8>(std::clamp(csy, 0, 255));
    p.trigger_l = static_cast<orca::u8>(std::clamp(tl, 0, 255));
    p.trigger_r = static_cast<orca::u8>(std::clamp(tr, 0, 255));
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcacore_android_MainActivity_nativeStatus(JNIEnv* env, jobject) {
    return java_string(env, g_last_run_status);
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcacore_android_MainActivity_nativeDrawFrame(JNIEnv*, jobject) {
    draw_frame();
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcacore_android_MainActivity_nativeShutdown(JNIEnv*, jobject) {
    g_system.reset();
    g_backend.reset();
    g_rgba.clear();
    g_frame_dirty = false;
    g_backend_attached = false;
    if (g_blit_texture) glDeleteTextures(1, &g_blit_texture);
    if (g_blit_vbo) glDeleteBuffers(1, &g_blit_vbo);
    if (g_blit_program) glDeleteProgram(g_blit_program);
    g_blit_texture = g_blit_vbo = g_blit_program = 0;
}
