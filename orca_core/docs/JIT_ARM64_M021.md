# Orca Core M021 — ampliación del JIT ARM64

## Base

M021 parte del ZIP M020. El original no se modifica; este milestone se construye en una copia separada.

## Cambio implementado

Se añade traducción nativa de `mulli` (opcode primario 7):

- Lee `GPR[RA]`, incluido `GPR[0]` cuando `RA=0` (a diferencia de `addi/addis`, aquí RA=0 no es una constante cero).
- Extiende con signo el inmediato de 16 bits.
- Emite una multiplicación ARM64 de 32 bits (`MUL Wd,Wn,Wm`). El resultado bajo de 32 bits coincide con el resultado arquitectónico de `mulli`.
- Escribe el resultado en `GPR[RT]`, incluso cuando `RT=0`.

La codificación de la instrucción ARM64 `mul w11, w9, w10` se contrastó con el ensamblador integrado de Clang para `aarch64-linux-gnu`: bytes `2b 7d 0a 1b`, palabra little-endian `0x1B0A7D2B`, coincidente con el emisor.

Se añadió una prueba de traducción de `mulli` con `RA=0` y `RT=0`. La prueba diferencial de estado completo en `__aarch64__` ahora incluye dos `mulli` consecutivos, escribiendo primero en `GPR[0]` y leyéndolo después.

## Cobertura nativa actual

- `mulli`
- `addi`, `addis`
- `ori`, `oris`, `xori`, `xoris`
- `and`, `or`, `xor` con `Rc=0`

No se traducen todavía instrucciones que actualizan CR/XER, ramas, loads/stores, FPU, excepciones ni operaciones con efectos laterales.

## Verificación

- Configuración: x86_64, GCC 14.2, C++20, Ninja, Release, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.
- Compilación completa: aprobada.
- CTest: 19 pruebas; 18 aprobadas y 1 omitida (`encodings`, requiere `llvm-mc`).
- La suite `jit_arm64` pasa en el anfitrión x86_64. En esa arquitectura `run_jit()` cae al intérprete; por tanto, la prueba diferencial condicional AArch64 no se ejecutó aquí.
- El ensamblador Clang AArch64 confirmó la palabra de la instrucción `MUL` usada por el emisor.
- No se compiló APK ni se ejecutó el JIT en ARM64 real, QEMU o el teléfono objetivo. No se afirma mejora de rendimiento ni compatibilidad con juegos.

## Pendiente

1. Ejecutar las pruebas diferenciales en AArch64 real o emulación confiable.
2. Diseñar salidas de bloque precisas para scheduler, timebase, decrementer e interrupciones antes de conectar el JIT a `System::run()`.
3. Añadir operaciones enteras con CR/XER y ramas con semántica exacta.
4. Incorporar loads/stores solo después de preservar MMU, excepciones, MMIO y efectos laterales.
5. Medir rendimiento JIT frente al intérprete en ARM64 real.
