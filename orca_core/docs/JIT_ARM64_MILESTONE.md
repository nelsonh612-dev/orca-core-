# Orca Core — primera cobertura del traductor JIT ARM64

## Punto de partida inspeccionado

M018 usa `CpuState` como estado arquitectónico: `gpr[32]`, `fpr[32]` (dos slots de 64 bits), `fpscr`, `pc`, `lr`, `ctr`, `cr`, `xer`, `msr`, registros de excepción/MMU, time base, decrementer y estado de reserva/cache. La memoria física está detrás de `mem::Bus`, con accesores big-endian, RAM/MMIO, observadores de escritura y seguimiento de páginas de código.

El intérprete está en `src/ppc/cpu.cpp` y `src/ppc/cpu_fpu.cpp`. `Cpu::step()` comprueba interrupciones, modo, alineación y MMU; obtiene una instrucción big-endian, la envía a `Cpu::execute()` y actualiza PC/retirement o entrega excepciones/host-stops. `Cpu::run()` repite `step()`. El intérprete no expone actualmente un API público para extraer un bloque traducible ni un dispatcher JIT.

## Cobertura del intérprete identificada

La cobertura es mucho mayor que la del JIT. `Cpu::execute()` contiene primarios 3, 7, 8, 10–18, 19, 20/21/23, 24–29, 31, 32–48, 50–57, 59–63, incluyendo subdecodificadores para integer XO/X, load/store, sistema, caché, FP, paired-single, MMU y periféricos. La cobertura real depende de los subdecodificadores y de sus restricciones; los opcodes primarios por sí solos no significan que cada forma esté implementada.

## JIT implementado en este cambio

`include/orca/ppc/jit/arm64.hpp` y `src/ppc/jit/arm64.cpp` introducen un emisor de instrucciones AArch64 nativas. El bloque opera directamente sobre el layout real de `CpuState` y termina con `RET`. Primera cobertura:

- `addi`, `addis`
- `ori`, `oris`, `xori`, `xoris`
- `and`, `or`, `xor` con `Rc=0`

El compilador produce el prefijo lineal máximo, deteniéndose ante instrucciones no admitidas (incluidos saltos), entradas desalineadas o cambio de página de código de 4 KiB. El PC arquitectónico se actualiza al final del bloque. En AArch64, `execute_arm64()` copia el código a memoria RW, limpia I-cache, cambia a RX, ejecuta y libera; fuera de AArch64 devuelve `false` sin alterar el estado. La ejecución mantiene una política W^X.

## Límites importantes

- **No está conectado aún a `Cpu::run()` ni al scheduler**: el caller debe proporcionar las palabras guest y decidir cuándo ejecutar el bloque. El motor de bloques/cache y el dispatcher siguen pendientes.
- No se emiten ramas, cargas/almacenamientos, instrucciones que actualizan CR/XER, excepciones, MMU ni operaciones con efectos laterales.
- El callback de invalidación de `mem::Bus`, `mark_code_page()` y `icbi` aún no están enlazados a una caché JIT; no existe caché de bloques que invalidar todavía.
- El código no cambia la semántica del intérprete ni sustituye su fallback.
- La prueba diferencial que ejecuta código generado está condicionada a `__aarch64__`; en esta máquina x86_64 solo se prueban decodificación, límites y estructura del bloque. No se afirma ejecución ARM64.

## Verificación de este cambio

Configuración host: CMake + Ninja, GCC 14.2, C++20, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.

Resultado: compilación host completa correcta. CTest: 19 pruebas totales, 18 aprobadas y 1 omitida (`encodings`, falta `llvm-mc`). El conjunto `jit_arm64` ejecuta pruebas de prefijo soportado, parada ante branch, límite de página y PC desalineado. En x86_64 `execute_arm64()` no ejecuta código generado, por lo que no hay prueba diferencial JIT real en este host.

Android SDK/NDK/Gradle no están disponibles; no se compiló APK. No hay prueba en ARM64 ni en dispositivo Redmi/Mali. No hay benchmarks de JIT y no se hacen afirmaciones de rendimiento o compatibilidad con juegos.


## M020 — dispatcher, caché e invalidación

El trabajo posterior está documentado en [`JIT_ARM64_M020.md`](JIT_ARM64_M020.md). Añade `Cpu::run_jit()`, caché de bloques con páginas nativas persistentes RW→RX, traducción de fetch MMU/BAT, revalidación de mapeos, invalidación por escrituras del bus e `icbi`, y fallback al intérprete. La ejecución nativa sigue pendiente de probar en ARM64; `System::run()` conserva `step()` para no alterar la temporización del scheduler/decrementer.


