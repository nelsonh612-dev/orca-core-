// Orca Core — Android bridge: an emulation thread that runs frontend::Emulator,
// copies each frame to the app's Surface (ANativeWindow, RGBA8888, scaled to fit
// by the system compositor through the buffer geometry), plays the audio with
// AAudio (48 kHz stereo, 16-bit), and receives controls from the Java side.
// Pacing: the emulated time follows the wall clock (the thread waits when it
// is ahead); when the phone is too slow the game runs slower instead of
// skipping work.
#include "orca/frontend/emulator.hpp"
#include "orca/wiimote/wiimote.hpp"

#ifdef ORCA_HAVE_GLES
#include "orca/gpu/gles/backend.hpp"
#include <EGL/egl.h>
#endif

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OrcaCore", __VA_ARGS__)

struct Session {
    orca::frontend::Emulator emu;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::mutex window_mutex;
    ANativeWindow* window{nullptr};
    AAudioStream* audio{nullptr};
    std::mutex input_mutex;
    orca::hw::PadState pad[4];
    bool pad_used[4]{true, false, false, false};
    orca::wiimote::InputState remote[4];
    std::atomic<unsigned> connect_mask{0}; // remotes to connect (handled on the emulation thread)
    std::atomic<int> button{0};            // 1 reset, 2 power
    std::mutex status_mutex;
    std::string status;
    std::string stop_reason;
    double speed{0};
    bool want_gles{true};
    std::string renderer{"software"};
#ifdef ORCA_HAVE_GLES
    EGLDisplay egl_display{EGL_NO_DISPLAY};
    EGLContext egl_context{EGL_NO_CONTEXT};
    EGLSurface egl_surface{EGL_NO_SURFACE};
    std::unique_ptr<orca::gpu::gles::Backend> backend;
#endif
    explicit Session(orca::frontend::Settings s) : emu(std::move(s)) {}
};

aaudio_data_callback_result_t audio_callback(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* s = static_cast<Session*>(user);
    auto* out = static_cast<int16_t*>(data);
    const std::size_t want = static_cast<std::size_t>(frames) * 2U;
    const std::size_t got = s->emu.take_audio(std::span<orca::s16>(out, want));
    if (got < want) std::memset(out + got, 0, (want - got) * sizeof(int16_t)); // underrun: silence
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void start_audio(Session& s) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_callback, &s);
    if (AAudioStreamBuilder_openStream(b, &s.audio) == AAUDIO_OK) AAudioStream_requestStart(s.audio);
    else s.audio = nullptr;
    AAudioStreamBuilder_delete(b);
}

void blit(Session& s) {
    const auto& f = s.emu.frame();
    if (f.width == 0 || f.rgba.empty()) return;
    std::lock_guard<std::mutex> lock(s.window_mutex);
    if (s.window == nullptr) return;
    ANativeWindow_setBuffersGeometry(s.window, static_cast<int32_t>(f.width), static_cast<int32_t>(f.height),
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(s.window, &buf, nullptr) != 0) return;
    auto* dst = static_cast<uint8_t*>(buf.bits);
    const uint32_t rows = std::min<uint32_t>(f.height, static_cast<uint32_t>(buf.height));
    const uint32_t cols = std::min<uint32_t>(f.width, static_cast<uint32_t>(buf.width));
    for (uint32_t y = 0; y < rows; ++y) {
        uint8_t* d = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(buf.stride) * 4U;
        const uint8_t* src = f.rgba.data() + static_cast<std::size_t>(y) * f.width * 4U;
        std::memcpy(d, src, cols * 4U);
        for (uint32_t x = 0; x < cols; ++x) d[x * 4U + 3U] = 0xFF; // opaque
    }
    ANativeWindow_unlockAndPost(s.window);
}

void run_loop(Session* s);

#ifdef ORCA_HAVE_GLES
// The GPU backend draws into its own framebuffers; a 1x1 pbuffer surface is
// enough for its context (the picture reaches the screen through the XFB).
bool start_gles(Session& s, std::string& error) {
    s.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s.egl_display == EGL_NO_DISPLAY || !eglInitialize(s.egl_display, nullptr, nullptr)) { error = "no EGL display"; return false; }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config{};
    EGLint n = 0;
    if (!eglChooseConfig(s.egl_display, attrs, &config, 1, &n) || n < 1) { error = "no EGL configuration for OpenGL ES 3"; return false; }
    const EGLint ctx_attrs[] = {0x3098 /* EGL_CONTEXT_MAJOR_VERSION */, 3, 0x30FB /* EGL_CONTEXT_MINOR_VERSION */, 2, EGL_NONE};
    s.egl_context = eglCreateContext(s.egl_display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (s.egl_context == EGL_NO_CONTEXT) { error = "OpenGL ES 3.2 context not available"; return false; }
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    s.egl_surface = eglCreatePbufferSurface(s.egl_display, config, pb);
    if (s.egl_surface == EGL_NO_SURFACE || !eglMakeCurrent(s.egl_display, s.egl_surface, s.egl_surface, s.egl_context)) {
        error = "cannot make the EGL context current";
        return false;
    }
    s.backend = std::make_unique<orca::gpu::gles::Backend>();
    if (!s.backend->init(error)) { s.backend.reset(); return false; }
    orca::gpu::gles::Backend* b = s.backend.get();
    s.emu.system()->gpu().set_backend(b, [b](orca::gpu::sw::Efb& f) { b->download(f); },
                                      [b](const orca::gpu::sw::Efb& f) { b->upload(f); });
    return true;
}

