# Orca Core — Milestone 023

Orca Core es un emulador independiente de GameCube/Wii escrito en C++20, con Android ARM64 como plataforma objetivo. **No contiene código de Dolphin** y no deriva de él. Desde M012, con permiso del usuario, se leyeron cuatro archivos de Dolphin solo como información de comportamiento del hardware (ver "Fuentes"); el código de Orca se escribió desde cero.

> **Estado:** la CPU Gekko/Broadway está completa como intérprete de referencia: enteros, sistema, MMU, FPU, paired-singles, cuantización GQR y caché bloqueada. Además, el sistema arranca programas DOL/ELF y **discos GameCube (GCM/ISO) a través de su apploader**, con planificador de tiempo, interrupciones, gather pipe y Disc Interface. Desde M007 también tiene **Video Interface** (temporización NTSC/PAL e interrupciones de display), **Serial Interface con mando estándar** y **Audio Interface** (contador de muestras e interrupción). M008 añade el **External Interface (EXI)** con sus 3 canales y el chip **RTC/SRAM/ROM del IPL**. M009 añade el **Memory Interface**, el bloque de **interfaz del DSP** (buzones, CSR, **ARAM de 16 MiB con su DMA** y registros de DMA de audio) y el lector del **FST** del disco. M010 añade el **núcleo del DSP** (intérprete completo del juego de instrucciones documentado, con opcodes extendidos en paralelo, buzones, DMA del DSP y arranque del microcódigo como el SDK). M011 añade el **acelerador del DSP** para muestras ADPCM (el formato de audio de los juegos) y el acceso crudo de 16 bits a la ARAM, con sus excepciones de fin de rango. M012 completa el acelerador (PCM, lecturas de 8/4 bits y las reglas de fin de rango del hardware), añade el **DMA de audio** del AI y la **tarjeta de memoria** en las dos ranuras. M013 empieza la **GPU**: el FIFO CPU→GPU con el Command Processor, el lector de comandos, los estados CP/XF/BP, el Pixel Engine (tokens e interrupción de fin de dibujo) y el decodificador de todos los formatos de textura. M014 añade el **rasterizador por software de referencia**: carga de vértices, transformación e iluminación XF, recorte, triángulos, líneas y puntos, TEV completo con texturas indirectas, texturas con mipmaps, EFB (Z, mezcla, alpha test, dither, z freeze), **copias del EFB a textura y a XFB** con borrado, y precargas de TMEM. M015 completa esa referencia (niebla, texturas RGBA8 en TMEM, vértices descartados, escala vertical del XFB, copias revisadas con la ruta de hardware de Dolphin) y produce la **primera imagen**: la VI describe el XFB y `orca_run --dump-xfb=cuadro.ppm` lo guarda como imagen. **Todavía no es un emulador jugable.** El rasterizador por software es la referencia de precisión, no el renderizador para el teléfono: M016 añade el **backend OpenGL ES 3.2**; en llvmpipe es idéntico bit a bit al rasterizador por software en las escenas de prueba. M019–M025 incorporan un traductor PPC→AArch64 y una caché de bloques con invalidación y fallback al intérprete. M026 conecta `Cpu::run_jit()` al bucle temporizado `System::run()` únicamente en hosts AArch64: limita cada lote al siguiente evento del scheduler, conserva el punto de parada de `run_until()` y contabiliza las instrucciones completadas antes de un fallback. M034–M042 amplían el JIT ARM64 con ramas `b`/`bc`/`bclr`/`bcctr`, `andi.`/`andis.`, `rlwinm`/`rlwimi`/`rlwnm`, `slw`/`srw`/`sraw`/`srawi`, extensiones de signo, `cntlzw`, operaciones lógicas XO y multiplicación baja/alta. La batería diferencial pasó bajo QEMU AArch64 (28 pruebas, 170 comprobaciones, 0 fallos). La cobertura nativa sigue siendo parcial: cargas/almacenamientos, FPU y paired-singles aún recurren al intérprete. Aún falta validar en un dispositivo ARM64 físico y medir rendimiento. M027 añade una ruta inicial de salida PCM16 estéreo desde AI DMA hacia Android AudioTrack, con buffer SPSC, control de underrun/overflow y adaptación entre 32/48 kHz. La salida requiere validación en dispositivo y solo produce sonido cuando el juego programa AI DMA. AES-128-CBC incluye cifrado y descifrado comprobados con vectores NIST. La lectura de particiones Wii cuenta con un fixture sintético de 64 clústeres cifrados que valida H0/H1/H2/H3, la IV AES, el hash H3 referenciado por TMD y la lectura de datos descifrados; todavía no se ha validado con una imagen Wii comercial. Más optimización del backend EFB y la validación de juegos reales siguen pendientes. El presentador XFB ya reutiliza almacenamiento y buffers para reducir trabajo por fotograma.