## M026 — integración temporizada del JIT ARM64

M026 conecta el dispatcher `Cpu::run_jit()` al bucle `System::run()` en compilaciones AArch64. El límite de cada lote se calcula hasta el siguiente evento del scheduler y hasta el final del presupuesto de ciclos. La CPU devuelve el número exacto de instrucciones completadas incluso cuando una instrucción no traducible obliga a volver al intérprete; el sistema avanza el scheduler por esos ciclos antes de continuar o informar la parada. También se pasa el `stop_pc` de `run_until()` al dispatcher para que una rama interpretada que llegue a la dirección de retorno no ejecute el stub.

En hosts no AArch64, `run_jit()` conserva la semántica del intérprete y reporta el número de instrucciones completadas; la ruta de `System::run()` sigue siendo el paso individual de referencia. El traductor nativo continúa limitado a operaciones enteras soportadas y bloques lineales: las ramas y las operaciones de memoria/FPU/paired-single no se traducen todavía.

**Verificación pendiente:** los tests host pueden comprobar la semántica de fallback y los límites de parada, pero no ejecutan bytes AArch64. El workflow M026 compila el núcleo con GCC/Clang y genera un APK ARM64; una compilación exitosa no sustituye una prueba diferencial en un dispositivo ARM64. No se afirman aún mejoras de FPS ni compatibilidad con juegos comerciales.


## M030–M035 — ampliación de enteros y validación CI

Los hitos M030–M032 amplían el subconjunto nativo sin cambiar el fallback arquitectónico:

- Lógicas inmediatas andi. / andis. con actualización de CR0.
- Formas X and, andc, nor, eqv, xor, orc, or y nand, incluidas las formas Rc=1.
- Rotación y máscaras rlwinm, rlwimi y rlwnm; las formas con Rc=1 actualizan CR0.
- Desplazamientos lógicos slw / srw, respetando el campo de seis bits del contador PPC.
- Desplazamientos aritméticos sraw / srawi con actualización de XER[CA] y CR0 cuando Rc=1 (M035).
- cntlzw, extsb y extsh.
- Comparaciones signed/unsigned cmpwi, cmplwi, cmpw y cmplw con actualización del campo CR seleccionado y XER[SO].

Se añadieron pruebas de traducción y pruebas diferenciales condicionadas a AArch64 que comparan el estado completo con el intérprete de referencia. En el workflow 44000274588, la ejecución de código ARM64 generado bajo QEMU terminó correctamente: 25 pruebas y 139 aserciones aprobadas, sin fallos. GCC y Clang también completaron sus suites; la compilación del APK Android ARM64 terminó correctamente y publicó el artefacto. Esto valida las rutas cubiertas por esos tests, no toda la ISA ni la ejecución en un dispositivo físico.

Se mantienen deliberadamente en fallback las formas que aún no tienen semántica completa en el emisor, entre ellas operaciones con overflow habilitado, saltos, accesos de memoria, FPU y paired-single.

### Estado real después de M035 y del CI 44000274588

| Área | Estado |
|---|---|
| Emisión de código ARM64 y política W^X | Implementada |
| Dispatcher, caché, invalidación y fallback | Integrados; validación de plataforma pendiente |
| Integración con el scheduler | Integrada en M026; validar temporización en hardware |
| Aritmética y lógica entera | Parcial, ampliada en M030 |
| Rotaciones, máscaras y desplazamientos lógicos | Ampliados en M031/M032 |
| Desplazamientos aritméticos y XER[CA] | sraw/srawi añadidos en M035; validar diferencialmente en ARM64 |
| Comparaciones signed/unsigned | cmpwi, cmplwi, cmpw y cmplw añadidas en M034 |
| CR0, campos CR y XER | Parcial; comparaciones y CR0 cubiertos en las formas indicadas, otras formas de flags pendientes |
| Saltos y flujo de control nativo | Pendiente |
| Load/store, MMU y excepciones desde bloques nativos | Pendiente; fallback actual |
| FPU y paired-single | Pendiente; fallback actual |
| Pruebas diferenciales ejecutadas en ARM64/QEMU | 25 pruebas, 139 aserciones; 0 fallos en CI 44000274588 |
| APK Android ARM64 | Compilado y publicado como artefacto CI; no probado en dispositivo físico |\n| Benchmarks frente al intérprete | Pendiente |

