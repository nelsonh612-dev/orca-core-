# Estado del proyecto — Milestone 018 (rama `orca-core-0.1`)

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
- `hw::MmioSplitter`: reparte la página 0x0C006000 (DI en 0x000, SI en 0x400, EXI en 0x800 y AI en 0xC00). Los accesos a un sub-rango vacío dan una parada clara.
- `hw::DiscInterface`: DISR (máscaras más bits de interrupción que se limpian escribiendo 1), DICVR (tapa cerrada), CMDBUF0-2, DIMAR, DILENGTH, DICR, DIIMMBUF. Comando 0xA8 (lectura por DMA, offset = CMDBUF1 << 2) con finalización asíncrona en el planificador: DIMAR avanza, DILENGTH queda en 0, se borra TSTART y se activa TCINT. Leer fuera de la imagen activa DEINT. TCINT/DEINT/BRKINT/CVRINT con sus máscaras levantan la causa DI del PI.
- Los motivos de las paradas de la CPU ahora nombran el dispositivo MMIO y su explicación (por ejemplo "[MMIO device DI rejected the access: DI: command 0x12 not implemented]").
- `System::call_guest` y `run_until`: llamadas a funciones invitadas con una dirección de retorno que nunca se ejecuta.
- `System::boot_disc`: copia de la cabecera del disco a 0x80000000, 0x0D15EA5E, tamaño de MEM1, relojes de bus y CPU (0xF8/0xFC), bi2.bin con su puntero en 0xF4, apploader en 0x81200000 con su pila, y el protocolo entry/init(report)/main/close; cada lectura solicitada se valida (RAM y límites del disco) antes de hacerse. `orca_run` acepta `.iso` y `.gcm`.

**Video, entrada y audio (M007)**
- `hw::VideoInterface` (0x0C002000): banco de registros de 16 bits (accesos de 16 y 32 bits). La temporización sale de los registros: el reloj de píxel es VICLK/2 (13,5 o 27 MHz), una semilínea son HLW píxeles y las semilíneas por campo son 3·EQU + PRB + 2·ACV + PSB (VTO para los campos impares, VTE para los pares). Con los valores por defecto documentados en YAGCD, NTSC da 525 semilíneas por campo (59,94 Hz) y PAL 625 (50 Hz exactos). DPV cuenta líneas por cuadro (o por campo con NIN) desde 1 y DPH cuenta píxeles desde 1. DI0-DI3 activan INT al llegar a (VCT, HCT) con ENB; el software solo puede borrar INT. La causa VI del PI sigue a cualquier INT activo y habilitado. Hay aviso de inicio de campo y direcciones de XFB (TFBL/BFBL con PGOFF).
- `hw::SerialInterface` (0x0C006400): SICnOUTBUF, SICnINBUFH/L (leerlos borra RDST), SIPOLL, SICOMCSR (TCINT que se limpia escribiendo 1, máscaras, COMERR, RDSTINT, longitudes donde 0 = 128, canal, TSTART), SISR (errores por canal que se limpian escribiendo 1, WR), SIEXILK y buffer de E/S de 128 bytes. Canal vacío → NOREP + COMERR; respuesta de longitud distinta → OVRUN/UNRUN. Sondeo automático cada X líneas de video para los canales habilitados.
- `hw::StandardController`: 0x00/0xFF → 09 00 status (necesita origin, motor, modo analógico); 0x40 modo 3 → 8 bytes; 0x41/0x42 → 10 bytes. El frontend fija `state`.
- `hw::AudioInterface` (0x0C006C00): AICR (PSTAT, AFR, AIINTMSK, AIINT que se limpia escribiendo 1, AIINTVLD, SCRESET), AIVR, AISCNT exacto respecto del tiempo de CPU (10.125 ciclos por muestra a 48 kHz en GC) y AIIT con interrupción vía PI.
- Planificador: ahora cada evento se dispara con `now` igual a su tiempo previsto. Antes veía el tiempo final del avance, y un dispositivo que se reprogramaba de forma relativa perdía eventos intermedios. Lo detectó la prueba de campos de la VI.

**External Interface y RTC/SRAM/ROM (M008)**
- `hw::ExternalInterface` (0x0C006800, ventana 0x40): 3 canales × CSR/MAR/LENGTH/CR/DATA (paso 0x14), solo accesos de 32 bits. CSR: ROMDIS (solo canal 0, permanente hasta el reset), EXT de solo lectura (dispositivo externo en CS0), EXTINT/TCINT/EXIINT que se limpian escribiendo 1, máscaras, CS (como mucho un bit) y CLK 1-32 MHz (110/111 reservados → parada). MAR y LENGTH con los bits 0-4 a cero. CR: TLEN, RW, DMA y TSTART. Transferencias inmediatas (1-4 bytes, MSB primero) y DMA (múltiplos de 32 bytes, solo a RAM), terminadas por el planificador tras bits / reloj EXI: se borra TSTART y se activa TCINT. Cualquier (estado & máscara) de un canal levanta la causa EXI del PI. Interfaz `ExiDevice` para conectar dispositivos, más `set_device_interrupt` (línea INT) y `notify_slot_change` (EXTINT).
- `hw::IplRtcSram` en EXI0/CS1: palabra de comando (bit 31 = escritura; dirección = (comando & 0x7FFFFFFF) >> 6) y después un flujo de datos con autoincremento hasta liberar CS. ROM 0x000000-0x1FFFFF (imagen opcional del usuario, tal como la lee la CPU con el descifrador activo), RTC en 0x800000 (comando 0x20000000) y SRAM de 64 bytes en 0x800004 (comando 0x20000100). El RTC cuenta segundos de tiempo emulado desde el valor que fije el anfitrión o el invitado.
- `System::exi()` / `System::ipl()`; `orca_run --ipl-rom=ARCHIVO --rtc=SEGUNDOS`.