void stop_gles(Session& s) {
    if (s.emu.system() != nullptr && s.backend) s.emu.system()->gpu().set_backend(nullptr, nullptr, nullptr);
    s.backend.reset();
    if (s.egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s.egl_surface != EGL_NO_SURFACE) eglDestroySurface(s.egl_display, s.egl_surface);
        if (s.egl_context != EGL_NO_CONTEXT) eglDestroyContext(s.egl_display, s.egl_context);
        eglTerminate(s.egl_display);
    }
    s.egl_display = EGL_NO_DISPLAY;
}
#endif

void run(Session* s) {
#ifdef ORCA_HAVE_GLES
    if (s->want_gles) {
        std::string error;
        if (start_gles(*s, error)) {
            s->renderer = "GLES";
        } else {
            LOGI("GLES backend unavailable, using the software renderer: %s", error.c_str());
            stop_gles(*s);
            s->renderer = "software (" + error + ")";
        }
    }
#endif
    run_loop(s);
#ifdef ORCA_HAVE_GLES
    stop_gles(*s);
#endif
}

void run_loop(Session* s) {
    using clock = std::chrono::steady_clock;
    const double hz = static_cast<double>(s->emu.cpu_hz());
    auto wall_start = clock::now();
    orca::u64 cycles_start = s->emu.emulated_cycles();
    auto speed_t = clock::now();
    orca::u64 speed_c = cycles_start;
    while (s->running) {
        if (s->paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            wall_start = clock::now();
            cycles_start = s->emu.emulated_cycles();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(s->input_mutex);
            for (unsigned p = 0; p < 4; ++p)
                if (s->pad_used[p]) s->emu.set_pad(p, s->pad[p]);
            for (unsigned r = 0; r < 4; ++r) s->emu.set_remote(r, s->remote[r]);
        }
        const unsigned connect = s->connect_mask.exchange(0);
        for (unsigned r = 0; r < 4; ++r)
            if ((connect & (1U << r)) != 0U) (void)s->emu.connect_remote(r);
        if (const int b = s->button.exchange(0); b != 0) s->emu.press_button(b == 2);
        if (!s->emu.run_frame()) {
            std::lock_guard<std::mutex> lock(s->status_mutex);
            s->stop_reason = s->emu.error();
            LOGI("emulation stopped: %s", s->stop_reason.c_str());
            s->running = false;
            break;
        }
        blit(*s);
        // follow the wall clock
        const double emulated = static_cast<double>(s->emu.emulated_cycles() - cycles_start) / hz;
        const double wall = std::chrono::duration<double>(clock::now() - wall_start).count();
        if (emulated > wall) std::this_thread::sleep_for(std::chrono::duration<double>(emulated - wall));
        else if (wall - emulated > 0.25) { wall_start = clock::now(); cycles_start = s->emu.emulated_cycles(); }
        const double since = std::chrono::duration<double>(clock::now() - speed_t).count();
        if (since >= 1.0) {
            const double speed = static_cast<double>(s->emu.emulated_cycles() - speed_c) / hz / since;
            speed_t = clock::now();
            speed_c = s->emu.emulated_cycles();
            std::lock_guard<std::mutex> lock(s->status_mutex);
            s->speed = speed;
            char b[96];
            std::snprintf(b, sizeof b, "%s %.0f%% frame %llu ", s->emu.game_id().c_str(), speed * 100.0,
                          static_cast<unsigned long long>(s->emu.frame().number));
            s->status = b + s->renderer;
        }
    }
}

