# Orca Core M020 — integración incremental del JIT ARM64

## Base y preservación

M020 parte del código M019 derivado del ZIP M018 recuperado en esta conversación. Los ZIP M018/M019 originales se conservaron; los cambios se realizaron en una copia de trabajo y se empaquetan en un nuevo archivo.

## Implementado

- `Cpu::run_jit(u64)` como ruta de ejecución opcional. En AArch64 puede ejecutar bloques nativos; en otros hosts delega explícitamente al intérprete.
- Dispatcher incremental: el JIT solo entra cuando el CPU no está detenido, no está en trace/little-endian/POW y no hay una interrupción habilitada pendiente. Si la instrucción no puede traducirse, el intérprete ejecuta el siguiente paso.
- Soporte de fetch traducido mediante MMU/BAT/PTE en lugar de exigir identidad EA=PA. Los bloques se limitan a una página efectiva y física, y a instrucciones físicamente contiguas.
- Caché por PC efectivo, con verificación del PA y revalidación de la traducción de cada instrucción antes de entrar al bloque.
- Invalidación al escribir en páginas físicas marcadas como código, usando el callback existente de `mem::Bus`; `icbi` invalida igualmente la caché.
- Páginas nativas persistentes RW→RX (W^X), I-cache flush y liberación RAII. El bloque nativo permanece en caché en vez de ejecutar `mmap/munmap` en cada entrada.
- Fallback al intérprete cuando la generación, asignación ejecutable, traducción o ejecución JIT no puede continuar.
- Pruebas para equivalencia `run`/`run_jit`, invalidación por escritura, frontera de salto, BAT/MMU y límites del traductor.

## Semántica y restricciones

La cobertura nativa sigue siendo deliberadamente pequeña: `addi`, `addis`, `ori`, `oris`, `xori`, `xoris`, y `and/or/xor` con `Rc=0`. No se traducen aún ramas, loads/stores, CR/XER, FPU, excepciones ni instrucciones con efectos laterales.

`run_jit` todavía no se conecta al bucle de `System::run()`. Ese bucle avanza scheduler y timebase entre instrucciones; ejecutar un bloque completo sin salida por eventos/decrementer cambiaría la temporización. La integración correcta requiere puntos de salida precisos para scheduler, interrupciones y decrementer, no simplemente sustituir `step()` por un bloque de varias instrucciones.

La invalidación del Bus tiene un único handler global; el CPU lo instala y lo limpia al destruirse. Si posteriormente se necesitan otros consumidores simultáneos de invalidación, habrá que convertirlo en una lista de observadores o un registro multiplexado.

## Verificación ejecutada

- Host x86_64, GCC 14.2, C++20, Ninja, Release, `ORCA_WERROR=ON`, `ORCA_GLES=OFF`.
- Compilación completa aprobada.
- CTest: 19 pruebas; 18 aprobadas, 1 omitida (`encodings`, requiere `llvm-mc`).
- La suite `jit_arm64` en x86_64 prueba decodificación, dispatcher de fallback, igualdad contra intérprete, límites, BAT y flujo de invalidación a nivel de API. **No ejecuta el código ARM64 generado** porque la arquitectura anfitriona es x86_64.
- No se detectaron Android SDK, NDK, Gradle ni un ejecutor ARM64/QEMU en el entorno. No se compiló APK.

## Pendiente

- Ejecutar el test diferencial bajo ARM64 real o emulación AArch64 confiable.
- Integrar en `System::run()` con límites de bloque que respeten scheduler/timebase/decrementer e interrupciones.
- Añadir más instrucciones (branches, integer XO/OE/Rc, loads/stores) con comparación de estado y memoria.
- Mejorar caché/dispatch y medir ciclos por instrucción contra intérprete en un dispositivo real.
- Compilar APK Android ARM64 y verificarlo en el dispositivo objetivo.

No se declara compatibilidad con juegos ni mejora de rendimiento.
