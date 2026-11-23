# Registro de pruebas — Milestone 003

Todos los resultados son de ejecuciones reales del 2026-11-14 en este entorno, sobre una copia limpia de los fuentes entregados.

**Entorno:** Linux x86-64 (Intel Xeon @ 2.10 GHz, 2 núcleos) · GCC 13.3.0 · Clang 18.1.3 · CMake 3.28.3 · Ninja · LLVM `llvm-mc` 18.1.3.

## Matriz de compilación y pruebas

| Configuración | Compilación | Advertencias (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`) | CTest |
|---|---|---|---|
| GCC Release | OK | 0 | 6/6 pasan |
| Clang Release | OK | 0 | 6/6 pasan |
| GCC Debug + ASan + UBSan (`-fno-sanitize-recover=all`) | OK | 0 | 6/6 pasan |
| Clang + ASan/UBSan | **NO EJECUTADO**: falta `compiler-rt` en el entorno y no se pueden instalar paquetes | — | — |
| Android NDK arm64-v8a | **NO EJECUTADO**: no hay NDK ni sysroot AArch64 | — | — |

## Detalle por suite (GCC + ASan + UBSan)

| Suite | Pruebas | Comprobaciones | Fallos |
|---|---|---|---|
| memory | 10 | 65 | 0 |
| cpu_integer (incluye las 9 pruebas de M002) | 33 | 215 | 0 |
| cpu_system | 22 | 214 | 0 |
| mmu | 14 | 70 | 0 |
| reference (diferencial aleatoria) | 6 | 152.001 | 0 |
| encodings (vs `llvm-mc`) | 137 instrucciones | 129 ensambladas + 8 desensambladas | 0 diferencias |

- **Diferencial aleatoria:** 152.000 ejecuciones del intérprete comparadas con un modelo de referencia independiente (aritmética ancha con signo, división por piso, máscaras por bucle de bits). Se compara **el estado completo** de la CPU tras cada instrucción. Los registros se eligen al azar y pueden coincidir entre sí (RT = RA = RB, RA = 0). La semilla es fija, así que es reproducible.
- **Codificaciones:** el codificador de pruebas coincide con LLVM en 137 instrucciones. Los saltos relativos y `mftb` se verifican desensamblando, porque LLVM los ensambla como relocalizaciones o alias. **No se pudieron verificar con LLVM:** `lswx`, `stswx` y `mcrxr`, porque LLVM no los implementa. Se revisaron a mano contra el PEM.

## Pruebas de mutación (solidez de las pruebas)

Se introdujeron errores a propósito en el código y se comprobó que alguna prueba fallara. Al final se restauró el código original y se confirmó que todo volvía a pasar.

| Mutación | Detectada |
|---|---|
| M1 fórmula de overflow | Sí |
| M2 `nor` ejecutado como `eqv` (el error de M002) | Sí |
| M3 acarreo de `sraw` | Sí |
| M4 LR solo se escribe si se toma el salto | **No** al principio → se añadió la prueba `link_register_written_even_when_branch_not_taken` → Sí |
| M5 MSR[EE] conservado al entrar en una excepción | Sí |
| M6 hash secundario de la tabla de páginas | **No** al principio (todas las pruebas usaban HTABMASK=0) → se añadió `page_table_with_htabmask_uses_upper_hash_bits` → Sí |
| M7 bit C en lecturas | Sí |
| M8 sin comprobar RA=RT en cargas con actualización | Sí |
| M9 decrementador con error de uno | Sí |
| M10 máscara de rotación | Sí |
| M11 destino de `bclr` sin limpiar los bits bajos | Sí |
| M12 contador de instrucciones retiradas con `sc` | **No** al principio → se añadió `retired_instruction_counter` → Sí |
| M13 `lwarx` sin reserva | Sí |
| M14 lectura antes de escribir en MMIO (el error de M002) | Sí |
| M15 escritura parcial al fallar | Sí |

Resultado final: **15 de 15 mutaciones detectadas.**

## Errores encontrados durante el desarrollo de M003

- 2 errores en mis propias pruebas, no en el emulador: un `ba` a 0x10000000 está fuera del alcance de ±32 MB, y había una dirección de lectura equivocada en la prueba del hash secundario. Se corrigieron las pruebas.
- 1 codificación equivocada en el benchmark (`rlwinm` con ME=29 en lugar de 28). La detectó la comparación con `llvm-mc` y se corrigió.
- Ningún error del intérprete apareció en la diferencial aleatoria.

## Rendimiento (solo como referencia en el anfitrión)

`orca_bench_interpreter 5000000`, compilación Release con GCC, bucle de 5 instrucciones (`addi`, `lwz`, `stw`, `rlwinm`, `bdnz`) en modo real, 3 ejecuciones:

| Ejecución | Instrucciones | Tiempo | Velocidad |
|---|---|---|---|
| 1 | 25.000.000 | 1,100 s | 22,7 M instr/s |
| 2 | 25.000.000 | 1,256 s | 19,9 M instr/s |
| 3 | 25.000.000 | 1,135 s | 22,0 M instr/s |

Esta cifra es **del anfitrión x86**, no del Redmi 15C, y no dice nada sobre los FPS de un juego. Gekko corre a 486 MHz, así que el intérprete sirve como referencia de precisión y el rendimiento tendrá que venir del JIT ARM64.