No se declara completo el JIT ni se atribuyen mejoras de rendimiento. El resultado QEMU cubre las instrucciones incluidas en los tests diferenciales; no acredita cobertura total, rendimiento real ni compatibilidad con juegos. El APK debe probarse en hardware ARM64 y las familias pendientes deben implementarse y validarse antes de cerrar el proyecto.


## M036 — salto inmediato incondicional nativo

Se añade traducción de opcode 18 (b / bl) como terminador de bloque. El destino relativo o absoluto se calcula con desplazamiento PowerPC de 26 bits con signo; la forma con LK guarda la dirección de retorno en LR. El bloque escribe el PC de destino y termina inmediatamente, de modo que el dispatcher vuelve a buscar/traducir desde el destino. Se mantienen en fallback bc, bclr y bcctr: requieren evaluación precisa de BO/BI, decremento de CTR, captura de LR/CTR y condiciones CR.

Se añadieron pruebas de traducción y una prueba diferencial AArch64 para b+LK frente al intérprete. El CI de este nuevo commit debe completar antes de considerar validado M036.


## M037 — salto condicional inmediato (bc)

Se añade la forma opcode 16 (bc) como terminador de bloque. El código nativo evalúa BO/BI, decrementa CTR cuando BO lo solicita, consulta el bit CR seleccionado y elige entre destino y fall-through; LK actualiza LR incluso cuando la condición no toma el salto, igual que el intérprete de referencia. Las formas indirectas bclr/bcctr siguen en fallback.

Se incorporó una prueba diferencial para una condición CR tomada y una prueba estructural de destino absoluto. La validación de estos últimos commits está pendiente del workflow CI más reciente; no se debe extrapolar el resultado anterior de M035 a M036/M037.


## M038 — operaciones lógicas del registro CR

Se añaden las operaciones CR lógicas crand, crandc, cror, crorc, crxor, crnand, crnor y creqv. La emisión respeta la numeración PowerPC de bits CR (BT/BA/BB, de MSB a LSB) y actualiza únicamente el bit destino. Se añaden tests estructurales y una comparación diferencial de CpuState completo contra el intérprete. El workflow que incluya M038 debe aprobar antes de considerar validado este hito.


## M039 — transferencias del registro CR

Se traducen mcrf, mfcr y mtcrf. mcrf copia solo el nibble de campo indicado; mfcr transfiere el CR completo a un GPR; mtcrf aplica la máscara FXM sin modificar los campos no seleccionados. Se añadieron pruebas estructurales y diferenciales para comparar el estado completo con el intérprete.

## M040 — saltos indirectos bclr/bcctr

Se añaden terminadores de bloque nativos para bclr y bcctr. El destino LR/CTR se captura antes de modificar LR/CTR, se alinea a palabra, se aplican BO/BI, decremento de CTR cuando corresponde, y LK actualiza LR después de capturar el destino. bcctr con BO[2]=0 permanece en fallback para que el intérprete entregue InvalidForm sin efectos parciales. El CI de estos últimos cambios debe aprobar antes de considerar validados M039/M040.


## M041 — mcrxr

Se añade mcrxr como operación nativa: copia XER[SO|OV|CA] al nibble CR indicado y limpia esos bits del XER, preservando los 28 bits inferiores. Su decodificación utiliza el XO completo de 10 bits (512), no el XO de 9 bits empleado por las instrucciones XO/OE. Incluye prueba diferencial de CpuState completo; CI pendiente para este commit.

## M042 — acarreo entero extendido

Se añaden formas nativas addze, addme, subfze, subfme y neg. Las cuatro primeras consumen y actualizan XER[CA]; las formas Rc actualizan CR0 con el SO adhesivo. Se incorporaron pruebas diferenciales contra el intérprete para ambos valores iniciales de CA. Workflow 44005781291: GCC, Clang, QEMU ARM64 y APK Android completados correctamente.

## M043 — overflow habilitado y XER[OV]/XER[SO]

Se amplía la aritmética XO para OE=1 en add, addc, adde, addze, addme, subf, subfc, subfe, subfze, subfme y neg. El emisor obtiene el overflow signed de NZCV[V], reemplaza XER[OV] y hace sticky XER[SO]. Se conserva XER[CA] para las formas con carry y Rc actualiza CR0 usando el SO resultante. Se añadieron pruebas diferenciales para los once casos y distintos estados iniciales de XER. CI del commit más reciente pendiente.

## M044 — operaciones unarias bit-exact de FPR

