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
  - `keys.bin` de BootMii (analizador propio, `disc/bootmii_keys`): exactamente 0x400 bytes. Disposición estudiada en `BootMiiKeyDump` de Dolphin (que la toma de mini):
    - 0x000-0x0FF: texto `BackupMii v1, ConsoleID: XXXXXXXX\n` y relleno;
    - OTP 0x100-0x17F: hash de boot1 (0x100), **clave común (0x114)**, NG ID (0x124), clave privada NG (0x128, 30 bytes), HMAC NAND (0x144), **clave NAND (0x158)**, clave de copia (0x168);
    - SEEPROM desde 0x200: MS ID (0x200), CA ID (0x204), NG key ID (0x208), firma NG (0x20C), contadores (0x248, versión de boot2), **clave coreana (0x274)**; relleno hasta 0x3FF.
    - Validación: tamaño, encabezado, ID del encabezado = NG ID, clave común no vacía y distinta de las de consola, clave NAND no vacía, coreana (0 = ausente) distinta de la común. Si falla, no se carga nada.
    - Clave común → índice 0, coreana → índice 1. Las claves de consola van **solo** a las entradas de consola de OrcaIOSC y nunca se usan como clave común. El índice 2 (vWii) no está en un keys.bin.
    - `orca_disc check-keys FILE` lista los campos encontrados y las comprobaciones, sin valores.
  - Prioridad entre fuentes: `$ORCA_WII_KEYS`, luego en cada carpeta `wii_keys.txt`, los archivos de 16 bytes y `keys.bin`. Gana la primera; un valor distinto posterior no se aplica y se informa sin mostrarlo. Al importar un keys.bin se guarda entero (0600) y se retiran del texto las entradas que lo contradicen.
  - Texto, una clave por línea: `common_key = <32 hex>`, `korean_common_key = …`, `vwii_common_key = …`, `dev_common_key = …`, y opcionalmente `root_key_file = <ruta>` (módulo de 512 bytes, o módulo + exponente, o un certificado).
- **Discos sin cifrado** (encabezado 0x61 ≠ 0): no necesitan ninguna clave.

### OrcaIOSC (`ios/iosc`): comparación técnica con el IOSC de Dolphin

**Cómo inicializa Dolphin sus claves (estudiado en `IOSC.cpp`).** El constructor `IOSC(ConsoleType)` llama a `LoadDefaultEntries()` y después a `LoadEntries()`.

- `LoadDefaultEntries()` rellena las 12 entradas por defecto. La clave común, la coreana, la clave SD y la clave pública raíz (juegos Retail y RVT) son **arrays de bytes compilados en el programa**: por eso Dolphin nunca pide un archivo.
- `LoadEntries()` lee `keys.bin` solo si existe, y de él solo toma las claves propias de la consola (NG, NAND, PRNG, IDs), nunca la clave común.

