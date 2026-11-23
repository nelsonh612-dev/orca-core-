# Registro de pruebas — Milestone 004

Todos los resultados son de ejecuciones reales del 2026-11-14 en este entorno, sobre una copia limpia de los fuentes entregados.

**Entorno:** Linux x86-64 (Intel Xeon @ 2.10 GHz, 2 núcleos) · GCC 13.3.0 · Clang 18.1.3 · CMake 3.28.3 · Ninja · LLVM `llvm-mc` 18.1.3.

## Matriz de compilación y pruebas

| Configuración | Compilación | Advertencias (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`) | CTest |
|---|---|---|---|
| GCC Release | OK | 0 | 8/8 pasan |
| Clang Release | OK | 0 | 8/8 pasan |
| GCC Debug + ASan + UBSan (`-fno-sanitize-recover=all`) | OK | 0 | 8/8 pasan (26,4 s) |
| Clang + ASan/UBSan | **NO EJECUTADO**: falta `compiler-rt` en el entorno | — | — |
| Android NDK arm64-v8a | **NO EJECUTADO**: no hay NDK ni sysroot AArch64 | — | — |

## Detalle por suite (GCC + ASan + UBSan)

| Suite | Pruebas | Comprobaciones | Fallos |
|---|---|---|---|
| memory | 10 | 65 | 0 |
| cpu_integer (incluye las 9 pruebas de M002) | 33 | 215 | 0 |
| cpu_system | 22 | 216 | 0 |
| mmu | 14 | 70 | 0 |
| reference (diferencial de enteros) | 6 | 152.001 | 0 |
| **fpu_engine** (M004) | 9 | 101.990 | 0 |
| **cpu_fpu** (M004) | 28 | 30.751 | 0 |
| encodings (vs `llvm-mc`) | 189 instrucciones | 181 ensambladas + 8 desensambladas | 0 diferencias |

### Referencia exacta de punto flotante (M004)

`tests/fp_reference.hpp` implementa la aritmética IEEE-754 con **racionales exactos** (enteros de precisión arbitraria) y redondeo escrito a mano a partir de la norma: cuatro modos, subnormales, overflow y tininess antes del redondeo. No comparte código ni técnica con el motor, que usa la FPU del anfitrión con `<cfenv>`.

- **101.917 operaciones aleatorias** (suma, resta, multiplicación, división, FMA en sus 4 variantes y redondeo a simple; en doble y simple; en los 4 modos) comparadas **bit a bit** en el valor, OX, UX, XX, FR, FI y FPRF. Los operandos incluyen subnormales, valores cerca del overflow, cancelaciones y mantisas cortas. El resultado es idéntico con GCC y con Clang.
- **15.000 instrucciones FP de extremo a extremo**: la CPU las decodifica y las ejecuta, y se compara el FPR destino, ps1, **el FPSCR completo** (con bits acumulativos aleatorios previos) y CR1 contra valores derivados de la referencia exacta.

### Diferencial de enteros (M003)

152.000 ejecuciones contra un modelo de referencia independiente, comparando el estado completo de la CPU tras cada instrucción, con registros que pueden coincidir entre sí.

### Codificaciones

Coinciden con LLVM en 189 instrucciones, 52 de ellas de FP clásica. **No se pueden verificar con LLVM** (no tiene el Gekko): `lswx`, `stswx`, `mcrxr`, las paired-single y `psq_*`. Se revisaron contra las tablas del manual del 750CL y la prueba los lista explícitamente.

## Pruebas de mutación

Se introducen errores a propósito y se comprueba que alguna prueba falle. Al final se restaura el código y todo vuelve a pasar.

**Motor FP (7 de 7 detectadas):**

| Mutación | Fallos que provoca |
|---|---|
| F1 sin redondeo a impar (doble redondeo en simple) | 1.657 |
| F2 tininess después del redondeo | 12 |
| F3 FR = FI | 59.099 |
| F4 modos de redondeo +∞/−∞ intercambiados | 48.507 |
| F5 FMA no fusionada | 3.348 |
| F6 FPRF de denormales simples | 2.566 |
| F7 truncamiento simple en el modo equivocado | 18.730 |

**Integración FP/paired-single en la CPU (12 de 12 detectadas tras corregir las pruebas):** P1 `lfs` no escribe ps1 · P2 una operación simple no escribe ps1 · P3 FX con cualquier excepción · **P4 `fcmpu` activa VXVC: no se detectaba al principio** → se añadió `fcmpu_with_qnan_sets_no_exception` · P5 escala GQR sin signo · P6 `ps_merge10` · P7 `ps_sum1` ejecutado como `ps_sum0` · P8 dirección del DMA · P9 FPRF nunca se escribe · P10 no se comprueba LSQE · P11 sin saturación · P12 slots de `ps_muls` intercambiados.

P13 (palabra alta de `fctiw`) sobrevivió como se esperaba, porque son bits indefinidos. Se añadió una prueba que fija el valor documentado.

**Enteros, sistema y MMU (M003): 15 de 15 detectadas.**

**Total: 34 mutaciones detectadas.**

## Pruebas modificadas en M004

- `cpu_system/floating_point_unavailable_and_unimplemented`: en M003 comprobaba que `lfs` con MSR[FP]=1 daba *Unsupported*. Ahora la FPU existe, así que comprueba que `lfs` se ejecuta y que el opcode 4 sin HID2[PSE] da una excepción de instrucción ilegal. Es una actualización por un cambio de comportamiento intencional, no se ocultó ninguna falla.

## Errores encontrados durante M004

- Una prueba no compilaba por un tipo de retorno de lambda; se corrigió la prueba.
- Las dos mutaciones que sobrevivieron (P4 y P13) dejaron al descubierto huecos en las pruebas, ya cubiertos.
- Ningún error del motor FP apareció en las comparaciones con la referencia exacta.

## Rendimiento (solo como referencia en el anfitrión)

`orca_bench_interpreter`, compilación Release con GCC, modo real, 3 ejecuciones de cada bucle:

| Bucle | Ejecuciones | Resultado |
|---|---|---|
| Enteros (`addi`, `lwz`, `stw`, `rlwinm`, `bdnz`) | 25 M instrucciones | 21,6 · 21,2 · 19,0 M instr/s |
| FP (`fmadd`, `fadds`, `ps_add`, `addi`, `bdnz`) | 10 M instrucciones | 4,2 · 4,3 · 4,2 M instr/s |

El bucle FP es más lento porque cada operación cambia el modo de redondeo del anfitrión para obtener resultados y banderas exactos. Estas cifras son **del anfitrión x86**, no del Redmi 15C, y no dicen nada sobre FPS. El intérprete es la referencia de precisión; el rendimiento tiene que venir del JIT ARM64.
