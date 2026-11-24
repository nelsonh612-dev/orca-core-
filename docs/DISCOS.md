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

## Claves: qué hace Dolphin y qué hace Orca

Recorrido de Dolphin, verificado en su código:

1. `VolumeWii::VolumeWii` lee las marcas de 0x60/0x61 y los cuatro grupos de particiones de 0x40000 (offsets × 4).
2. Para cada partición lee el ticket (`TicketReader`, 0x2A4 bytes), el TMD, la cadena de certificados y la tabla H3.
3. Crea la clave AES de la partición con `TicketReader::GetTitleKey()`:
   - toma el IV de los 8 bytes del ID de título en 0x1DC más 8 ceros;
   - toma la clave cifrada de 16 bytes en 0x1BF y el índice de clave común en 0x1F1;
   - descifra con `IOSC::Decrypt(COMMON_KEY_HANDLES[índice])`.
4. `VolumeWii::Read` descifra cada bloque de 0x8000 bytes con esa clave: `DecryptBlockData` usa como IV los bytes cifrados 0x3D0-0x3DF y descifra 0x7C00 bytes desde 0x400. Guarda el último bloque descifrado.

**La diferencia que importa:** `COMMON_KEY_HANDLES` apunta a claves cuyos bytes están escritos dentro de `Core/IOS/IOSC.cpp` (la normal y la coreana, más la clave pública raíz). Por eso Dolphin abre discos Wii sin pedir ningún archivo: **Dolphin no usa `keys.bin` para leer discos.**

Orca sigue el mismo procedimiento (mismos offsets, mismo IV, misma estructura de bloque), pero no incluye las claves de Nintendo, que son material secreto. Las aporta el usuario desde su propia consola. Este es el único paso en el que Orca se aparta de Dolphin, y es intencionado.

- **Administrador de claves** (`WiiKeys`): guarda solo las claves comunes de la sesión y las borra al destruirse.
  - Ranuras: retail 0 (normal), 1 (coreana), 2 (vWii), la de desarrollo, y la clave pública raíz opcional.
  - Las claves de título no están ahí: cada `WiiPartition` deriva la suya de su propio ticket, la guarda y la borra al cerrarse. Una partición nunca usa la clave de otra.
  - El lector pide la clave con `decrypt_title_key(ticket, keys)`, que elige la ranura según el índice y el emisor del ticket. Los tickets de `Root-CA00000002-XS00000006` usan la clave de desarrollo, como `TicketReader::GetConsoleType` de Dolphin.
- **Errores**:
  - si la clave falta, el error dice cuál;
  - si es incorrecta, se detecta al abrir la partición, porque el bloque 0 no pasa H0;
  - el índice mayor que 2 se rechaza. Dolphin usa la clave 0 tras una alerta. Orca admite el índice 2 (vWii) si el usuario aporta esa clave; Dolphin no tiene ranura para él.
- **Archivos de claves** (`--wii-keys=` en las herramientas; el selector de la app):
  - 16 bytes exactos: una clave (índice 0 por defecto).
  - `keys.bin` de BootMii: 0x400 bytes, encabezado de texto «BackupMii v1» de 0x100 bytes y luego la OTP de la consola. Solo se lee la clave común (OTP 0x14 = archivo 0x114). Las claves privadas de la consola que también contiene no se usan ni se guardan. Un archivo de 1 KiB sin ese encabezado se rechaza: **no es el mismo formato que usa Dolphin internamente**.
  - Texto, una clave por línea: `common_key = <32 hex>`, `korean_common_key = …`, `vwii_common_key = …`, `dev_common_key = …`, y opcionalmente `root_key_file = <ruta>` (módulo de 512 bytes, o módulo + exponente, o un certificado).
- **Discos sin cifrado** (encabezado 0x61 ≠ 0): no necesitan ninguna clave.

### OrcaIOSC (`ios/iosc`): el equivalente del IOSC de Dolphin