| Aspecto | Dolphin | Orca | Estado |
|---|---|---|---|
| Tabla de objetos | `std::array<KeyEntry, 32>` | `kEntries = 32` | Igual, probado |
| Handles por defecto | 0-11 y `HANDLE_ROOT_KEY` = 0xFFFFFFF | Mismos números; más `kOrcaVwiiCommonKey` (0x100) para el índice 2 | Igual + extensión, probado |
| `KeyEntry` | in_use, tipo, subtipo, datos, misc_data, owner_mask | Lo mismo + `provided`, nombre, origen del valor | Probado |
| Tamaños por tipo (`s_type_to_size_map`) | AES 16, MAC 20, ECC privada 30, RSA 256/512, ECC pública 60 | `size_for_type`, mismos valores | Probado |
| `CreateObject` / `DeleteObject` | Primera entrada libre; propietario = 1 << pid; las entradas por defecto no se borran | `create_object` / `delete_object`, mismas reglas; además borra el valor con `secure_zero` | Probado (incluido `IOSC_FAIL_ALLOC` con la tabla llena) |
| `ImportSecretKey` (simple y descifrando con otra clave) | Solo en objetos no predeterminados del propietario, AES-128 | `import_secret_key`, las dos variantes | Probado |
| `ImportPublicKey` | Tamaño según tipo; exponente RSA en misc_data | `import_public_key`, mismas reglas | Probado |
| `Get/SetOwnership`, `HasOwnership` | La raíz la poseen todos; solo el único propietario cede; los 3 PID bajos se conservan | Iguales | Probado |
| `Decrypt` / `Encrypt` (`DecryptEncrypt`) | Comprueba propietario → existencia → tipo AES → tamaño; AES-CBC con el IV actualizado | Mismo orden y mismos códigos (`IOSC_EACCES`, `EINVAL`, `INVALID_OBJTYPE`, `FAIL_INTERNAL`) | Probado |
| Selección de clave común | `COMMON_KEY_HANDLES[índice]`; índice ≥ 2 → clave 0 con alerta | `kCommonKeyHandles[índice]`; 2 → vWii; > 2 → error explícito | Diferencia intencionada, probada |
| Perfil de consola | `TicketReader::GetConsoleType`: emisor `Root-CA00000002-XS00000006` → RVT | `console_type_of`, igual | Probado |
| Clave de título | `GetTitleKey`: `iosc.Decrypt(common, IV = ID de título ‖ 0⁸, clave cifrada)` | `title_key`: crea un objeto de ES, `ImportSecretKey` descifrando con la clave común, entrega el valor al lector y borra el objeto (como hace ES) | Mismo resultado (`wii_dolphin_reference`: 0 diferencias); probado que no deja objetos |
| Descifrado de bloques | `DecryptBlockHashes` (IV 0) y `DecryptBlockData` (IV = bytes cifrados 0x3D0) | Igual, con AES por instrucciones ARMv8 cuando la CPU las tiene | Probado; la ruta ARMv8 se prueba en ARM64 |
| Valores de las entradas | En el código fuente de Dolphin | **Fuera del código**: el paquete de valores del compilado (`-DORCA_DEFAULT_KEYS_FILE`) y los valores de sesión del usuario (importados una vez) | Mecanismo probado con valores de prueba |
| `LoadEntries()` desde `keys.bin` | Claves de consola (NG, NAND, PRNG, IDs); nunca la común | Mismas entradas de consola e IDs MS/CA desde el keys.bin del usuario; además Orca toma su clave común (Dolphin la tiene en el código) | Probado (`bootmii_keys`) |

**Arquitectura equivalente en Orca (sin pedir nada al usuario).**

- **Mecanismo** (`ios::Iosc`): entradas, objetos, importaciones, propietarios y AES. No contiene valores.
- **Selección**: índice del ticket → handle; emisor → perfil Retail/RVT.
- **Valores**, en el mismo orden que Dolphin:
  1. `load_default_entries()` toma los del **paquete del compilado** (`include/orca/ios/key_bundle.hpp`). Quien compila Orca y tiene derecho a esos valores pasa un archivo con `-DORCA_DEFAULT_KEYS_FILE=archivo`. En Android, Gradle reenvía la variable de entorno `ORCA_DEFAULT_KEYS_FILE`; en el CI existe el secreto opcional `ORCA_DEFAULT_KEYS`. Los valores quedan compilados en el binario como en Dolphin, **sin cambiar ni una línea del emulador**, y entonces ningún juego pide claves.
  2. `load_entries()` aplica los valores de la sesión: las claves de la consola del usuario, importadas una vez, que sustituyen a las del paquete.

  El repositorio no contiene valores. Un compilado sin paquete tiene todas las entradas pero vacías, y lo dice en sus errores. La APK que lleva valores compilados no se publica como prerelease pública.

**Qué está bloqueado y por qué.** Solo los **valores**: la clave común, la coreana, la de desarrollo, la clave SD y la clave pública raíz. Son datos de Nintendo. No los incluyo en el repositorio ni los genero: no se pueden calcular, porque son claves secretas, no el resultado de un algoritmo. El mecanismo para cargarlos sin tocar el código está implementado y probado de punta a punta con valores de prueba (`key_bundle`): un programa Wii arranca desde una partición cifrada y lee su disco **sin ninguna clave del usuario**.

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
