# Orca Core M026 — integración temporizada del JIT ARM64

## Cambios

- El presentador XFB reutiliza los buffers YUV/RGBA entre campos, conserva la asignación de la textura de salida cuando la resolución no cambia y carga el VBO de pantalla una sola vez. Esto elimina asignaciones y transferencias de almacenamiento innecesarias por fotograma.
- El diagnóstico Android distingue el caso de `GL_ARM_shader_framebuffer_fetch` sin `GL_EXT_shader_framebuffer_fetch`: el backend EFB actual no es compatible con esa variante ARM porque depende de dos salidas enteras con fetch EXT.

- `System::run()` usa `Cpu::run_jit()` en compilaciones AArch64; las compilaciones de escritorio no AArch64 conservan el bucle de referencia paso a paso.
- El tamaño del lote JIT queda limitado por el siguiente evento del scheduler y por el presupuesto de ciclos restante. El sistema avanza el reloj por el número exacto de instrucciones completadas antes de seguir procesando eventos.
- `Cpu::run_jit()` informa cuántas instrucciones completó incluso si encuentra una instrucción no traducible, un estado Idle o una parada del host.
- `run_until()` / `call_guest()` pasan su dirección de parada al dispatcher. Esto impide que una rama ejecutada por fallback llegue a la dirección de retorno y continúe ejecutando el stub dentro del mismo lote.
- Se añadió una prueba para el caso de rama interpretada que aterriza en `stop_pc`.
- Se corrigió la ubicación del workflow: GitHub Actions solo descubre workflows en `.github/workflows/` desde la raíz del repositorio. Ahora prueba GCC y Clang y compila el APK Android ARM64.
- Se corrigió la instalación del SDK Android: se evita el paquete retirado `tools`.
- La versión Android se actualizó a `0.26.0-dev` (versionCode 20).

## Límites que permanecen

- La traducción nativa sigue siendo un subconjunto de operaciones enteras lineales. Ramas, cargas/almacenamientos, FPU, paired-single, excepciones y operaciones con efectos laterales usan el intérprete.
- No se ha medido rendimiento ni se ha ejecutado el código generado en un teléfono ARM64.
- Un APK compilado correctamente no demuestra compatibilidad con juegos reales.
- La salida de audio Android y la optimización adicional del backend GPU siguen pendientes.

## Validación

El workflow `Orca Core M026 CI` ejecuta:
- Configuración, compilación y CTest con GCC y Clang, `ORCA_WERROR=ON`.
- Compilación de APK Debug limitada a `arm64-v8a` y publicación del APK como artefacto.

Los resultados finales de cada job deben consultarse en la pestaña Actions; no se deben inferir del hecho de que el workflow haya arrancado.
