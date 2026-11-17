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


## M030–M032 — ampliación de enteros y validación pendiente

Los hitos M030–M032 amplían el subconjunto nativo sin cambiar el fallback arquitectónico:

- Lógicas inmediatas andi. / andis. con actualización de CR0.
- Formas X and, andc, nor, eqv, xor, orc, or y nand, incluidas las formas Rc=1.
- Rotación y máscaras rlwinm, rlwimi y rlwnm; las formas con Rc=1 actualizan CR0.
- Desplazamientos lógicos slw / srw, respetando el campo de seis bits del contador PPC.
- cntlzw, extsb y extsh.

Se añadieron pruebas de traducción para estas formas y pruebas diferenciales condicionadas a AArch64 que comparan el estado completo con el intérprete de referencia. Estas pruebas nuevas aún necesitan ejecutarse en CI/QEMU ARM64; la presencia de los tests no equivale a un resultado aprobado.

Se mantienen deliberadamente en fallback las formas que aún no tienen semántica completa en el emisor, entre ellas sraw / srawi (XER[CA]), operaciones con overflow habilitado, saltos, accesos de memoria, FPU y paired-single.

### Estado real después de M032

| Área | Estado |
|---|---|
| Emisión de código ARM64 y política W^X | Implementada |
| Dispatcher, caché, invalidación y fallback | Integrados; validación de plataforma pendiente |
| Integración con el scheduler | Integrada en M026; validar temporización en hardware |
| Aritmética y lógica entera | Parcial, ampliada en M030 |
| Rotaciones, máscaras y desplazamientos lógicos | Ampliados en M031/M032 |
| CR0 y XER | Parcial; no se traducen todas las formas que afectan flags |
| Saltos y flujo de control nativo | Pendiente |
| Load/store, MMU y excepciones desde bloques nativos | Pendiente; fallback actual |
| FPU y paired-single | Pendiente; fallback actual |
| Pruebas diferenciales ejecutadas en ARM64 | Pendiente de resultado CI/dispositivo |
| Benchmarks frente al intérprete | Pendiente |

No se declara completo el JIT ni se atribuyen mejoras de rendimiento hasta que las rutas nativas y sus efectos laterales superen las pruebas diferenciales y de rendimiento en ARM64.