**Memory Interface, interfaz del DSP, ARAM y FST (M009)**
- `hw::MemoryInterface` (0x0C004000): registros de 16 bits con accesos de 16 y 32 bits. Regiones 0-3, tipo de protección (2 bits por canal), máscara (bits 0-4) y causa (se limpia escribiendo 1) → causa MEM del PI, estado, dirección del fallo (solo lectura), temporizadores (escribirlos los reinicia; leerlos da una parada porque no está documentado qué cuentan). La protección no se aplica: cada escritura que deja un canal con tipo distinto de 3 añade un aviso (como mucho 64) a `warnings()`.
- `hw::DspInterface` (0x0C005000): buzón hacia el DSP (bit 15 = pendiente), buzón hacia la CPU (leer la mitad baja lo consume), CSR (RES, HALT, AIDINT/ARINT/DSPINT con sus máscaras → causa DSP del PI, bit 9 = DMA de ARAM en curso, bit 11 guardado), AR_SIZE/AR_MODE/AR_REFRESH guardados. **ARAM de 16 MiB** (solo GameCube) con DMA en 32 bytes por unidad: se inicia al escribir AR_DMA_CNT_L, los registros quedan bloqueados mientras dura y al terminar MMADDR y ARADDR avanzan, la cuenta queda en 0 y se activa ARINT. Registros de DMA de audio guardados.
- `disc::parse_fst` / `parse_fst_bytes` / `find_file`: FST con directorios anidados, validación de tipos, nombres (offsets de 24 bits, terminados en 0), índices de padre y siguiente, y archivos dentro del disco. `orca_run --list-files`.

**Núcleo del DSP (M010)**
- `dsp::DspCore`: los 32 registros con sus anchos documentados ($acX.h de 8 bits con lectura extendida en signo, $config de 8 bits, bit 8 de $sr siempre 0, $prod.h de 8 bits); acumuladores de 40 bits; modo 16/40 bits ($sr.SXM) con extensión de signo en las cargas a $acX.m y saturación a 0x7fff/0x8000 en las lecturas, en exactamente las instrucciones que lista el manual; $prod como l + (m1+m2)<<16 + h<<32 (CLRP deja los valores exactos del manual); multiplicador con $sr.SU y $sr.AM; banderas OS (persistente), LZ, TB, AS, S, Z, O, C con las reglas de acarreo y desbordamiento del manual (acarreo en la resta, no préstamo); las 16 condiciones.
- Control: JMP/Jcc, CALL/CALLcc, JMPR/JRcc, CALLR/CALLRcc, RET/RETcc, RTI/RTIcc, IFcc, LOOP/LOOPI (con contador de registro y saturación), BLOOP/BLOOPI con la pila de bucles, HALT; pila de llamadas de 8 y de datos/bucles de 4.
- Todos los opcodes principales de la tabla "Instructions sorted by opcode" y los 30 extendidos; los extendidos ven el estado anterior a la instrucción y, si escriben el mismo registro que la principal, el resultado es el OR de ambos (ejemplos del manual 0x6113 y 0x7660 fijados en pruebas). Opcodes de nibble 3 con extensión de 7 bits.
- Memoria: IRAM 0x0000-0x0FFF, IROM 0x8000-0x8FFF (volcado), DRAM 0x0000-0x0FFF, COEF 0x1000-0x17FF (volcado, solo lectura), IFX 0xFF00-0xFFFF.
- IFX: buzones DMBH/DMBL (DSP→CPU) y CMBH/CMBL (CPU→DSP) con su bit M, DIRQ → DSPINT del lado de la CPU, DMA del DSP (DSCR/DSBL/DSPA/DSMAH/DSMAL, IMEM o DMEM, ambos sentidos), registros del acelerador y coeficientes ADPCM guardados.
- `hw::DspInterface`: RES reinicia el núcleo; con el bit 11 copia los 1024 bytes de 0x01000000 a la IRAM y arranca en 0 (como describe el manual a partir de libogc); HALT del CSR detiene el núcleo y la instrucción HALT lo activa; PIINT pide la interrupción externa; el núcleo corre en porciones de 100 instrucciones cada 600 ciclos de CPU.
- `System::run` se detiene con `device_stop` y el motivo cuando el DSP encuentra algo no documentado; `orca_run` lo muestra.

**Acelerador del DSP (M011)**
- ACDSAMP con FORMAT = 0x0000 (ADPCM desde la ARAM, 4 bits): direcciones en nibbles; al empezar cada trama de 16 nibbles se lee el byte de cabecera a PRED_SCALE (predictor en los bits 6-4, escala en 3-0) y ACCA avanza 2; cada muestra = sat16((n·2^s·2048 + 1024 + A1·YN1 + A2·YN2) >> 11), con el nibble alto primero; YN2 ← YN1, YN1 ← muestra.
- ACDRAW con tamaño de 16 bits: lectura de una palabra (dirección en palabras); escritura de 16 bits que exige el bit 31 de ACCA, como dice el manual.
- Tras cada acceso, ACCA + 1; si pasa de ACEA se recarga ACSA y queda pendiente ACRROV, ACRWOV o ACSOV (niveles 3, 4 y 5; vectores 0x0006, 0x0008 y 0x000A).
- `DspHost` da acceso a la ARAM de la consola; en Wii no hay ARAM y el acelerador se detiene con el motivo.
- (M012 reemplazó las reglas de fin de rango y añadió PCM y lecturas de 8/4 bits: ver abajo.)

**Acelerador del DSP, segunda versión (M012)** — fuente de comportamiento: `DSPAccelerator.cpp` de Dolphin, usado solo como información (el código se escribió desde cero; ver "Fuentes"):
- Las direcciones van en unidades del tamaño de FORMAT: nibbles (4 bits, el alto primero), bytes (8 bits) o palabras (16 bits). ACCA se guarda con la máscara 0xBFFFFFFF; ACSA y ACEA con 0x3FFFFFFF.
- ACDRAW lee el valor crudo extendido con ceros; si la dirección leída era ACEA, ACCA ← ACSA y queda pendiente ACRROV. Una escritura sin el bit 31 no se acepta; al escribir en ACEA, ACCA ← ACSA con el bit 31 y queda ACRWOV.
- ACDSAMP ADPCM: muestra = sat16(2^escala · n + ((0x400 + A1·YN1 + A2·YN2) >> 11)); cabecera leída justo después del último nibble de la trama (ACCA + 2); los dos casos especiales de fin (ACEA & 0xF = 0 o 1) vuelven a ACSA sin excepción ni cabecera.
- ACDSAMP PCM (FORMAT bits 3-2 = 1, 2 o 3): (GAIN·x >> g) + (A1·YN1 >> g) + (A2·YN2 >> g), truncado a 16 bits, con g = 11, 0 o 16 según la escala de ganancia; decode 1 y 3 toman el valor de ACIN y decode 1 no avanza ACCA.
- Fin de rango de muestras: ACCA ← ACSA, ACSOV pendiente y ACDSAMP devuelve 0 hasta que se escribe YN2. PRED_SCALE se guarda con la máscara 0x7F.
- Paradas claras: tamaño 3 de FORMAT y escala de ganancia 3.