## Qué incluye este milestone

| Área | Estado |
|---|---|
| Auditoría de M001/M002 | Completa. 8 errores confirmados con pruebas y corregidos (`docs/AUDITORIA_M002.md`) |
| Bus de memoria física | RAM, MMIO, DMA, observadores de escritura, seguimiento de páginas de código para invalidar el JIT |
| Mapas de RAM | GameCube (MEM1 de 24 MiB) y Wii (MEM1 + MEM2 de 64 MiB) |
| ISA entera Gekko/Broadway | Completa en el intérprete de referencia, salvo `eciwx`/`ecowx` (ver `docs/ESTADO.md`) |
| **JIT ARM64 (M019–M026)** | Emisor nativo PPC→AArch64, caché e invalidación, fallback de instrucciones no soportadas e integración temporizada en `System::run()` para ARM64; cobertura parcial y validación de hardware pendiente (`docs/JIT_ARM64_M026.md`) |
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
| **Acelerador del DSP, segunda versión (M012)** | Formatos PCM con GAIN y sus tres escalas, PCM desde ACIN, lecturas crudas de 4/8/16 bits en sus propias unidades de dirección, fin de rango con ACSA/ACEA y casos especiales de ADPCM, ACDSAMP = 0 tras el fin hasta escribir YN2 |
| **DMA de audio (M012)** | AIDMA con bloques de 32 bytes al ritmo del AI (48/32 kHz según el AICR), recarga y AIDINT, AIDMA_LEFT; registros de la ARAM con valores y máscaras reales, tiempo del DMA de ARAM y modo espejo |
| **Tarjeta de memoria (M012)** | Ranuras A y B (EXI0/EXI1, CS0), 4-128 Mbit, comandos de identificación, lectura, borrado, programa de página, estado e interrupción, DMA con su tiempo; `orca_run --card-a=/--card-b=` |
| **GPU, parte 1 (M013)** | FIFO enlazado CPU→GPU con el Command Processor (registros, vuelta del anillo, breakpoint, marcas de agua e interrupción CP); lector de todos los comandos del FIFO y display lists; estado CP (formatos de vértice y tamaño de cada vértice), memoria XF y registros BP con su máscara; cargas XF indexadas; Pixel Engine (tokens, fin de dibujo, interrupciones); decodificador de texturas I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/C4/C8/C14X2/CMPR con paletas |
| **GPU, parte 2 (M014)** | Rasterizador por software de referencia: vértices (todos los formatos y cuantización), transformación XF con iluminación y texgen, recorte, culling, raster de punto fijo con scissor, líneas y puntos, TEV de 16 etapas con konst, swap e indirectas, alpha test, z textura, texturas con LOD/mipmaps/bilinear/wrap/paletas, EFB en 4 formatos con Z, mezcla, lógica y dither, z freeze; copias del EFB a textura (13 formatos, media escala, intensidad, profundidad, filtro de copia y gamma) y a XFB (YUV 4:2:2), borrado, precargas de TMEM |
| **GPU, parte 3 (M015)** | Niebla (con rango), texturas RGBA8 en TMEM, vértices con índice 0xFF/0xFFFF descartados, copias con el filtro/gamma/intensidad de la ruta de hardware de Dolphin, escala vertical del XFB, descripción del campo de la VI y conversión XFB → RGB; programa PowerPC que dibuja hasta el XFB |
| **Backend OpenGL ES 3.2 (M016)** | `orca_gles`: rasterización en la GPU con pendientes de atributos exactas por triángulo, ubershader del TEV/EFB, EFB R32UI con framebuffer fetch, texturas decodificadas con caché, copias/borrados por la CPU; comparado píxel a píxel con el software |
| Pruebas | 19 suites; 268.917 casos diferenciales (enteros y FP contra referencias independientes, la de FP con aritmética racional exacta); 189 codificaciones verificadas con `llvm-mc`; 356 mutaciones detectadas; 9 programas C y un apploader compilados con clang, arrancados también desde discos sintéticos |

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
-DORCA_GLES=OFF      # no compilar el backend OpenGL ES (por defecto se compila si hay EGL y GLES)
```

Ejecutar un DOL o ELF sin interfaz gráfica (imprime por qué se detuvo y los registros):

```sh
./build/orca_run programa.dol [ciclos] [gc|wii]
./build/orca_run juego.iso [ciclos]        # arranca un disco GameCube vía su apploader
./build/orca_elf2dol programa.elf programa.dol
./build/orca_run --dump-xfb=cuadro.ppm programa.dol   # guarda el XFB del campo superior al terminar
```

La prueba `programs` necesita `clang` y `ld.lld` con soporte para PowerPC; si no los encuentra, se marca como *skipped*. La prueba `gles` necesita un contexto OpenGL ES 3.2 con `GL_EXT_shader_framebuffer_fetch` (en Linux sin pantalla usa Mesa con EGL_PLATFORM=surfaceless); si no lo hay, se marca como *skipped*. La prueba `encodings` busca `llvm-mc` en el PATH. Si no lo encuentra, CTest la marca como *skipped*, no como aprobada.

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
include/orca/hw/command_processor.hpp  include/orca/hw/pixel_engine.hpp   CP y PE
include/orca/gpu/gpu.hpp            ejecución de comandos de la GPU (CP/XF/BP, TLUT, precargas, copias)
include/orca/gpu/sw/vertex.hpp      carga de vértices y transformación XF (software)
include/orca/gpu/sw/renderer.hpp    rasterizador por software de referencia (TEV, texturas, EFB)
include/orca/gpu/sw/efb_copy.hpp    copias del EFB a textura y a XFB, borrado
include/orca/video/xfb.hpp          XFB (YUV 4:2:2) a RGB para mostrar la imagen
include/orca/gpu/gles/backend.hpp   backend OpenGL ES 3.2 (src/gpu/gles: backend y shaders)
third_party/khronos/                cabeceras EGL/GLES de Khronos sin modificar (Apache-2.0 / MIT)
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
- **Dolphin (GPL-2.0-or-later), solo como información, con permiso del usuario (M012):** archivos aportados por el usuario: `DSPAccelerator.cpp` (unidades de dirección, fórmulas ADPCM/PCM, reglas de fin de rango), `AudioInterface.cpp` (divisores del ritmo de muestra, AIDFR), `DSP.cpp` (DMA de audio, registros y tiempo de la ARAM, modo espejo, RES) y `EXI_DeviceMemoryCard.cpp` (comandos de la tarjeta, respuesta de identificación, estado, tiempos). No se copió código: cada regla se reescribió desde cero y se probó con valores calculados a mano. Donde Dolphin contradice otra fuente (AISFR frente a YAGCD) se dejó la implementación anterior y se anotó como pendiente de verificación.
- gc-memcard-adapter (licencia MIT): códigos de comando de la tarjeta y formato de la dirección, contrastados con Dolphin.
- **Dolphin para la GPU (M013), solo como información, con permiso del usuario:** archivos aportados por el usuario (`OpcodeDecoding.h/.cpp`, `CommandProcessor.h/.cpp`, `PixelEngine.h/.cpp`, `CPMemory.h`, `XFMemory.h`, `XFStructs.cpp`, `BPMemory.h`, `BPStructs.cpp`, `VertexLoader_*.cpp`, `TextureDecoder_Generic.cpp`, `TextureDecoder.h`, `TextureDecoder_Util.h`) y fragmentos de páginas públicas de GitHub encontrados con búsquedas: el bucle de `Fifo.cpp` (coincide en cuatro versiones), `GPFifo.h`, y `LoadCPReg` en `VertexLoaderManager.cpp` de una versión antigua (commit 52af077). La base del PE (0xCC001000) está confirmada además en el PR #13219 de Dolphin.
- Metaforce / Aurora (implementación propia de GX, independiente de Dolphin): expansión de 3, 4, 5 y 6 bits a 8 bits.
- **Dolphin para el rasterizador por software (M014), solo como información, con permiso del usuario:** archivos aportados por el usuario (`SWVertexLoader.cpp`, `TransformUnit.cpp`, `SetupUnit.cpp`, `Clipper.cpp`, `Rasterizer.cpp`, `Tev.cpp`, `Tev.h`, `TextureSampler.cpp`, `SWEfbInterface.cpp`, `TextureEncoder.cpp`, `TMEM.cpp/.h`, `VertexLoaderBase.cpp`) además de los de M013. Las páginas de los PR 10204 y 10466 de Dolphin y su issue 14020 (encontradas con búsquedas) indican que el filtro de copia y la gamma se aplican también a las copias de textura. No se copió código: cada regla se reescribió desde cero y se probó con valores calculados a mano; las contradicciones dentro de Dolphin se anotaron en `docs/ESTADO.md`.
- **Repositorio completo de Dolphin (M015), aportado por el usuario, solo como información:** `BPMemory.cpp` (niebla), `CopyRegion.h` (escala del XFB), `TextureDecoder_Common.cpp` (RGBA8 en TMEM, XFB → RGB), `TextureConversionShader.cpp` y `TextureCacheBase.cpp` (filtro de copia, gamma, intensidad, bloques), `VertexLoader.cpp` (vértices descartados), `VideoInterface.cpp` (campo del XFB), `BPFunctions.cpp` y `GeometryShaderManager.cpp` (bit de aspecto). Se guardó fuera del proyecto; no se copió código.
- Cabeceras de Khronos (OpenGL ES 3.2, EGL, Vulkan 1.4 v368; licencias MIT/Apache) aportadas por el usuario: las de EGL y GLES se incluyen sin modificar en `third_party/khronos` para el backend de M016; las de Vulkan siguen fuera del proyecto.

## M017 — Android bootstrap en desarrollo

El paquete siguiente añade `android/`, una aplicación de diagnóstico ARM64 que crea un contexto EGL/OpenGL ES 3.2 y prueba `Backend::init()` en el dispositivo. **No es todavía una aplicación jugable ni completa M017**: no conecta el bucle `System` con el renderizador, no presenta el XFB, no mapea mandos ni entrega audio. El alcance, requisitos y limitaciones están en `docs/M017_ANDROID_BOOTSTRAP.md`.

La re-auditoría local del M016 y sus límites reproducibles están registrados en `docs/REGISTRO_PRUEBAS_M016_REAUDITORIA.md`.


## Generar APK Android de diagnóstico

El workflow `Orca Core CI` compila una APK debug ARM64 y la publica como artefacto `orca-core-android-arm64-debug`. Consulta `android/README_BUILD_APK.md` para subir el proyecto a un repositorio privado, ejecutar el workflow, descargar e instalar la APK. La APK es una sonda gráfica, no un emulador jugable. La compilación Android aún debe pasar en CI y la ejecución debe comprobarse en un teléfono físico.

## Android status (0.17.0-dev2)

`android/` contains an ARM64 OpenGL ES capability probe for the M016 renderer. It is not yet a playable Android emulator: game loading, Android frame scheduling, XFB presentation, input mapping, PCM audio output, save states and a complete frontend remain unimplemented. The Android/NDK build and device runtime must be verified with Android SDK/NDK and a physical target. See `docs/M017_ANDROID_BOOTSTRAP.md`.
