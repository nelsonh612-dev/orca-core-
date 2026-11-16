# M017 — Android bootstrap / GPU capability probe (development scaffold (audited and hardened))

## Scope of this package

This is **not a playable Android emulator** and it does not claim M017 is complete. It adds the first Android host layer needed to test the M016 GLES backend on real hardware:

- Kotlin `Activity` with a `GLSurfaceView`.
- EGL context request for OpenGL ES 3.2.
- ARM64-only Android ABI configuration.
- JNI entry point that reports vendor, renderer, GL/GLSL versions, extension count, `GL_EXT_shader_framebuffer_fetch`, `GL_ARM_shader_framebuffer_fetch` and `GL_EXT_texture_buffer`. It rejects contexts below ES 3.2 before constructing the backend and checks `glGetError()` after initialization.
- Calls `orca::gpu::gles::Backend::init()` while the GL context is current, then releases its GL resources on the GL thread.
- The app deliberately reports backend initialization failure instead of silently switching to a different renderer.

The screen is a diagnostic surface. It does not yet load a game, execute the emulated CPU, present an XFB, map controller input, or stream DSP audio. The Android view hierarchy was simplified so the GL surface receives the remaining screen height directly instead of being nested in a scroll container.

## Build requirements

- Android Studio with Android SDK Platform 35.
- Android NDK installed through SDK Manager.
- CMake 3.22.1 and a compatible Gradle/JDK installation.
- An ARM64 Android device/emulator capable of creating an ES 3.2 context for runtime testing.

Open the `android/` directory as the Gradle project, select `app`, and build the `debug` variant. The configured ABI is `arm64-v8a`.

## Runtime acceptance test

On the target device, record the complete status text and Logcat tag `OrcaCore`. A successful context alone is not a successful backend test. The acceptance condition for this probe is:

1. EGL creates an ES 3.2 context.
2. The device reports its actual GL vendor, renderer and version.
3. `GL_EXT_shader_framebuffer_fetch` is present.
4. `Backend::init()` succeeds and creates a complete EFB framebuffer.
5. No GL errors are reported during initialization.

This probe is intentionally strict. The M016 shader requires `GL_EXT_shader_framebuffer_fetch`; seeing the related `GL_ARM_shader_framebuffer_fetch` extension is **not** treated as equivalent. Its shader interface and integer attachment semantics differ and need a separate implementation and conformance tests before it can be enabled.

## Current blockers / not implemented

- The `System` still executes the PowerPC interpreter in a host-driven run loop; there is no Android frame scheduler or pacing loop.
- `Gpu::set_backend()` exists, but the Android shell does not yet connect a running `System` to the backend or present the VI XFB to the window.
- Audio Interface and DSP DMA timing exist, but no PCM sample stream is exposed to Android's audio output API.
- Touch/gamepad input is not translated to the emulated SI controller protocol.
- No Android file picker, game library, save-state, settings or UI exists.
- No JIT exists in this package. The PowerPC interpreter remains the execution engine.
- The GLES backend still downloads/uploads the EFB around CPU copies and decodes/hashes texture data on each draw. It is not yet optimized for low-end devices.
- No NDK is installed in the build environment used for this package, so Android compilation and APK generation have not been verified here.

## Mali-G52 / Redmi 15C risk

M016 checks specifically for `GL_EXT_shader_framebuffer_fetch`. Arm's Mali documentation describes `GL_ARM_shader_framebuffer_fetch` and the `gl_LastFragColorARM` interface. The Android runtime probe reports both extension names separately. Do not assume the G52 driver exposes the EXT variant, or that ARM's variant can be substituted into the current integer EFB shader. The Redmi 15C must be queried directly.

If EXT fetch is absent, the next engineering task is a separately designed backend path (for example a tested ping-pong or compute/raster hybrid), not simply changing the extension directive.


## Re-audit 0.17.0-dev2

The native desktop core was rebuilt from this source tree with GCC 14.2, CMake 3.31.6, Ninja and `ORCA_WERROR=ON`. The full available CTest run completed with 17 passing suites and `encodings` skipped because `llvm-mc` is not installed. The PowerPC guest test programs were cross-compiled as part of this build.

The GLES backend was not built in this environment: EGL/GLES development libraries are not installed. The Android Gradle/NDK build was not run: no Android SDK, NDK, Gradle executable or connected Android device is available. Therefore no APK or Redmi 15C compatibility result is claimed.

The JNI probe now reports GL vendor and GLSL version, checks the numeric GL major/minor version, distinguishes EXT and ARM framebuffer-fetch extensions, refuses to initialize the M016 backend when ES 3.2 or the exact EXT extension is absent, and checks for a pending GL error after backend initialization. This remains a probe only, not an emulator frontend.


## Continuous integration

`.github/workflows/ci.yml` defines reproducible GCC/Clang core test jobs and an Android ARM64 debug APK job. The Android job installs SDK 35, NDK 27.2.12479018 and CMake 3.22.1, then builds with Gradle 8.9. This workflow has been added to the source package but has **not** been run on GitHub because this workspace is not connected to the user's private repository. A successful CI artifact would verify compilation, not runtime behavior on the Redmi 15C.
