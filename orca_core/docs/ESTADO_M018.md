# Orca Core 0.18.0-dev — auditoría del avance Android

Esta nota separa código presente de funciones completas. La compilación y las pruebas host no prueban compatibilidad con un teléfono ni que un juego comercial arranque.

| Componente solicitado | Estado comprobado en este hito | Pendiente real |
|---|---|---|
| CPU PowerPC | Intérprete existente con grupos enteros, sistema, MMU y FPU/paired-single cubiertos por código y suites propias. | No es una implementación completa del Gekko/Broadway; hay instrucciones, excepciones, modos, temporización y servicios de sistema aún no implementados. |
| JIT ARM64 | No implementado. | Traductor real PPC→AArch64, bloques, invalidación coherente, manejo de excepciones y comparación diferencial contra el intérprete. |
| GPU | Backend software y backend GLES 3.2 existentes. El frontend conecta el backend GLES a `System::gpu()` cuando el driver expone `GL_EXT_shader_framebuffer_fetch`; si no, queda el renderer software. | Validar equivalencia y estabilidad en Mali-G52; optimizar transferencias EFB y completar estados/funciones no soportados. |
| XFB | VI notifica campos; el frontend lee el XFB de RAM, convierte YUV 4:2:2 a RGBA y lo presenta en una textura GLES. | Verificar modos PAL/NTSC, entrelazado, offsets, escalado y comportamiento en juegos reales. |
| Audio DSP | Intérprete DSP, interfaz DSP/ARAM y AI register model presentes. | No existe aún una ruta PCM completa DSP→mezclador→Android `AudioTrack`; no hay sonido jugable. |
| Controles | El sistema crea y conecta un `StandardController` a SI canal 0. Android envía botones táctiles y eventos de mandos físicos/joystick al pad. | Perfiles, calibración, asignación editable, rumble y pruebas de hardware. |
| Archivos | Selector Android copia un archivo a caché; `FileDisc` y `boot_disc()` intentan arrancar discos GameCube sin cifrar. | Wii cifrado, RVZ/GCZ/WBFS, DOL/ELF desde UI, memory-card manager, guardados persistentes y savestates. |
| Sincronización | `System::run()` usa el scheduler y el frontend estima ciclos por campo desde el VI cuando está configurado. | Temporización CPU/GPU/DSP precisa, audio como reloj maestro, pacing real, frame skipping y sincronización A/V. |

## Cambios concretos de M018

- `System` ahora posee un `StandardController` conectado por defecto al canal SI 0 y expone su estado al frontend.
- Se añadió una prueba del protocolo de lectura de estado del pad.
- El JNI Android crea un `System`, abre `FileDisc`, llama a `boot_disc()`, conecta el backend GLES, ejecuta el intérprete y captura el XFB al comenzar cada campo.
- Se añadió presentación de XFB con una textura GLES y un blitter sencillo.
- La actividad Android incluye selector de discos GameCube, controles táctiles y entrada de botones/ejes de mandos Android.
- La ruta de CPU usa el presupuesto de ciclos estimado por el VI; no es un limitador de velocidad ni prueba que alcance 50/60 FPS.

## Verificación realizada en este entorno

- Configuración host: CMake + Ninja, GCC 14.2, C++20, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.
- Compilación host: correcta.
- CTest: 17 suites pasaron; `encodings` quedó omitida porque la herramienta externa requerida no está disponible. CTest reporta 100% de las pruebas ejecutadas.
- Sintaxis del JNI: comprobada con `g++ -fsyntax-only`, headers Khronos del proyecto y stubs locales para las cabeceras Android. Esto NO sustituye una compilación con NDK.
- Backend GLES host: no compilado porque no hay bibliotecas EGL/GLES instaladas en este entorno.
- APK: no generada ni instalada. Aquí no hay Android SDK/NDK ni Gradle ejecutable. El workflow del repositorio puede provisionarlos, pero requiere ejecutar CI en un repositorio GitHub.
- Dispositivo real: no probado. En particular, la extensión `GL_EXT_shader_framebuffer_fetch` y el backend deben verificarse en el Mali-G52 objetivo.

## Riesgos conocidos del frontend experimental

- La ejecución de hasta un campo de CPU se realiza en el hilo de renderizado; sin JIT puede bloquear la interfaz o no alcanzar la velocidad requerida.
- La carga del disco y el arranque del apploader pueden tardar y todavía no hay una pantalla de progreso dedicada.
- La captura XFB depende de que el juego configure correctamente los registros VI y de que el buffer resida en RAM accesible.
- No se debe anunciar compatibilidad de juegos hasta probar discos reales y registrar la primera instrucción/dispositivo no soportado.