**DMA de audio y ARAM (M012)** — fuentes: `AudioInterface.cpp` y `DSP.cpp` de Dolphin, como información:
- AIDMA_H con máscara 0x03FF en GameCube y 0x1FFF en Wii; AIDMA_L 0xFFE0. Al activar el bit 15 de AIDMA_CTRL se cargan la dirección y la cuenta de bloques de 32 bytes, AIDINT se activa 200 ciclos después y cada bloque se lee cada 8·f_CPU·divisor/108 MHz (divisor 2248/3372 en GC, 2250/3375 en Wii, según el bit 6 del AICR). Al llegar la cuenta a 0 se recargan y se activa AIDINT. AIDMA_LEFT = restantes − 1. Desactivar el bit o RES detiene la DMA. Los bloques se entregan a `set_audio_sink` (todavía no hay salida de sonido).
- AR_MODE se lee como 1 y no se puede escribir; AR_REFRESH empieza en 156 (máscara 0x7FF); AR_SIZE máscara 0x7F; registros altos de DMA con máscara 0x03FF (CNT_H 0x83FF). Tiempo del DMA de ARAM: 246 ciclos por unidad de 32 bytes. Con (AR_SIZE & 0xF) = 4 las escrituras por debajo de 4 MiB se repiten 4 MiB más arriba.

**Tarjeta de memoria (M012)** — fuentes: `EXI_DeviceMemoryCard.cpp` de Dolphin y gc-memcard-adapter (MIT); solo los comandos en los que coinciden:
- `hw::MemoryCard` en EXI0/CS0 (ranura A) y EXI1/CS0 (ranura B) con `System::insert_memory_card`: imagen cruda de 4, 8, 16, 32, 64 o 128 Mbit (128 KiB por Mbit); una tarjeta nueva está llena de 0xFF. Activa EXT y EXTINT del canal.
- Comandos: 0x00 identificación (0x80 y luego el tamaño en Mbit como u32), 0x52 lectura (dirección en 4 bytes: >> 17, >> 9, (>> 7) & 3, & 0x7F; datos desde el byte 9 con vuelta dentro del sector de 512 bytes), 0xF1 borrado de bloque de 8 KiB, 0xF2 programa de página (búfer de 128 bytes escrito al liberar CS), 0x83 estado, 0x89 borrar estado, 0x81 interruptor de interrupción.
- Estado inicial 0xC1 (BUSY, UNLOCKED, READY); borrado y programa ponen BUSY y terminan 5000 ciclos después (READY, interrupción EXI si está activada).
- DMA: lectura o escritura en la dirección actual, con 512 KiB/s de lectura y 96,1 KiB/s de escritura (f_CPU/524288 y f_CPU/98432 ciclos por byte).
- Cualquier otro comando, y leer antes del byte de comando, detienen la emulación con el código. `orca_run --card-a=` / `--card-b=` cargan la imagen (o crean una de 16 Mbit) y la guardan si cambió.

**CPU: cierre de pendientes (M017)** — fuentes: PEM de 32 bits (NXP MPCFPE, 6.4.6/6.4.7), manual del 750GX, Linux `align.c` (DSISR), Dolphin como información:
- **BAT 4-7 de Broadway** (SPR 560-575): existen solo con `CpuConfig::extended_bats` (Wii) y se usan en la traducción cuando HID4[SBE] (bit 0x02000000) está activo, después de los BAT 0-3. En Gekko esas SPR se detienen.
- **DSISR de las excepciones de alineación** a partir de la instrucción (X-form: bits 29-30, 25 y 21-24; D-form: bits 5 y 1-4; registros 6-15 en ambos).
- **DAR al cruzar página:** "un byte de la primera palabra accedida en el segmento o área BAT que causó la DSI": EA original si las dos páginas están en el mismo segmento por tabla de páginas; primer byte de la segunda página si cambia el segmento o interviene un BAT.
- **eciwx/ecowx:** DSI con DSISR[11] si EAR[E] = 0, alineación si no están alineadas; con EAR[E] = 1 parada (no hay dispositivo de control externo documentado).
- **mfspr 268/269** leen la time base (también en modo usuario), como el intérprete de Dolphin.
- **Excepción FP habilitada precisa:** con MSR[FE0|FE1] ≠ 0, una instrucción de cálculo (aritmética, redondeo, conversión, comparación) que deja FPSCR[FEX] = 1 termina (destino escrito salvo VE/ZE) y después se toma la excepción de programa con SRR0 = esa instrucción y SRR1[11]. El 750 trata los modos imprecisos como precisos.
- **Resultados con FPSCR[OE]/[UE] = 1** (tablas 3-14 y 3-16 del PEM): el resultado exacto redondeado con el exponente ajustado en −/+1536 (doble) o −/+192 (simple), con FR/FI de ese redondeo, OX o UX y XX si es inexacto. Se calcula reescalando los operandos con potencias de dos exactas para que el motor existente redondee en rango.

**GPU: bounding box y contadores del Pixel Engine (M018)** — fuentes: Dolphin como información (`PixelEngine.h/.cpp`, `BPStructs.cpp`, `SWBoundingBox.cpp`, `Tev.cpp`, `Rasterizer.cpp`):
- **Bounding box:** BP 0x55 (izquierda/derecha, 10 bits cada una) y BP 0x56 (arriba/abajo); cada píxel que llega a la mezcla (tras alpha test y Z) lo amplía hasta los bordes de su bloque 2x2. Registros PE 0x10-0x16 de solo lectura.
- **Contadores de rendimiento:** quads que entran/pasan la Z temprana, entran/pasan la Z tardía (solo con la prueba Z activa) y entran a la mezcla (siempre), en PE 0x18-0x2A como mitades baja/alta; BP 0x57 los borra. Un quad 2x2 cuenta una vez por triángulo si al menos uno de sus píxeles llega a la etapa. Los ciclos de copia del EFB (0x2C/0x2E) siguen detenidos: no hay modelo de tiempo de las copias.
- **Backend GLES:** los mismos valores con un *storage buffer* (atomicMin/Max/Add) y una imagen R32UI de 320x264x5 con el último triángulo que contó cada quad (imageAtomicExchange). Se activan con la primera escritura de BP 0x55/0x56 (caja) o 0x57 (contadores), como Dolphin; leer antes da una parada clara. Antes de que la CPU reescriba media caja se sincroniza con la GPU. Si el GL no ofrece buffers/imágenes en el fragment shader, el shader se compila sin esto y la lectura se detiene.

