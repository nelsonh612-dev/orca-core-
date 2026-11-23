# Orca Core — Milestone 011

Orca Core es un emulador independiente de GameCube/Wii escrito en C++20, con Android ARM64 como plataforma objetivo. **No contiene código de Dolphin** y no deriva de él.

> **Estado:** la CPU Gekko/Broadway está completa como intérprete de referencia: enteros, sistema, MMU, FPU, paired-singles, cuantización GQR y caché bloqueada. Además, el sistema arranca programas DOL/ELF y **discos GameCube (GCM/ISO) a través de su apploader**, con planificador de tiempo, interrupciones, gather pipe y Disc Interface. Desde M007 también tiene **Video Interface** (temporización NTSC/PAL e interrupciones de display), **Serial Interface con mando estándar** y **Audio Interface** (contador de muestras e interrupción). M008 añade el **External Interface (EXI)** con sus 3 canales y el chip **RTC/SRAM/ROM del IPL**. M009 añade el **Memory Interface**, el bloque de **interfaz del DSP** (buzones, CSR, **ARAM de 16 MiB con su DMA** y registros de DMA de audio) y el lector del **FST** del disco. M010 añade el **núcleo del DSP** (intérprete completo del juego de instrucciones documentado, con opcodes extendidos en paralelo, buzones, DMA del DSP y arranque del microcódigo como el SDK). M011 añade el **acelerador del DSP** para muestras ADPCM (el formato de audio de los juegos) y el acceso crudo de 16 bits a la ARAM, con sus excepciones de fin de rango. **Todavía no es un emulador jugable.** Faltan la tarjeta de memoria, los formatos PCM del acelerador y la salida de audio, la GPU (CP/PE/XF/BP/TEV), el JIT y los componentes de Wii.

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
| **Discos (M006)** | Lectura de imágenes GCM/ISO crudas (desde archivo o memoria); cabecera de disco y de apploader validadas; los discos Wii se rechazan con un mensaje explícito |
| **Disc Interface (M006)** | Registros DISR, DICVR, CMDBUF0-2, DIMAR, DILENGTH, DICR, DIIMMBUF; lectura 0xA8 por DMA asíncrona con TCINT/DEINT e interrupción vía PI; cualquier otro comando da una parada clara con el motivo |
| **Arranque desde disco (M006)** | HLE del IPL: globales documentados en memoria baja, bi2, carga del apploader en 0x81200000 y ejecución de su protocolo entry/init/main/close con llamadas a funciones invitadas |
| **Video Interface (M007)** | Registros 0x00-0x7F; temporización derivada de HTR0/VTR/VTO/VTE/DCR/VICLK (NTSC sale a 59,94 Hz y PAL a 50 Hz a partir de los valores documentados); DPV/DPH; DI0-DI3 → PI; campos y direcciones de XFB |
| **Serial Interface (M007)** | Transferencias SICOMCSR con buffer de E/S, SISR con errores por canal, sondeo automático SIPOLL cada X líneas de video; mando estándar: identificación (0x09 0x00), poll en modo analógico 3, origin y calibrate |
| **Audio Interface (M007)** | AICR/AIVR/AISCNT/AIIT; contador exacto a 48/32 kHz; interrupción AIIT → PI. Sin salida de sonido todavía |
| **EXI (M008)** | 3 canales con CSR/MAR/LENGTH/CR/DATA, transferencias inmediatas y por DMA asíncronas (duración = bits / reloj EXI), TCINT/EXIINT/EXTINT → PI, ROMDIS permanente; cualquier caso sin comportamiento documentado da una parada clara |
| **RTC/SRAM/ROM del IPL (M008)** | Dispositivo en EXI0/CS1: palabra de comando de 32 bits (bit 31 = escritura, dirección << 6); contador RTC que avanza con el tiempo emulado; 64 bytes de SRAM; ROM opcional que aporta el usuario (`orca_run --ipl-rom=`). No se incluye ninguna ROM |
| **Memory Interface (M009)** | Regiones protegidas, tipo de protección, máscara/causa de interrupción → PI, estado y dirección del fallo. La protección **no se aplica**: cada configuración que la activaría queda registrada como aviso (`orca_run` los muestra) |
| **Interfaz del DSP y ARAM (M009)** | Buzones CPU↔DSP, CSR (máscaras, bits que se limpian escribiendo 1, HALT, RES, ocupado), **ARAM de 16 MiB** con DMA asíncrono en ambos sentidos (se inicia al escribir AR_DMA_CNT_L; al terminar las direcciones avanzan, la cuenta queda en 0 y se activa ARINT). Sin núcleo de DSP: enviar un mensaje a un DSP en marcha, PIINT o iniciar la reproducción por DMA de audio da una parada clara |
| **FST (M009)** | Lectura y validación completa de la tabla de archivos del disco; `orca_run --list-files juego.iso` |
| **Núcleo del DSP (M010)** | Intérprete escrito desde el *GameCube DSP User's Manual* v0.1.8 (sin código de emuladores): registros, acumuladores de 40 bits con modos 16/40 bits y saturación, banderas según las reglas del manual, multiplicador (con/sin signo, ×2), pilas de hardware, LOOP/BLOOP, saltos condicionales, IFcc, interrupción externa y RTI, los ~120 opcodes principales y los 30 extendidos ejecutados en paralelo (con la regla OR del manual), IRAM/DRAM, buzones CPU↔DSP, DIRQ y DMA del DSP. Corre en porciones desde el planificador (1 instrucción cada 6 ciclos de CPU). IROM y COEF requieren volcados del usuario (`--dsp-irom=`, `--dsp-coef=`). Lo no documentado detiene la emulación con el motivo |
| **Acelerador del DSP (M011)** | Lectura de muestras ADPCM desde la ARAM (direcciones en nibbles con las cabeceras de trama incluidas, PRED_SCALE cargado de cada cabecera, YN1/YN2, coeficientes 0xFFA0-0xFFAF, saturación a 16 bits), lecturas y escrituras crudas de 16 bits (ACDRAW), recarga de ACCA desde ACSA al pasar ACEA y excepciones ACRROV/ACRWOV/ACSOV. PCM y lecturas crudas de 8/4 bits: parada clara |
| Pruebas | 15 suites; 268.917 casos diferenciales (enteros y FP contra referencias independientes, la de FP con aritmética racional exacta); 189 codificaciones verificadas con `llvm-mc`; 158 mutaciones detectadas; 8 programas C y un apploader compilados con clang, arrancados también desde discos sintéticos |

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
./build/orca_run juego.iso [ciclos]        # arranca un disco GameCube vía su apploader
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
include/orca/system/system.hpp      consola: bus + CPU + planificador + dispositivos + arranque desde disco
include/orca/disc/disc_image.hpp    imágenes de disco GCM/ISO
include/orca/hw/disc_interface.hpp  Disc Interface (DI)
include/orca/hw/mmio_splitter.hpp   reparte la página 0x0C006000 (DI/SI/EXI/AI)
include/orca/hw/video_interface.hpp VI
include/orca/hw/serial_interface.hpp SI + mando estándar
include/orca/hw/audio_interface.hpp AI
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