| Dolphin (`Core/IOS/IOSC`) | Orca (`ios::Iosc`) |
|---|---|
| `DefaultHandle` 0-11 y `HANDLE_ROOT_KEY` | Los mismos números (`HANDLE_COMMON_KEY` = 4, `HANDLE_NEW_COMMON_KEY` = 11, …), más `kOrcaVwiiCommonKey` (0x100) para el índice 2 |
| `KeyEntry` (tipo, subtipo, datos, `misc_data`, máscara de propietarios) | Igual, más `provided` |
| `LoadDefaultEntries()` (las claves comunes, la clave SD y la clave raíz con los valores de Nintendo escritos en el código) | Registra las mismas entradas, en el mismo orden, con los mismos tipos y propietarios. Las que llevan valores de Nintendo quedan sin valor («not provided») hasta que se provisionan |
| `LoadEntries()` (`keys.bin`: solo las claves propias de la consola, nunca la común) | No se carga: el lector de discos no usa esas claves |
| `ConsoleType` Retail / RVT (`TicketReader::GetConsoleType`) | Igual: el emisor `Root-CA00000002-XS00000006` elige el perfil RVT, que usa la clave de desarrollo |
| `COMMON_KEY_HANDLES[índice]` y `Decrypt(…, PID_ES)` | `common_key_handle(índice)` y `decrypt()` con comprobación de propietario, tipo y tamaño. Códigos de error: `IOSC_EACCES`, `IOSC_EINVAL`, `IOSC_INVALID_OBJTYPE`, `IOSC_FAIL_INTERNAL` |

`disc::decrypt_title_key` crea un `Iosc` del perfil del ticket con la provisión de la sesión y le pide la clave de título, como hace `TicketReader::GetTitleKey`. El lector, el descifrado AES y H0-H3 no cambian.

**Provisión automática.** Al crearse un sistema Wii, `load_default_key_provision` rellena las claves sin preguntar, buscando en este orden:

1. `$ORCA_WII_KEYS`;
2. los directorios que indique el frontend (en Android, el directorio de la app);
3. `$ORCA_KEYS_DIR`, `$XDG_CONFIG_HOME/orca` y `~/.config/orca`, donde mira `wii_keys.txt`, `common-key.bin`, `korean-common-key.bin`, `vwii-common-key.bin`, `dev-common-key.bin` y `keys.bin`.

La clave se importa **una sola vez** y sirve para todos los juegos:

- en la app, con el botón «Import Wii keys»;
- en las herramientas, con `orca_disc import-keys ARCHIVO [ÍNDICE] [DIR]`.

Ambos escriben `wii_keys.txt` con permisos solo para el propietario. Desde entonces ningún juego pide archivo de claves. Si falta una clave, el error dice cuál (por su handle) y cómo importarla.

**Estado de las claves predeterminadas:** Orca no las trae. Mientras el usuario no importe las de su consola, los discos Wii cifrados no se pueden leer. Esta es la única diferencia de experiencia con Dolphin.

### Comparación con el procedimiento de Dolphin

`tests/dolphin_wii_reference.py` vuelve a implementar, de forma independiente, las funciones de Dolphin citadas arriba y `CheckBlockIntegrity` / `CheckH3TableIntegrity`. Usa OpenSSL para AES y `hashlib` para SHA-1. La prueba `wii_dolphin_reference`:

1. escribe 7 imágenes, cada una con 3 particiones con claves de título e índices distintos (una correcta y seis con corrupción en datos, H0, H1, H2, H3 y el bloque 0);
2. compara, partición por partición, el ID de título, la clave de título (por su SHA-1), la comprobación H3, el veredicto H0-H3 de cada bloque y los datos descifrados.

Resultado: 21 particiones, 0 diferencias. Comprobado que la prueba detecta diferencias: con un IV fijo en la referencia salen 19.

No se ejecutó el propio Dolphin: compilarlo necesita decenas de bibliotecas que no se pueden instalar en este entorno sin red.

### Diferencias de comportamiento que quedan (intencionadas)

| | Dolphin | Orca |
|---|---|---|
| Claves comunes | En su código fuente | Del usuario |
| Verificación H0-H3 | Solo en «Verificar» (`VolumeVerifier`); el juego recibe los datos sin comprobar | En cada lectura, antes de entregar datos, como el IOS real; un fallo detiene la emulación con el motivo |
| Índice de clave ≥ 2 | Clave 0 y una alerta | Índice 2 = vWii si se aporta; > 2 es un error |
| H3 incorrecta o bloque 0 corrupto | Abre la partición | Rechaza abrirla, con el motivo |

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