std::string str(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

Session* session(jlong h) { return reinterpret_cast<Session*>(h); }

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_orca_emu_NativeLib_create(JNIEnv* env, jclass, jstring nand_dir, jstring key_file,
                                                           jboolean jit, jint region, jboolean gles) {
    orca::frontend::Settings s;
    s.jit = jit != JNI_FALSE;
    s.dsp_hle = true;
    s.nand_dir = str(env, nand_dir);
    s.wii_key_file = str(env, key_file);
    switch (region) {
    case 1: s.new_nand.region = orca::ios::WiiRegion::Europe; s.new_nand.country = 110; break;
    case 2: s.new_nand.region = orca::ios::WiiRegion::Japan; s.new_nand.language = 0; s.new_nand.country = 1; break;
    case 3: s.new_nand.region = orca::ios::WiiRegion::Korea; s.new_nand.language = 9; s.new_nand.country = 136; break;
    default: break;
    }
    auto* session = new Session(std::move(s));
    session->want_gles = gles != JNI_FALSE;
    return reinterpret_cast<jlong>(session);
}

// Loads the disc behind `fd` (ownership passes to the core). Returns null or an error message.
JNIEXPORT jstring JNICALL Java_org_orca_emu_NativeLib_loadDisc(JNIEnv* env, jclass, jlong h, jint fd) {
    std::string error;
    auto disc = orca::disc::FdDisc::open(fd, error);
    if (!disc || !session(h)->emu.load_disc(std::move(disc), error)) return env->NewStringUTF(error.c_str());
    return nullptr;
}

JNIEXPORT jboolean JNICALL Java_org_orca_emu_NativeLib_isWii(JNIEnv*, jclass, jlong h) {
    return session(h)->emu.wii() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_setSurface(JNIEnv* env, jclass, jlong h, jobject surface) {
    Session* s = session(h);
    std::lock_guard<std::mutex> lock(s->window_mutex);
    if (s->window != nullptr) ANativeWindow_release(s->window);
    s->window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_start(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    if (s->running || !s->emu.loaded()) return;
    s->running = true;
    start_audio(*s);
    s->thread = std::thread(run, s);
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_setPaused(JNIEnv*, jclass, jlong h, jboolean p) {
    session(h)->paused = p != JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_destroy(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    s->running = false;
    if (s->thread.joinable()) s->thread.join();
    if (s->audio != nullptr) { AAudioStream_requestStop(s->audio); AAudioStream_close(s->audio); }
    {
        std::lock_guard<std::mutex> lock(s->window_mutex);
        if (s->window != nullptr) ANativeWindow_release(s->window);
        s->window = nullptr;
    }
    delete s;
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_setPad(JNIEnv*, jclass, jlong h, jint port, jint buttons, jint sx,
                                                          jint sy, jint cx, jint cy, jint l, jint r) {
    Session* s = session(h);
    if (port < 0 || port > 3) return;
    std::lock_guard<std::mutex> lock(s->input_mutex);
    auto& p = s->pad[port];
    p.buttons = static_cast<orca::u16>(buttons);
    p.stick_x = static_cast<orca::u8>(sx);
    p.stick_y = static_cast<orca::u8>(sy);
    p.cstick_x = static_cast<orca::u8>(cx);
    p.cstick_y = static_cast<orca::u8>(cy);
    p.trigger_l = static_cast<orca::u8>(l);
    p.trigger_r = static_cast<orca::u8>(r);
    s->pad_used[port] = true;
}

// Wii Remote: core buttons (wiimote::button bits), pointer (-1..1, NaN = off screen),
// tilt as 10-bit accelerometer values, and an optional Nunchuk.
JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_setRemote(JNIEnv*, jclass, jlong h, jint index, jint buttons,
                                                             jfloat px, jfloat py, jint ax, jint ay, jint az,
                                                             jboolean nunchuk, jint nsx, jint nsy, jboolean c,
                                                             jboolean z) {
    Session* s = session(h);
    if (index < 0 || index > 3) return;
    std::lock_guard<std::mutex> lock(s->input_mutex);
    auto& r = s->remote[index];
    r.buttons = static_cast<orca::u16>(buttons);
    r.accel_x = static_cast<orca::u16>(ax);
    r.accel_y = static_cast<orca::u16>(ay);
    r.accel_z = static_cast<orca::u16>(az);
    r.points = (px == px && py == py) ? orca::wiimote::pointer_points(px, py) : std::array<orca::wiimote::CameraPoint, 4>{};
    if (nunchuk != JNI_FALSE) {
        orca::wiimote::NunchukState n;
        n.stick_x = static_cast<orca::u8>(nsx);
        n.stick_y = static_cast<orca::u8>(nsy);
        n.c = c != JNI_FALSE;
        n.z = z != JNI_FALSE;
        r.nunchuk = n;
    } else {
        r.nunchuk.reset();
    }
}

// Like pressing a button on a sleeping remote: it asks the console to connect.
JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_connectRemote(JNIEnv*, jclass, jlong h, jint index) {
    if (index >= 0 && index < 4) session(h)->connect_mask |= 1U << index;
}

JNIEXPORT void JNICALL Java_org_orca_emu_NativeLib_pressButton(JNIEnv*, jclass, jlong h, jboolean power) {
    session(h)->button = power != JNI_FALSE ? 2 : 1;
}

JNIEXPORT jstring JNICALL Java_org_orca_emu_NativeLib_status(JNIEnv* env, jclass, jlong h) {
    Session* s = session(h);
    std::lock_guard<std::mutex> lock(s->status_mutex);
    return env->NewStringUTF(s->status.c_str());
}

JNIEXPORT jstring JNICALL Java_org_orca_emu_NativeLib_stopReason(JNIEnv* env, jclass, jlong h) {
    Session* s = session(h);
    std::lock_guard<std::mutex> lock(s->status_mutex);
    if (s->stop_reason.empty()) return nullptr;
    return env->NewStringUTF(s->stop_reason.c_str());
}

} // extern "C"