## Fuentes de hardware consultadas

- PowerPC Programming Environments Manual (PEM) y manual del IBM PowerPC 750CL: CPU, FPU y MMU.
- YAGCD (*Yet Another GameCube Documentation*, capítulos 5 y 9; espejo hitmen.c02.at): registros de VI, SI, AI, DI y PI, y protocolo Joy-Bus.
- NetBSD `sys/arch/evbppc/wii/dev/vireg.h` (licencia BSD): campos de bits de la VI, derivados de YAGCD.
- libjoybus (loopj): longitudes de los comandos 0x00/0x40/0x41/0x42 y formato del estado del mando.
- WiiBrew, "Hardware/External Interface" y "MX23L4005": registros EXI, mapa de dispositivos y direcciones del RTC/SRAM.
- NetBSD `sys/arch/evbppc/wii/dev/rtcsram.c` (licencia BSD) y Linux `drivers/rtc/rtc-gamecube.c`: protocolo del RTC/SRAM (comando 0x20000000 / 0x20000100, bit de escritura 0x80000000, registro del bias = byte 0x0C de la SRAM).
- *GameCube DSP User's Manual* v0.1.8 (Duddie y colaboradores, licencia GFDL; el archivo `.tex` lo aportó el usuario): juego de instrucciones del DSP, registros, excepciones, IFX, buzones, DMA e inicialización. Se usó solo como documentación; no se leyó ni copió código de ningún emulador.
- Nintendo, "DSPADPCM" (especificación del formato, hcs64.com): direcciones del DSP en nibbles que incluyen las cabeceras de trama.
- wiki.axiodl.com "DSP (File Format)", el decodificador GC-ADPCM de Citra y EveryFileExplorer: tramas de 8 bytes (cabecera + 14 nibbles, nibble alto primero) y fórmula de decodificación; las tres coinciden.
- WiiBrew "Hardware/DSP" y el driver `gcn-aram.c` de gc-linux: inicio, unidades de 32 bytes, avance de registros y fin (ARINT) del DMA de ARAM; bits del CSR del DSP.
- wiki.re.virtualworld.fr "GCM (File format)" y `nod` (`src/disc/fst.rs`): formato del FST.
- "GameCube Hacking" (21C3, 2004): formato de la dirección de lectura de la ROM (`00000AAA ... AAxxxxxx`).
- Patentes de Nintendo US6609977 / US7976392: descripción de los registros de display de la VI.

No se usó código de Dolphin.
