# Estado del proyecto — Milestone 006

## Funciones implementadas (todas con pruebas)

**Memoria (M003)**
- `mem::Bus`: regiones RAM y MMIO alineadas a 4 KiB, que se rechazan si se solapan. Accesos de 1, 2, 4 y 8 bytes en big-endian, desalineados o entre dos regiones RAM. Validación sin efectos secundarios antes de escribir.
- DMA solo sobre RAM y observadores de escritura con origen CPU o DMA. Páginas de código marcadas con un *handler* de invalidación (base para el JIT). `ram_pointer` para caminos rápidos.
- Mapas: GameCube (MEM1 24 MiB @ 0) y Wii (MEM1 + MEM2 64 MiB @ 0x10000000).

**CPU entera y de sistema (M003)**
- Toda la ISA entera del 750CL, con OE/Rc. Cargas y almacenamientos de todas las formas, `lmw/stmw`, instrucciones de cadena, `lwarx/stwcx.`.
- Saltos, operaciones de CR, SPR, MSR, SR, BAT, SDR1, time base y decrementador, `sc`, `rfi`, `tw`/`twi`, sincronización, operaciones de caché e `icbi` (aviso al JIT).
- Excepciones arquitectónicas con SRR0/SRR1/MSR y vectores (DSI, ISI, externa, alineación, programa, FP no disponible, decrementador, llamada al sistema). MSR[POW] queda en `Idle`.
- MMU: modo real, IBAT/DBAT y tabla de páginas hash (hash primario y secundario, claves, PP, N, G, R/C). Los accesos que cruzan página se traducen por partes antes de escribir.

**Punto flotante (M004)**
- Motor `fpu::arith` sin estado: suma, resta, multiplicación, división, las cuatro FMA, redondeo a simple y estimaciones. Cuatro modos de redondeo. Los resultados de precisión simple usan redondeo a impar, así que no hay doble redondeo. Underflow con tininess antes del redondeo, calculado por software, así que no depende de si el anfitrión es x86 o ARM. Prioridad de NaN frA > frB > frC y QNaN por defecto 0x7FF8000000000000.
- FPSCR completo: FX solo con excepciones nuevas, FEX y VX recalculados, bits acumulativos, FR, FI, FPRF por clase en la precisión de destino, RN, y CR1 con Rc.
- Con VE=1 o ZE=1, el destino no se modifica, igual que en el hardware.
- Instrucciones: `fadd(s) fsub(s) fmul(s) fdiv(s) fmadd(s) fmsub(s) fnmadd(s) fnmsub(s) fres frsqrte fsel frsp fctiw fctiwz fcmpu fcmpo fmr fneg fabs fnabs mffs mtfsf mtfsfi mtfsb0 mtfsb1 mcrfs`.
- Cargas y almacenamientos FP de todas las formas, más `stfiwx`. `lfs` y `stfs` hacen la conversión bit a bit del PEM, sin redondear y conservando los SNaN. Un acceso FP desalineado a palabra da excepción de alineación.
- `fsqrt` y `fsqrts` dan una parada clara, porque el 750CL no las implementa.

**Gekko/Broadway (M004)**
- Registros FP con dos slots (ps0/ps1). Las operaciones simples, `lfs` y `frsp` escriben los dos slots; las dobles y `lfd` solo escriben ps0.
- Las 32 instrucciones paired-single: aritmética por slot, formas cruzadas (`ps_sum0/1`, `ps_muls0/1`, `ps_madds0/1`), merges, comparaciones por slot, `ps_sel`, `ps_neg/abs/nabs/mr` y estimaciones.
- `psq_l`, `psq_lu`, `psq_lx`, `psq_lux`, `psq_st`, `psq_stu`, `psq_stx` y `psq_stux`, con los GQR (tipos float, u8, u16, s8 y s16; escala con signo de 6 bits; W=1 pone ps1 = 1.0; saturación al guardar).
- Sin HID2[PSE] las paired-singles son ilegales; `psq_*` también necesita HID2[LSQE].
- Caché bloqueada: HID2[LCE], `dcbz_l` y accesos de la CPU que pasan por las líneas bloqueadas, con capacidad de 512 líneas. DMA por DMAU/DMAL en ambos sentidos, con *flush* y el bit T, que se borra al terminar. Si se apaga LCE, se descarta el contenido.