**GPU, parte 1 (M013)** — fuentes: archivos de Dolphin como información (ver README) y YAGCD; el código es propio:
- `hw::CommandProcessor` (0x0C000000): STATUS calculado (marcas de agua, lectura y comando inactivos, breakpoint), CTRL (6 bits), CLEAR, PERF_SELECT (3 bits), registro 0x0A (8 bits), los ocho punteros del FIFO como pares LO/HI con máscaras 0xFFE0 / 0x03FF (GC) o 0x1FFF (Wii), métricas que leen 0 y 0x64 que lee 4.
- FIFO enlazado: cada ráfaga del gather pipe avanza el puntero de escritura del CP (a la base cuando es igual al final, si no +32), suma 32 a la distancia y copia los punteros al PI; la GPU lee bloques de 32 bytes mientras la lectura está activada, hay distancia y no está en un breakpoint activado. Interrupción CP (causa PI 0x800) por breakpoint, marca alta o marca baja con su habilitación, solo con la lectura activada.
- Lector de comandos: NOP, carga CP/XF/BP, cargas XF indexadas A-D, display lists, primitivas 0x80-0xBF; 0x01-0x07, 0x3F, 0x44 y 0x48 sin efecto; un comando cortado espera más bytes; los opcodes desconocidos detienen la emulación con el código.
- Estado CP: índices de matrices, VCD, VAT de los 8 formatos, bases y strides de los 16 arrays; tamaño de cada vértice con todos los formatos de posición, normal (incluida NBT con tres índices), color y coordenadas de textura.
- Memoria XF (0x0000-0x1057) y registros BP con la máscara 0xFE; tokens (BP 0x47/0x48) y fin de dibujo (BP 0x45 = 2) llegan al Pixel Engine con el tiempo estimado de Dolphin (mínimo 500 ciclos).
- `hw::PixelEngine` (0x0C001000): registros de configuración, CTRL con habilitaciones y confirmaciones, TOKEN; causas PI 0x200 y 0x400.
- Decodificador de texturas (imagen completa y texel a texel) para los 11 formatos, con paletas IA8/RGB565/RGB5A3.
- `System` conecta CP, PE y GPU; una parada de la GPU se devuelve como `device_stop` con el motivo.

**GPU, parte 2: rasterizador por software de referencia (M014)** — fuentes: archivos de Dolphin como información (`SWVertexLoader.cpp`, `VertexLoader_*.cpp`, `TransformUnit.cpp`, `SetupUnit.cpp`, `Clipper.cpp`, `Rasterizer.cpp`, `Tev.cpp`, `TextureSampler.cpp`, `SWEfbInterface.cpp`, `TextureEncoder.cpp`, `BPStructs.cpp`, `BPMemory.h`, `TMEM.cpp`); el código es propio. Es la referencia de precisión para los backends GLES/Vulkan:
- Carga de vértices: índices de matriz (> 63 detiene), posición, normal (divisores 128/64/32768/16384, NBT), colores en los 6 formatos (el color 1 alimenta el canal 0 si falta el 0; un color ausente es blanco), coordenadas con ByteDequant, datos directos o indexados desde los arrays.
- Transformación XF: matriz de posición, proyección ortográfica y perspectiva (z · (1 − 1e-7)), normales (la principal renormalizada), iluminación de los dos canales con las 8 luces (difusa, especular, foco, atenuación), generación de coordenadas normal / emboss / color, fila de origen 0-12, AB11/ABC1, ST/STQ, dual tex con matriz posterior y normalización, q = 0, escala de textura.
- Ensamblado de triángulos, quads, tiras y abanicos; recorte contra los 6 planos, culling con la regla de Dolphin, división de perspectiva y viewport.
- Rasterizador de punto fijo (28.4 con −9, regla de relleno con sesgo, bloques 2x2), interpolación con desplazamiento 0,495, rectángulos de scissor con las 9 posiciones del offset.
- **Líneas y puntos (M014):** líneas recortadas con el plano +z de Dolphin (−z + w), ensanchadas por medio grosor (BP 22, en 1/6 de píxel) en el eje menor, con remates horizontales o verticales y desplazamiento de coordenadas en el borde lejano; puntos como cuadrado de tamaño BP 22 bits 8-15 (sin recorte, como el software de Dolphin) con desplazamiento de coordenadas en los lados +x/+y.
- TEV completo: 16 etapas, entradas de color/alpha, bias, escala, resta, modos de comparación, clamps, registros y constantes (11 bits), tablas de konst y de intercambio, texturas indirectas (formatos de 8/5/4/3 bits, bias, matrices con escala, matrices dinámicas S/T, wrap, suma con la etapa anterior, alpha bump), alpha test con sus 4 operaciones, z textura (8/16/24 bits, sumar o reemplazar), z temprana y tardía.
- Texturas: los 11 formatos, paletas en TMEM, nearest/bilinear, wrap clamp/repeat/mirror, LOD por bloque 2x2 (FixedLog2, bias en 1/32, min/max), mip point y lineal, imágenes en RAM o en TMEM gestionada a mano.
- EFB: los formatos RGB8_Z24, RGBA6_Z24, RGB565_Z16 (como en el software de Dolphin: color de 8 bits y Z de 24) y Z24; comparación Z, mezcla (factores con +1 desde 128), resta, operaciones lógicas, alpha constante, dither en RGBA6, actualización de color/alpha/Z.
- **Z freeze (M014):** con GENMODE bit 19 la profundidad sale de la pendiente del último triángulo dibujado o descartado por culling antes de congelar (también fuera del scissor; no si fue rechazado trivialmente); valor inicial = constante 1,0 (la suposición de Dolphin).
- **Copias del EFB (BP 0x52, M014; filtro, gamma, intensidad y profundidad revisados en M015, ver abajo):** rectángulo de BP 49/4A + 1 (se ignora si empieza fuera del EFB, se recorta si sale), destino BP 4B << 5, separación BP 4D << 5 (0 = sin copia). A textura: los 13 formatos (R4, R8, RA4, RA8, RGB565, RGB5A3, RGBA8, A8, G8, B8, RG8, GB8 y la variante 0x1 de R8) con la disposición en bloques, media escala con filtro de caja (suma de 6 bits s + (s >> 6) o suma de 8 bits >> 2), intensidad (4096 + 66R + 129G + 25B) >> 8 con los bits 15 y 16, copias de profundidad (R4, R8, RGBA8, G8, B8, RG8, GB8; las demás detienen). A XFB: filtro vertical con los 7 pesos, gamma 1,0 / 1,7 / 2,2, YUV BT.601 con redondeo, croma 1/4-1/2-1/4 y formato Y U Y V. Borrado posterior del rectángulo con las reglas de ClearScreen (alpha solo en RGBA6, cuantización RGB565 y Z16).
- **Precargas de TMEM (BP 0x63, M014):** tipo 3 (RGBA8) alterna líneas de 32 bytes entre los bancos par (BP 61) e impar (BP 62); los demás copian cuenta · 32 bytes; se recorta al final de TMEM.

