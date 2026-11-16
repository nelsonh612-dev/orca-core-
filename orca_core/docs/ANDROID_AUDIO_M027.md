# Orca Core M027 — salida de audio Android

## Ruta implementada

- El DSP AI DMA entrega bloques de 32 bytes al sink de audio.
- Android convierte cada muestra PCM16 big-endian a PCM16 nativo sin alterar sus bits.
- Un ring buffer SPSC de tamaño fijo separa el hilo de emulación (productor) del hilo de AudioTrack (consumidor); no necesita bloquear el hilo de renderizado.
- El consumidor envía PCM estéreo a Android AudioTrack. Si cambia AIDFR, recrea la pista a 32 kHz o 48 kHz.
- Si el ring se vacía, se entregan ceros para evitar bloquear el render. Si se llena, se descartan las muestras nuevas y se contabilizan.
- Al cambiar de disco o pausar la actividad, el hilo de audio se detiene antes de destruir el sistema emulado.

## Límites y validación pendiente

- La ruta asume que los bloques AI DMA contienen PCM16 estéreo intercalado big-endian.
- No se genera audio si el juego no configura y activa AI DMA.
- No sustituye la emulación completa del DSP ni demuestra que el firmware/DSP de todos los juegos produzca muestras correctas.
- La compilación Android y la prueba auditiva en un teléfono físico son obligatorias antes de declarar M027 validado.
- Aún no hay mediciones de latencia, underruns, clipping ni sincronización audiovisual en hardware.
