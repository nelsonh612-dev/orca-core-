# Registro de pruebas — Milestones 018-021 e integración final

Todos los resultados son de ejecuciones reales del 2026-11-22 en este entorno, sobre una copia limpia de los fuentes de la rama `orca-core-0.1`.

**Entorno:** Linux x86-64 (Intel Xeon @ 2.10 GHz, 2 núcleos) · GCC 13.3.0 · Clang 18.1.3 · CMake 3.28.3 · Ninja · LLVM `llvm-mc` 18.1.3.

## Matriz de compilación y pruebas

| Configuración | Compilación | Advertencias (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`) | CTest |
|---|---|---|---|
| GCC Release (`-DORCA_WERROR=ON`), x86-64 | OK | 0 | 28/28 pasan (2026-11-23, 847dbaa) |
| Clang Release (`-DORCA_WERROR=ON`), x86-64 | OK | 0 | 28/28 pasan |
| GCC Debug + ASan + UBSan (`-fno-sanitize-recover=all`, `-DORCA_WERROR=ON`), x86-64 | OK | 0 | 28/28 pasan |
| ARM64 real (GitHub `ubuntu-24.04-arm`): GCC, Clang, ASan+UBSan | Ver "Integración final" (resultado por commit en GitHub Actions) | 0 | — |
| Android NDK arm64-v8a (núcleo y app) | GitHub Actions | 0 | — |
| Clang + ASan/UBSan en x86-64 | **NO EJECUTADO**: falta `compiler-rt` en el entorno | — | — |

## Detalle por suite (GCC + ASan + UBSan)

| Suite | Pruebas | Comprobaciones | Fallos |
|---|---|---|---|
| memory | 10 | 65 | 0 |
| cpu_integer (incluye las 9 pruebas de M002) | 33 | 215 | 0 |
| cpu_system (+2 en M017) | 25 | 237 | 0 |
| mmu (+5 en M017) | 19 | 111 | 0 |
| reference (diferencial de enteros) | 6 | 152.001 | 0 |
| **fpu_engine** (M004, +1 en M017) | 10 | 142.779 | 0 |
| **cpu_fpu** (M004, +2 en M017) | 30 | 30.788 | 0 |
| **system** (M005 + EFB por la CPU M018) | 14 | 127 | 0 |
| **programs** (M005) | 3 | 34 | 0 |
| **disc** (M006) | 12 | 145 | 0 |
| **av** (M007 + campo del XFB M015) | 12 | 299 | 0 |
| **exi** (M008 + tarjeta de memoria M012) | 23 | 1.445 | 0 |
| **dsp_mi** (M009, actualizada en M010 y M012) | 15 | 458 | 0 |
| **dsp_core** (M010 + M011 + M012) | 29 | 534 | 0 |
| **gpu** (M013 + precargas M014 + programa `gpu_draw` M015 + registros PE M018) | 27 | 1.066 | 0 |
| **gpu_sw** (M014 + M015 + M018) | 31 | 790 | 0 |
| **efb_copy** (M014 + M015) | 11 | 409 | 0 |
| **gles** (M016 + M018, Mesa llvmpipe 25.2.8, GLES 3.2) | 14 | 2.820 | 0 |
| encodings (vs `llvm-mc`) | 189 instrucciones | 181 ensambladas + 8 desensambladas | 0 diferencias |

### JIT AArch64 (M019) — ejecutado en ARM64 real (GitHub Actions `ubuntu-24.04-arm`)

- `jit_emitter`: 132 instrucciones idénticas a `llvm-mc -show-encoding` (enteros, FP escalar, ADR, codificaciones de enlace); secuencias `mov_imm` verificadas.
- `jit` (cualquier anfitrión): un bloque entero de 7 instrucciones PowerPC (196 AArch64) y uno de FP de 10 (992 AArch64), todas válidas para `llvm-mc`. El algoritmo del FP nativo comparado con `fpu::arith`: 2.550.000 operaciones (suma, resta, multiplicación, división, las cuatro FMA, redondeo a simple; simple y doble; operandos con cancelación, a 1-2 ulp, exactos, puntos medios de simple, extremos y especiales), 1.525.670 por el camino nativo, **0 diferencias** en resultado, FI, FR, excepciones y FPRF.
- `jit` (ARM64): 3000 programas aleatorios comparados con el intérprete (registros, CR, XER, LR, CTR, FPR, FPSCR, GQR, RAM alcanzable directa y por los dos mapeos DBAT0, épocas de escritura del bus por página y global, resultado e instrucciones retiradas): **0 diferencias**; incluyen traducción por DBAT0 (también de solo lectura), `mtspr DBAT0L` en mitad del programa y `dcbz_l`. 2000 programas de FP/paired singles con FPSCR mayormente RN = 0: 480.423 instrucciones en bloques, 220.744 por el camino lento, **0 diferencias**. Enlaces a través de 60 cambios de MSR[IR] con código distinto en las mismas direcciones (126 enlaces): idéntico.
- `programs`: integer_suite (ELF/DOL), float_suite y hardware_suite con y sin JIT: idénticos. Tiempo del System completo en el mismo runner (intérprete → JIT): 157,5 → 6,7 ms; 39,6 → 7,9 ms; 3,5 → 1,6 ms.
- ASan+UBSan en ARM64: todas las suites pasan (ejecución de a15f01c, con la caché de traducción).
- **Errores encontrados por el CI de ARM64 y corregidos:** (1) en una carga con actualización y desplazamiento de más de 12 bits, el registro temporal del valor cargado se reutilizaba para recalcular la dirección; (2) una función sin usar que Clang rechaza con `-Werror`; (3) un commit que no compilaba (faltaba `<chrono>`).
- **Errores encontrados antes de ejecutar:** base r0 con ADD inmediato (31 es SP); en el algoritmo FP, un producto que se redondea a 0 sin ser exactamente 0 (lo encontró la comparación con `fpu::arith`: el producto cero solo vale si uno de los operandos es cero); una fusión de rutas que dejaba el estado de la caché de registros distinto en cada rama (store con actualización en la ruta que sale del bloque), rehecha antes de compilar.
- **Infraestructura:** con `cancel-in-progress` cada push cancelaba el trabajo de ASan en curso; ahora un trabajo en curso termina y solo se reemplazan los que esperan.

### Wii, unidad de disco y audio (M020)

- `crypto`: AES-128 con FIPS-197 B y C.1 (cifrado y descifrado), CBC con SP 800-38A F.2.1/F.2.2 (también en dos llamadas), SHA-1 con "abc", el mensaje de 448 bits, el vacío y un millón de "a" en trozos irregulares.
- `wii_disc`: tabla de particiones, clave de título (y error claro sin clave común), lectura completa de 70 bloques en dos grupos y lecturas desalineadas entre bloques y grupos, rechazo de datos, H0, H1, H2 y H3 manipulados, clave común equivocada, índice coreano, archivos de clave de 16 bytes y `keys.bin`.
- `disc`: estados de la unidad y códigos de error (0x05020401 sin ID, 0x00052402 configuración tardía, 0x00052100 fuera del disco, 0x04020400 motor parado), identificación, ID del disco, búsqueda, streaming (decodificador con filtros 0 y 1 calculados a mano, cola de pistas, parada al final de pista, posición, sin búfer de audio), sin disco.
- `audio`: paso directo a 48 kHz tras la latencia, 32 → 48 kHz con una rampa exacta, canales y volumen del streaming, suma con saturación, anillo lleno, y streaming de un disco a través de la AI hasta el mezclador (100 ms, muestras constantes, sin cortes).
- `av`: AISCNT con las frecuencias reales (480 muestras = 4.855.680 ciclos) y AISFR; `av_suite` mide 404.640 ticks.
- `dsp_mi`: hash de Ector (8, 80 y 11 rotaciones = 0x1FE), protocolo de la ROM (correo invisible con HALT, eco 0xFEEExxxx, subida de un ucode desconocido que se detiene con su hash), ucode init.
- `dsp_hle`: AX (protocolo y correos de tarea, comando desconocido y correo inesperado; SETUP y OUTPUT con valores y deltas; UPLOAD_LRS y SET_LR; una voz PCM16 por el acelerador con envolvente y volúmenes distintos por canal, parada al final y dpop; conversión lineal a media velocidad con bucle y una actualización en el milisegundo 3, comparadas con un modelo del mismo conversor), arranque por la interfaz del DSP con una imagen cuyo hash es el de AX (construida resolviendo el sistema lineal del hash sobre GF(2)); AXWii en sus tres disposiciones de PB (y la numeración antigua), altavoz del Wii Remote y versión de disposición desconocida.
- **Errores encontrados:** en mi prueba de AX olvidé el correo de tarea entre dos listas; en la prueba del hash calculé 30 rotaciones en lugar de 33. Al revisar AXWii antes de las pruebas: la tabla de coeficientes del altavoz del Wii Remote es siempre la primera (no la del PB), y dos versiones no tienen biquad aunque no sean las antiguas. Clang rechazó constantes sin usar (verificación completa antes de cada commit).
- **Error de proceso:** el commit e2ebfc8 se subió sin compilar con Clang (`aram()` sin `override`), por encadenar el commit a una verificación fallida; corregido en ca8beea. Desde entonces el commit va en un paso aparte, solo después de ver el resultado.
- **Mutaciones del JIT en ARM64** (`tools/jit_mutations.py`, rama `orca-core-0.1-mutations`): 22 mutaciones. Primera ejecución: 15 detectadas (dos colgaban la prueba: se añadió un tiempo límite). Las 7 restantes llevaron a cinco pruebas dirigidas (segundo slot de paired singles, división de números diminutos, almacenamiento en un bloque enlazado, página de datos que pasa a ser código, enlaces desde una página igual en dos contextos); dos son equivalentes (documentado en el script). Segunda ejecución: 19 de 22, quedando las dos equivalentes y la división, cuya prueba se reforzó con operandos desnormales (el modelo del algoritmo sin esa comprobación da 777 diferencias en esa distribución).
- **Tercera y cuarta ejecución de mutaciones:** en 2725008 añadí una comprobación (camino lento < mitad de las instrucciones) que era incorrecta: con dividendos menores que 2^-767 el camino lento es el esperado, así que esa ejecución "detectó" las 22 mutaciones por una prueba rota (la validación de ese commit se canceló y no lo mostró). La causa real de la supervivencia de la división: `fdivs` se ejecutaba después de `fdiv` y su segundo redondeo pone FI por sí solo, ocultando el FI/FR de `fdiv`. Corregido en 7b0de24 (`fdiv` al final, comprobación eliminada). Ejecución de 7b0de24: **20 de 22 detectadas**; sobreviven solo las dos equivalentes.
- **Error en mis expectativas:** en la prueba de DTK esperé que, tras pedir "parar al final de la pista", sonara la pista encolada; la orden para al terminar la pista actual.

### Integración final: Bluetooth, Wii Remote, NAND nueva, frontend, app Android y optimización

Ejecutado el 2026-11-23 en x86-64: GCC Release, Clang Release y GCC Debug + ASan + UBSan, **28/28 suites en las tres, 0 advertencias** (commit 847dbaa).

- `ios` (22 pruebas, 2.709 comprobaciones; +4): Bluetooth con un anfitrión guionizado (`BtHost`): inicialización HCI, conexión del mando, L2CAP en los dos sentidos (PSM 0x11 y 0x13, configuración), HID SET_REPORT y DATA, informes a 200 Hz desde el planificador, desconexión, y SDP que se detiene con el motivo; mandos emparejados leídos del SYSCONF; NAND nueva (SYSCONF, setting.txt descifrado, uid.sys, dueños); directorio de datos del título creado por `/dev/di`.
- `wiimote` (6 pruebas, 110 comprobaciones): informes de salida y modos de datos, botones y acelerómetro empaquetados, IR en los tres formatos, lectura/escritura de memoria y sus errores, altavoz ADPCM, puntero → puntos IR, Nunchuk (identificación, calibración, cifrado con clave cero y parada con otra clave).
- `frontend` (3 pruebas, 19 comprobaciones): rechazo de lo que no se puede cargar, volcados del DSP de tamaño incorrecto que dan error, y `gpu_draw` ejecutado por cuadros con su imagen capturada.
- `programs`: `busy_wait_skip_changes_nothing` compara resultado, ciclos e instrucciones retiradas con y sin el salto de bucles de espera.
- `gles` (14 pruebas, 2.820 comprobaciones): idéntico al software con las copias del EFB limitadas al área.
- **Medidas del salto de bucles de espera (x86-64, mismo binario, mismos resultados):** av_suite 2034 → 397 ms; hardware_suite 3,8 → 0,0 ms; dvd_suite 175 s → 0,17 s; integer_suite (sin esperas) 161 → 171 ms (coste de la comprobación). No son medidas del teléfono.
- **App Android:** el flujo `android-app.yml` compila Java y la biblioteca nativa con Gradle 8.9 y el NDK 30.0.16248370 y comprueba que `liborca_android.so` es AArch64 (correcto en 847dbaa). **No se ha ejecutado en un dispositivo.**
- **Errores encontrados:** (1) carrera en las respuestas diferidas de Bluetooth: se respondía a un búfer antes de que el kernel lo registrara como pendiente; ahora solo se rellena al instante si no hay otro pendiente; (2) la NAND nueva cambiaba el dueño de archivos ya escritos (-115): ahora se asigna con el archivo vacío; (3) volcados del DSP no válidos caían al HLE en silencio: ahora dan error; (4) Clang `-Wsign-conversion` en wiimote.cpp, bluetooth.cpp y nand_setup.cpp, detectados por la verificación antes del commit; (5) en mis pruebas: tamaño de ACL (16 en vez de 20), un paquete SDP mal formado, estrechamiento en listas de inicialización; (6) la prueba del salto con dvd_suite tardaba 175 s sin el salto y se quitó de la suite.
- **Error de proceso:** el commit 907ff30 se hizo antes de ejecutar la verificación (contra mi propia regla); la verificación posterior pasó.

### Sistema Wii: Hollywood, IOS, NAND y arranque de discos Wii (M021)

Ejecutado el 2026-11-23 en este entorno (x86-64): GCC Release, Clang Release y GCC Debug + ASan + UBSan, 26/26 suites en las tres, 0 advertencias.

- `ios` (18 pruebas, 1.901 comprobaciones): IPC de Hollywood (tiempos: el acuse llega 500 ticks después de X1 y la interrupción 100 ticks después de Y1/Y2; escribir Y1/Y2 con IY activo levanta la bandera; máscara), kernel (24 descriptores, comandos desconocidos, fd cerrado, respuestas en orden, respuestas diferidas, paradas explícitas), GPIO de Broadway (pines ajenos, sensor de ranura, expulsión, I2C del A/V, reset), STM (gancho de eventos, liberación, botones, apagado), ES (vistas de ticket y TMD, directorio de datos, consumo, título no activo), NAND (rutas, permisos de dueño/grupo/otros, uso, renombrado, borrado recursivo, orden de listado, metadatos que sobreviven a la recarga, nombres con caracteres no válidos en el anfitrión), `/dev/fs` (crear, abrir, leer, escribir, posicionar, atributos, listar, uso, estadísticas, borrar), `/dev/di` (ID del disco por DMA a MEM2, lectura de partición descifrada a través del límite de bloque, errores de la unidad, lecturas sin cifrar dentro y fuera de las áreas permitidas, prueba de fuera de límites, reinicio, cola detrás de la unidad), uid.sys, valores de memoria de IOS.
- **Arranque de un disco Wii de punta a punta:** `integer_suite` dentro de una partición cifrada con hashes H0-H3, con un apploader de prueba compilado para Wii (desplazamientos en palabras), NAND con setting.txt: `boot_wii_disc` y el programa termina con código 0.
- `wii_disc`: el constructor del disco de prueba pasó a `tests/wii_disc_builder.hpp` (mismo resultado: 4 pruebas, 42 comprobaciones).
- **Errores encontrados:** (1) la interrupción de la respuesta se levantaba al instante cuando Broadway liberaba el IPC, sin los 100 ticks; ahora Y1/Y2 se hacen visibles a la lógica de interrupción con su retraso; (2) en el ARM64 con Clang del CI faltaba `<algorithm>` en `test_dsp_hle.cpp` (validación de 929df8a); se añadió el include en los 13 archivos que usaban sus funciones; (3) en mis pruebas: reutilizar un bloque de petición ya respondido (tiene el comando 8), una tabla de vectores sobrescrita, esperar el acuse de B antes de la respuesta de A con una unidad sin latencia, y comprobar memoria baja que el propio programa sobrescribe (enlazado en 0x80003100).

### CI en ARM64 real y Android NDK (M018)

- Flujo `.github/workflows/orca-core-0.1.yml` (sin APK): pruebas en ARM64 nativo (`ubuntu-24.04-arm`) con GCC, Clang y ASan+UBSan, y compilación del núcleo con el NDK de Android para arm64-v8a, API 26.
- **Infraestructura:** mientras el repositorio era privado, GitHub no arrancó ningún trabajo ("recent account payments have failed or your spending limit needs to be increased"); no es un fallo del código. Desde que es público, los trabajos corren.
- **Primera ejecución real (7bbaf93):** compilación con el NDK correcta; en ARM64 dos errores que en x86 no se veían:
  1. **Contracción FMA:** los compiladores de AArch64 fusionan `a*b+c` por defecto (GCC `-ffp-contract=fast`, Clang `on`), lo que cambió la profundidad (0x3FFFFF en vez de 0x400000) en `gpu_sw` y en la referencia de `gles`. Se reprodujo en x86 con `-mfma -ffp-contract=fast` y se corrigió con `-ffp-contract=off` en todo el proyecto (las operaciones fusionadas del hardware emulado ya usan `std::fma` de forma explícita). Con ese build forzado, 19/19.
  2. **Referencia nativa de `float_suite` con Clang en ARM64:** el valor 45 (hash de comparaciones) difería solo en el bit de signo. Clang 18 desenrolló el bucle sobre la tabla constante y convirtió `a > b ? a : b` en `fmaxnm`, que da +0.0 para (0.0, −0.0) cuando C exige −0.0. Orca daba el valor correcto (igual que x86 y GCC en ARM64). Se comprobó en el ensamblador de AArch64 generado aquí con las mismas opciones (8 `fmaxnm` en el bucle) y se puso esa selección en una función `noinline` (ahora `fcmp` + `fcsel gt`). Primero lo descarté por un error mío: miré el ensamblador con `-ffreestanding`, que no expande `memcpy` y no desenrolla el bucle.


- **Software (valores a mano):** caja inicial 1023/0 por BP 0x55/0x56; el triángulo x + y <= 6 da 0..7 x 0..7; un píxel en (13, 20) la amplía a 13 y 21; BP 0x55 solo cambia izquierda/derecha. Contadores: con Z apagada, 0 en Z y 10 quads en la mezcla (bloques con bx + by <= 6); Z tardía ALWAYS: 10/10/10; Z temprana NEVER: 10 entran, 0 pasan, 0 en mezcla y la caja no crece; Z temprana LESS con profundidad mayor: 10/10/10.
- **Registros PE:** lectura de caja y contadores (mitades baja/alta, también 32 bits), escritura rechazada, ciclos de copia detenidos, y parada cuando el backend no los sigue.
- **GLES frente a software:** las 14 palabras de PE 0x10-0x2A idénticas tras cuatro grupos de dibujos (Z apagada; Z tardía LEQUAL con planos que se cortan; Z temprana con alpha test que rechaza la mitad y BP 0x55 reescrito; borrado de contadores y otro offset de scissor) y tras BP 0x56 escrito justo después de un dibujo. Valores de control: Z temprana 9162 entran / 4581 pasan, Z tardía 7540 / 5026, mezcla 7339. EFB idéntico (0 diferencias). Leer antes de BP 0x55-0x57 en GLES se detiene.
- **Caché de texturas GLES:** invalidación con las cinco formas de escribir: `write32` en la segunda página de la textura (1 subida), los mismos bytes otra vez (0 subidas, un solo hash y ninguno en el dibujo siguiente), escritura en la página vecina (0 subidas), escritura externa por `ram_pointer` + `notify_external_write` (1 subida) y `dma_write` (1 subida); EFB idéntico al software. Bus: épocas por página (último byte de página, escritura de 64 bits que cruza dos páginas, MEM2 por DMA, escritura externa, límites del rango vigilado). Medida de coste: 400 dibujos con una textura de 512x512, 690-790 ms → 371 ms → 55-61 ms.
- **Mutaciones de la caché:** 7, todas detectadas. T7 (época no renovada tras un hash) sobrevivió al principio porque solo cuesta tiempo: se añadió el contador `texture_hashes()` a la prueba. T3 (límite del rango vigilado) solo se ve con ASan (heap-buffer-overflow), que forma parte de la verificación. **Acumulado: 402.**
- **Acceso de la CPU al EFB:** compresión Z16 a mano (lineal 0xABCDEF → 0xABCD, near 0x123456 → 0x091A y 0xFFFFFF → 0xFFFF, far 0xF0F000 → 0x41E0); escritura y lectura de color/Z en RGB8 con los tres modos de alfa; píxel dibujado leído por la CPU; RGB565 (0xFF213042) y Z16; RGBA6 (0x82FF4120 y alfa 0); coordenadas fuera del EFB que no tocan el píxel al que apuntarían; paradas. GLES frente a software: 600 lecturas (color y Z) sobre un triángulo con profundidad idénticas, y 80 escrituras con el EFB completo idéntico. Sistema completo (GameCube y Wii): `stw`/`lwz` en 0xC8000000 por el DBAT1 escriben y leen el EFB.
- **Mutaciones del acceso al EFB:** 9; 8 detectadas (E5, coordenadas fuera del EFB, sobrevivió al principio: se añadió la comprobación del píxel al que apuntaría). E7 es equivalente (el alfa de los formatos sin alfa ya es 0xFF). Error en mi prueba: calculé 0xAB >> 2 como 0x2B (es 0x2A). **Acumulado: 395.**
- **Pruebas modificadas:** `gpu_pe_registers` comprobaba que 0x10 y 0x2C se detenían "no implementado"; ahora 0x10 se lee y 0x2C sigue detenido con el motivo nuevo.
- **Errores encontrados (todos en mis pruebas):** esperé 0 quads de mezcla con la Z apagada (la mezcla siempre cuenta, como en Dolphin); una escena de Z temprana no pasaba ningún píxel porque la profundidad era 0, así que no probaba nada (se detectó al imprimir los valores y se corrigió); un quad quedaba fuera de pantalla porque seguía activo el offset de scissor anterior.
- **Mutaciones (M018):** 13 sobre el shader (cuentas, intercambio atómico, ampliación de la caja), el backend (base de ids, subida por época, sincronización), el software y el PE: **13 detectadas** (S11, la sincronización antes de que la CPU escriba media caja, sobrevivió al principio; se añadió BP 0x56 justo tras un dibujo). **Acumulado: 386.**

### CPU: cierre de los pendientes de la CPU (M017)

Fuentes nuevas: manual *Programming Environments for 32-Bit Implementations* (NXP MPCFPE, secciones 6.4.6 y 6.4.7, leído en línea), manual del 750GX (excepciones FP siempre precisas), `arch/powerpc/kernel/align.c` de Linux (disposición del DSISR de alineación), y Dolphin como información (`Gekko.h`, `MMU.cpp`, `Interpreter_SystemRegisters.cpp`, `Interpreter_LoadStore.cpp`).
- **BAT 4-7 de Broadway:** DBAT5 por `mtspr 570/571` y lectura con `mfspr`; con HID4[SBE] = 0 se ignora (DSI), con SBE = 1 (por `mtspr 1011`) traduce; IBAT7 para búsqueda de instrucciones (y ISI sin SBE); en Gekko las SPR 560-575 se detienen y BAT 4-7 nunca se buscan.
- **DSISR de alineación:** lfd 0x2424, lfdx 0x1A424, lfdux 0x1E424 (bit 25 → DSISR[17]), lmw 0x1FA4, lwz 0x64; de punta a punta con `lfd` y `lmw` desalineados. Los valores se calcularon a mano y coinciden con las etiquetas de la tabla de Linux ("11 0 1001: lfdx", "00 0 0000: lwz").
- **DAR al cruzar página:** mismo segmento por tabla de páginas → EA original; primera página por BAT y segunda por tabla (mismo segmento) → 0x10020000; cruce de segmento → 0x10000000.
- **eciwx/ecowx:** EAR[E] = 0 → DSI con DSISR[11] (y [6] en ecowx), DAR = EA (también con rA = 0); EAR[E] = 1 desalineado → alineación; EAR[E] = 1 alineado → parada clara.
- **mfspr 268/269:** leen la time base, también en modo usuario; `mtspr 268` sigue detenido.
- **Excepción FP habilitada (precisa):** fsub inf−inf con VE y FE0 → excepción de programa, SRR0 = la instrucción, SRR1[11], MSR[FE0/FE1/FP] a 0, destino sin cambios, instrucción contada como retirada; fadd inexacto con XE y FE1 → resultado escrito y excepción; fcmpo con QNaN y VE → excepción con CR2 escrito; sin FE no hay excepción. mtfsfi que deja FEX = 1, mtmsr y rfi que activan FE con FEX = 1 → parada clara sin cambios.
- **Resultados con OE/UE = 1:** 19 525 resultados ajustados (y 3826 no representables) comparados bit a bit con el modelo exacto de las pruebas (`fp_reference.hpp`, que no comparte código con el motor): suma, resta, multiplicación, división, las cuatro FMA y frsp, simple y doble, los 4 modos de redondeo, con operandos que fuerzan el desborde, el subdesborde y sumandos de cualquier magnitud. A nivel de CPU: fmul 2^1000·2^1000 → 2^464 con OX, FPRF normal y CR1 = 0xD; fmuls 2^-100·2^-100 con UE → 2^-8 en los dos slots y UX sin XX (exacto); frsp 2^200 → 256; frsp 2^400 → parada (no representable); ps_mul con OE → parada.
- **Pruebas modificadas por cambio de comportamiento intencional (no se ocultó ninguna falla):** `invalid_operation_enabled_and_disabled` y `zero_divide_and_overflow_enables` comprobaban la parada "no implementado" de la excepción FP precisa y de OE; ahora comprueban la excepción y el resultado ajustado. `page_crossing_fault_writes_nothing` comprueba además el DAR.
- **Errores encontrados:** (1) la primera versión del DAR seguía mi recuerdo del PEM ("primer byte de la página que falló"); el texto real dice "segmento o área BAT", así que se corrigió antes de entregar. (2) En los resultados ajustados, el operando sustituto que solo aporta el bit *sticky* se construía antes de escalar y desbordaba a infinito o se anulaba en los extremos: lo detectó la prueba diferencial (4452 fallos en FMA) y ahora se construye ya escalado. (3) Mi expectativa de CR1 (0xC) olvidaba que CR1 copia OX: el valor correcto es 0xD.
- **Mutaciones (M017):** 17 sobre BAT extendidos, DSISR, DAR, eciwx, mfspr, la excepción precisa y los resultados ajustados: **17 detectadas** (C2 y C3 sobrevivieron al principio; se añadieron el cruce BAT → tabla en el mismo segmento y lfdux). Tres mutaciones se reescribieron porque `-Werror` impedía compilarlas. **Acumulado: 373.**

### Backend OpenGL ES 3.2 (M016)

Entorno: EGL "surfaceless" de Mesa 25.2.8, llvmpipe (LLVM 20.1.2), OpenGL ES 3.2. Cada escena se envía como bytes de comando reales a dos GPU (software y GLES) y se compara el EFB completo (640x528 colores y profundidades).
- Resultados: **0 píxeles distintos en color y 0 en profundidad** en las 9 escenas: triángulos, tiras, abanicos, regla de relleno con aristas sobre las muestras (dos órdenes de vértices), triángulos grandes con pendientes irregulares, profundidad con planos que se cortan; degradados de color y TEV (lerp, resta con escala, comparación, dos etapas); alpha test, mezcla, resta, XOR, RGBA6 con alpha constante y dither; texturas nearest/repeat/mirror/bilinear, mipmaps point y lineal, quad rotado y minificado; niebla, líneas, puntos, z freeze; perspectiva con colores, texturas y niebla con rango, matriz indirecta en los 4 formatos, z textura, otro offset de scissor; escala/bias del TEV, z temprana con alpha test que falla, z sin actualización, z textura de 8/16/24 bits, sesgo de LOD y LOD por bloque en perspectiva con 3 niveles; copia del EFB + borrado + dibujo encima (también los bytes copiados a RAM).
- Programa `gpu_draw` en el `System` completo con el backend GLES: el XFB es idéntico byte a byte al del software (614 400 bytes).
- La primera versión interpolaba los atributos con la GPU y los corregía con derivadas: la profundidad difería en 1 en 2272 píxeles y, con perspectiva, 8113 píxeles diferían en 1-2 niveles. Por eso se pasó a evaluar las pendientes del software por triángulo (ver ESTADO); desde entonces, 0 diferencias.
- Tiempo (solo informativo, llvmpipe corre en la CPU de este equipo): 64 quads texturizados a pantalla completa, 21,6 M píxeles: software 4451 ms, GLES/llvmpipe 2569 ms. **No dice nada sobre un teléfono.**
- **Mutaciones (M016):** 17 sobre el shader y el backend (ajuste de vértices, tabla konst, dither, factores de mezcla, redondeo de escala, exp², sesgo de LOD, LOD por bloque, espejo, actualización de Z, alpha test, z textura, contexto de pendientes, 1/w, niveles de mip, caché de texturas, subida tras el borrado): **16 detectadas** tras añadir la escena "TEV scale/bias, z, ztex, LOD" y la z temprana sin actualización. G11 es equivalente (con `inout`, el color ya contiene el valor leído; restaurarlo no cambia nada). **Acumulado: 356.**
- **No probado:** ninguna GPU real (Mali, Adreno, PowerVR), Android, rendimiento en teléfono.

### GPU, parte 3: referencia completa y primera imagen (M015)

Fuente: el repositorio de Dolphin aportado por el usuario (solo como información). Valores esperados calculados a mano; los de la niebla con una calculadora float32 independiente del código (numpy), porque dependen del redondeo de `pow`; se eligieron parámetros lejos de los límites de cuantización (margen ≥ 0,1 de un nivel).
- Niebla: lineal 115, exponencial 234, exponencial² 172, inversas 12 y 47 (rojo → (255·(256−f)) >> 8, azul (f·255) >> 8); perspectiva con B_MAG 0x100000 y B_SHF 4 (191); rango con K = 4 en x = 301 (65 frente a 63 sin rango); tabla de A y C infinitos (con niebla / sin niebla según los signos); paradas por B_SHF 32 y denominador 0.
- RGBA8 en TMEM: 4x4 cargada con una precarga tipo 3 (los 16 texels), 8x8 con 2x2 bloques (bloques 1, 2 y 3), una dirección de RAM falsa que no se usa, y la parada con mipmaps.
- Vértices descartados: tira de 5 con el tercero a 0xFF (cuadrado de 16 píxeles), triángulo de 4 con el segundo descartado (6 píxeles), todos descartados (nada, sin error).
- Copias revisadas: intensidad redondeada (137 en lugar de 136), U en lugar de G con la conversión (157), intensidad y RGB565 desde profundidad (55, 0x11AA), A8 desde profundidad (255), gamma redondeada (3 → 34), desborde a 9 bits (200 → 78; 100 → 255), filas del bloque bajo el rectángulo con y sin clamp, profundidad filtrada (128 → 186 con gamma 2,2), bloques por el tamaño reducido (17 de ancho a media escala → 1 bloque), última columna/fila repetida fuera del EFB.
- Escala del XFB: 0,5 con 4 filas (2 líneas: filas 0 y 2), 2 con 2 filas (3 líneas: 0, 1, 1), una fila a 0,5 (parada: 0 líneas), escala 0.
- Conversión XFB → RGB: blanco 254, rojo (255, 0, 0), Y 16 con croma roja (178, 0, 0), segunda línea con separación, datos cortos y ancho impar; ida y vuelta de un gris 0x80 (127-129).
- VI: dirección del campo inferior con el bit de página de TFBL, y descripción del campo (640, 1280, 240).
- **Programa `gpu_draw` (PowerPC, clang):** FIFO como GX, transformación y estado como el SDK, un rectángulo rojo y un triángulo verde, copia al XFB con borrado y configuración de la VI. La prueba lee la imagen por los registros de la VI: rojo (≥ 250, ≤ 3, ≤ 3) dentro del rectángulo, verde en el triángulo, negro fuera, el EFB borrado a azul oscuro después de la copia. `orca_run --dump-xfb` con el mismo programa produjo la imagen esperada (se revisó visualmente; se entrega aparte).
- **Mutaciones (M015):** 25 mutaciones (niebla N1-N9, RGBA8 en TMEM T1-T3, descarte S1, copias C1-C8, XFB → RGB X1-X2, VI V1-V2): **23 detectadas** tras añadir las pruebas de T2 (textura de 2x2 bloques) y C8 (clamp de la fila actual, que reveló que esa parte del cambio no se había aplicado al código: corregido). Sobreviven: C1 (desborde con suma exactamente 128: equivalente, 255·128 >> 6 = 510 < 512) y N3 (16777215 frente a 16777216 en la niebla en perspectiva: diferencia relativa 6·10⁻⁸, menor que un nivel de niebla con parámetros de 11 bits de mantisa; no se encontró una entrada que la distinga). **Acumulado: 340.**
- **No probado:** un juego real (no hay frontend; el rasterizador por software es lento), la niebla y el XFB contra hardware real.

### GPU, parte 2: rasterizador por software (M014)

Todos los valores esperados se calcularon a mano a partir de las reglas documentadas en las cabeceras (`sw/vertex.hpp`, `sw/renderer.hpp`, `sw/efb_copy.hpp`, `gpu.hpp`), no ejecutando el código. Cuando una expectativa mía falló, se revisó contra la fuente antes de tocar nada (ver "Errores encontrados durante M014").
- Carga de vértices: posiciones (s16 con frac, u8 con y sin ByteDequant, float, indexadas con base virtual enmascarada), índices de matriz y el límite 63, salto 0xFF/0xFFFF, normales con sus divisores y NBT, colores en los 6 formatos, color 1 en el canal 0, coordenadas en tres grupos del VAT.
- Transformación: posición y proyección ortográfica y perspectiva (incluido el factor 1 − 1e-7, que deja z 1,5 ulp por debajo), normales renormalizadas, iluminación difusa / con signo / clamp, foco, especular con y sin normalizar la distancia, luces 4-7 (bits 11-14), ambiente del vértice, segundo canal; texgen regular (filas 0, 3, 4 y 5, AB11 frente a ABC1, STQ), emboss con la tangente y la binormal, color, dual tex con y sin normalizar, q = 0 (x/2 e y/2 con clamp), NaN → 1.
- Rasterizado: cobertura de un triángulo (28 píxeles, x + y ≤ 6) y de un quad, regla de relleno con un rectángulo alineado con las muestras (32 píxeles con los 8 órdenes de vértices), tiras, abanicos, culling, Z (LEQUAL, LESS con la misma profundidad, ALWAYS, NEVER, temprana/tardía, sin actualización), recorte y scissor, interpolación de color.
- TEV: lerp con c = 128 + 1, resta con bias y escala 2, bias −128, escala 1/2 sin redondeo, comparaciones GT/EQ (incluida GT con a = b), konst 1/8 y la selección inválida 9 (= 0), dos etapas con registros intermedios.
- Alpha test (GREATER/NEVER con OR, XOR), mezcla (factores con +1 desde 128: 192 en lugar de 191), resta, XOR lógico, actualización de color desactivada, RGBA6 con alpha constante, solo alpha, dither (incluido un valor que distingue el orden [y][x] de la tabla).
- Texturas: nearest, repeat, mirror, clamp, bilinear (pesos 1/127/127/16129), paletas C8 en TMEM, selección de mip point/lineal con límites, sesgo de LOD en 1/32 (14 → nivel 0, 16 → nivel 1), alpha bump indirecto de 8 y 5 bits, matriz indirecta con escala 17 en los cuatro formatos (8, 5, 4, 3 bits: texels 1, 2, 1, 2), z textura de 8/16/24 bits con reemplazo y suma.
- **Z freeze:** pendiente inicial 1,0 (profundidad 1), pendiente congelada de un triángulo inclinado (mismo dz/dx y dz/dy), triángulo descartado por culling y triángulo fuera del scissor (pero dentro del volumen de recorte) que actualizan la pendiente, dibujo congelado que no la cambia.
- **Líneas y puntos:** línea horizontal (fila 4, x 2-9), vertical de ancho 2 (columnas 19-20), tira de dos segmentos (16 píxeles), recorte por el primer y por el segundo vértice, línea larga recortada cerca del extremo, plano +z de las líneas (z = 1,5 se descarta; z = 0,5 se dibuja), 45° con remates horizontales, color interpolado en el extremo recortado, puntos de 2x2 y de 1 píxel, desplazamiento de coordenadas de puntos (texels 0 y 2 en cada eje) y de líneas, parada por el ajuste de aspecto.
- **Copias del EFB** (suite `efb_copy`): RGBA8, RGB565 y RGB5A3 desde RGB8; RGB5A3 (4443 y 5551), RGBA8, A8, RA4, RA8, RG8 y GB8 desde RGBA6; R8, I8 = 136 para (100, 150, 200), intensidad sin conversión automática, G8, R4 con dos texels por byte; disposición de bloques (8x4, 8x8, dos bloques y segunda fila en la separación); media escala (6 bits: 100 → 101; 8 bits: 15 >> 2; fila 1; B8 de 8 de ancho; límite 224 de RGB5A3; filtro activo sobre RGBA6); profundidad (Z 0x123456 en RGBA8, R8, G8, B8, RG8, GB8, R4; formatos inválidos); relleno de bloque, direcciones lineales, rectángulo recortado, lectura más allá del EFB, rectángulo fuera (ni copia ni borrado), separación 0 (solo borrado), destino fuera de RAM.
- Filtro y gamma: pesos por defecto = copia exacta; deflicker 8,8,10,12,10,8,8 sin clamp (100), con clamp superior (125) e inferior (75); brillo ×2 con saturación (202, 255); gamma 2,2 de 128 → 186 (y el valor 3 igual que 2,2); profundidad sin filtrar; pesos a 0 dan negro.
- XFB: fila roja (Y 82, Cb 90, Cr 240, los valores BT.601), blanco 235/128 y negro 16/128, filtro de croma con resultado negativo redondeado hacia abajo (−28,5 → −29), gamma 2,2 (Y 176), clamp superior con el filtro vertical (125 → 16), columna impar, escala 0,5 y 2, escala 0 con inversión, escala 1 con inversión.
- Borrado: RGB8 solo color dentro del rectángulo, RGBA6 con color, alpha y Z, solo alpha, RGB565_Z16 con cuantización (0x102031) y Z16 (0xABCDAB), nada habilitado.
- A través de comandos reales de la GPU: copia y borrado, segunda copia que ve el color de borrado, máscara BP 0xFE aplicada a BP 0x52.
- **Precargas de TMEM** (suite `gpu`): valor 0 sin efecto, 3 líneas a la línea 2 de TMEM, recorte al final, inicio fuera de TMEM, RGBA8 alternando bancos, final de TMEM en el banco par y en el impar, origen fuera de RAM.
- **Mutaciones (M014):** 78 mutaciones del código nuevo (`efb_copy.cpp`, líneas/puntos/z freeze de `renderer.cpp`, `vertex.cpp`, TEV/mezcla/texturas de `renderer.cpp`, precargas de `gpu.cpp`): **75 detectadas**. Las 3 que sobreviven son equivalentes: F15 (quitar el `min(255)` del filtro: la gamma ya satura a 255), P1 (una precarga con valor 0 copia 0 bytes) y P5 (`even > kTmem` en lugar de `>=`: con `even = kTmem` la longitud es 0). La primera pasada dejó 28 sin detectar: se añadieron las pruebas que faltaban (listadas arriba) y se eliminaron dos líneas de código muerto que revelaron (la segunda copia del borde del XFB, que nunca se lee, y una condición redundante en el borrado).
- **No probado:** el camino de un juego real hasta una imagen en pantalla (no hay salida de video), la niebla, las texturas RGBA8 en TMEM y la escala vertical del XFB (no implementados: detienen la emulación).

### GPU, parte 1 (M013)

Valores esperados calculados a mano a partir del formato de cada comando y registro.
- Lector de comandos: cada tipo de comando con sus campos; los 64 opcodes de primitiva (tipo y VAT); 16 palabras XF como máximo y parada con más; cortes en cada byte de seis comandos (no se consume nada); display lists (dirección y tamaño & ~31); los ocho opcodes sin efecto; siete opcodes desconocidos; parada que viene del receptor o del tamaño de vértice.
- CP: todas las cargas y sus índices; tamaños de vértice de posición (2/3 componentes, s16, float y formatos 5-7), matrices, normales (directa NBT, índices de 8/16 bits, con y sin index3), los 6 formatos de color directos y los inválidos, coordenadas de textura en los tres grupos del VAT.
- XF: carga en memoria, en registros, cruzando de memoria a registros, en la zona sin uso, recortada al final y fuera del espacio. BP: máscara 0xFE (incluida una máscara aplicada a sí misma) y reinicio.
- PE: registros, confirmaciones, mínimo de 500 ciclos, un solo evento para varias peticiones, petición de interrupción que se mantiene, habilitación posterior.
- FIFO a través de la CPU (bus real y gather pipe): token y su interrupción; vuelta del anillo con un comando XF de 69 bytes repartido en tres ráfagas y datos comprobados en RAM; breakpoint (la GPU se para, bits de estado, interrupción), marca alta y baja con y sin habilitación, desbloqueo; FIFO sin enlazar; puntero de lectura más allá del final; display list con una carga CP dentro y carga XF indexada desde el array 12; máscaras de los registros (GC y Wii); tiempos de GPU.
- Programa PowerPC `gpu_suite` (compilado con clang): configura PI y CP como GX, envía tokens y "draw done" por el gather pipe y espera las causas PE por sondeo: token 0x1234, punteros avanzados 0x20, distancia 0, token 0x5678, causas 0x600, confirmadas, STATUS 0x0C.
- Texturas: valores a mano de RGB565, RGB5A3 (los dos modos), IA8, IA4, I4, I8, RGBA8, segunda tesela, CMPR en los dos modos (incluidos extremos iguales) y paletas C4/C8/C14X2 en los tres formatos; errores (índice fuera de la paleta, datos cortos, formatos inválidos); comparación de los 11 formatos entre el decodificador por teselas (recorrido secuencial) y el de texel suelto, con tamaños 13x11, 8x8, 1x1 y 17x4 y las tres paletas: 0 diferencias.
- **No probado:** el camino de un juego real hasta la GPU (se detendría al dibujar).

### Acelerador, DMA de audio y tarjeta de memoria (M012)

Fuentes de comportamiento: los archivos de Dolphin aportados por el usuario (solo como información; ver README). Todos los valores esperados se calcularon a mano.

Acelerador (reescrito):
- ADPCM con la fórmula de Dolphin: trama a mano (muestras y reinicio en ACSA), 84 muestras contra un decodificador de referencia aparte (0 diferencias), los dos casos especiales de fin (ACEA & 0xF = 0 y 1), saturación, ACDSAMP = 0 tras el fin y vuelta a funcionar al escribir YN2.
- PCM: 16 bits con escala 1/1 (0x1234·2 = 0x2468, truncado sin saturar 16384·2 = 0x8000), escala 1/2048 con A1 = 0,5 (200 y 300), **escala 1/65536 (0x4000·0x4000 >> 16 = 0x1000)**, 8 bits extendido con ceros, PCM desde ACIN (decode 1 no avanza ACCA, decode 3 sí), escala 3 → parada.
- Crudas: 16 bits con escritura (bit 31), lectura y ACRROV; 8 y 4 bits (nibble alto primero, direcciones en nibbles); escritura sin bit 31 ignorada; **desbordamiento de escritura: la tercera escritura cae en ACSA con el bit 31 conservado y ACRWOV pendiente**; tamaño 3 y ARAM ausente → parada.

DMA de audio y ARAM:
- Ritmo: 121 392 ciclos por bloque a 32 kHz y 80 928 a 48 kHz (8 · 486 MHz · 3372 o 2248 / 108 MHz); AIDINT a los 200 ciclos; 3 bloques leídos en orden, recarga y AIDINT al agotar la cuenta, AIDMA_LEFT; desactivar y RES detienen; máscara 0x1FFF en Wii; el bit 6 del AICR del `System` cambia el ritmo.
- ARAM: AR_MODE = 1 de solo lectura, AR_REFRESH = 156 con máscara 0x7FF, AR_SIZE 0x7F, máscaras de los registros altos, 246 ciclos por unidad y el espejo de 4 MiB.

Tarjeta de memoria (a través de los registros EXI reales):
- Identificación: primer byte 0x80 y luego 0x00000010 (16 Mbit) / 0x00000004 (4 Mbit); estado inicial 0xC1; bit EXT con la tarjeta puesta.
- Lectura 0x52 en 0x12345 (bytes de dirección 0x00 0x91 0x02 0x45) con 4 bytes de espera, y vuelta dentro del sector (0x21FE, 0x21FF, 0x2000).
- DMA de 512 bytes desde 0x4200: 0 diferencias y **474 112 ciclos** (512 × 926).
- Programa de página: los datos solo se escriben al liberar CS; DMA de 128 bytes en **631 936 ciclos** (128 × 4937); 128 bytes inmediatos llenan el búfer entero; programa sin datos termina igual.
- Borrado: el bloque de 8 KiB queda a 0xFF y el siguiente intacto; BUSY hasta 5000 ciclos después, luego READY.
- Interrupción: sin 0x81 no hay interrupción; con 0x81 = 1 llega EXIINT al terminar.
- Errores: comando 0x85 → "command 85 is not implemented"; leer antes del comando; tamaño 12 Mbit; imagen de 100 bytes; tarjeta en blanco = 0xFF.
- `System`: ranura ocupada, ranura 2 inexistente, EXT/EXTINT en el canal 0 y ranura B vacía.
- `orca_run --card-a=/tmp/c.raw` con un archivo inexistente arranca con una tarjeta en blanco y no la guarda (no cambió); una imagen de 1000 bytes da "memory card size must be 4, 8, 16, 32, 64 or 128 Mbit". **No probado:** guardar una imagen modificada desde `orca_run` (ningún programa de prueba escribe en la tarjeta).

### Acelerador del DSP (M011)

- Valores calculados a mano: trama con cabecera 0x00, A1 = 0x0800 (1,0), nibbles 1, 2, 3, −1 → muestras **1, 3, 6, 5**; al pasar ACEA = 5 la dirección vuelve a ACSA = 2 y la quinta muestra es **6** (mismo nibble con otra historia); el manejador de ACSOV corre una vez y RTI vacía las pilas.
- 84 muestras (6 tramas con 6 pares de coeficientes y escalas distintas, YN1 = 100, YN2 = −50) comparadas con un decodificador de referencia escrito aparte a partir de las descripciones del formato: **0 diferencias**; al consumir todo el rango queda ACSOV pendiente.
- Saturación: escala 2^15 con nibbles 7 y −8 → 0x7FFF y 0x8000; PRED_SCALE cargado de la cabecera, YN1/YN2 y ACCA (cabecera + 2 muestras = 4).
- Crudas de 16 bits: escritura con el bit 31 (0xBEEF, 0x1234 en la ARAM), lectura, recarga desde ACSA al pasar ACEA y manejador de ACRROV tomado al activar IE.
- Paradas claras: formato PCM, lectura cruda de 8 bits, escritura cruda sin el bit 31, ARAM fuera de rango y ARAM en Wii.
- Integración: el núcleo lee la ARAM de 16 MiB del `System` de GameCube.

### Núcleo del DSP (M010)

Todas las codificaciones salen de los campos de bits del manual del DSP y todos los valores esperados se calcularon a mano con sus fórmulas:
- Anchos de registro, modo 40 bits (extensión de signo y saturación a 0x7fff/0x8000), banderas de ADD/SUB con desbordamiento y acarreo según el manual, OS persistente, CLR, inmediatos, NEG (acarreo solo con 0), ABS, CMPIS.
- Lógica sobre $acX.m (ANDI/ORI/XORI/NOT/ANDF/ANDCF, XORR/ORR/XORC/ANDC/ORC), desplazamientos inmediatos y por registro (lógicos frente a aritméticos en 40 bits), ASR16/LSL16/LSR16, CLRL.
- Multiplicador: con/sin signo, ×2 con AM = 0, CLRP con los valores exactos, MUL/MULAC/MULMV/MULMVZ, MULX*, MULC*, MADD/MSUB, MADDX/MSUBX, MADDC/MSUBC, MULAXH, MOVP/MOVNP/MOVPZ, ADDPAXZ, ADDP/SUBP.
- Control: BLOOPI/LOOPI/BLOOP/LOOP (incluido contador 0), CALL/RET, Jcc, IFcc, CALLR/JMPR, RETcc/RTIcc no tomados, las 16 condiciones con 24 combinaciones de $sr, interrupción externa con RTI y enmascarada sin EIE.
- Direccionamiento: ILRR/ILRRD/ILRRI/ILRRN, LRR/LRRD/LRRI/LRRN, SRR/SRRD/SRRI/SRRN, LRS/SRS/SRSH con $config, LRIS, SBSET/SBCLR, ADDARN/SUBARN/DAR/IAR.
- Los 22 tipos de opcode extendido, cada uno con sus registros, memoria y punteros; los dos ejemplos del manual (0x6113 intercambia, 0x7660 hace OR).
- IFX: buzones en ambos sentidos con el bit M, DIRQ, DMA del DSP en ambos sentidos.
- 15 paradas claras con el motivo esperado (opcode desconocido, $st genérico, $wr, IROM/COEF sin volcado, COEF de solo lectura, direcciones no documentadas, acelerador, subdesbordamiento y desbordamiento de pila, LOOP sobre 2 palabras, longitud de DMA impar).
- Integración: la interfaz del DSP arranca un microcódigo con el valor del SDK 0x08AD, intercambia mensajes y el DSP termina con HALT; un fallo del DSP detiene `System::run` con `device_stop`.

Programa `dsp_suite` (clang, 750): copia un microcódigo a 0x01000000, escribe 0x08AD y luego 0 en el CSR, recibe el primer mensaje, envía uno y recibe la respuesta.

| Valor | Esperado | Obtenido |
|---|---|---|
| Primer mensaje (bits 30-16 / 15-0) | 0x5CD1 / 0x0000 | 0x5CD1 / 0x0000 |
| Respuesta a 0x92345678 (alto sin el bit 15, bajo + 1) | 0x1234 / 0x5679 | 0x1234 / 0x5679 |
| DSPINT tras DIRQ / HALT tras la instrucción HALT | 0x80 / 0x4 | 0x80 / 0x4 |

### MI, interfaz del DSP, ARAM y FST (M009)

Unitarias:
- MI: valores de los registros, máscara de 5 bits, causa que se limpia escribiendo 1, dirección de fallo de solo lectura, temporizadores (escritura permitida, lectura con parada), avisos de protección no aplicada (con el canal, el tipo y las páginas; como mucho 64).
- CSR del DSP: máscaras, bits que se limpian escribiendo 1 sin tocar los demás, bit 9 de solo lectura, PIINT con parada.
- Buzones: mensaje pendiente con el DSP detenido, parada con el DSP en marcha, mensaje del DSP consumido al leer la mitad baja, RES.
- DMA de ARAM en ambos sentidos con escrituras de 32 bits (como el driver de Linux): bit de ocupado, registros bloqueados, ARINT → PI, direcciones avanzadas y cuenta a 0 (como describe WiiBrew), bits bajos a 0, errores (longitud 0, fuera de la ARAM, fuera de la RAM, Wii sin ARAM) y el modelo de tiempo (4 unidades × 100 = 400 ciclos).
- FST: directorios anidados, búsqueda por ruta, rechazo de 9 tipos de tabla mal formada, offsets de nombre de 24 bits, lectura desde una imagen de disco.

Programa `aram_suite` (clang, 750): copia 1 KiB de la RAM a la ARAM y de vuelta a otro buffer esperando el bit 9 del CSR, y reconoce ARINT escribiendo solo ese bit.

| Valor | Esperado | Obtenido |
|---|---|---|
| Bit de ocupado justo después de iniciar / tras esperar | 1 / 0 | 1 / 0 |
| ARINT | 0x20 | 0x20 |
| Palabras distintas tras la ida y vuelta | 0 | 0 |
| ARADDR bajo tras el segundo DMA / cuenta | 0x400 / 0 | 0x400 / 0 |
| Causa DSP del PI tras el reconocimiento | 0 | 0 |

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

**MI, DSP/ARAM y FST (M009), 21 de 21 detectadas tras completar las pruebas:** M1 la causa no se limpia escribiendo 1 · M2 el tipo 2 no genera aviso · M3 máscara sin limitar a 5 bits · M4 la dirección de fallo se puede escribir · **D1 escribir el CSR borra todas las interrupciones y D2 ARINT ignora su máscara: no se detectaban al principio** → se añadió `dsp_acking_one_interrupt_keeps_the_others` · D3 leer el buzón no consume el mensaje · D4 se aceptan mensajes con el DSP en marcha · D5 dirección del DMA invertida · D6 MMADDR no avanza · D7 la cuenta no vuelve a 0 · D8 sin bit de ocupado · D9 ARADDR sin alinear · D10 tiempo por unidad de 16 bytes · D11 el bit de ocupado se puede escribir · D12 no se comprueba el final del DMA dentro de la ARAM · F1 cierre de directorios con error de uno · F2 un subdirectorio puede pasarse del final de su padre · **F3 solo se comprueba el inicio de los archivos y F4 offsets de nombre de 16 bits: no se detectaban al principio** → se añadieron un archivo que empieza dentro del disco y termina fuera, y `fst_name_offsets_use_24_bits` · F5 nombres sin terminar.

**Núcleo del DSP (M010), 32 de 32 detectadas tras completar las pruebas:** Q1 acarreo de la resta con > · Q2 sin desbordamiento · **Q3 OS deja de ser persistente: no se detectaba al principio** → se añadió `dsp_os_is_sticky_and_orc_keeps_flags` · Q4 TB sobre los bits 39/38 · Q5 AM invertido · Q6 siempre con signo · Q7 extensión de signo también en modo 16 bits · Q8 sin saturación · Q9 $acX.h sin extensión de signo · Q10 condición G sin Z · Q11 condición xA con AND · Q12 sin la regla OR · Q13 LS/SL intercambiados · Q14 N/M intercambiados · Q15 mitades de 'LDAX intercambiadas · Q16 el nibble 3 sin extensión · Q17 extensión de 8 bits en el nibble 3 · Q18 LOOP con una iteración menos · Q19 fin de BLOOP comparado con el PC siguiente · Q20 leer CMBL no borra M · Q21 escribir DMBL no activa M · Q22 bits de DSCR intercambiados · Q23 CLRL sin redondeo a par · Q24 CLRP a ceros · Q25 LSR aritmético · Q26 sin comprobar $wr · Q27 interrupción sin EIE · Q28 el bit 8 de $sr se puede escribir · Q29 campos de CMPAXH intercambiados · **Q30 ORC actualiza banderas: no se detectaba al principio** → misma prueba nueva · Q31 $prod sin m2 · Q32 RES ignora el bit 11. Tres mutaciones se reescribieron porque no compilaban con `-Werror` y tres porque no encontraban el texto; se reportan con su versión final.

**Acelerador (M011), 16 de 16 detectadas tras completar las pruebas:** A1 fin con >= · A2 sin recarga desde ACSA · A3 cabecera cada 8 nibbles · A4 cabecera de 1 nibble · A5 nibble bajo primero · A6 nibble sin signo · A7 sin el +1024 de redondeo · A8 sin saturación positiva · A9 historia actualizada en orden inverso · A10 índice de coeficientes desde la escala · A11 excepción equivocada · A12 escritura cruda sin exigir el bit 31 · A13 bytes de la palabra cruda invertidos · A14 excepciones sin IE · **A15 vector desplazado: no se detectaba al principio**, porque los vectores vacíos contenían ceros (NOP) y la ejecución resbalaba hasta el manejador correcto → ahora las pruebas llenan los vectores no usados con HALT · A16 PRED_SCALE mal extraído.

**Total: 158 mutaciones detectadas.**


### Mutaciones de M016 (17)

`/tmp/mut/run15.py` sobre `gles`: 16 detectadas, 1 equivalente (G11). **Acumulado: 356.**

### Mutaciones de M015 (25)

`/tmp/mut/run14.py` sobre `gpu_sw`, `efb_copy`, `gpu` y `av`: 23 detectadas, 2 que sobreviven (C1 equivalente, N3 no distinguible con los parámetros de 11 bits; ver la sección de M015). **Acumulado: 340.**

### Mutaciones de M014 (78)

`/tmp/mut/run12.py` (copias del EFB F1-F28, líneas/puntos L1-L10, z freeze Z1-Z3, precargas P1-P5) y `run13.py` (vértices y transformación V1-V15, rasterizador/TEV/mezcla/texturas R1-R20) sobre las suites `efb_copy`, `gpu_sw` y `gpu`.
- Primera pasada: 47 detectadas, 28 supervivientes y 4 que no compilaban con `-Werror` (se reescribieron sin variables sin usar y se reportan con su versión final).
- Supervivientes cubiertos con pruebas nuevas: F3, F6, F8, F9, F28 (copias), L1, L3, L4 (líneas), Z1 (z freeze fuera del scissor), P4 (banco impar), V5, V8, V10, V11, V12, V15 (transformación), R1, R2, R4, R6, R8, R10, R12, R13, R14, R15, R17, R18, R19 (raster, TEV, mezcla, LOD, dither, alpha test, z textura, konst, indirectas). Todas detectadas después.
- Código muerto eliminado por F19 y F20 (no se cuentan).
- Equivalentes: F15, P1, P5 (explicadas en la sección de M014).
- **Total: 75 detectadas, 3 equivalentes. Acumulado: 317.**

### Mutaciones de M013 (48)

`/tmp/mut/run11.py` sobre el lector de comandos, XF, BP, estado CP, GPU, Command Processor, Pixel Engine y texturas.
- 40 detectadas al principio. Supervivientes: P8 y K7 no compilaban (variable sin usar con `-Werror`) y K6 apuntaba al archivo equivocado (se rehicieron: detectadas); G1 (fin de dibujo solo mira el byte bajo), K2 (el puntero de lectura vuelve solo cuando es igual al final), E4 (la petición de interrupción del token se mantiene) y T3 (extremos CMPR iguales) → se añadió una prueba para cada una: detectadas.
- X1 (no recortar la carga XF al final) solo escribe fuera del array: la detecta la build ASan (stack-buffer-overflow), no la Release.
- B2 (máscara de 24 bits en la carga BP) es equivalente: la máscara nunca supera 24 bits.
- **Total: 47 detectadas, 1 equivalente. Acumulado: 242.**

### Mutaciones de M012 (39)

Cada mutación se aplicó al fuente, se recompiló y se ejecutó la suite correspondiente (`/tmp/mut/run9.py` y `run10.py`).

| Grupo | Mutaciones | Detectadas al principio | Tras añadir pruebas |
|---|---|---|---|
| Tarjeta de memoria (M1-M18) | 18 | 14 | 17 detectadas + 1 eliminada (M16: el campo `interrupt_set_` nunca se leía; se borró el código muerto) |
| DMA de audio y ARAM (A1-A8) | 8 | 7 | 7 detectadas + 1 equivalente (A8: quitar el aumento de generación al desactivar no cambia nada porque `audio_block()` ya sale sin reprogramarse; se borró la línea redundante) |
| Acelerador (C1-C13) | 13 | 11 | 13 detectadas |

Supervivientes iniciales y prueba añadida: M3 (programa sin datos), M4 (búfer de 128 bytes), M5 (primer byte 0x80 de la identificación), M10 (interruptor de interrupción apagado), C5 (escala de ganancia 2), C9 (bit 31 tras el desbordamiento de escritura). **Total acumulado: 195 mutaciones detectadas.**

## Errores encontrados durante M016

- Errores de compilación de shaders en llvmpipe: GLSL ES 3.20 no admite arrays como atributos de vértice (se separaron en 8 atributos; luego se eliminaron al pasar a pendientes por triángulo).
- Una escena de prueba en perspectiva no dibujaba nada: los parámetros de proyección dejaban z fuera de [−w, 0]. Se corrigieron (cerca 1, lejos 10, como GX).
- La escena de niebla con rango hacía que el software se detuviera (k = 0 en registros K sin escribir, factor NaN) y el GLES no: es una diferencia documentada; la prueba escribe los 5 registros K.
- Las diferencias de interpolación descritas arriba (corregidas cambiando el diseño).

## Errores encontrados durante M015

- **Error real en el código (encontrado por la mutación C8):** al reescribir las copias, el cambio que hacía que la fila actual también respetara clamp_top/clamp_bottom no se aplicó (el script que lo hacía falló a mitad y lo repetí sin esa parte). Lo detectó la mutación C8 al no encontrar el texto. Se corrigió y se añadió la prueba de las filas del bloque bajo el rectángulo.
- **Comportamientos de M014 que cambiaron por fuentes mejores** (no son errores de prueba, sino decisiones revisadas): copias de profundidad sin filtro, intensidad truncada, formatos de profundidad no válidos, lectura lineal fuera del EFB, gamma truncada. Las expectativas de M014 se actualizaron con los nuevos valores calculados a mano (listados arriba).
- Las pruebas de M014 que esperaban paradas en la niebla, las texturas RGBA8 en TMEM, los vértices 0xFF/0xFFFF, la escala del XFB y el bit de aspecto se cambiaron porque esas funciones ya existen.
- `orca_run` se detenía en el primer reporte del programa (el puerto de pruebas no existe fuera de las pruebas) antes de configurar la VI: se movió la configuración de la VI antes del primer reporte en `gpu_draw`.

## Errores encontrados durante M014

- No apareció ningún error en el código del emulador a partir de las pruebas.
- **Errores en mis expectativas, corregidos tras revisar la fuente:** R4 de profundidad (0x12 da 0x10 en el primer texel, porque el segundo texel es Z 0, no 0x11); el blanco del factor de mezcla (255·63 >> 8 = 62, no 63); una línea a z = 0,5 que esperaba descartada, cuando el plano +z de las líneas en Dolphin es −z + w (solo z > w se descarta; los triángulos usan otro plano); una copia a través de la GPU que salía negra porque los pesos del filtro de copia estaban en 0 (estado tras el reset) — se documentó como comportamiento y la prueba fija los pesos de GX; y un comentario del Y del XFB (27940 >> 8 = 109 sin bit de redondeo).
- **Pruebas que no probaban lo que decían** (las encontraron las mutaciones): el triángulo "fuera del scissor" estaba fuera del volumen de recorte (rechazo trivial, que no actualiza la pendiente) y además la pendiente ya era la esperada; el recorte por el segundo vértice daba el mismo resultado con la fórmula equivocada (se añadió una línea larga con el extremo justo fuera); q = 0 con valores que el clamp igualaba; dither con un valor que daba lo mismo con los índices cambiados.
- Las pruebas de M013 que esperaban una parada en líneas, copias del EFB, precargas y z freeze se cambiaron por paradas que siguen existiendo (fin de dibujo con valor 1, formato de píxel 5, precarga fuera de RAM), porque esas funciones ya están implementadas.

## Errores encontrados durante M013

- **Errores en mis pruebas (el código estaba bien):** opcode 0x9A tomado por triángulos (es tira de triángulos; triángulos con VAT 2 es 0x92); la máscara BP aplicada a sí misma mal calculada (0xFFFF00 con máscara 0xFF da 0, no 0xFF); índice de paleta 3 tomado por 2; un caso de "fin de dibujo" con byte bajo 2 puesto entre los que deben fallar.
- **Diseño corregido antes de terminar:** la primera versión del decodificador de imágenes llamaba al de texel suelto, así que la comparación entre los dos no demostraba nada; se reescribió como recorrido secuencial por teselas.
- **Incoherencia evitada:** el PI de Orca da la vuelta cuando la siguiente dirección llega al final; el CP, cuando el puntero es igual al final. Como en Dolphin, el CP manda mientras están enlazados y copia sus punteros al PI tras cada ráfaga.

## Errores encontrados durante M012

- **Error real en mi script (no llegó a ningún milestone):** al insertar la DMA de audio, el reemplazo borró los casos de lectura de AIDMA_CTRL y AIDMA_LEFT de `read16`. Las pruebas lo detectaron; se restauraron las lecturas y la escritura se movió a `write16`.
- **Advertencia solo en Debug:** `-Wconversion` en el operador ternario de la respuesta de identificación de la tarjeta (0x80 es `int`). Mi primera verificación no pasó `-DORCA_WERROR=ON` y la build terminó con 1 advertencia; se corrigió el código y se repitieron las tres configuraciones con `-Werror`: 0 advertencias.
- **Errores en mis pruebas:** una comprobación sin sentido del bloque siguiente al borrado (usaba un `CHECK_NE` que no existe y una expresión siempre cierta); se cambió por el valor calculado (0x10). Las pruebas de M009 que esperaban el DMA de audio rechazado y AR_SIZE de 16 bits se sustituyeron por pruebas del nuevo comportamiento (cambio intencional).

## Errores encontrados durante M011

- No apareció ningún error en el acelerador.
- **Errores en mis pruebas:** el programa principal se cargaba encima de los vectores de excepción (0x0006 y 0x000A) y la dirección de fin de un BLOOPI estaba mal contada (0x24 en lugar de 0x1F). Ahora las pruebas del acelerador ponen el código principal en 0x0060 detrás de un salto de reset, como los microcódigos reales.

## Errores encontrados durante M010

- No apareció ningún error en el núcleo del DSP durante las pruebas (todas las fallas fueron de las pruebas).
- **Errores en mis pruebas, corregidos antes de entregar:** siete codificaciones mal escritas a mano (ASR $ac1 escrito como LSR; DEC $ac1, INC $ac1 ×2 y MOVP $ac1 ×2 escritos como la versión de $ac0 más una extensión; CALLR con $ar1 en lugar de $ar0); una expectativa de CLRL que seguía la nota del manual en vez de su operación; esperar que DMBH conservara el bit 15 (el bit 15 es la bandera de mensaje pendiente); y pedir una interrupción antes de un reset que la borra.
- La prueba de buzones de M009 se reescribió porque ahora los buzones van al núcleo real en vez de detenerse con "DSP core not emulated".

## Errores encontrados durante M009

- No apareció ningún error en el código del emulador.
- **Error en mis pruebas:** esperé el valor 1 en el byte 3 de la ARAM, pero `src[1] = 1` ocupa los bytes 4-7 en big-endian, así que el 1 está en el byte 7. Se corrigió la expectativa.
- Cuatro mutaciones sobrevivieron al principio (D1, D2, F3, F4). Se añadieron las pruebas que faltaban y ahora las cuatro se detectan.

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