**GPU, parte 3: referencia por software completa y primera imagen (M015)** — con el repositorio de Dolphin aportado por el usuario, leído solo como información (`BPMemory.cpp`, `CopyRegion.h`, `TextureDecoder_Common.cpp`, `TextureConversionShader.cpp`, `TextureCacheBase.cpp`, `VertexLoader.cpp`, `VideoInterface.cpp`, `BPFunctions.cpp`, `GeometryShaderManager.cpp`):
- **Niebla:** A y C como flotantes de 11 bits de mantisa (BPMemory.cpp), tabla del caso infinito/NaN, perspectiva A / (B_MAG − (z >> B_SHF)) y ortográfica A · z, ajuste por rango con los 10 coeficientes K, lineal, exponencial, exponencial², inversas, mezcla con el color de niebla (fog · 256). B_SHF ≥ 32 o denominador 0 detienen.
- **Texturas RGBA8 en TMEM:** mitades AR y GB en los dos bancos (líneas de TX_SETIMAGE1/2), 32 bytes por bloque; con mipmaps detiene (ver la tabla).
- **Vértices con índice 0xFF/0xFFFF:** se descartan y la primitiva se arma con los demás, como los cargadores de vértices de Dolphin (intérprete, x64 y ARM64).
- **Copias del EFB revisadas** con la ruta de hardware de Dolphin (la verificada en consola para el filtro y la gamma): el filtro de copia también en copias de profundidad, desborde a 9 bits cuando los pesos suman 128 o más, gamma redondeada, conversión YUV con redondeo para cualquier formato (también desde profundidad), todos los formatos desde profundidad, número de bloques por el tamaño reducido, lecturas fuera del EFB con el último píxel.
- **Escala vertical del XFB** (juegos PAL): filas por el vecino más cercano de CopyRegion.h y número de líneas de BPStructs.cpp.
- **Salida de video:** la VI describe el campo (ancho WPL · 16, separación STD · 32, líneas ACV, dirección con el bit de página de TFBL para los dos campos) y `video::decode_xfb` lo convierte a RGB con la conversión de Dolphin. `orca_run --dump-xfb=archivo.ppm` guarda la imagen.
- **Primer cuadro de punta a punta:** el programa PowerPC `gpu_draw` dibuja un rectángulo y un triángulo por el FIFO real, copia el EFB al XFB, configura la VI, y la prueba lee la imagen por los registros de la VI.
- El bit de aspecto de líneas y puntos (BP 22 bit 22) se ignora, como en Dolphin (el SDK siempre lo pone).

**Backend OpenGL ES 3.2 (M016)** — `orca_gles` (opcional en CMake; se compila si hay EGL y GLES):
- La etapa de geometría de referencia (transformación, recorte, culling, líneas/puntos, pendiente de z) sigue en la CPU y entrega triángulos en pantalla; la GPU solo rasteriza, con los vértices ajustados a 1/16 de píxel como el rasterizador por software.
- Atributos exactos: por triángulo, la CPU escribe en un *texture buffer* el contexto y las pendientes (dfdx, dfdy, f0) de z, 1/w, los dos colores y las 8 coordenadas; el shader los lee con `gl_PrimitiveID` y los evalúa en el mismo orden (`precise`), incluido el LOD por bloque 2x2.
- Un único programa (*ubershader*) que es la versión GLSL de la etapa por píxel del software (TEV completo, indirectas, texturas con filtrado entero, alpha test, z textura, niebla, Z, mezcla, lógica, dither, formatos del EFB), con los registros BP en uniforms: no hay compilación de shaders durante el juego.
- EFB en dos texturas R32UI (color y profundidad de 24 bits, misma codificación que `sw::Efb`) leídas y escritas con `GL_EXT_shader_framebuffer_fetch`. En GPUs de mosaicos (Mali, Adreno, PowerVR) esto lee la memoria del mosaico.
- Texturas decodificadas en la CPU a RGBA8 (todos los niveles en un atlas, con los tamaños del hardware) y caché por contenido.
- Copias y borrados del EFB: el EFB se descarga a la CPU, se usa el código de referencia y se vuelve a subir si hubo borrado.
- **Resultado en llvmpipe (Mesa 25.2.8): idéntico bit a bit al software** en las 9 escenas de prueba (cobertura, regla de relleno, profundidad, degradados, TEV, alpha test, mezcla, RGBA6 con dither, texturas con mipmaps y bilineal, niebla, líneas, puntos, z freeze, perspectiva, indirectas, z textura, offsets de scissor, copia y borrado) y en el XFB del programa `gpu_draw`.
- Requisitos: OpenGL ES 3.2 con `GL_EXT_shader_framebuffer_fetch`; `init()` informa si faltan.

## Funciones pendientes (en orden)

