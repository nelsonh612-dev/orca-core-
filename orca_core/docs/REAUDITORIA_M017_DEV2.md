# Reauditoría de Orca Core 0.17.0-dev2

Fecha: 2026-11-15

## Cambios realizados sobre el paquete M017 recibido

- El diagnóstico JNI ahora muestra fabricante, GPU, versión OpenGL ES, versión GLSL y cantidad de extensiones.
- Se comprueba el nivel real de OpenGL ES antes de intentar crear el backend.
- `GL_EXT_shader_framebuffer_fetch` y `GL_ARM_shader_framebuffer_fetch` se informan por separado. La extensión ARM no se acepta como sustituto de EXT.
- Si falta ES 3.2 o la extensión EXT requerida, la app informa el motivo sin intentar inicializar un backend incompatible.
- Después de `Backend::init()` se consulta `glGetError()` y se informa un error pendiente.
- La creación de contexto Android intenta ES 3.2 y, si no está disponible, usa ES 3.0 únicamente para permitir que la sonda JNI identifique y comunique la capacidad real.
- Se quitó el contenedor ScrollView alrededor de GLSurfaceView para que la superficie GL reciba correctamente el espacio restante.
- Se actualizó la versión de proyecto a 0.17.0-dev2 y se documentó el alcance real.

## Pruebas reproducidas

| Entorno | Resultado |
|---|---|
| GCC 14.2, `ORCA_WERROR=ON` | Compilación correcta |
| GCC / CTest | 17 suites pasan; `encodings` se omite por falta de `llvm-mc` |
| Clang 17, `ORCA_WERROR=ON` | Compilación correcta |
| Clang / CTest | 17 suites pasan; `encodings` se omite por falta de `llvm-mc` |
| ASan/UBSan | La compilación completa termina. La ejecución CTest se queda bloqueada en la suite `av` después de imprimir sus 12 casos como correctos; por tanto, no se certifica el conjunto ASan/UBSan completo. |
| PowerPC test programs | Se compilan los programas de prueba con clang/LLD para PowerPC 750 durante las compilaciones GCC y Clang |

## Integración continua añadida

Se añadió `.github/workflows/ci.yml` con jobs GCC/Clang y compilación de APK Android ARM64. El workflow es una definición versionable; no se ha ejecutado en GitHub ni se afirma que haya producido un APK.

## No verificado en este entorno

- Backend GLES: no se compiló porque no están instaladas las bibliotecas de desarrollo EGL/GLES del host.
- Android Gradle/NDK: no se compiló porque este entorno no tiene Android SDK, NDK ni Gradle instalado.
- APK: no generado.
- Teléfono real: no conectado; no hay datos medidos del Redmi 15C/Mali-G52.
- No se verificó presentación de XFB, juego ejecutándose en Android, entrada, sonido, guardados ni rendimiento móvil.

## Estado funcional del proyecto

Este paquete es un núcleo de emulación en desarrollo y una sonda Android de capacidades gráficas. No es todavía un emulador Android jugable ni una implementación completa de GameCube/Wii. El estado del código indica limitaciones funcionales en dispositivos, audio, sistema de arranque, compatibilidad de discos Wii y renderizado; consultar `ESTADO.md` y `M017_ANDROID_BOOTSTRAP.md`.
