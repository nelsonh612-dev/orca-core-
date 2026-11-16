# Orca Core M025 — operaciones aritméticas con acarreo de entrada

## Cambios respecto de M024

- Añadida traducción de `adde` (XO 138), con `XER[CA]` como carry-in y actualización de `XER[CA]` con el carry-out.
- Añadida traducción de `subfe` (XO 136), equivalente a `RB - RA - (1 - CA)`, usando el carry-in y carry-out de `XER[CA]`.
- Ambas operaciones admiten `Rc=1` y actualizan CR0 con LT/GT/EQ y XER[SO], preservando CR1–CR7.
- `OE=1` sigue rechazado por el traductor y cae al intérprete.
- Añadidas pruebas de traducción y ampliada la prueba diferencial condicional AArch64 para incluir ambas operaciones.

## Verificación

- Compilación completa host x86_64, GCC 14.2, C++20, Ninja, Release y `ORCA_WERROR=ON`: aprobada.
- CTest: 19 pruebas registradas; 18 aprobadas y `encodings` omitida por falta de `llvm-mc`.
- `jit_arm64`: 14 casos, 57 comprobaciones, 0 fallos.
- Ensamblador integrado de Clang para AArch64 confirmó `MSR NZCV`, `ADCS` y `SBCS` con las codificaciones usadas.

## Límites

- No se ejecutó el código generado en ARM64: host x86_64, sin QEMU AArch64 ni dispositivo conectado. La prueba diferencial condicional sigue sin ejecutarse.
- No se integró el JIT en `System::run()`; siguen pendientes las salidas precisas para scheduler, timebase, decrementer, interrupciones y excepciones.
- No se produjo APK ni se midió rendimiento.

## Pendiente

- `addze`, `addme`, `subfze`, `subfme`, `neg`, variantes OE y tratamiento completo de XER[OV/SO].
- Loads/stores, ramas, FPU y paired-single en JIT con MMU, fallos y efectos laterales correctos.
- Integración temporizada segura, validación ARM64 real, frontend Android completo y mediciones en el Helio G81/Mali-G52.