1. **Restos de M012 (sin hacer):** los demás comandos de la tarjeta que Dolphin nombra (ReadID, ReadErrorBuffer, WakeUp, Sleep, ArrayToBuffer, WriteBuffer, ExtraByteProgram, ChipErase; ahora con sus códigos de `EXI_DeviceMemoryCard.h`: 0x53, 0x82, 0x85, 0x86, 0x87, 0x88, 0xF3, 0xF4) y el ID del flash en la SRAM; la salida de audio (los bloques del DMA de audio ya se leen pero nadie los reproduce); la mezcla de streaming del AI. También quedan el puerto serie 1 (EXI0/CS2), el AD16 (EXI2/CS0), el mapeo de la ROM en el vector de reset 0xFFF00000, la aplicación de la protección del MI y los comandos restantes de la DI.
2. **CPU, lo que queda detenido a propósito:** modo little-endian, modo traza (MSR[SE]/[BE]), breakpoints IABR/DABR activos, machine check, eciwx/ecowx con EAR[E] = 1, y los casos de FP que el PEM no fija (ver la tabla de verificación). Ningún juego conocido los usa; cada uno da una parada que explica el motivo. Sin TLB ni caché L1/L2 normal (solo la bloqueada).
3. **M018 — rendimiento del backend GLES y app Android:** probar en el Redmi 15C (Mali-G52) que existen `GL_EXT_shader_framebuffer_fetch` y la lectura de adjuntos enteros, y medir; mover la transformación de vértices y las copias del EFB a la GPU; shaders especializados por estado (con el ubershader mientras compilan); caché de texturas sin hash por dibujo; ventana/superficie Android, entrada y audio. Después Vulkan (aquí solo compila: no hay controlador; hay que decidir glslang o un generador SPIR-V propio).
4. **Lo que queda de la GPU por software:** texturas RGBA8 en TMEM con mipmaps, anti-aliasing de RGB565_Z16, formatos de píxel 4-7 (YUV) y el contador de ciclos de copia del EFB (bounding box y contadores de quads hechos en M018).
5. **M019 — JIT ARM64:** emisor ARM64 verificado con el desensamblador de LLVM, bloques básicos, asignación de registros, caché de bloques conectada a la invalidación del bus, y pruebas diferenciales intérprete vs JIT, que tienen que correr en un dispositivo ARM64.
6. **Después:** audio real, DI y discos, SI y controles, EXI, AI y los componentes de Wii (Hollywood, IOS, Bluetooth).

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
| BAT 4-7 solapados con BAT 0-3 | Gana el primero que coincide (0-3 antes que 4-7); en Dolphin gana el último escrito en su tabla. El PEM no define solapamientos |
| DSISR[27-31] de alineación en formas sin actualización | El PEM solo lo define para las formas con actualización; se pone rA siempre, como reconstruye Linux |
| eciwx/ecowx: orden de las comprobaciones EAR[E] y alineación | Primero EAR[E] (como Dolphin) |
| Excepción FP habilitada causada por mtfsf/mtfsfi/mtfsb1/mcrfs, o por mtmsr/rfi que activan FE con FEX = 1 | Parada clara: el PEM dice "antes del siguiente evento de sincronización" sin fijar SRR0 |
| Paired singles con OE/UE = 1 | Parada clara: no está documentado cómo ajusta el Gekko cada slot |
| Resultado con OE/UE = 1 que sigue fuera del formato tras el ajuste (p. ej. frsp de 2^400) | Parada clara (el PEM no lo define) |
| Bits del FR/FI en estimaciones | Se ponen en 0 |
| `mfspr rD,268/269` (alias moderno de `mftb`, el que genera clang) | Desde M017 lee la time base, también en modo usuario, como el intérprete de Dolphin. El 750CL documenta `mftb` (XO 371) |
| DEC = 0 tras el arranque HLE (deja pendiente una excepción de decrementador) | Igual que se deja; falta saber qué valor deja el IPL real |
| Escribir INTSR con bits distintos de RSW | Se ignora |
| DI: DIMAR avanza y DILENGTH queda en 0 al terminar la lectura | Igual que se describe públicamente; sin verificar |
| DI: tiempo de las lecturas | Sin modelo de la unidad: termina en el siguiente paso del planificador o tras `latency_cycles` |
| Arranque HLE desde disco: dirección de bi2 (0x817FE000), pila del apploader (0x816FFFF0), stubs en 0x81300000 | Elecciones propias, porque el IPL real usa direcciones que no documenté; un apploader real solo necesita que sean memoria válida |
| Globales del SO que no se escriben (tipo de consola, arena, modo de video, FST) | El apploader del juego escribe algunos; los demás quedan en 0 |
| VI: los registros de temporización se aplican al momento y reinician la posición (el hardware tiene doble buffer para algunos campos) | Aproximación |
| VI: INT de DIn solo se activa con ENB=1 | YAGCD no aclara si se activa sin ENB |
| VI: escribir DPV/DPH se ignora | Son contadores de solo lectura |
| SI: bit EN del canal c = bit (7 − c) de SIPOLL; Y (sondeos por cuadro) y VBCPY no se modelan; el sondeo empieza en el siguiente campo de video | Sin verificar |
| SI: solo el modo analógico 3 del mando (el que usa el SDK); los modos 0-2 y 4 dan una parada clara | El orden exacto de los nibbles en esos modos no está confirmado |
| SI: las transferencias terminan en el siguiente paso del planificador (sin el tiempo real del bus Joy-Bus) | Aproximación |
| SI: bit 7 del segundo byte de estado = 1 (USE_ORIGIN) | Coinciden dos fuentes independientes; YAGCD lo marca como "unused" |
| EXI: duración de una transferencia | Bits / reloj EXI, redondeado hacia arriba en ciclos de CPU, sin sobrecoste de protocolo |
| EXI: momento del intercambio de datos | Con el dispositivo al escribir TSTART (así un error para la CPU en ese store); DATA, la RAM del DMA, TSTART y TCINT cambian al terminar |
| EXI: lectura inmediata de N < 4 bytes | Solo se reemplazan los N bytes altos de DATA |
| EXI: DATA tras una escritura inmediata; MAR/LENGTH tras un DMA | No cambian |
| EXI: DMA con LENGTH = 0, transferencias lectura+escritura, CS sin dispositivo | Parada clara (no hay comportamiento documentado) |
| RTC/SRAM: escritura parcial del RTC (menos de 4 bytes antes de liberar CS) | Se descarta |
| RTC/SRAM: pasar del final del RTC o de la SRAM en un mismo flujo | Parada clara |
| RTC: valor inicial | 0 o el que fije el anfitrión; la SRAM empieza a ceros. El IPL real la valida con sus checksums |
| ROM del IPL con ROMDIS = 1 (datos sin descifrar) | Parada clara: no se modelan los datos cifrados |
| MI: valor del registro de tipo tras el reset | 0xFF (todos los canales lectura/escritura) |
| DSP: RES | El reset termina al instante: se vacían los buzones y RES se lee como 0 |
| DSP: leer la mitad baja del buzón hacia la CPU | Consume el mensaje (borra el bit 15) |
| DSP: AR_SIZE, AR_MODE, AR_REFRESH | Valores y máscaras de Dolphin (AR_MODE = 1, AR_REFRESH = 156); solo el modo espejo de AR_SIZE tiene efecto |
| ARAM: tiempo del DMA | 246 ciclos de CPU por unidad de 32 bytes (Dolphin); no se midió |
| ARAM: bit de dirección tras el DMA | Se conserva; la cuenta queda en 0 |
| DMA de audio: momento de AIDINT al activar | 200 ciclos después (Dolphin); desactivar antes no lo cancela |
| DMA de audio: ritmo | 8 muestras estéreo por bloque al ritmo del AI; el divisor de 48 kHz da 48 043 Hz según Dolphin |
| AI: sentido del bit AISFR | **Contradicción entre fuentes:** Dolphin dice que está invertido respecto a YAGCD. El AI de M007 sigue a YAGCD; no se cambió |
| AI: valor de AIDFR tras el reset | Dolphin lo inicia a 1 (32 kHz); Orca deja el AICR a 0 (48 kHz) |
| DSP: tiempo | 1 instrucción = 1 ciclo de DSP; 6 ciclos de CPU por ciclo de DSP en GC y en Wii |
| DSP: valores tras el reset | Registros a 0, $wr0-3 = 0xFFFF, $config = 0xFF ("usualmente" según el manual), PC = 0 (con bit 11) o 0x8000 |
| DSP: HALT | El PC queda después de HALT (el manual no lo dice) |
| DSP: interrupción externa | Se toma con $sr.IE y $sr.EIE a 1; no durante LOOP; no cambia $sr al entrar |
| DSP: TSTAXH | Banderas calculadas sobre $axR.h << 16 (TB = bits 15/14 de $axR.h) |
| DSP: C en ORI/ORR | El manual la marca "X" sin definirla: se pone a 0 |
| DSP: C en MOVP/MOVPZ/TSTPROD/..MV/..MVZ | Acarreo de la suma interna h<<32 + (m1+m2)<<16 + l más allá de 40 bits |
| DSP: CLRL | Se sigue la operación del manual (+0x8000 si m es impar, +0x7fff si es par); la nota alternativa del manual dice 0x7fff para m par con otro significado y no coincide |
| DSP: AMDM | El manual da dos direcciones (0xFFED en la tabla, 0xFFEF en la descripción); ambas se guardan sin efecto |
| DSP: DMA del DSP | Termina al instante; DSCR.t nunca se ve a 1 |
| DSP: arranque con el bit 11 | HLE de lo que describe el manual (copia de 1024 bytes); el código real del IROM no se ejecuta |
| DSP: IFcc falso seguido de una instrucción de 2 palabras, LOOP sobre una de 2 palabras o que cambia el flujo | Parada clara (no documentado) |
| Acelerador: fórmula ADPCM | La de Dolphin (redondeo 0x400 sobre la predicción y escala sin redondeo); coincide con el formato DSP-ADPCM |
| Acelerador: reglas de fin de rango, casos ACEA & 0xF = 0/1, ACDSAMP = 0 tras el fin | Las observaciones documentadas en Dolphin; no se midieron aquí |
| Acelerador: escritura cruda en ACEA (ACRWOV) | **Elección propia:** la misma regla que la lectura; Dolphin no la documenta |
| Acelerador: excepciones ACRROV/ACRWOV/ACSOV | Se toman con $sr.IE = 1 (no durante LOOP), primero el nivel más bajo; con IE = 0 quedan pendientes |
| Acelerador: ARAM en Wii | Parada clara |
| Tarjeta: interrupción | Flanco: cada comando terminado activa EXIINT una vez. Dolphin consulta un nivel que 0x89 borra; no se modela |
| Tarjeta: desbloqueo de los juegos oficiales | No se emula: la tarjeta informa UNLOCKED desde el principio, como en Dolphin |
| CP: momento en que corre la GPU | Inmediatamente después de cada ráfaga o escritura en CTRL/CLEAR/distancia (sin tiempo de GPU); Dolphin en un núcleo la hace correr por franjas de tiempo |
| CP: carga VCD baja con bits por encima del 16 | La versión antigua de Dolphin los mezclaba con la parte alta; Orca guarda las dos mitades por separado |
| CP: subcomandos 0x00/0x10/0x20 | Se aceptan sin efecto (contadores de rendimiento según las notas de `CPMemory.h`) |
| XF: carga que empieza o termina fuera de 0x0000-0x1057 | Parada (Dolphin la ignora o la recorta con un aviso) |
| Comandos: XF con campo de cantidad > 15, display list dentro de otra, VAT 8-15 | Parada (Dolphin solo lo comprueba con un assert) |
| Texturas: expansión de 3 bits a 8 | Solo una fuente (Metaforce); 4, 5 y 6 bits coinciden en dos |
| Tarjeta: tiempos de borrado/programa y de DMA | 5000 ciclos y las tasas de Dolphin; no se midieron |
| DSP: accesos genéricos a $st0-$st3, desbordamiento de pilas (STOVF), aritmética de direcciones con $wrN ≠ 0xFFFF | Parada clara (no documentado) |
| Copias: filtro de copia, gamma e intensidad | Desde M015 se sigue la ruta de hardware de Dolphin (`TextureConversionShader.cpp`, verificada en consola según sus comentarios y los PR 10204/10466): filtro también en profundidad, desborde a 9 bits, gamma e intensidad redondeadas, YUV en cualquier formato. Su codificador por software (sin filtro, intensidad truncada) se considera desactualizado (issue 14020). Pesos todos a 0 (estado tras el reset) dan negro |
| Copias a media escala | El shader de Dolphin promedia con el filtro bilineal de la GPU en float (depende de la GPU). Orca usa el filtro de caja entero del codificador por software y después el filtro vertical con los texels dos filas arriba/abajo |
| Copias: texels del relleno de bloque y lecturas fuera del EFB | Se repite el último píxel/fila (como un sampler con clamp). Dolphin dice que el hardware lee basura fuera del EFB |
| Copias de profundidad a media escala RG8/GB8/RGBA8 | El codificador por software de Dolphin invierte bytes; su shader no. Orca sigue el shader |
| Copias de XFB | Croma con el filtro 1/4-1/2-1/4 del codificador por software (el shader de Dolphin promedia pares y marca su redondeo como no verificado). Columna inicial y ancho pares obligatorios. Si se escriben más líneas que filas resultantes de la escala, se repite la fila más cercana |
| Niebla: ajuste por rango | Dolphin lo marca como no probado contra hardware (centro, escala de K × 4); el índice 10 lee el registro 0xEE con peso 0 |
| Texturas RGBA8 en TMEM con mipmaps | Dolphin solo avanza el banco AR (no el GB) al pasar de nivel: parece un error, así que Orca se detiene |
| Vértices con índice 0xFF/0xFFFF | Se descartan como en los cargadores de vértices de Dolphin; su renderizador por software no los descarta |
| Bit de aspecto de líneas (BP 22 bit 22) | Se ignora, como Dolphin (TODO en su código) |
| Conversión del XFB a RGB para mostrarlo | La de Dolphin (BT.601 inversa en float); Dolphin duda de que el hardware haga esta conversión |
| Dirección del campo inferior del XFB | Usa el bit de página de TFBL ("POFF for XFB bottom is connected to POFF for XFB top", Dolphin) |
| Backend GLES en GPUs reales | Solo se probó en llvmpipe (CPU). **No se ha probado en un Mali-G52**: hay que confirmar `GL_EXT_shader_framebuffer_fetch` (Mali-G57 lo anuncia según la documentación de MediaTek), la lectura de adjuntos R32UI y la precisión de división/exp2 |
| Backend GLES: niebla con denominador 0 o factor NaN | Sigue IEEE en lugar de detenerse (el shader no puede informar errores) |
| Backend GLES: varios rangos de scissor | Usan las pendientes del rango principal |
| Precarga de TMEM: máscara de la dirección de origen | Orca aplica la máscara física de la consola (Dolphin lo marca como TODO) |
| Líneas a 45° (\|dx\| = \|dy\|) | Remates horizontales, como Dolphin (que lo marca como FIXME) |
| Puntos | No se recortan (el software de Dolphin tampoco; lo marca como TODO) |
| Contadores de quads del PE | Orden de los registros de Dolphin ("not verified"); un quad 2x2 por triángulo cuando al menos un píxel llega a la etapa (Dolphin por software cuenta cada tercer píxel). Las cuentas de Z solo con la prueba Z activa |
| Bounding box | Bloques 2x2 tras alpha test y Z, como el software de Dolphin; sin verificar en consola |
| Z freeze antes del primer triángulo | Pendiente constante 1,0 (suposición de Dolphin) |
| RGB565_Z16 | Se guarda como RGB8 con Z de 24 bits, como el software de Dolphin; el anti-aliasing de 3 muestras no se emula |

