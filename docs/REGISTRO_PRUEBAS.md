# Registro de pruebas — Milestone 008

Todos los resultados son de ejecuciones reales del 2026-11-14 en este entorno, sobre una copia limpia de los fuentes entregados.

**Entorno:** Linux x86-64 (Intel Xeon @ 2.10 GHz, 2 núcleos) · GCC 13.3.0 · Clang 18.1.3 · CMake 3.28.3 · Ninja · LLVM `llvm-mc` 18.1.3.

## Matriz de compilación y pruebas

| Configuración | Compilación | Advertencias (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`) | CTest |
|---|---|---|---|
| GCC Release | OK | 0 | 13/13 pasan (3,9 s) |
| Clang Release | OK | 0 | 13/13 pasan (3,0 s) |
| GCC Debug + ASan + UBSan (`-fno-sanitize-recover=all`) | OK | 0 | 13/13 pasan (71,8 s) |
| Clang + ASan/UBSan | **NO EJECUTADO**: falta `compiler-rt` en el entorno | — | — |
| Android NDK arm64-v8a | **NO EJECUTADO**: no hay NDK ni sysroot AArch64 | — | — |

## Detalle por suite (GCC + ASan + UBSan)

| Suite | Pruebas | Comprobaciones | Fallos |
|---|---|---|---|
| memory | 10 | 65 | 0 |
| cpu_integer (incluye las 9 pruebas de M002) | 33 | 215 | 0 |
| cpu_system | 23 | 221 | 0 |
| mmu | 14 | 70 | 0 |
| reference (diferencial de enteros) | 6 | 152.001 | 0 |
| **fpu_engine** (M004) | 9 | 101.990 | 0 |
| **cpu_fpu** (M004) | 28 | 30.751 | 0 |
| **system** (M005) | 13 | 108 | 0 |
| **programs** (M005) | 3 | 34 | 0 |
| **disc** (M006) | 12 | 145 | 0 |
| **av** (M007) | 12 | 292 | 0 |
| **exi** (M008) | 16 | 481 | 0 |
| encodings (vs `llvm-mc`) | 189 instrucciones | 181 ensambladas + 8 desensambladas | 0 diferencias |

### EXI y RTC/SRAM/ROM (M008)

Unitarias:
- Registros: estado de reset, paso 0x14 entre canales, bits 0-4 de MAR/LENGTH a cero, solo 32 bits, fin de la ventana en 0x3C.
- CSR: máscaras, bits que se limpian escribiendo 1 (escribir 1 nunca los activa), EXT de solo lectura, ROMDIS permanente y solo en el canal 0, rechazo de dos bits CS y de CLK 110/111 sin cambiar nada.
- Interrupciones: TCINT/EXIINT/EXTINT con y sin máscara hacia la causa EXI del PI.
- Tiempos medidos con el planificador: 32 bits a 8 MHz = **1.944 ciclos**; 8 bits a 1 MHz = **3.888**; 64 bytes a 32 MHz = **7.776**; 8 bits a 32 MHz = 121,5 → **122** (redondeo hacia arriba).
- Los resultados solo aparecen al terminar: DATA y la RAM del DMA no cambian antes.
- Errores con motivo: sin CS, CS vacío, lectura+escritura, RW = 11, DMA de longitud 0 o fuera de RAM, CR o CS cambiados durante una transferencia.
- RTC: cuenta segundos emulados (1000 → 1003 tras 3 s), escritura, vuelta a cero del contador de 32 bits, lectura por bytes.
- SRAM: escritura del bias en el byte 0x0C con el comando 0x20000400 (el mismo que usa Linux), DMA de escritura y lectura de los 64 bytes, parada al pasar del final.
- ROM: sin imagen → parada; con imagen (formato de dirección del 21C3) lectura inmediata y por DMA, parada al pasar del final de la imagen, escritura rechazada, parada tras ROMDIS mientras RTC/SRAM siguen funcionando.
- Orden de bytes de las escrituras inmediatas de 1 a 4 bytes (MSB primero) con un dispositivo de prueba.

Programa `exi_suite` compilado con clang para el 750 (polling de TSTART y de la causa EXI del PI, como hace el software de GameCube):

| Valor | Esperado | Obtenido |
|---|---|---|
| RTC fijado a 0x12345678 por el anfitrión | 0x12345678 | 0x12345678 |
| Ticks del time base de una transferencia de 4 bytes a 8 MHz (1.944 ciclos = 162 ticks + polling) | 162-175 | 163 |
| CR tras el DMA | 0x2 (DMA, TSTART borrado) | 0x2 |
| TCINT / causa del PI tras borrarlo | 8 / 0 | 8 / 0 |
| SRAM por DMA: bytes 0-3 escritos por el programa, 4-7 y 60-63 del anfitrión | DEADBEEF, 04050607, 3C3D3E3F | iguales |
| RTC escrito con 0x100 y leído de nuevo | 0x100 | 0x100 |
| EXT de las ranuras A y B sin tarjeta | 0 / 0 | 0 / 0 |

### Video, entrada y audio (M007)

Unitarias, con los registros cargados con los valores por defecto de YAGCD:
- La temporización NTSC da 525 semilíneas por campo y 429 píxeles por semilínea: **8.108.100 ciclos por campo = 59,94 Hz**. PAL da 625 × 432 × 36 = **9.720.000 ciclos = 50 Hz exactos**.
- DPV/DPH: VCT 263 con HCT 430 al inicio del segundo campo; **VCT 264 es la primera línea completa del segundo campo**, como dice YAGCD; 525 es la última línea del cuadro; en modo no entrelazado va de 1 a 263.
- DI0 (valor por defecto, 263/430) dispara una vez por cuadro. DI1 (1/1) y DI0 quedan separados **exactamente un campo**, medido con el planificador.
- SI: identificación 09 00 23, origin, poll con botones y sticks, canal vacío con NOREP + COMERR, OVRUN/UNRUN, modos no soportados con una parada que da el motivo, sondeo automático que respeta las líneas de video.
- AI: 480 muestras a 48 kHz tras 10.125 × 480 ciclos exactos; a 32 kHz, la interrupción AIIT llega a los 4.860.000 ciclos exactos (10 ms); SCRESET y AIINTVLD.

Programa `av_suite` compilado con clang: programa la VI con los valores NTSC, usa el mando por transferencias y por sondeo automático, cuenta interrupciones de display en el vector 0x500 y espera la coincidencia de la AI:

| Medición en Orca | Valor | Esperado |
|---|---|---|
| Identificación del mando | 0x09002300 | 09 00 + estado (necesita origin, modo 3) |
| Poll con A + START, stick (0xA0, 0x60) | 0x1180A060 | igual |
| Canal 1 vacío | NOREP | NOREP |
| Time base entre interrupciones DI0 (un cuadro) | **1.351.350 ticks** las dos veces | 2 × 675.675 = 1.351.350 |
| DPV leído dentro del manejador | 263 | 263 |
| AISCNT al coincidir con AIIT = 480 | 480 | 480 |
| Time base durante 480 muestras | 405.001 ticks | 405.000 (10 ms a 40,5 MHz) |

### Arranque desde disco con un apploader real (M006)

Las pruebas construyen discos GameCube sintéticos de 2 MiB, con cabecera, magic, apploader en 0x2440, DOL principal en 0x20000 y un patrón de datos en 0x100000. El apploader (`tests/programs/apploader.c`) está escrito a partir del protocolo documentado del IPL, se compila con clang y se convierte en un binario plano con llvm-objcopy. `System::boot_disc` lo ejecuta tal como lo haría el IPL: entry, init(report), main en bucle con cada lectura pedida, y close.

| Prueba | Resultado |
|---|---|
| `integer_suite.dol` arrancado desde el disco vía apploader | 20 valores **idénticos** al anfitrión; punto de entrada 0x80003100 devuelto por close() |
| `dvd_suite.dol` arrancado desde el disco: lee el disco por los registros de la DI, con sondeo y con interrupción (manejador en 0x500 que reconoce TCINT), y fuera de rango | 12 valores iguales a lo esperado: ID del juego copiado por el IPL, reloj de CPU, datos del DMA, DIMAR/DILENGTH, una interrupción DI atendida, DEINT al leer fuera del disco |
| Fallos del arranque (sin disco; apploader que no vuelve) | Mensaje de error explícito, sin bloquearse |

### Programas reales compilados con clang (M005)

Tres programas C (`tests/programs/`) se compilan con **clang 18 para `powerpc-unknown-eabi -mcpu=750`** y se enlazan con ld.lld usando un crt0 y un linker script propios. Cada uno se ejecuta en Orca dos veces: como ELF y como DOL convertido con `orca_elf2dol`. El código lo genera un compilador externo, así que esta prueba no depende de mi ensamblador de pruebas.

| Programa | Contenido | Instrucciones en Orca | Resultado |
|---|---|---|---|
| integer_suite | CRC-32, quicksort, criba, aritmética de 64 bits, divisiones con signo y sin signo, recursión, punteros a función, `switch`, extensión de signo, trucos de bits | 3.419.606 | 20 valores **idénticos** al mismo programa compilado para x86-64 (ELF y DOL) |
| float_suite | Series en doble, acumulación en simple, Newton, matriz 4×4, conversiones int↔float, comparaciones, `frsp`, denormales, overflow | 518.813 | 60 valores **idénticos bit a bit** al anfitrión (ELF y DOL) |
| hardware_suite | Vectores de excepción instalados en tiempo de ejecución (`dcbst`/`icbi`), `sc`, interrupciones del decrementador con `rfi`, time base, FIFO del PI y gather pipe, INTMR | 96.400 | 9 valores iguales a lo esperado (ELF y DOL) |

Ambos lados se compilan con `-ffp-contract=off` para que ningún compilador fusione multiplicaciones y sumas por su cuenta.

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

**Sistema (M005), 9 mutaciones reales, 9 detectadas:** S1 eventos simultáneos en orden inverso · S2 se pierde el resto del time base · S3 mapas de RAM GC/Wii intercambiados · S4 el PI ignora la máscara · S5 vuelta del FIFO con error de uno · S6 bit de ejecución del ELF · S7 inicio del BSS del DOL desplazado · S8 DBAT1 del arranque HLE en una dirección incorrecta (la primera versión de esta mutación era equivalente y no rompía nada; se reemplazó) · S9 bloqueo en `Idle` no informado: la prueba se cuelga, así que se añadió un TIMEOUT de 300 s a cada prueba de CTest para que un cuelgue cuente como falla.

**Disco y DI (M006), 11 de 11 detectadas:** D1 DIMAR no avanza · **D2 escribir DISR borra todas las interrupciones: no se detectaba al principio** → se añadió `di_writing_masks_keeps_pending_interrupts` · D3 TCINT ignora la máscara · D4 offset del disco sin multiplicar por 4 · D5 una lectura fuera de rango informa éxito · D6 finalización síncrona · D7 bucle del apploader cortado · D8 la cabecera del disco no se copia a memoria baja · D9 el divisor pasa offsets absolutos · D10 los discos Wii no se rechazan · D11 falta el global del reloj de CPU.

**Video, entrada, audio y planificador (M007), 14 de 14 detectadas:** V1 fórmula de longitud de campo · V2 VCT empezando en 0 · **V3 el software puede activar INT: no se detectaba al principio** → se añadió `vi_software_cannot_set_int` · V4 VICLK ignorado · V5 PGOFF del XFB ignorado · V6 NIN ignorado · S1 falta el bit 7 del segundo byte de estado · S2 leer INBUF no borra RDST · S3 canal vacío sin NOREP · S4 orden de los bits EN invertido · A1 SCRESET ignorado · A2 AIINTVLD ignorado · A3 la coincidencia AIIT llega tarde · C1 los eventos ven el tiempo final del avance (el error real corregido en este hito).

**EXI y RTC/SRAM (M008), 21 de 22 detectadas:** E1 escribir CSR borra todas las interrupciones · E2 se acepta CLK 110 · E3 duración sin redondeo hacia arriba · E4 bytes de DATA en orden inverso · E5 una lectura corta borra los bytes bajos de DATA · E6 TCINT ignora la máscara · E7 ROMDIS en cualquier canal · E8 DATA no se actualiza al terminar · E9 no se activa TCINT · E10 CS1 y CS2 intercambiados · E11 MAR sin alinear · E12 se aceptan dos bits CS · E13 EXT activo con cualquier dispositivo · E14 se permite cambiar CS durante una transferencia · I1 dirección desplazada 8 bits en vez de 6 · I2 ROMDIS ignorado · I3 el RTC no avanza · I4 se permite un byte de más · I6 se puede escribir la ROM · I7 el RTC se actualiza con el primer byte · I8 se aceptan datos durante una lectura.
I5 (liberar CS no reinicia la secuencia) sobrevivió y es **equivalente**: cada nueva selección llama a `select()`, que ya reinicia la secuencia, así que no hay forma de observar la diferencia.

**Total: 89 mutaciones detectadas.**

## Errores encontrados durante M008

- No apareció ningún error en el código del emulador.
- **Errores en mis pruebas:** combiné la base de la SRAM con el desplazamiento usando OR (0x20000100 | 0x300 = 0x20000300) en lugar de sumarlos (0x20000400); esperé CR = 0 tras el DMA, cuando el bit DMA se conserva y solo se borra TSTART (CR = 0x2); y dos comprobaciones mal planteadas en la prueba de la ROM (una esperaba un fallo al escribir DATA y otra era siempre verdadera). Las cuatro se corrigieron antes de entregar.
- `orca_run` con `exi_suite.dol` se para limpiamente en el primer acceso al puerto de pruebas (0x0E000000), que solo existe en las pruebas: es lo esperado.

## Errores encontrados durante M007

- **Error real en el planificador (presente desde M005):** al avanzar varios eventos de una vez, cada callback veía el tiempo final del avance y no su propio tiempo previsto, así que un dispositivo que se reprogramaba de forma relativa perdía eventos intermedios. Lo descubrió `vi_field_listener_and_xfb`: con 4 campos solo se disparaba 1. Ahora el tiempo avanza evento por evento. Se actualizaron las expectativas de `scheduler_cancel_reschedule_and_lateness`, que dependían de la semántica anterior, y ahora también comprueba que cada callback ve su tiempo exacto.
- **Error real en la VI durante el desarrollo:** al reiniciarse la temporización, un evento en la posición (1,1) se reprogramaba indefinidamente en el mismo instante, y la prueba se colgaba. Se corrigió con un indicador de arranque reciente que solo vale para la primera programación.
- **Errores en mis pruebas:** expresiones con ternarios sin sentido (se cambiaron por constantes calculadas), un origin esperado incorrecto (es el de fábrica, no el estado actual), una prueba del SI que borraba TCINTMSK, DI0 esperado a medio campo en lugar de uno completo, y 10 ms calculados como 40.500 ticks en vez de 405.000.

## Errores encontrados durante M006

- **Ninguno en el emulador.** `dvd_suite` se detuvo al principio en el vector 0x900. La traza mostró que, al activar EE, se tomaba la excepción del decrementador que estaba pendiente desde el arranque, el mismo comportamiento del PEM documentado en M005. El programa no había instalado ese vector; ahora instala todos los vectores antes de activar EE, como hace un sistema operativo real.
- Mejora de diagnóstico: antes, una escritura rechazada por un dispositivo se informaba como "unmapped". Ahora la parada nombra el dispositivo y su motivo.

## Errores encontrados durante M005

- **Ninguno en el emulador.** Las fallas que aparecieron venían de mis propias expectativas, y se investigaron antes de cambiar nada:
  - Un evento periódico se desfasaba porque lo reprogramaba con `now`; la prueba ahora usa el tiempo previsto (`now − late`).
  - Esperaba que SRR1 guardara MSR[POW], pero el PEM no lo incluye entre los bits que se guardan.
  - `hardware_suite` esperaba 5 periodos del decrementador. Al trazar TB y DEC en cada interrupción se vio que la primera ya estaba pendiente desde el arranque (DEC pasa de 0 a −1 con el primer tick), y según el PEM esa petición solo se borra al tomar la excepción. Se corrigió la expectativa (4 periodos completos) y se añadió `writing_dec_does_not_cancel_a_pending_request`.
- clang ensambla `mftb` como `mfspr 268`, que Orca no acepta (el 750CL documenta `mftb` con XO 371). El programa usa la codificación del 750 escrita directamente.
- El linker cargaba las cabeceras ELF en 0x80000000, encima de los vectores de excepción. Se resolvió con PHDRS explícitos en el linker script.

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