Se añaden fmr, fneg, fnabs y fabs como operaciones enteras nativas sobre el patrón binario de FPR[ps0]. No se altera ps1 ni FPSCR; Rc copia FPSCR[0:3] a CR1. Los bloques marcan requires_fp y Cpu::run_jit conserva el camino de excepción FloatingPointUnavailable cuando MSR[FP]=0. Se añaden pruebas diferenciales con NaN/patrones de signo y una prueba del límite MSR[FP] en AArch64/QEMU. CI pendiente.

## Áreas que continúan sin implementación nativa

- Load/store enteros y FP/paired-single todavía terminan el bloque y se ejecutan mediante el intérprete. La ruta del intérprete sí aplica traducción MMU/BAT/segmentos, endianess big-endian, accesos Bus/MMIO, reservas, alineación y excepciones DSI/ISI; todavía no existe un ABI de contexto JIT seguro que permita emitir accesos nativos sin saltarse esas reglas.
- Aritmética FPU y paired-single, FPSCR completo, operaciones cuantizadas y sus excepciones siguen en fallback.
- No está completa toda la ISA CR/XER ni las formas de multiplicación/división con OE; las formas no emitidas permanecen en fallback.
- Las comprobaciones de interrupción y el scheduler siguen limitando los lotes del dispatcher, pero falta validación temporal en hardware bajo carga real.
- La invalidación por escrituras del Bus e icbi está conectada a la caché de bloques; se necesita más estrés diferencial con DMA, alias de memoria y cambios de BAT/PTE.
- QEMU ARM64 y el APK CI no equivalen a pruebas en un dispositivo físico. No se ha ejecutado una sesión física en el dispositivo del usuario ni se han obtenido benchmarks comparativos fiables.

## M045 — mullw y overflow firmado

Se añade la multiplicación signed de 32 bits usando SMULL AArch64. El resultado arquitectónico conserva los 32 bits inferiores; con OE=1, se compara el producto completo de 64 bits con la extensión de signo del resultado para calcular overflow exacto y actualizar XER[OV]/XER[SO]. Rc actualiza CR0. Pruebas diferenciales cubren overflow positivo, producto negativo dentro de rango y el caso INT32_MIN × -1. CI pendiente.

## M046 — paired-single bitwise y merge

Se añaden ps_neg, ps_mr, ps_nabs, ps_abs y ps_merge00/01/10/11 como operaciones nativas sobre ambos slots FPR. No se alteran FPSCR ni los bits de datos fuera de los slots destino; Rc copia FPSCR[0:3] a CR1. Los bloques registran requires_paired y el dispatcher verifica MSR[FP] y HID2[PSE] antes de entrar; si falta la habilitación, se vuelve al intérprete para conservar la excepción programática correcta. Se agregaron pruebas diferenciales y de excepción pendiente de CI.

## M047 — D-form integer load/store guarded fast path

Se añaden lwz/lwzu, lbz/lbzu, lhz/lhzu, lha/lhau, stw/stwu, stb/stbu y sth/sthu sobre RAM física contigua. El código ARM64 calcula EA y realiza la carga/store; un resolver de runtime usa MMU/BAT y Bus para entregar un puntero sólo si el acceso es RAM ordinaria, de una sola página, sin locked cache y con WIMG soportado. El emisor corrige el endianess big-endian con REV/REV16 y hace sign extension para LHA. Si no es seguro (MMIO, traducción especial/fallida, page crossing, no RAM, locked cache), el bloque sale sin cambiar PC y el dispatcher reintenta exactamente la instrucción con el intérprete. Stores notifican al Bus después de completar para invalidar código y avisar a observadores. Workflow 44008955212: GCC, Clang, QEMU ARM64 y APK Android aprobados; QEMU 52 tests / 469 checks / 0 failures.

## M048 — indexed integer load/store y byte-reverse

Se añaden lwzx/lwzux, lbzx/lbzux, lhzx/lhzux, lhax/lhaux, stwx/stwux, stbx/stbux, sthx/sthux, más lwbrx/lhbrx/stwbrx/sthbrx. Se reutiliza el mismo resolver guarded Bus/MMU; los reverse forms omiten REV para conservar la inversión de bytes definida por PowerPC. Pruebas cubren indexed/update, loads sign-extended, byte-reverse, write invalidation, unmapped y page-crossing fallback. CI pendiente.

## M048 — indexed/update/reverse-endian memory path

CI completo aprobado en workflow 44009281338: GCC, Clang, QEMU ARM64 y APK Android. QEMU: 55 tests / 498 checks / 0 failures. Incluye lwzx/lwzux, lbzx/lbzux, lhzx/lhzux, lhax/lhaux, stwx/stwux, stbx/stbux, sthx/sthux y formas byte-reverse, además de tests de update y de invalidación de código tras un store JIT.

