# Lectura de discos GameCube y Wii en Orca Core

Este documento describe cómo Orca Core lee, verifica, descifra y arranca imágenes de disco de GameCube y Wii, qué hace igual que Dolphin, en qué se aparta, y cómo probar imágenes reales. Dolphin (`Source/Core/DiscIO`, `Common/Crypto`, `Core/IOS/ES`, `Core/IOS/IOSC`, `Core/HW/WII_IPC`) se consultó solo como información: el código de Orca es propio.

## Capas

| Capa | Código | Qué hace |
|---|---|---|
| Lectura física | `disc/blob.*`, `disc/disc_image.*` | Abre el archivo de imagen (ruta o descriptor de Android) y detecta su formato por el contenido: ISO/GCM, NKit (compatible con ISO), CISO, WBFS, GCZ. Rechaza con el motivo WIA, RVZ, TGC y NFS. |
| Lectura lógica de partición | `disc/wii_disc.*` (`WiiPartition`) | Tabla de particiones (4 grupos), ticket, TMD, cadena de certificados, tabla H3, clústeres de 0x8000 bytes. |
| Descifrado | `crypto/aes.*` | AES-128-CBC: clave de título con el IV del ID de título; área de hashes con IV cero; datos con el IV de los bytes cifrados 0x3D0-0x3DF. |
| Verificación | `crypto/sha1.*`, `crypto/rsa.*`, `disc/wii_meta.*` | H0-H3 de cada clúster antes de entregar un solo byte; H3 contra el TMD; firmas RSA de ticket, TMD y certificados. |
| Sistema de archivos | `disc/fst.*` (`FileSystem`) | FST de GameCube y de Wii (offsets ×4), directorios, lecturas acotadas, DOL principal, apploader. |
| Entrega al juego | `hw/disc_interface.*` (GameCube), `ios/di_device.*` (Wii) | GameCube: comandos de unidad por DMA con latencia e interrupciones. Wii: IOS `/dev/di` lee la partición ya descifrada y verificada; nunca entrega datos cifrados al juego. |
| Arranque | `system/system.cpp` | `boot_disc()` arranca GameCube y, en un sistema Wii, deriva a `boot_wii_disc()`: abre la partición de juego, prepara ES (título, uid/gid), la memoria baja de IOS y del menú, ejecuta el apploader y entra en el DOL. |

## Formatos de imagen

- **ISO/GCM** y **NKit** (tratado como ISO).
- **CISO**: mapa de bloques; los bloques no guardados se leen como cero. El tamaño es una cota superior, como en Dolphin.
- **WBFS**: un disco por archivo; no admite archivos partidos (`.wbf1`). Los bloques no guardados se leen como cero.
- **GCZ**: zlib (Linux y NDK de Android). Se comprueba el Adler-32 de cada bloque; un bloque corrupto es un error, no un aviso como en Dolphin.
- **WIA, RVZ, TGC, NFS**: se rechazan nombrando el formato. RVZ y WIA necesitan Zstandard, LZMA o bzip2, que no están en este entorno; hay que convertirlos a ISO con Dolphin.

## Claves

Dolphin lleva las claves comunes de Nintendo (y la clave pública raíz) dentro de su código fuente, en `IOSC.cpp`, y descifra la clave de título del ticket con ellas. **Orca no incluye ninguna clave.** El mecanismo equivalente:

- **Discos sin cifrado** (encabezado 0x61 ≠ 0): no necesitan clave.
- **Particiones cifradas**: necesitan la clave común que indica el ticket, extraída por el usuario de su propia consola:
  - índice 0: normal;
  - índice 1: coreana;
  - índice 2: vWii;
  - clave de desarrollo: para tickets emitidos por `Root-CA00000002-XS00000006`, como en Dolphin.
- **Formatos de archivo de claves** (`--wii-keys=` en las herramientas; el selector de la app):
  - 16 bytes: una clave (índice 0 por defecto);
  - `keys.bin` de BootMii (0x400 bytes, clave en 0x114);
  - texto, una clave por línea: `common_key = <32 hex>`, `korean_common_key = …`, `vwii_common_key = …`, `dev_common_key = …`, y opcionalmente `root_key_file = <ruta>` (clave pública raíz: módulo de 512 bytes, o módulo + exponente, o un certificado).
- **Errores**:
  - si falta la clave, el error nombra cuál;
  - si la clave es incorrecta, se detecta al abrir la partición, porque el clúster 0 no pasa H0 tras descifrarlo;
  - un índice de clave mayor que 2 se rechaza (Dolphin usa la clave 0 con una alerta).
- **Memoria**: la clave de título, las tablas de rondas AES, la caché de clústeres y los búferes de los archivos de claves se borran con `secure_zero`.

## Verificación de integridad

- **Por clúster**, antes de copiar ningún dato:
  - H0: 31 SHA-1 de los fragmentos de 0x400 bytes;
  - H1: SHA-1 de la tabla H0, en la entrada `clúster % 8`;
  - H2: SHA-1 de la tabla H1, en la entrada `(clúster / 8) % 8`;
  - H3: SHA-1 de la tabla H2, en la entrada `clúster / 64` de la tabla H3 de 0x18000 bytes.

  Mismas constantes que `VolumeWii::CheckBlockIntegrity` de Dolphin.