## Problemas conocidos

| # | Problema | Impacto |
|---|---|---|
| 1 | (Resuelto en M017: DSISR de alineación) | — |
| 2 | (Resuelto en M017: DAR al cruzar página según el PEM) | — |
| 3 | Sin TLB: cada acceso recorre los BAT y la tabla de páginas | Rendimiento |
| 4 | No se emulan las cachés normales de datos e instrucciones (el gather pipe sí existe desde M005) | — |
| 5 | Modo little-endian, modo traza y breakpoints IABR/DABR activos: parada del anfitrión | No se ejecuta nada mal en silencio |
| 6 | Machine check no modelado (`MemoryFault`) | Diagnóstico |
| 7 | (Resuelto en M017: BAT 4-7 de Broadway y `eciwx`/`ecowx`; con EAR[E] = 1 se detiene) | — |
| 8 | Excepciones FP habilitadas y resultados con OE/UE = 1 implementados en M017. Siguen deteniéndose: los casos que el PEM no fija (ver la tabla de verificación) | Muy bajo |
| 9 | `lmw/stmw`, instrucciones de cadena y `lwarx/stwcx.` no pasan por la caché bloqueada | Bajo |
| 10 | El DMA de la caché bloqueada y las ráfagas del gather pipe terminan al instante | Falta conectarlos al planificador con tiempos reales |
| 11 | Coste fijo de 1 ciclo por instrucción: no hay modelo de pipeline ni de caché | El tiempo es determinista, no exacto al ciclo |
| 12 | Existe el backend GLES (exacto en llvmpipe) pero sin optimizar: transformación en la CPU, ubershader, copias del EFB por la CPU y texturas con hash en cada dibujo; no hay Vulkan | Un juego puede detenerse en texturas RGBA8 en TMEM con mipmaps, niebla con parámetros degenerados o formatos de píxel YUV (o cuando su microcódigo DSP lee el IROM/COEF sin volcados, o al usar un comando de tarjeta no implementado), siempre con una parada que explica el motivo |
| 13 | Rendimiento en el anfitrión x86: unos 20 M instr/s en enteros y unos 4 M instr/s en FP. El motor FP cambia el modo de redondeo del anfitrión en cada operación | Esperado: el intérprete es la referencia de precisión; la velocidad vendrá del JIT |
| 14 | No se ha compilado para Android/ARM64 (no hay NDK en este entorno) | Hay que verificarlo |
| 15 | `lswx`, `stswx`, `mcrxr` y todas las instrucciones paired-single y `psq` no se pueden verificar con LLVM. Se revisaron contra las tablas de opcodes del manual del 750CL | Medio: hace falta otra fuente (binutils con -m750cl o hardware) |
| 16 | La imagen del XFB se puede obtener (`orca_run --dump-xfb`), pero no hay ventana ni app Android que la muestre, ni salida de audio | Sin frontend todavía |
| 17 | DI: solo existe la lectura 0xA8 por DMA; los discos Wii, el audio streaming y los formatos comprimidos no están implementados | Parada clara con el comando |
