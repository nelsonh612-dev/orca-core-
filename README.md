# Orca Core — Milestone 005

Orca Core es un emulador independiente de GameCube/Wii escrito en C++20, con Android ARM64 como plataforma objetivo. **No contiene código de Dolphin** y no deriva de él.

> **Estado:** la CPU Gekko/Broadway está completa como intérprete de referencia: enteros, sistema, MMU, FPU, paired-singles, cuantización GQR y caché bloqueada. Además, el sistema ya arranca y ejecuta programas DOL/ELF reales con un planificador de tiempo, interrupciones del Processor Interface y el gather pipe. **Todavía no es un emulador jugable.** Faltan VI (video), DSP/audio, discos, controles, EXI, el JIT, la GPU y los componentes de Wii.

## Qué incluye este milestone

| Área | Estado |
|---|---|
| Auditoría de M001/M002 | Completa. 8 errores confirmados con pruebas y corregidos (`docs/AUDITORIA_M002.md`) |
| Bus de memoria física | RAM, MMIO, DMA, observadores de escritura, seguimiento de páginas de código para invalidar el JIT |
| Mapas de RAM | GameCube (MEM1 de 24 MiB) y Wii (MEM1 + MEM2 de 64 MiB) |
| ISA entera Gekko/Broadway | Completa, salvo `eciwx`/`ecowx` (ver `docs/ESTADO.md`) |
| XER | CA, OV y SO acumulativo en todas las formas `o` y `.` |
| Registros especiales | SPR, MSR, segment registers, BAT, SDR1, TB/DEC, HID0/1/2/4, GQR, PMC |
| Excepciones arquitectónicas | DSI, ISI, externa, alineación, programa, FP no disponible, decrementador y llamada al sistema; SRR0/SRR1, MSR, prefijo IP, `rfi` |
| MMU | Modo real, IBAT/DBAT y tabla de páginas hash (hash primario y secundario, claves y PP, bit N, R/C) |
| Reservas | `lwarx` y `stwcx.`; las escrituras DMA cancelan la reserva |
| **FPU (M004)** | Todas las instrucciones FP del 750CL; FPSCR completo (FX, FEX, VX, OX, UX, ZX, XX, VX*, FR, FI, FPRF, RN); propagación de NaN según el PEM; FMA con un solo redondeo; precisión simple sin doble redondeo; `lfs`/`stfs` bit a bit |
| **Paired-singles (M004)** | Las 32 instrucciones de opcode 4, más `psq_l(u)(x)` y `psq_st(u)(x)` con cuantización GQR (float, u8, u16, s8, s16 y escala con signo) |
| **Caché bloqueada (M004)** | HID2[LCE], `dcbz_l`, DMA DMAU/DMAL en ambos sentidos, 16 KiB |
| **Planificador (M005)** | Eventos deterministas en ciclos de CPU; time base a 1/12 del reloj (486 MHz GC, 729 MHz Wii); la CPU en `Idle` salta al siguiente evento |
| **Processor Interface (M005)** | INTSR/INTMR y línea de interrupción externa; registros del FIFO de CPU; gather pipe (0x0C008000) con ráfagas de 32 bytes y vuelta al inicio. Los registros no implementados dan una parada clara |
| **Cargadores (M005)** | DOL y ELF32 big-endian con validación de límites; `orca_elf2dol`; arranque HLE mínimo (BAT estándar, MSR, globales documentados) |
| **Programas reales (M005)** | 3 programas C compilados con clang para el 750 se ejecutan en Orca como ELF y como DOL; enteros y FP dan resultados **idénticos bit a bit** al mismo programa compilado para el anfitrión |
| Pruebas | 10 suites; 268.917 casos diferenciales (enteros y FP contra referencias independientes, la de FP con aritmética racional exacta); 189 codificaciones verificadas con `llvm-mc`; 43 mutaciones detectadas; 3 programas C reales compilados con clang |

## Compilar y probar

Se necesita CMake 3.20 o superior y un compilador C++20 (probado con GCC 13.3 y Clang 18.1).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Opciones:

```sh
-DORCA_SANITIZE=ON   # AddressSanitizer + UndefinedBehaviorSanitizer
-DORCA_WERROR=ON     # trata las advertencias como errores
```

Ejecutar un DOL o ELF sin interfaz gráfica (imprime por qué se detuvo y los registros):

```sh
./build/orca_run programa.dol [ciclos] [gc|wii]
./build/orca_elf2dol programa.elf programa.dol
```

La prueba `programs` necesita `clang` y `ld.lld` con soporte para PowerPC; si no los encuentra, se marca como *skipped*. La prueba `encodings` busca `llvm-mc` en el PATH. Si no lo encuentra, CTest la marca como *skipped*, no como aprobada.

Benchmark del intérprete. Mide el equipo anfitrión, no un teléfono:

```sh
./build/orca_bench_interpreter 5000000        # bucle de enteros
./build/orca_bench_interpreter 2000000 fp     # bucle de FP y paired-singles
```

### Android ARM64

Todavía no se ha compilado ni probado en Android. Este entorno no tiene el Android NDK. El código no usa ensamblador ni intrínsecos de x86, y el orden big-endian se arma byte a byte, sin depender del anfitrión. El comando previsto es este (sin verificar):

```sh
cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DORCA_BUILD_TESTS=ON
```

## Estructura

```
include/orca/core/bits.hpp          utilidades de bits
include/orca/memory/bus.hpp         bus físico (RAM, MMIO, DMA, invalidación)
include/orca/memory/console_maps.hpp
include/orca/ppc/registers.hpp      números de SPR, bits de MSR/XER/SRR1/DSISR, vectores
include/orca/ppc/state.hpp          estado arquitectónico
include/orca/ppc/mmu.hpp            traducción BAT + tabla de páginas
include/orca/core/scheduler.hpp     planificador de eventos
include/orca/hw/processor_interface.hpp  PI + gather pipe
include/orca/loader/executable.hpp  DOL/ELF
include/orca/system/system.hpp      consola: bus + CPU + planificador + dispositivos
include/orca/ppc/fpu.hpp            motor de punto flotante (puro, sin estado)
include/orca/ppc/cpu.hpp            intérprete de referencia
src/ppc/cpu_fpu.cpp                 FP, paired-singles, psq, caché bloqueada
src/...                             implementaciones
tests/                              suites, ensamblador de pruebas, verificación con llvm-mc
tests/programs/                     programas C + crt0 + linker script (se compilan con clang para el 750)
tools/                              orca_run (ejecutor sin interfaz), orca_elf2dol, benchmark
docs/AUDITORIA_M002.md  docs/REGISTRO_PRUEBAS.md  docs/ESTADO.md
```

## Reglas de diseño del intérprete

1. Una instrucción termina de una de tres formas: **se completa**, **lanza una excepción arquitectónica** (como el hardware real) o produce una **parada del anfitrión**.
2. Una parada del anfitrión (`UnsupportedInstruction`, `InvalidForm`, `MemoryFault`) **no modifica nada del estado**, ni siquiera el PC. Nada no soportado se ejecuta como NOP.
3. No se toca el estado hasta que todos los accesos a memoria de la instrucción están traducidos y validados.
4. El intérprete es la referencia de precisión para el futuro JIT.