## M049 — fsel y ps_sel

Se emite selección bit-exact sin alterar FPSCR: la condición detecta signo, ±0 y NaN en el patrón IEEE-754, eligiendo C cuando A >= 0 y B en caso contrario. fsel afecta ps0; ps_sel procesa ambas lanes. Se respeta Rc→CR1 y los requisitos MSR[FP]/HID2[PSE]. Pruebas diferenciales para finitos positivos/negativos, -0, NaN y selección paired-single. Workflow 44009754403 aprobado: GCC, Clang, QEMU y APK; QEMU 58 tests / 509 checks / 0 failures.

## M050 — lmw/stmw

Se añaden lmw/stmw como accesos de bloque nativos sobre RAM contigua ordinaria, con traducción MMU/Bus y conversión big-endian por palabra. El resolver acepta spans hasta 128 bytes; si cruza página, no es RAM, la traducción falla o hay caché bloqueada/WIMG especial, se usa el intérprete. Prueba diferencial carga r28–r31 y los almacena en otra dirección. Workflow 44009852639 aprobado: GCC, Clang, QEMU y APK; QEMU 59 tests / 525 checks / 0 failures.

## M051 — harness comparativo para Android ARM64

Se amplía tools/bench_interpreter.cpp para aceptar modo interpreter, jit o compare. El modo compare ejecuta el mismo loop guest en dos CPUs/Buses independientes, imprime instrucciones retiradas, tiempo, MIPS y speedup, e incluye el coste de compilación del primer bloque. El loop entero cubre addi/lwz/stw/rlwinm/bdnz; el loop FP actual se etiqueta explícitamente como fallback porque sus operaciones aritméticas aún no están traducidas. El smoke test de QEMU en workflow 44010866517 confirmó estado final idéntico: intérprete y JIT retiraron 50 000 instrucciones y terminaron con r3=10 000; el JIT reportó 20 000 RAM fast-path hits. La cifra 0.367x observada bajo QEMU no representa rendimiento del dispositivo y no se usa como speedup real. Este harness permite medir en el dispositivo real, pero todavía no se ha ejecutado en un Android físico. El runner ARM64 standalone se configura con ORCA_GLES=ON; el paso aún no se ha validado porque los runs posteriores terminaron sin asignar runners de GitHub Actions.

## M052 — memoria FPU escalar

Se extiende el fast path guarded de RAM para lfs/lfsu, lfd/lfdu, stfs/stfsu y stfd/stfdu, incluidas las variantes indexadas lfsx/lfsux, lfdx/lfdux, stfsx/stfsux y stfdx/stfdux. El acceso a memoria y el endianess se ejecutan en ARM64; las conversiones single/double usan las rutinas bit-exactas fpu::load_single/store_single para preservar NaN signaling, subnormales y la semántica de truncamiento documentada del emulador. lfs actualiza ambos slots FPR como el intérprete. Las direcciones no seguras se derivan al intérprete por el resolver. Se añadieron pruebas de traducción de las 16 formas, pruebas diferenciales para load/store single y una comparación diferencial individual de las ocho variantes indexadas, verificando estado CPU, actualización de RA, fast path y contenido de memoria frente al intérprete. También se cubren los fallbacks FPR indexados para cargas/almacenamientos que cruzan una página y para una dirección no mapeada, comparando estado arquitectónico y memoria con el intérprete y verificando que no se use el fast path. Todas estas pruebas están añadidas, pero aún no han sido ejecutadas en un runner ARM64 de CI.


## M053 — stfiwx guarded native store

Se añade la forma indexada stfiwx al fast path FPR: el resolver traduce y valida una escritura RAM de 4 bytes; ARM64 toma literalmente los 32 bits bajos de FPR[ps0], corrige el endianess PowerPC y almacena sin convertir el valor flotante. MMIO, páginas cruzadas y mapeos especiales siguen usando el intérprete. Se amplía la prueba de traducción de las formas FPR y se añade comparación diferencial contra el intérprete, incluyendo el patrón 0xE5F60718. La compilación y ejecución aún requieren un runner ARM64 disponible. En la revisión posterior se corrigió el desplazamiento de lectura del slot en memoria host: el word bajo de `ps0` reside en el offset 0 de su `u64` en ARM64 little-endian; la prueba diferencial usa un patrón de 64 bits con mitades distintas para detectar esta clase de error.
