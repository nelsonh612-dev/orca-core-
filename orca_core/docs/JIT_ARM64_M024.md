# Orca Core M024 — ampliación aritmética del JIT ARM64

## Base

M024 parte del código M023. Se preserva el artefacto M023 y se trabaja en una copia independiente.

## Cambios

- Añadida traducción nativa de `add` (XO 266) cuando `OE=0`.
- Añadida traducción nativa de `addc` (XO 10) cuando `OE=0`, con extracción del carry AArch64 y actualización exclusiva de `XER[CA]`.
- Añadida traducción nativa de `subf` (XO 40) cuando `OE=0`.
- Añadida traducción nativa de `subfc` (XO 8) cuando `OE=0`, usando `RB - RA`; el carry AArch64 indica ausencia de préstamo y actualiza `XER[CA]`.
- Las cuatro operaciones admiten `Rc=1`: calculan `CR0` a partir del resultado firmado e incorporan `XER[SO]`, conservando `CR1–CR7`.
- Las variantes `OE=1` siguen en el intérprete: no se traducen hasta implementar y verificar el desbordamiento y el comportamiento sticky de `XER[OV/SO]`.
- Se agregan pruebas de traducción y rechazo explícito de `OE=1`.

## Verificación

- Compilación completa del proyecto en host x86_64 con GCC 14.2, C++20, Ninja, Release y `ORCA_WERROR=ON`: aprobada.
- CTest: 19 pruebas registradas; 18 aprobadas y `encodings` omitida porque `llvm-mc` no está disponible.
- Prueba `jit_arm64`: aprobada (13 casos; 52 comprobaciones según el ejecutable).
- Codificaciones AArch64 contrastadas con ensamblador integrado de Clang para `ADD`, `ADDS`, `MVN` y `SUBS`; coinciden con las palabras emitidas por el traductor.

## Límites importantes

- La máquina de compilación es x86_64. El código ARM64 generado no se ejecutó aquí; la prueba diferencial bajo `__aarch64__` no se ejecutó.
- No se dispone de QEMU AArch64, Android SDK/NDK, Gradle ni dispositivo Android conectado en este entorno.
- El JIT todavía no se integra en el bucle principal temporizado de `System::run()`: antes hacen falta salidas precisas por eventos de scheduler, timebase, decrementer, interrupciones y excepciones.
- No se afirma mejora de rendimiento, compatibilidad de juegos ni APK instalable.

## Siguientes bloques pendientes

1. Implementar con equivalencia diferencial `adde`, `subfe`, `addze`, `addme`, `subfze`, `subfme`, `neg` y variantes OE/Rc.
2. Ampliar cargas/almacenamientos y ramas con comprobación de MMU, excepciones y efectos laterales; no cruzar límites inseguros de bloque.
3. Diseñar salidas del JIT que respeten el presupuesto de instrucciones y eventos de `System::run()`; integrar solo tras pruebas de temporización.
4. Validar código nativo en ARM64 real o QEMU confiable.
5. Completar y validar frontend Android, GLES/Vulkan, audio, mandos, carga de juegos y guardados; producir APK solo cuando exista toolchain y compilar realmente.
6. Medir rendimiento en Redmi 15C/Helio G81-Ultra/Mali-G52 y corregir cuellos de botella medidos.
