# Orca Core

Independent GameCube / Wii PowerPC emulator core, with an Android ARM64 frontend.

The C++20 emulator sources, build system, tests, Android project and technical reports are in [orca_core/](orca_core/).

## Current development status

- Gekko/Broadway reference interpreter and system/device models: partial implementation documented in `orca_core/README.md`.
- ARM64 JIT: native AArch64 code generation and block cache are present. M026 connects the partial JIT to the timed system loop on AArch64, while retaining interpreter fallback for unsupported instructions.
- GPU: software reference renderer and an OpenGL ES 3.2 backend are present; device-specific validation and additional performance work remain.
- Android: ARM64 frontend and APK build configuration are present. Android audio output and validation with commercial games are not complete.

**Important:** a successful host test or APK build does not establish full game compatibility or a performance improvement. The generated ARM64 code still needs differential execution tests on real ARM64 hardware.

## Build and test on a desktop

Requires CMake, Ninja and a C++20 compiler.

```bash
cmake -S orca_core -B orca_core/build -G Ninja -DORCA_BUILD_TESTS=ON -DORCA_WERROR=ON -DORCA_GLES=OFF
cmake --build orca_core/build --parallel
ctest --test-dir orca_core/build --output-on-failure
```

## Build Android ARM64 debug APK

Requires JDK 21, Android SDK/NDK and Gradle 8.9.

```bash
gradle -p orca_core/android :app:assembleDebug
```

GitHub Actions runs the desktop GCC/Clang tests and builds an ARM64 debug APK on pushes and pull requests.

## Technical reports

- [M026 — timed ARM64 JIT integration](orca_core/docs/JIT_ARM64_M026.md)
- [JIT implementation history](orca_core/docs/JIT_ARM64_MILESTONE.md)
- [Project status and subsystem notes](orca_core/README.md)
