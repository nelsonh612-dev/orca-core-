# Orca Core 0.18.0-dev — Android ARM64

Esta aplicación integra el frontend Android con el núcleo nativo. No debe confundirse con un emulador completo ni con una versión de compatibilidad comercial.

## Integración añadida en este hito

- Carga mediante selector de archivos de imágenes de disco GameCube sin cifrar (`.iso` / `.gcm`) y arranque por el apploader HLE existente.
- Conexión de un `StandardController` al canal SI 0 y controles táctiles básicos (D-pad, A/B/X/Y/START).
- Ejecución del intérprete PowerPC desde el hilo de renderizado con un presupuesto de 8.1 millones de ciclos por iteración.
- Captura del XFB indicado por el VI, conversión YUV 4:2:2 a RGBA y presentación mediante una textura GLES.
- Conexión del backend GLES al renderer de GPU cuando la extensión `GL_EXT_shader_framebuffer_fetch` está disponible.

## Limitaciones conocidas y explícitas

- No hay JIT ARM64: la CPU sigue usando el intérprete. El presupuesto de ejecución no implica rendimiento en tiempo real.
- La carga de discos Wii cifrados, RVZ, GCZ, WBFS y otros formatos comprimidos no está soportada.
- El arranque HLE no sustituye la IPL/OS real; el propio núcleo documenta servicios y globals aún ausentes.
- La presentación XFB es una ruta de salida de respaldo. No demuestra que el backend GLES de rasterización esté validado en todos los dispositivos.
- El DSP tiene intérprete y registros, pero aún no hay ruta de mezcla/streaming PCM conectada a `AudioTrack`.
- Los controles táctiles básicos se conectan al pad SI; todavía no hay configuración, perfiles ni soporte de mandos Android externos.
- Guardados de estado, administración de memory cards desde la interfaz y sincronización A/V completa siguen pendientes.
- La compatibilidad del backend GLES con Mali-G52 debe comprobarse en el teléfono real.

## Construcción

El workflow de GitHub Actions del repositorio compila `arm64-v8a` usando Android SDK/NDK y Gradle. Para compilar localmente se necesita Android SDK, NDK 27.2.12479018, CMake 3.22.1, JDK y Gradle disponible mediante el wrapper del proyecto (si está incluido).
