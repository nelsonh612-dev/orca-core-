# Auditoría de Milestone 001 y Milestone 002

Fecha: 2026-11-14. Método: lectura completa del código, compilación con GCC 13.3 y ASan/UBSan, sondas ejecutables contra el código original, y verificación de las codificaciones con `llvm-mc -triple=powerpc -mcpu=750`.

## Estado verificado de lo entregado

- M001 y M002 compilan sin advertencias y sus pruebas pasan, también con ASan/UBSan. Lo que decían los informes es cierto.
- **M002 es la versión más reciente y correcta.** M001 tenía `subf` mal (`RA = RB − RT` en vez de `RT = RB − RA`) y no restauraba el PC cuando ocurría un fallo. M002 corrigió las dos cosas.

## Lo que M002 hacía bien (verificado)

`addi`/`addis` con RA=0 · comparaciones con y sin signo, y SO copiado a CR · CR0 en las formas con punto · `rlwinm`/`rlwimi` con máscara MB>ME · `slw`/`srw` con desplazamientos de 32 a 63 · acarreo de `addc`/`subfc` · `b`/`bc`/`bclr` (BO, BI, CTR, AA, LK, destino de LR tomado antes de actualizar LR) · extensión de signo de `lha` · lectura de instrucciones big-endian.

## Errores encontrados (todos confirmados ejecutando el código original)

| # | Error | Evidencia | Corrección en M003 |
|---|---|---|---|
| 1 | **`nor` (XO 124) se ejecutaba como `eqv`**: resultado incorrecto silencioso | `nor r3,r4,r5` con F0F0F0F0/FF00FF00 daba `f00ff00f`; lo correcto es `000f000f` | XO 124 = nor, XO 284 = eqv. Prueba de regresión y prueba aleatoria |
| 2 | `eqv` (XO 284) daba "no soportada" aunque el README la declaraba implementada | sonda | implementada |
| 3 | `lwz`/`stw`/`lhz` desalineados daban `AlignmentFault` | sonda: `lwz r3,0x81(r4)` daba AlignmentFault | El 750CL resuelve en hardware los accesos enteros desalineados en big-endian. Solo `lmw/stmw`, `lwarx/stwcx.` y `dcbz` con W/I generan excepción de alineación |
| 4 | Forma inválida aceptada: `lwzu rT,d(rA)` con rA = rT | sonda: se ejecutaba | `InvalidForm` (parada del anfitrión, sin cambiar el estado) |
| 5 | No había OE ni overflow: `addo` y similares daban "no soportada" y `set_xer_ov` nunca se usaba | sonda | OV y SO acumulativo en todas las instrucciones XO |
| 6 | Las escrituras se validaban **leyendo** el destino | código (`load_store`) | El bus valida el mapeo sin leer. Una prueba con un dispositivo MMIO contador confirma 0 lecturas |
| 7 | No había modelo de excepciones: cualquier fallo detenía el emulador | código | Entrega arquitectónica con SRR0/SRR1/MSR y vectores |
| 8 | Memoria solo `FlatMemory` | código | Bus físico, mapas de consola, MMU |

Además faltaban instrucciones básicas que dan "no soportada" (sin ejecutarse como NOP): `bcctr`, `sraw`, `srawi`, `neg`, `divw`, `mulhw`, `extsb`, `cntlzw`, `addic`, `subfic`, `adde`, `mfspr`, `mfmsr`, `rfi`, `sc` y `sync`, entre otras.

## Pruebas de M002 conservadas

Las 9 pruebas de M002 siguen en `tests/test_cpu_integer.cpp` (prefijo `m002_`), adaptadas a la nueva API. No se eliminó ninguna.