**Sistema (M005)**
- `core::Scheduler`: eventos ordenados por (tiempo, orden de llegada). Se pueden cancelar, se reprograman desde su propio callback, informan del retraso y el tiempo nunca retrocede.
- `system::System`: bus, CPU, planificador, PI y gather pipe. Bucle de ejecución que avanza el time base 1 tick cada 12 ciclos (con resto acumulado). Si la CPU queda en `Idle` (MSR[POW]), salta al siguiente evento; si nada puede despertarla, lo informa.
- PI: INTSR (escribir RSW la reconoce), INTMR, FIFO_BASE, FIFO_END y FIFO_WP (con bit de vuelta), y la línea de interrupción externa hacia la CPU. **Cualquier otro registro del PI da `MemoryFault` con la dirección**, en lugar de devolver valores inventados.
- Gather pipe en 0x0C008000: acepta escrituras de 1, 2, 4 y 8 bytes y las vuelca en ráfagas de 32 bytes a la RAM en FIFO_WP, volviendo a BASE al llegar a END. Una ráfaga que no pueda escribirse no modifica nada. Hay un *hook* para el futuro procesador de comandos.
- Cargadores DOL y ELF32 big-endian (EM_PPC, PT_LOAD), con validación de límites y de direcciones que dan la vuelta. La carga a RAM se valida entera antes de escribir. `make_dol` y la herramienta `orca_elf2dol`. `orca_run` es un ejecutor sin interfaz gráfica.
- Arranque HLE mínimo: IBAT0/DBAT0 en caché y DBAT1 sin caché (256 MiB cada uno), MSR = FP|IR|DR, 0x80000020 = 0x0D15EA5E y 0x80000028 = tamaño de MEM1.

**Discos y arranque (M006)**
- `disc::MemoryDisc` y `disc::FileDisc` (acceso aleatorio, sin cargar la imagen entera). Cabecera: ID del juego, nombre, número de disco, versión, magic GC/Wii, offset del DOL principal y FST. Cabecera del apploader: fecha, entry, tamaño y trailer, con su punto de entrada validado dentro de la imagen cargada. Los discos Wii se rechazan y se explica el motivo.
- `hw::MmioSplitter`: reparte la página 0x0C006000 (DI en 0x000; SI, EXI y AI todavía sin dispositivo). Los accesos a un sub-rango vacío dan una parada clara.
- `hw::DiscInterface`: DISR (máscaras más bits de interrupción que se limpian escribiendo 1), DICVR (tapa cerrada), CMDBUF0-2, DIMAR, DILENGTH, DICR, DIIMMBUF. Comando 0xA8 (lectura por DMA, offset = CMDBUF1 << 2) con finalización asíncrona en el planificador: DIMAR avanza, DILENGTH queda en 0, se borra TSTART y se activa TCINT. Leer fuera de la imagen activa DEINT. TCINT/DEINT/BRKINT/CVRINT con sus máscaras levantan la causa DI del PI.
- Los motivos de las paradas de la CPU ahora nombran el dispositivo MMIO y su explicación (por ejemplo "[MMIO device DI rejected the access: DI: command 0x12 not implemented]").
- `System::call_guest` y `run_until`: llamadas a funciones invitadas con una dirección de retorno que nunca se ejecuta.
- `System::boot_disc`: copia de la cabecera del disco a 0x80000000, 0x0D15EA5E, tamaño de MEM1, relojes de bus y CPU (0xF8/0xFC), bi2.bin con su puntero en 0xF4, apploader en 0x81200000 con su pila, y el protocolo entry/init(report)/main/close; cada lectura solicitada se valida (RAM y límites del disco) antes de hacerse. `orca_run` acepta `.iso` y `.gcm`.

## Funciones pendientes (en orden)

1. **M007 — Video y entrada:** VI (registros, contadores de línea, interrupciones de display, XFB y modos NTSC/PAL), SI (controles estándar), EXI (tarjeta de memoria, RTC/SRAM), AI y MI. Para estos dispositivos hace falta investigar la temporización con fuentes verificables antes de implementarla. Otros comandos de la DI (inquiry, request error, audio streaming) y el FST.
2. **M008 — JIT ARM64:** emisor ARM64 verificado con el desensamblador de LLVM, bloques básicos, asignación de registros, caché de bloques conectada a la invalidación del bus, y pruebas diferenciales intérprete vs JIT, que tienen que correr en un dispositivo ARM64.
3. **Después:** GPU (CP/FIFO, XF, BP, TEV, EFB/XFB → Vulkan para Mali), DSP y audio, DI y discos, SI y controles, EXI, AI y los componentes de Wii (Hollywood, IOS, Bluetooth).

## Pendiente de verificación en hardware real

Son comportamientos que el PEM deja indefinidos o que el manual del 750CL no detalla. Elegí una opción, la documenté y la fijé con pruebas, pero **no se ha comparado con una consola real**:

