# Estado del proyecto — Milestone 003

## Funciones implementadas (todas con pruebas)

**Memoria**
- `mem::Bus`: regiones RAM y MMIO alineadas a 4 KiB, que se rechazan si se solapan. Accesos de 1, 2, 4 y 8 bytes en big-endian, desalineados o entre dos regiones RAM. Validación sin efectos secundarios antes de escribir.
- DMA (`dma_read`/`dma_write`) solo sobre RAM, más observadores de escritura con origen CPU o DMA.
- Páginas de código marcadas con un *handler* de invalidación. Se activa con escrituras de la CPU, DMA o `notify_external_write`. Es la base para invalidar el JIT.
- `ram_pointer` para caminos rápidos (JIT, subidas de la GPU).
- Mapas: GameCube (MEM1 24 MiB @ 0x00000000) y Wii (MEM1 + MEM2 64 MiB @ 0x10000000). Los registros de hardware quedan sin mapear hasta que existan sus módulos.

**CPU (intérprete de referencia)**
- Aritmética: `addi addis addic addic. subfic mulli add addc adde addze addme subf subfc subfe subfze subfme neg mullw mulhw mulhwu divw divwu`, todas con las formas `o` y `.` que les corresponden.
- Lógica: `andi. andis. ori oris xori xoris and andc or orc xor nand nor eqv extsb extsh cntlzw`.
- Desplazamientos y rotaciones: `slw srw sraw srawi rlwinm rlwimi rlwnm`.
- Comparaciones: `cmp cmpl cmpi cmpli` (L=1 da InvalidForm).
- Cargas y almacenamientos: todas las formas D, X, con actualización e indexadas; `lhbrx lwbrx sthbrx stwbrx`; `lmw stmw`; `lswi lswx stswi stswx`; `lwarx stwcx.`.
- Saltos: `b bc bclr bcctr`, con AA y LK.
- CR: `crand cror crxor crnand crnor creqv crandc crorc mcrf mfcr mtcrf mcrxr`.
- Sistema: `mfspr mtspr mftb mfmsr mtmsr mfsr mtsr mfsrin mtsrin sc rfi tw twi sync isync eieio tlbie tlbsync`.
- Caché: `dcbz` (zero real; excepción de alineación si W o I), `dcbf dcbst dcbt dcbtst dcbi` (traducen la dirección y pueden dar DSI) e `icbi` (avisa al *handler* con el bloque físico).
- Excepciones: DSI (DAR y DSISR), ISI (bits de SRR1), externa (línea por nivel), alineación, programa (ilegal, privilegiada, trap), FP no disponible, decrementador y llamada al sistema. MSR nuevo según el PEM: se conservan IP, ME e ILE, y LE := ILE. Prefijo 0xFFF00000 con MSR[IP].
- Instrucciones privilegiadas en modo usuario: excepción de programa.
- Time base y decrementador, con detección exacta de la transición 0 → −1 incluso en avances grandes.
- MSR[POW] con modo DOZE/NAP/SLEEP en HID0: la CPU queda en `Idle` hasta una interrupción.
- MMU: modo real; IBAT y DBAT (Vs/Vp, BL, OR de BRPN, PP); tabla de páginas hash (HTABORG/HTABMASK, hash primario y secundario, API, claves Ks/Kp, PP, bit N, G en *fetch*, bits R/C). Los accesos que cruzan una página se traducen por partes antes de escribir nada.

## Funciones pendientes (en orden)

1. **M004 — FPU y paired-singles:** FPR, FPSCR, todas las instrucciones FP con redondeo y excepciones IEEE, paired-singles, `psq_l/psq_st` con cuantización GQR, HID2[PSE]. También la caché bloqueada (`dcbz_l`, DMAU/DMAL) y el *write-gather pipe*.
2. **M005 — Planificación y arranque:** planificador de eventos con la relación CPU/bus/timebase, controlador de interrupciones PI, esqueleto de MI/PI/VI, cargador DOL/ELF y ejecución de programas homebrew de prueba.
3. **M006 — JIT ARM64:** emisor ARM64 verificado con el desensamblador de LLVM, bloques básicos, asignación de registros, caché de bloques conectada a la invalidación del bus, y pruebas diferenciales intérprete vs JIT que tienen que correr en un dispositivo ARM64.
4. **Después:** GPU (CP/FIFO, XF, BP, TEV, EFB/XFB → Vulkan para Mali), DSP y audio, DI y discos, SI y controles, EXI, AI y los componentes de Wii (Hollywood, IOS, Bluetooth).

## Problemas conocidos

| # | Problema | Impacto |
|---|---|---|
| 1 | DSISR de las excepciones de alineación queda en 0. No se codifican los campos de la instrucción | Bajo: pocos manejadores lo leen |
| 2 | Resultado de `divw`/`divwu` al dividir por 0 o con INT_MIN/−1: es indefinido en la arquitectura; se usa el comportamiento reportado públicamente para el 750 (−1/0 según el signo; 0 para `divwu`). **Falta verificarlo en hardware real** | Medio |
| 3 | Si falla la segunda página de un acceso que cruza páginas, DAR guarda la EA original del acceso | Bajo |
| 4 | Sin TLB: cada acceso recorre los BAT y la tabla de páginas. Es exacto pero lento | Rendimiento |
| 5 | No se emulan las cachés de datos e instrucciones, la caché bloqueada ni el *write-gather pipe*. `mtspr DMAL` da Unsupported | Algunos juegos usan la caché bloqueada |
| 6 | Modo little-endian, modo traza (SE/BE) y breakpoints IABR/DABR activos: parada del anfitrión | No se ejecuta nada mal en silencio |
| 7 | Machine check no modelado: un error de bus detiene el emulador (`MemoryFault`) | Diagnóstico |
| 8 | No se modelan los BAT extra de Broadway (HID4[SBE]) ni `eciwx`/`ecowx` | Wii: pendiente |
| 9 | Prioridad externa > decrementador: asumida, falta confirmarla en la tabla del manual del 750CL | Bajo |
| 10 | Escribir DEC por software nunca genera la excepción, aunque cambie el bit de signo | Falta verificar |
| 11 | Los bits reservados de las instrucciones se ignoran, salvo en los casos marcados como InvalidForm | Bajo |
| 12 | `mfspr` 268/269 (TBL por mfspr) da Unsupported. La forma del 750 es `mftb` (XO 371) | Bajo |
| 13 | El PVR lo define la capa de consola, que todavía no existe | — |
| 14 | `lswx`, `stswx` y `mcrxr` no se pueden verificar con LLVM; se revisaron a mano contra el PEM | Bajo |
| 15 | Rendimiento del intérprete: unos 20–24 M instrucciones/s en el anfitrión x86 (ver el registro). Gekko corre a 486 MHz, así que el JIT es imprescindible | Esperado |
| 16 | No se ha compilado para Android/ARM64 (no hay NDK en este entorno) | Hay que verificarlo |
