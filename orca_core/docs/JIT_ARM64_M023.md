# Orca Core M023 — `addic.` y actualización de CR0 en JIT ARM64

## Base

M023 parte del código fuente M022. El ZIP M022 permanece intacto; este hito se trabaja en una copia independiente.

## Cambio implementado

Se añade traducción de `addic.` (opcode 13), además de conservar `addic` y `subfic`:

- `addic.` calcula la suma con inmediato de 16 bits extendido con signo.
- `ADDS W` produce el resultado y el carry-out, que se copia a `XER[CA]`.
- `CMP Wresult,#0` y `CSET` determinan LT, GT y EQ según interpretación signed de 32 bits.
- Se incorpora `XER[SO]` al nibble CR0 después de actualizar CA. SO es sticky y no cambia con `addic.`.
- Solo se reemplaza el nibble CR0; los campos CR1–CR7 se preservan.
- El resto de XER se conserva al actualizar CA.

La traducción usa registros scratch W9–W15 y no modifica GPR adicionales. `RA=0` y `RT=0` siguen siendo referencias al registro GPR0 real.

## Validación de codificación

Clang integrado (`--target=aarch64-linux-gnu`) y `llvm-objdump` confirmaron las instrucciones usadas para CR0:

- `CMP W11,#0`: `0x7100017F`
- `CSET W12,LT`: `0x1A9FA7EC`
- `CSET W14,GT`: `0x1A9FD7EE`
- `CSET W15,EQ`: `0x1A9F17EF`
- `LSR W13,W9,#31`: `0x531F7D2D`
- `LSL W12,W12,#31`: `0x5301018C`
- `LSL W14,W14,#30`: `0x530205CE`
- `LSL W15,W15,#29`: `0x530309EF`
- `LSL W13,W13,#28`: `0x53040DAD`

La prueba de traducción comprueba que el bloque contiene las operaciones de comparación y CSET. La prueba diferencial condicional AArch64 incluye `addic.` con XER[SO]/[OV] activos y un CR inicial no nulo para comprobar preservación de campos cuando se ejecute en ARM64.

## Verificación ejecutada

- Host x86_64, GCC 14.2, C++20, Ninja, Release, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.
- Compilación limpia completa aprobada.
- CTest: 19 pruebas; 18 aprobadas, 1 omitida (`encodings`, falta `llvm-mc`).
- `jit_arm64` pasó con la nueva prueba de traducción de `addic.`.
- Clang ensambló las instrucciones AArch64 y `llvm-objdump` confirmó sus palabras.
- En x86_64 `run_jit()` cae al intérprete. No se ejecutó el bloque nativo ni la prueba diferencial condicional AArch64.
- Sin prueba en QEMU ni en el Redmi 15C/Mali-G52; sin APK ni benchmark.

## Cobertura nativa acumulada

- `addi`, `addis`
- `mulli`
- `addic`, `addic.`, `subfic`
- `ori`, `oris`, `xori`, `xoris`
- `and`, `or`, `xor` con `Rc=0`

## Pendiente

1. Ejecutar las pruebas diferenciales en ARM64 real o emulación AArch64 confiable.
2. Ampliar operaciones XO con cálculo preciso de carry, overflow, CR0 y SO sticky.
3. Diseñar salidas de bloque que respeten scheduler, timebase, decrementer e interrupciones antes de integrar el JIT en `System::run()`.
4. Añadir ramas y loads/stores con semántica MMU, excepciones y efectos laterales.
5. Medir rendimiento frente al intérprete en hardware ARM64.

No se afirma compatibilidad con juegos ni mejora de rendimiento.
