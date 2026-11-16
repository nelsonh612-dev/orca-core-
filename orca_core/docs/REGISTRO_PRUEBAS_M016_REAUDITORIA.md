# Re-auditoría local del paquete M016

Fecha del entorno: 2026-11-15. Paquete auditado: `orca_core_milestone_016.zip` proporcionado en esta conversación.

## Compilación y pruebas reproducidas

| Configuración | Resultado |
|---|---|
| GCC 14.2, Release, `ORCA_WERROR=ON` | Compila correctamente |
| GCC / CTest | 17 pruebas pasan; `encodings` se omite porque no se encontró `llvm-mc` |
| Clang 17, Release, `ORCA_WERROR=ON` | Compila correctamente |
| Clang / CTest | 17 pruebas pasan; `encodings` se omite porque no se encontró `llvm-mc` |
| Clang 17 + ASan/UBSan + `ORCA_WERROR=ON` | Compila correctamente |
| ASan/UBSan / CTest | 17 pruebas pasan; `encodings` se omite porque no se encontró `llvm-mc` |

En los tres entornos CMake informó: `EGL/GLES libraries not found, not built`. Por tanto, `orca_gles` y `orca_test_gles` no se compilaron ni ejecutaron en esta re-auditoría. Esta sesión **no reproduce** las 19 suites declaradas por el informe M016 ni la comparación de imagen píxel por píxel; esos resultados quedan como resultados reportados por el paquete y pendientes de reproducción en un entorno con EGL/GLES.

## Observaciones de integración

- El backend GLES es una biblioteca separada (`orca_gles`), no está enlazado automáticamente al `System`.
- `Gpu::set_backend()` permite inyectar un `TriangleSink` y las operaciones de descarga/carga del EFB, pero no hay aún integración de ciclo de vida/presentación Android.
- El backend requiere ES 3.2 y `GL_EXT_shader_framebuffer_fetch`; si no existe, `Backend::init()` falla explícitamente.
- Las texturas se decodifican y se calcula su clave en cada `flush`; la subida a GL se evita si la clave no cambió, pero el trabajo de decodificación/hash sí se repite.
- Las copias/limpiezas EFB siguen pasando por la ruta CPU mediante descarga y subida.
- `AudioInterface` mantiene el contador y las interrupciones; la salida PCM no está implementada.
- La CPU sigue siendo intérprete. No hay JIT ARM64 en el paquete M016.

## Fuentes técnicas consultadas para M017

- Android NDK documenta el enlace de `libEGL` y `libGLESv3`, y exige consultar la versión/extensiones del contexto real: https://developer.android.com/ndk/guides/stable_apis
- Android documenta la variación de extensiones GL entre dispositivos: https://developer.android.com/develop/ui/views/graphics/opengl/about-opengl
- Arm documenta `GL_ARM_shader_framebuffer_fetch` para Mali: https://documentation-service.arm.com/static/649ad0f138511951cb799268
- MediaTek identifica la GPU Mali-G52 MC2 del Helio G81: https://www.mediatek.com/products/smartphones/mediatek-helio-g81

La compatibilidad concreta del controlador gráfico del Redmi 15C no puede establecerse sin ejecutar la sonda en ese teléfono.
