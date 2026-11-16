# Orca Core M022 — aritmética con acarreo en el JIT ARM64

## Base

M022 parte del código fuente M021. Se conserva M021 sin modificar y el trabajo se realiza en una copia separada.

## Cambio implementado

Se añaden dos formas D de PowerPC que actualizan XER[CA]:

- `addic` (opcode 12): suma `GPR[RA]` y el inmediato de 16 bits extendido con signo. Emite `ADDS W` y recoge el bit C de NZCV para escribir XER[CA].
- `subfic` (opcode 8): calcula inmediato extendido con signo menos `GPR[RA]`. Emite `SUBS W`; el bit C de ARM64 representa “sin préstamo”, que coincide con la semántica de acarreo de esta operación PowerPC.

En ambos casos se conserva el resto de XER: el traductor limpia exclusivamente `XER_CA` y vuelve a insertar el nuevo valor. `GPR[0]` se trata como registro normal en RA y RT, según la codificación de estas instrucciones.

La lectura de NZCV se hace inmediatamente después de la operación aritmética y del almacenamiento del resultado. Las instrucciones de carga/almacenamiento no modifican NZCV. El traductor todavía no admite `addic.` porque también requiere actualizar CR0 con LT/GT/EQ/SO.

## Validación de codificación

Se ensamblaron las instrucciones AArch64 emitidas con Clang integrado (`--target=aarch64-linux-gnu`) y se contrastaron con las palabras del emisor:

- `ADDS W11,W9,W10`: `0x2B0A012B`
- `SUBS W11,W10,W9`: `0x6B09014B`
- `MRS X12,NZCV`: `0xD53B420C`
- `UBFX W12,W12,#29,#1`: `0x531D758C`
- `LSL W12,W12,#29`: `0x5303098C`
- `BIC W9,W9,W13`: `0x0A2D0129`
- `ORR W9,W9,W12`: `0x2A0C0129`

Se añadió una prueba de traducción que comprueba la presencia de las operaciones de lectura/extracción de C y actualización de XER. La prueba diferencial completa para AArch64 incluye `addic` y `subfic`, pero solo se ejecutará cuando las pruebas se compilen y ejecuten en AArch64.

## Verificación ejecutada

- Host x86_64, GCC 14.2, C++20, Ninja, Release, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.
- Compilación completa aprobada.
- CTest: 19 pruebas; 18 aprobadas, 1 omitida (`encodings`, falta `llvm-mc`).
- `jit_arm64` pasó con la nueva prueba de traducción.
- Clang ensambló el conjunto de instrucciones AArch64 y `llvm-objdump` confirmó sus mnemónicos y palabras.
- En x86_64, `run_jit()` usa el intérprete; no se ejecutó el bloque nativo ni la prueba diferencial condicional AArch64.
- No se ejecutó en QEMU ni en el Redmi 15C/Mali-G52. No se generó APK ni se midió rendimiento.

## Cobertura nativa acumulada

- `addi`, `addis`
- `mulli`
- `addic`, `subfic`
- `ori`, `oris`, `xori`, `xoris`
- `and`, `or`, `xor` con `Rc=0`

`addic.` y las formas XO que actualizan CR/XER siguen fuera de la cobertura nativa. También siguen pendientes las ramas, loads/stores, FPU, excepciones y operaciones con efectos laterales.

## Riesgos y pendientes

1. Ejecutar las pruebas diferenciales en ARM64 real o emulación AArch64 confiable.
2. Añadir `addic.` solo al implementar la actualización exacta de CR0 (incluido XER[SO]).
3. Continuar con operaciones XO después de modelar CR/XER y las formas `OE`/`Rc`.
4. Diseñar puntos de salida precisos para scheduler, timebase, decrementer e interrupciones antes de conectar el JIT al bucle de `System::run()`.
5. Añadir ramas y después loads/stores con MMU, excepciones y efectos laterales.
6. Medir el JIT frente al intérprete en ARM64 real.

No se afirma compatibilidad con juegos ni mejora de rendimiento.
