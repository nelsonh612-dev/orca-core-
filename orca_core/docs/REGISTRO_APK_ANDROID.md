# Registro de preparación de APK Android — M017 dev2

## Verificaciones realizadas en este entorno

- Configuración y compilación host del núcleo en Release con `ORCA_WERROR=ON` y `ORCA_GLES=OFF`: correcta.
- CTest: 17 suites aprobadas; `encodings` omitida porque no está disponible `llvm-mc`.
- Compilación individual de `src/gpu/gles/backend.cpp` y `src/gpu/gles/shaders.cpp` con GCC, C++20 y `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`, usando las cabeceras Khronos incluidas: correcta.
- No se compiló el proyecto Android ni se generó una APK en este entorno: no hay Android SDK, NDK ni Gradle instalados.
- No se ejecutó en un teléfono físico; no hay resultados de Mali-G52 que certificar.

## Ruta de generación de APK

`.github/workflows/ci.yml` incluye el job `android-arm64`: instala JDK 21, Android SDK, NDK `27.2.12479018` y CMake `3.22.1`; ejecuta `gradle -p android :app:assembleDebug`; y publica `app-debug.apk` como artefacto `orca-core-android-arm64-debug`.

Para los pasos de publicación e instalación, ver `android/README_BUILD_APK.md`.

## Alcance

La APK generada es únicamente una sonda de GPU y de inicialización del backend GLES. No es una versión jugable del emulador.