| Comportamiento | Elección actual |
|---|---|
| Resultado de `divw/divwu` con divisor 0 o INT_MIN/−1 | −1 o 0 según el signo; 0 para `divwu` |
| Palabra alta de `fctiw/fctiwz/mffs` | 0xFFF80000 |
| `fres`, `frsqrte`, `ps_res`, `ps_rsqrte` | Valor exacto redondeado. Es válido para la arquitectura (más preciso que el mínimo exigido), **pero no es idéntico bit a bit a las tablas de estimación del Gekko** |
| Precisión del operando frC en multiplicaciones simples (se reporta que el Gekko redondea frC a 25 bits) | No se modela: se usa IEEE estándar |
| ps1 en `frsp`, `ps_merge*` (¿se redondea a simple?) | `frsp` escribe los dos slots; los merges copian sin redondear |
| NaN resultante en operaciones simples | Fracción truncada al formato simple |
| `psq_st` a entero: redondeo y NaN | Truncamiento hacia cero, saturación, NaN → 0 |
| `psq_*` desalineado | Sin excepción de alineación |
| Orden de las comprobaciones HID2[PSE/LSQE] frente a MSR[FP] | Primero HID2 (ilegal) y después MSR[FP] |
| `stfs` de magnitudes menores que un denormal simple | Se guarda un cero con signo |
| Modo no-IEEE FPSCR[NI]=1 | Se ignora: siempre se usa IEEE |
| Bits del FR/FI en estimaciones | Se ponen en 0 |
| `mfspr rD,268/269` (alias moderno de `mftb`, el que genera clang) | Parada del anfitrión. El 750CL documenta `mftb` (XO 371) |
| DEC = 0 tras el arranque HLE (deja pendiente una excepción de decrementador) | Igual que se deja; falta saber qué valor deja el IPL real |
| Escribir INTSR con bits distintos de RSW | Se ignora |
| DI: DIMAR avanza y DILENGTH queda en 0 al terminar la lectura | Igual que se describe públicamente; sin verificar |
| DI: tiempo de las lecturas | Sin modelo de la unidad: termina en el siguiente paso del planificador o tras `latency_cycles` |
| Arranque HLE desde disco: dirección de bi2 (0x817FE000), pila del apploader (0x816FFFF0), stubs en 0x81300000 | Elecciones propias, porque el IPL real usa direcciones que no documenté; un apploader real solo necesita que sean memoria válida |
| Globales del SO que no se escriben (tipo de consola, arena, modo de video, FST) | El apploader del juego escribe algunos; los demás quedan en 0 |

## Problemas conocidos

| # | Problema | Impacto |
|---|---|---|
| 1 | DSISR de las excepciones de alineación queda en 0 | Bajo |
| 2 | Si falla la segunda página de un acceso que cruza páginas, DAR guarda la EA original | Bajo |
| 3 | Sin TLB: cada acceso recorre los BAT y la tabla de páginas | Rendimiento |
| 4 | No se emulan las cachés normales de datos e instrucciones (el gather pipe sí existe desde M005) | — |
| 5 | Modo little-endian, modo traza y breakpoints IABR/DABR activos: parada del anfitrión | No se ejecuta nada mal en silencio |
| 6 | Machine check no modelado (`MemoryFault`) | Diagnóstico |
| 7 | BAT extra de Broadway (HID4[SBE]) y `eciwx`/`ecowx` sin implementar | Wii pendiente |
| 8 | Excepciones FP habilitadas con MSR[FE0/FE1] ≠ 0, o resultados escalados con OE=1/UE=1: parada clara | Los juegos suelen tener las excepciones FP desactivadas; falta verificarlo |
| 9 | `lmw/stmw`, instrucciones de cadena y `lwarx/stwcx.` no pasan por la caché bloqueada | Bajo |
| 10 | El DMA de la caché bloqueada y las ráfagas del gather pipe terminan al instante | Falta conectarlos al planificador con tiempos reales |
| 11 | Coste fijo de 1 ciclo por instrucción: no hay modelo de pipeline ni de caché | El tiempo es determinista, no exacto al ciclo |
| 12 | Solo PI, gather pipe y DI existen como dispositivos; VI, SI, EXI, AI, DSP, MI y CP/PE dan `MemoryFault` al tocarse | Un juego comercial puede pasar su apploader, pero se detiene en cuanto toca el video o los controles |
| DI: solo existe la lectura 0xA8 por DMA; los discos Wii, el audio streaming y los formatos comprimidos no están implementados | Parada clara con el comando |
| 13 | Rendimiento en el anfitrión x86: unos 20 M instr/s en enteros y unos 4 M instr/s en FP. El motor FP cambia el modo de redondeo del anfitrión en cada operación | Esperado: el intérprete es la referencia de precisión; la velocidad vendrá del JIT |
| 14 | No se ha compilado para Android/ARM64 (no hay NDK en este entorno) | Hay que verificarlo |
| 15 | `lswx`, `stswx`, `mcrxr` y todas las instrucciones paired-single y `psq` no se pueden verificar con LLVM. Se revisaron contra las tablas de opcodes del manual del 750CL | Medio: hace falta otra fuente (binutils con -m750cl o hardware) |