- **Al abrir**: SHA-1 de la tabla H3 contra el hash del contenido 0 del TMD. También se comprueba que el ID de título del TMD coincida con el del ticket.
- **Caché**: LRU de 8 clústeres. Una lectura repetida no vuelve a descifrar; un clúster que falla la verificación nunca entra en la caché.
- **Firmas** (`verify_chain`), PKCS#1 v1.5 con SHA-1:
  - ticket con la clave del certificado XS, TMD con la del CP, XS y CP con la del CA, y el CA con la clave raíz solo si el usuario la aporta;
  - una firma toda a cero se informa como *fakesigned*.

  Como en Dolphin, las firmas no impiden leer el disco: se informan (`orca_disc info/verify`).
- **Discos sin hashes** (0x60 ≠ 0): datos contiguos, como en Dolphin. Si el disco está cifrado pero no tiene hashes, se rechaza con el motivo.

## Validaciones de estructura

- **Ticket**:
  - exactamente 0x2A4 bytes, firma RSA-2048, emisor `Root-…`, versión 0;
  - índice de clave de 0 a 2.
- **TMD**:
  - tamaño entre 0x1E4 y 0x49E4 y exactamente 0x1E4 + 0x24·n;
  - n ≥ 1, firma RSA-2048.
- **Cadena de certificados**: solo estructuras completas y conocidas (0x400, 0x300, 0x240, 0x180 bytes, como el `CertReader` de Dolphin); se rechaza basura al final o certificados truncados.
- **Offsets**:
  - todos los campos «>> 2» se convierten a 64 bits y cada rango se comprueba contra el tamaño de la imagen sin desbordamiento;
  - los datos de una partición pueden terminar más allá del final de una imagen recortada (como en Dolphin), pero leer un clúster ausente es un error que lo nombra.

## Lecturas fallidas

- **Fuera del disco o de la partición**: es asunto del juego. En GameCube, DEINT con «block out of bounds»; en Wii, `/dev/di` responde *drive error* (2).
- **Dentro del disco, pero la imagen no puede entregar los datos** (bloque GCZ corrupto, clúster que no pasa H0-H3, archivo truncado): **la emulación se detiene** con el motivo concreto. Nunca se entregan datos inventados.

## Comunicación con IOS (Wii)

- Las peticiones del PowerPC siguen la secuencia de registros de libogc: X1 para enviar, Y2 de acuse, Y1 de respuesta y luego X2.
- El kit oficial escribe X2 («respuesta recogida») tras cada respuesta, y 0x38 al iniciar. **Hasta este trabajo, Orca se detenía al ver X2**, así que cualquier juego Wii se habría parado en su primera respuesta de IOS. Ahora X2 se guarda sin efecto, como en Dolphin.
- Lo comprueba `wii_dvd_suite`: un programa PowerPC que, tras arrancar desde la partición cifrada, abre `/dev/di` y lee el disco durante la ejecución.

## Herramientas

```sh
orca_disc [--wii-keys=FILE] info   juego.wbfs   # formato, particiones, título, IOS, índice de clave, firmas
orca_disc [--wii-keys=FILE] verify juego.iso    # toda la imagen: bloques GCZ, cada clúster Wii (H0-H3), FST, DOL, apploader
orca_disc [--wii-keys=FILE] ls     juego.iso
orca_disc [--wii-keys=FILE] extract juego.iso /ruta/en/el/disco salida.bin
orca_run  [--wii-keys=FILE] [--nand=DIR --create-nand=usa] juego.(iso|gcm|ciso|wbfs|gcz) [ciclos]   # la consola la decide el disco
```

## Pruebas con imágenes reales

En este entorno no hay ninguna imagen real de juego, así que **no hay resultados de compatibilidad con juegos comerciales**. La ruta está preparada:

```sh
ORCA_GC_IMAGE=/ruta/juego_gc.iso \
ORCA_WII_IMAGE=/ruta/juego_wii.wbfs ORCA_WII_KEYS=/ruta/keys.bin \
ORCA_REAL_FRAMES=600 ORCA_REAL_FULL_VERIFY=1 \
ctest --test-dir build -R real_images --output-on-failure
```

- **Archivos que hacen falta**, volcados de discos propios:
  - un disco de GameCube (ISO/GCM, CISO o GCZ);
  - un disco Wii con su partición de juego cifrada y con hashes (ISO o WBFS);
  - el `keys.bin` de BootMii de la propia consola (o un archivo de 16 bytes o de texto con la clave común).
- **Qué comprueba la prueba**:
  - abre la imagen, lee la tabla de particiones e informa de las firmas;
  - con `ORCA_REAL_FULL_VERIFY`, verifica todos los clústeres;
  - lee el FST, el DOL principal y el apploader;
  - arranca por el mismo camino que la app de Android y ejecuta los campos de vídeo pedidos. Cualquier parada es un fallo e imprime su motivo.
- **Sin imágenes**, ctest la marca como *Skipped*, nunca como superada.

## Límites

- **Formatos**: WIA/RVZ, TGC, NFS y WBFS partido no se leen.
- **Clave raíz**: las firmas de los certificados CA solo se comprueban si el usuario aporta la clave raíz.
- **Retrocompatibilidad**: un disco de GameCube en un sistema Wii (MIOS) no está emulado; `boot_disc` lo rechaza con el motivo.
- **Pruebas reales**: no hay pruebas con juegos reales ni en un teléfono.
