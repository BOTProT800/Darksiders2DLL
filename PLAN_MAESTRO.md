# Plan maestro actualizado: DLL de modding para Darksiders II – Deathinitive Edition (PC)

> Estado verificado localmente: 11 de septiembre de 2026. Este documento sustituye el borrador especulativo anterior. Las rutas, hashes y offsets marcados como confirmados se obtuvieron de los archivos presentes en este equipo. El proxy y `first_test_mod` ya están desplegados; los `.upak` permanecen intactos.

## 1. Objetivo

Construir una DLL x64 que se instale como proxy `dinput8.dll` junto a `Darksiders2.exe` y actúe como framework de modding en tiempo de ejecución.

El resultado final deberá:

1. Reenviar correctamente DirectInput al `dinput8.dll` real de Windows.
2. Inicializar el framework fuera del *loader lock* de `DllMain`.
3. Descubrir mods de archivos sueltos bajo `mods/<id_mod>/`.
4. Interceptar una función interna del motor en la que todavía sea posible identificar el recurso solicitado y sustituir sus bytes por un archivo del mod.
5. Volver siempre al comportamiento original si no hay reemplazo, si el mod está desactivado o si el archivo no supera la validación.
6. Dejar una base segura para hooks de gameplay y plugins DLL posteriores, sin mezclar esos plugins con los assets sueltos.

La primera prueba controlada será el mod `first_test_mod`, que reemplazará el icono de la habilidad pasiva de agilidad mejorada.

### Fuera del alcance del primer hito

- Reempaquetar o escribir permanentemente en los `.upak` desde la DLL.
- Convertir modelos, esqueletos o audio.
- Soportar varias versiones desconocidas de `Darksiders2.exe` mediante una misma firma no validada.
- Hot reload, interfaz gráfica, descarga de mods o importación de ZIP/RAR.
- Cargar automáticamente cualquier DLL encontrada dentro de las carpetas de assets.

## 2. Estado local confirmado

### 2.1 Rutas de trabajo

- Proyecto DLL: `C:\Users\vicen\Documents\Proyectos\Software\Darksiders2DLL`
- Juego: `C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition`
- Extracción disponible: `C:\Users\vicen\Documents\Extractions\Darksiders`
- Darkstractor: `C:\Users\vicen\Documents\Proyectos\Software\Darkstractor`
- DarksideModManager: `C:\Users\vicen\Documents\Proyectos\Software\DarksideModManager`
- Anansi: `C:\Users\vicen\Documents\Proyectos\Software\Anansi`

La instalación se detecta correctamente desde Steam. Actualmente contiene el proxy local `dinput8.dll` y el DDS bajo `mods\first_test_mod\...`. DarksideModManager sigue informando que no hay modificaciones permanentes instaladas porque el despliegue no altera sus paquetes. Los `.upak` conservaron tamaño y fecha durante el despliegue y la actualización del proxy.

### 2.2 Huella de la versión que se investigará

- Ejecutable: `Darksiders2.exe`
- Arquitectura: PE32+ x64 (`machine 0x8664`)
- Tamaño: `33,637,376` bytes
- SHA-256: `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`
- `media\manifest.bin`: `449,222` bytes
- SHA-256 del manifiesto: `C30209E55D05EA7AF1D7E4901F0E998A50F00658C04884C0F37A7C1846F90824`

El ejecutable importa `DINPUT8.dll!DirectInput8Create`, por lo que el proxy `dinput8.dll` es un punto de carga válido para esta versión. Cualquier offset o patrón descubierto se considerará compatible únicamente con la huella anterior hasta demostrar lo contrario.

Archivos confirmados en `media\`:

- `manifest.bin`
- `media.upak`
- `maps.upak`
- `anim_streams.upak`

El nombre correcto es `anim_streams.upak`; el borrador anterior decía por error `anime_streams.upak`.

### 2.3 Estado real del proyecto C++

El proyecto x64 ya está implementado y compila en Debug y Release:

- C++20, configuraciones x64 soportadas y CRT estática `/MT` en Debug y Release.
- Salida `dinput8.dll` con los seis exports del proxy conservados por nombre y ordinal.
- Carga del `dinput8.dll` real mediante ruta absoluta a `System32`.
- MinHook estático y fijado mediante el manifiesto/baseline de vcpkg.
- Huella SHA-256 del ejecutable con comportamiento *fail-closed* para builds desconocidos.
- Logger por sesión, índice inmutable y seguro de mods, límites de recursos, validación DDS y almacenamiento estable de bytes.
- Prueba de concepto de reemplazo exacto en D3DX9 y D3DX11: solo sustituye el DDS original de `4,224` bytes y SHA-256 conocido por el DDS editado también conocido.
- Trampolines publicados mediante variables atómicas y activación conjunta de D3DX9/D3DX11 con `MH_QueueEnableHook` más una única llamada a `MH_ApplyQueued`.
- Rollback compensatorio; si su resultado es indeterminado, el reemplazo se descarta y el estado queda *fail-closed* para el resto del proceso.
- Validación del rango fuente seguida de una copia completa con `ReadProcessMemory` antes de calcular el hash, para que una lectura parcial o inválida produzca passthrough seguro.
- Eventos de primera entrada independientes para D3DX9 y D3DX11; el callback del detour solo publica estado y despierta al worker de logging.
- Pruebas offline y de humo del proxy para Debug/Release; el proxy Release superó `20/20` ciclos consecutivos de la prueba de humo.

El binario Release final tiene SHA-256 `8CE46A305F62BC862EEBE2B70D5A5D1ECF0EBE4ECA0F264DE3F1473383F3DBDA`; esa misma huella está desplegada como `dinput8.dll` junto al ejecutable del juego.

El bootstrap se programa desde `DLL_PROCESS_ATTACH` mediante un worker y se vuelve a intentar desde `DirectInput8Create` si la programación inicial falla. `DllMain` no abre logs, no calcula hashes, no recorre carpetas, no inicializa MinHook y no espera al worker: únicamente guarda el módulo y solicita la creación asíncrona. Las notificaciones de hilo permanecen activas porque ambas configuraciones enlazan la CRT estática `/MT`. El trabajo real comienza después de que el loader libere su bloqueo.

Las ejecuciones reales confirmaron en el log `BUILD_SUPPORTED`, `MOD_INDEXED first_test_mod`, `ASSET_OVERRIDE_READY` y `TEXTURE_HOOK_ACTIVE` para D3DX9 y D3DX11. Todavía no se ha observado `ASSET_REQUEST`/`ASSET_OVERRIDE_HIT`, ni se ha confirmado visualmente el cambio o el fallback. También faltan reinicios largos. El resolver general del motor y su ABI interna siguen pendientes; el hook D3DX es deliberadamente un POC estrecho para este DDS.

Durante la validación apareció un crash aislado en el PID `16272`: WER registró `0xc0000005` en `Darksiders2.exe+0x9E553F`. Se conservaron el dump `%LOCALAPPDATA%\CrashDumps\Darksiders2.exe.16272.dmp` y el `Report.wer` correspondiente bajo `C:\ProgramData\Microsoft\Windows\WER\ReportArchive`. El incidente no se descarta, pero tampoco se atribuye al proxy o a los hooks con la evidencia actual.

Después del hardening se ejecutó una matriz A/B/C, tres veces por brazo y 15 segundos por ejecución:

- A: vanilla, con `dinput8.dll` temporalmente renombrado.
- B: proxy activo y DDS objetivo temporalmente renombrado; el bootstrap emitió `ASSET_FALLBACK` y no llamó a `MH_Initialize`.
- C: proxy y DDS activos, con los hooks D3DX9/D3DX11 activos.

Las nueve ejecuciones fueron estables, terminaron con exit code `0` y no generaron nuevos informes WER. Al acabar se restauraron tanto el proxy como el DDS. Esta matriz corta no reprodujo el crash y acota el riesgo inmediato, aunque no sustituye la prueba visual ni los reinicios prolongados.

### 2.4 Corpus extraído

La extracción existente contiene únicamente la raíz virtual `media\`:

- `56,673` archivos.
- Aproximadamente `11.043 GiB`.
- `8,222` DDS, además de `.bmat`, `.o3d`, `.2`, `.anm`, `.wem`, `.bod`, `.4`, `.bnk`, `.gfx`, `.loc` y otros tipos.
- `media\ui` contiene `2,524` archivos; `media\ui\ui_icons_small` contiene `116` DDS.

La extracción conserva rutas virtuales útiles, pero no incluye un manifiesto o índice lateral. Se usará como corpus de referencia y como fuente de assets editados; no como sustituto del catálogo construido desde la instalación real.

DarksideModManager verificó directamente el `manifest.bin` y los paquetes reales:

- Un único manifiesto versión `13` (`0x0D`) indexa `media.upak` y `maps.upak` con offsets de 64 bits.
- `1,681` segmentos OBPK.
- `75,627` miembros catalogados.
- Las rutas y los nombres necesarios para el caso inicial son legibles; el manifiesto también conserva hashes de 64 bits.

Esto cierra la antigua incógnita de “rutas legibles o solo hashes” para el primer caso. Algunos OBPK sí pueden omitir su tabla de nombres, pero no es el caso del segmento elegido.

## 3. Primer mod y asset de prueba

Los `\_` vistos en el texto original eran escapes de Markdown, no carpetas adicionales. La ruta real es:

```text
C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds
```

Identidad canónica del recurso, siempre con `/` en logs e índices internos:

```text
media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds
```

Destino de la prueba de runtime:

```text
<carpeta_del_juego>\mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds
```

Ruta absoluta esperada:

```text
C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition\mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds
```

No se moverá ni se sobrescribirá el archivo fuente de la extracción. Al preparar una prueba se copiará a la ruta del mod.

### 3.1 Datos verificados del DDS

- Tamaño: `4,224` bytes (`0x1080`).
- Formato: DDS tradicional, `64 × 64`, `DXT5/BC3`, un nivel base, sin cabecera DX10.
- SHA-256 editado: `2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719`.
- SHA-256 original extraído del `.upak` limpio: `E2173F05C575677756071AD7DEC88E4CF49BBD0A734F902159E93C79F191B22D`.
- El contenido de píxeles es distinto del original.
- El `--dry-run` estricto de DarksideModManager lo acepta: tamaño, dimensiones, formato y mipmaps son compatibles.

El editor cambió además tres bytes de cabecera: los flags DDS, `depth` (`0 → 1`) y el campo explícito `mipMapCount` (`0 → 1`). Ambos encabezados describen efectivamente una textura 2D con un solo nivel y el validador la considera compatible. Para reducir variables, se conservará el original intacto y, si el juego rechaza el DDS, se creará una copia de staging con los primeros 128 bytes normalizados a partir del original antes de culpar al hook.

### 3.2 Ubicación dentro del paquete

- Paquete: `media.upak`.
- Segmento: `media/ui/ui_icons_small.`.
- Offset del segmento: `0x33250800`.
- Tamaño del segmento: `0x52800` (`337,920` bytes).
- Rango en el `.upak`: `0x33250800–0x332A3000`.
- Disposición OBPK: `stream`.
- Cabecera/payload: el zlib empieza en `segmento + 0x3004`, es decir `0x33253804`.
- Tamaño comprimido del stream: `325,628` bytes (`0x4F7FC`).
- Tamaño descomprimido del stream: `705,792` bytes (`0xAC500`).
- Miembro: `#72`.
- Offset del miembro en el stream descomprimido: `0x4F800`.
- Tamaño del miembro: `0x1080` (`4,224` bytes).
- Tipo OBPK: `6` (`.dds`).
- Código interno de textura: `0x11`.

La consecuencia técnica es importante: `ReadFile` ve un segmento OBPK comprimido compartido, no el DDS individual. Se usará como punto de rastreo, pero devolver un DDS suelto desde un hook de `ReadFile` corrompería el contrato esperado. El hook final debe estar después de descomprimir/seleccionar el miembro y antes de que el consumidor interprete sus bytes.

## 4. Papel de los proyectos existentes

### 4.1 Darkstractor: oráculo de extracción y formatos

Usar como herramienta offline principal para inventariar o reextraer recursos faltantes. Ya aporta:

- Lectura de `manifest.bin` con offsets de 64 bits.
- Extracción nombrada de `media.upak` y `maps.upak`.
- Parser de OBPK Deathinitive.
- Recuperación de `anim_streams.upak` buscando streams zlib, sin depender de offzip.
- Extracción de DDS, modelos, materiales, animación, Wwise y Scaleform.

Archivos de referencia prioritarios:

- `darkstractor\formats\manifest.py`
- `darkstractor\formats\deathinitive_obp.py`
- `darkstractor\service.py`

No portar todo a C++. Para el primer hook, conservar Darkstractor como herramienta de diagnóstico externa.

### 4.2 DarksideModManager: catálogo, validación y control A/B

Es la fuente local más útil para este plan. Ya implementa y prueba:

- Traducción `ruta virtual → paquete → segmento → miembro`.
- Normalización de `/` y `\`, comparación insensible a mayúsculas y rechazo de `..`.
- Parser detallado de OBPK `stream` y `blocks`.
- Validación DDS y metadatos internos.
- Orden seguro para parcheo offline: temporal, verificación, respaldo y escritura.
- Fixtures sintéticos y pruebas contra copias aisladas de segmentos reales.

Archivos de referencia prioritarios:

- `darkside\formats\manifest.py`
- `darkside\formats\obp.py`
- `darkside\payload.py`
- `darkside\catalog.py`
- `darkside\formats\dds.py`
- `darkside\modpack.py`

`patcher.py`, `backup.py`, la GUI, ZIP/RAR y PyInstaller quedan fuera de la ruta crítica: resuelven instalación permanente, mientras esta DLL busca override virtual. El matching flexible “por sufijo” tampoco se copiará al runtime; el loader exigirá rutas canónicas completas para evitar ambigüedades.

DarksideModManager sí se conservará como control independiente: puede demostrar que el asset correcto cambia visualmente antes de investigar el hook, y después restaurar el juego.

### 4.3 Anansi: reservar para modelos

Anansi aporta parsers y conversión offline de OBP, geometría `.2`, esqueletos `.o3d`, skinning y GLB. No contiene un VFS, proxy `dinput8`, MinHook ni conversión DDS, así que no participa en `first_test_mod`.

Revisarlo únicamente cuando el loader ya funcione con archivos sueltos y se quiera probar modelos. No asumir cobertura universal de todos los OBPK a partir de su parser actual.

### 4.4 Herramientas comunitarias externas

QuickBMS, `darksiders2.bms`, DS2Extract, offzip, DS2-RE, Darksiders-2-DLL-Loader, x64dbg y Ghidra permanecen como fuentes y contingencias. Para extracción y estructura de paquetes se priorizan ahora los parsers locales ya validados. Para localizar el resolver en runtime siguen siendo relevantes DS2-RE, x64dbg y Ghidra.

## 5. Arquitectura objetivo

```text
Darksiders2.exe
  └─ carga .\dinput8.dll
       ├─ Proxy DirectInput → %SystemRoot%\System32\dinput8.dll
       └─ Programa un worker de bootstrap sin esperarlo en DllMain
            ├─ Huella/compatibilidad del ejecutable
            ├─ Logger
            ├─ Índice inmutable de mods
            ├─ Escáner de firmas preparado para el resolver futuro
            └─ POC exacto D3DX9/D3DX11
                 ├─ reconocer tamaño y SHA-256 del DDS original
                 ├─ usar el override ya validado e indexado
                 ├─ conservar los bytes con vida útil estable
                 └─ llamar a D3DX con los argumentos originales si no coincide
```

### 5.1 Reparto propuesto del código

- `proxy_dinput8.*`: carga segura de la DLL del sistema y reenvío de exports.
- `bootstrap.*`: inicialización única y estado global mínimo.
- `game_build.*`: hash/identificación de la versión soportada.
- `logging.*`: log rotativo o por sesión, sin consola obligatoria en Release.
- `mod_index.*`: descubrimiento, prioridad y snapshot inmutable.
- `virtual_path.*`: normalización y validación de rutas.
- `signature_scan.*`: búsqueda acotada en `.text` y validación del candidato.
- `asset_hook.*`: POC exacto D3DX9/D3DX11, trampolines y lógica de fallback; el typedef del resolver interno futuro aún debe investigarse.
- `byte_storage.*`: propiedad y vida útil de los buffers de reemplazo.
- `dinput8.def`: lista de exports del proxy.
- `tests/`: pruebas unitarias que no arrancan ni modifican el juego.

### 5.2 Convención de mods

Para el MVP, cada subdirectorio inmediato de `mods\` es un mod de assets. El contenido debe empezar por el paquete virtual:

```text
mods/
  first_test_mod/
    media/
      ui/
        ui_icons_small/
          ui_hudicon_passiveability_improved_agility.dds
```

Reglas:

- Internamente, las claves usan `/` y comparación ASCII/Unicode insensible a mayúsculas de forma consistente.
- Rechazar rutas absolutas, UNC, componentes `..`, componentes vacíos peligrosos, ADS y cualquier resolución que salga de la raíz del mod.
- Preservar el nombre original para logs, aunque la clave de búsqueda esté normalizada.
- El MVP solo tendrá un mod activo; después se añadirá un orden explícito. Nunca depender del orden que devuelva `FindFirstFile`.
- Los plugins de código vivirán en una carpeta separada, por ejemplo `plugins\`; no se cargarán DLL arbitrarias desde `mods\`.

## 6. Fases de ejecución

### Fase 0 — Congelar una base reproducible

**Estado: completada para el POC x64.** La salida, CRT, dependencia fijada, avisos, huella y proxy ya están implementados y probados.

1. Registrar en el log la huella SHA-256 de `Darksiders2.exe` y rechazar hooks desconocidos por defecto.
2. Configurar salida x64 con nombre `dinput8.dll`.
3. Fijar `/MT` en Debug y Release x64; retirar Win32 del camino soportado aunque pueda seguir en la solución.
4. Añadir un manifiesto de vcpkg y MinHook estático compatible con el CRT elegido, o documentar otra integración reproducible.
5. Añadir `CREDITS.md` y conservar avisos/licencias si se adapta código MIT de los proyectos locales.
6. Compilar primero un proxy sin hooks.

Criterio de salida: el proxy carga en el proceso real, crea un log identificable y mantiene operativo DirectInput. La carga y el log ya se confirmaron; la comprobación interactiva prolongada de teclado/control queda incluida en la validación end-to-end pendiente.

### Fase 1 — Congelar la evidencia de assets y paquetes

Esta fase está mayormente completada; no hace falta repetir una extracción de 11 GiB.

1. Registrar la ruta canónica y los datos del apartado 3 en una nota de investigación versionada, sin copiar el asset comercial al repositorio.
2. Usar DarksideModManager/Darkstractor para regenerar el catálogo cuando cambie el juego.
3. Reextraer solo archivos faltantes o dudosos.
4. Mantener `anim_streams.upak`, audio y modelos fuera del primer caso.

Criterio de salida: dado el virtual path del DDS, una herramienta offline devuelve de forma determinista el paquete, segmento y miembro indicados arriba.

### Fase 2 — Validación independiente del asset

El preflight ya pasó:

```powershell
python -B -m darkside install `
  --target media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds `
  --file "C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds" `
  --dry-run
```

Resultado confirmado: un archivo aceptado, miembro `#72`, `64x64 DXT5`, compatible en modo estricto.

Como control offline opcional, antes del runtime hook se puede instalar temporalmente mediante DarksideModManager, comprobar que el icono correcto cambia y restaurar inmediatamente. Esa prueba debe hacerse con el juego cerrado y usando el flujo de respaldo del manager. Antes de probar la DLL, `darkside status` debe volver a indicar que no hay modificaciones instaladas.

Criterio de salida: el asset se reconoce visualmente en la situación de juego elegida, o se documenta con precisión cómo provocar su aparición.

### Fase 3 — Proxy `dinput8` y bootstrap seguro

**Estado: implementada; integración básica y matriz corta validadas en el proceso real.**

1. Reenviar los seis exports del `dinput8.dll` de Windows:
   - `DirectInput8Create`
   - `DllCanUnloadNow`
   - `DllGetClassObject`
   - `DllRegisterServer`
   - `DllUnregisterServer`
   - `GetdfDIJoystick`
2. Resolver la DLL real mediante una ruta absoluta obtenida con `GetSystemDirectoryW`; nunca llamar a `LoadLibrary("dinput8.dll")` desde la carpeta del juego, porque recargaría el proxy.
3. Mantener `DllMain` mínimo: guardar el `HMODULE` y programar un worker. Esa programación no realiza la inicialización en línea. No llamar a `DisableThreadLibraryCalls`, porque el proyecto enlaza la CRT estática y esta puede necesitar las notificaciones de hilo.
4. Hacer toda la inicialización real en el worker después de salir del *loader lock*. No inicializar MinHook, recorrer carpetas, abrir logs, calcular hashes ni esperar al worker dentro de `DllMain`.
5. Usar `DirectInput8Create` como punto seguro de reintento si `CreateThread` hubiera fallado durante `DLL_PROCESS_ATTACH`; la inicialización sigue protegida para ejecutarse una sola vez.
6. En terminación normal del proceso, permitir que Windows recupere recursos; cualquier desinstalación explícita de hooks deberá ocurrir fuera de `DllMain`.

Criterio de salida: todas las exportaciones resuelven, el proxy llama al DirectInput real y las pruebas repetidas no producen un crash reproducible ni un cuelgue. Los seis exports, la ruta de `System32`, una llamada real a `DirectInput8Create`, `20/20` ciclos de humo Release y las nueve ejecuciones de la matriz corta ya están verificados. El crash aislado conserva evidencia para análisis y los reinicios largos del juego aún deben realizarse.

### Fase 4 — Localizar el punto correcto de hook

**Estado: POC D3DX activo; resolver general pendiente.** Las dos funciones D3DX se interceptan de forma estrecha para reconocer el buffer original exacto. Esto permite probar el primer icono sin afirmar que ya se conoce la firma o ABI del resolver interno.

#### 4.1 Rastrear el segmento conocido

Con x64dbg, identificar primero el `HANDLE` de `media.upak` mediante `CreateFileW/CreateFileA`. Colocar breakpoints condicionales en `SetFilePointerEx`, `ReadFile` y `ReadFileEx` para el rango:

```text
0x33250800 <= offset < 0x332A3000
```

`ReadFile` solo es una baliza para recuperar la pila. Desde el retorno de la descompresión, localizar el buffer de `705,792` bytes y el miembro que empieza en `buffer + 0x4F800` con longitud `0x1080`. Seguir sus accesos hacia arriba hasta una función que seleccione el miembro o entregue sus bytes.

#### 4.2 Usar el cargador gráfico como segunda baliza

El ejecutable importa rutas de creación de texturas tanto D3DX11 como D3DX9. Poner breakpoints, al menos, en:

- `d3dx11_43!D3DX11CreateTextureFromMemory`
- `d3dx9_43!D3DXCreateTextureFromFileInMemoryEx`

Buscar llamadas cuyo buffer sea un DDS de `4,224` bytes y cuyo contenido coincida con el SHA-256 original. Caminar la pila hacia atrás hasta la última función del motor que todavía conozca ruta, segmento, índice o identificador del recurso.

Estas APIs ya forman el POC exclusivo de texturas. No son el hook general definitivo.

#### 4.3 Validar cada candidato antes de reemplazar

1. Hook pasivo: registrar argumentos y llamar siempre al original.
2. Provocar varias veces la aparición del icono.
3. Confirmar una relación uno-a-uno con el miembro `#72` o su identificador.
4. Observar en qué hilo se invoca y quién posee/libera el buffer devuelto.
5. Comprobar que otros menús, cargas y partidas siguen funcionando.

En x64 no existe un `thiscall` separado como en x86: todos los métodos siguen la ABI x64 de Microsoft y `this` llega normalmente en `RCX`. La firma se deducirá de registros, stack y call sites, no por intuición.

#### 4.4 Hacer resistente la localización

- Guardar un RVA solo como dato de investigación para la huella soportada.
- Derivar una firma de bytes que evite direcciones relativas variables.
- Escanear únicamente secciones ejecutables esperadas.
- Exigir contexto adicional alrededor del patrón y exactamente una coincidencia.
- Si no coincide la huella o la firma es ambigua, desactivar el override y mantener únicamente el proxy/log.

Criterio de salida: el hook pasivo identifica de forma repetible la solicitud exacta en la versión soportada y el juego permanece estable.

### Fase 5 — Implementar el override de archivos sueltos

**Estado: infraestructura e implementación exacta del primer DDS completadas; adaptación al resolver general pendiente.** El índice seguro se construye al inicio y el POC D3DX consume el snapshot, pero todavía no traduce solicitudes arbitrarias del motor a rutas virtuales.

1. Construir al inicio un snapshot inmutable de `mods\`; no consultar el disco en cada llamada del resolver.
2. Normalizar la identidad observada en runtime a la clave canónica `media/...`.
3. Buscar el primer override habilitado según un orden determinista.
4. Validar el archivo antes de publicarlo. Para DDS: magic, tamaño mínimo, dimensiones, formato, mipmaps y tamaño esperado.
5. Mantener los bytes en almacenamiento con vida útil suficiente. Nunca devolver un puntero a un `std::vector` temporal o a memoria liberada al salir del hook.
6. Respetar el contrato del motor: si espera copiar a un buffer propio, copiar; si devuelve objeto/handle, construir el mismo tipo de resultado. No asumir que basta con cambiar un `void*`.
7. Añadir guardia de reentrada y sincronización apropiada si el resolver se usa desde varios hilos.
8. Ante cualquier error, registrar el motivo y llamar a la función original sin alterar argumentos.

Eventos mínimos de log:

```text
BUILD_SUPPORTED
MOD_INDEXED first_test_mod
D3DX11_FIRST_ENTRY
D3DX9_FIRST_ENTRY
ASSET_REQUEST media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds
ASSET_OVERRIDE_HIT first_test_mod ... size=4224 sha256=2C0D...
ASSET_FALLBACK <ruta> <motivo>
```

Criterio de salida: con el archivo presente solo las solicitudes exactas generan `ASSET_OVERRIDE_HIT`; al retirarlo se obtiene fallback limpio y no cambia el comportamiento original. La ruta de fallback temprano ya quedó validada por log en el brazo B, pero falta comprobar su resultado visual.

### Fase 6 — Prueba end-to-end de `first_test_mod`

**Estado al 11 de septiembre de 2026: en curso.** El proxy Release final y el asset están desplegados sin modificar `.upak`. Los logs confirman `BUILD_SUPPORTED`, `MOD_INDEXED first_test_mod`, `ASSET_OVERRIDE_READY` y `TEXTURE_HOOK_ACTIVE`; el brazo B confirmó además `ASSET_FALLBACK` al retirar temporalmente el DDS. La matriz corta A/B/C terminó `9/9` estable, con exit code `0` y sin nuevos WER. Falta provocar la carga concreta del icono, obtener `ASSET_REQUEST` y `ASSET_OVERRIDE_HIT`, comprobar el reemplazo y el fallback visualmente y realizar reinicios largos.

1. Confirmar que DarksideModManager no tiene parches instalados.
2. Verificar nuevamente el SHA-256 del archivo editado.
3. Copiarlo —no moverlo— a la ruta absoluta del apartado 3. Como la instalación está bajo `Program Files (x86)`, el despliegue puede requerir elevación; la herramienta de build no debe ocultar ese fallo ni escribir parcialmente.
4. Instalar el proxy compilado como `<juego>\dinput8.dll` y conservar fuera del juego los símbolos PDB.
5. Arrancar el juego y provocar la aparición del icono de agilidad mejorada.
6. Confirmar las dos señales de éxito:
   - El log muestra la ruta canónica y `first_test_mod` como origen.
   - El icono cambia visualmente.
7. Renombrar temporalmente el DDS o desactivar el mod y confirmar visualmente que vuelve el original; el fallback temprano del bootstrap ya está confirmado por log.
8. Repetir cargas de menú/partida y reinicios de mayor duración para detectar vida útil incorrecta, cachés o carreras.

No considerar éxito una modificación hecha simultáneamente dentro de `media.upak`: eso impediría saber si funcionó la DLL o el parche offline.

### Fase 7 — Generalizar y endurecer

Después del primer DDS:

- Definir `mods.json` o formato equivalente para `enabled` y prioridad explícita.
- Resolver conflictos con una regla documentada y visible en logs.
- Añadir caché negativo/positivo sin impedir que un nuevo escaneo explícito reconstruya el snapshot.
- Probar nombres Unicode, mayúsculas, `/`/`\` y rutas maliciosas.
- Añadir límites de tamaño y presupuesto de memoria.
- Separar configuración, logs, assets y plugins.
- Incorporar una API de plugins versionada solo después de estabilizar el loader de assets.
- Probar un segundo DDS en otro segmento y luego un asset no DDS cuyo contrato de carga sea conocido.
- Mantener una lista explícita de hashes de ejecutables soportados.

### Fase 8 — Modelos y herramientas de Anansi

Solo cuando el loader general sea estable:

1. Usar Anansi para inspeccionar `.2`, `.o3d`, skeleton/skin y OBP.
2. Definir validadores por tipo antes de aceptar reemplazos.
3. Probar primero archivos con mismo tamaño/estructura y luego variaciones controladas.
4. No extrapolar el comportamiento del DDS a modelos: pueden tener referencias cruzadas, streaming o cachés distintos.

## 7. Estrategia de pruebas

### Unitarias, sin el juego

- Normalización de rutas y rechazo de traversal/UNC/ADS.
- Descubrimiento de mods y prioridad determinista.
- Índice inmutable y búsquedas concurrentes.
- Lectura/validación de DDS con casos válidos, truncados y formatos incompatibles.
- Scanner de firmas con cero, una y varias coincidencias.
- Selección por huella de build.
- Vida útil de buffers y fallback ante errores simulados.

Los fixtures deben ser sintéticos o copias mínimas aisladas. No ejecutar pruebas que escriban en la instalación real.

### Integración del proxy

- Comprobar con `dumpbin /exports` los seis exports.
- Verificar que la DLL del sistema se cargó desde `System32` y no recursivamente desde el juego.
- Probar con MinHook desactivado, patrón ausente y mod ausente.
- Probar arranque/salida repetidos.

### En el juego

- Confirmado en matriz corta: vanilla sin proxy, tres ejecuciones de 15 segundos.
- Confirmado en matriz corta: proxy con DDS ausente, fallback temprano y sin inicializar MinHook, tres ejecuciones de 15 segundos.
- Confirmado en matriz corta: proxy con DDS presente y hooks D3DX9/D3DX11 activos, tres ejecuciones de 15 segundos.
- Hook pasivo con logging.
- `first_test_mod` habilitado y pantalla objetivo abierta hasta obtener `ASSET_REQUEST`/`ASSET_OVERRIDE_HIT`.
- Mod deshabilitado/archivo ausente con comprobación visual del fallback.
- Asset inválido: debe rechazarse y caer al original.
- Varias cargas prolongadas y cambios de zona/menú.

La matriz corta completa terminó `9/9` con exit code `0` y sin nuevos WER. El crash aislado del PID `16272` permanece documentado y no se considera explicado por esa ausencia de reproducción.

## 8. Riesgos y mitigaciones

- **Hook demasiado bajo:** `ReadFile` entrega el stream comprimido compartido. Usarlo solo para rastrear; hookear selección/entrega del miembro.
- **Firma o ABI incorrectas:** validar registros, stack y varios call sites antes de escribir bytes.
- **Puntero con vida útil insuficiente:** modelar propiedad del buffer y mantener almacenamiento estable.
- **Trabajo bajo loader lock:** `DllMain` solo guarda el módulo y programa el worker sin esperarlo; logs, hashes, índice, MinHook y teardown se ejecutan fuera de él. `DirectInput8Create` permite reintentar la programación. Las notificaciones de hilo permanecen activas por compatibilidad con la CRT estática.
- **Activación parcial de hooks:** publicar los trampolines de forma atómica, encolar ambos enables y aplicar el conjunto una sola vez; si falla también el rollback compensatorio, descartar el reemplazo y dejar el estado `indeterminate` cerrado a reintentos.
- **Buffer fuente inválido o cambiante:** validar el rango y copiar los `4,224` bytes completos con `ReadProcessMemory` antes del hash; una copia fallida o parcial hace passthrough.
- **Crash aislado sin causalidad demostrada:** conservar dump y WER, comparar configuraciones A/B/C y exigir pruebas largas antes de cerrar el riesgo. La matriz corta posterior fue estable, pero no prueba que el incidente sea ajeno al POC.
- **Actualización del juego:** huella obligatoria y fail-closed sin instalar hooks desconocidos.
- **Patrón falso positivo:** sección acotada, contexto y coincidencia única.
- **Ruta runtime sin prefijo `media/`:** registrar el valor crudo y transformarlo solo mediante una regla demostrada; la convención en disco seguirá siendo canónica `media/...`.
- **DDS exportado con cabecera diferente:** el preflight ya confirma compatibilidad; normalizar una copia solo si aparece un fallo reproducible.
- **Prueba contaminada por parche offline:** exigir `darkside status` limpio antes de cada prueba del DLL.
- **Conflicto entre mods:** orden explícito, nunca dependiente del filesystem.
- **Traversal o carga de código no deseada:** containment estricto y carpetas separadas para assets/plugins.
- **OBPK sin nombres:** fuera del MVP; requerirá mapear índices/hashes demostrados.

## 9. Criterios de finalización del primer hito

El primer hito termina únicamente cuando se cumpla todo lo siguiente. Al 11 de septiembre de 2026, los puntos marcados como confirmados ya tienen evidencia local:

- Confirmado: `dinput8.dll` x64 carga, conserva los seis exports y reenvía DirectInput al DLL de `System32`.
- Confirmado: el build del juego coincide con la huella soportada.
- Confirmado: MinHook y toda operación pesada se inicializan en el worker, fuera de `DllMain`.
- El hook pasivo identifica de forma estable el asset elegido.
- Confirmado: `first_test_mod` se indexa desde la ruta canónica correcta y queda listo para override.
- El log registra un hit de exactamente `4,224` bytes y el SHA-256 editado.
- El icono cambia visualmente sin modificar `media.upak`.
- Al deshabilitar el mod vuelve el original.
- No hay crashes reproducibles tras cargas repetidas y reinicios largos; el incidente aislado del PID `16272` queda explicado o suficientemente acotado con evidencia.
- El repositorio contiene instrucciones reproducibles, pruebas, licencias y créditos de todo código o conocimiento adaptado.

## 10. Orden inmediato recomendado

1. Arrancar el juego con el proxy desplegado y abrir la pantalla que muestra la habilidad pasiva de agilidad mejorada.
2. Confirmar `ASSET_REQUEST`, `ASSET_OVERRIDE_HIT` con `size=4224` y el cambio visual.
3. Retirar o renombrar de forma controlada el DDS del mod y comprobar visualmente el fallback al icono original; el evento de fallback temprano ya está confirmado.
4. Repetir cargas de menú/partida y reinicios completos de mayor duración para descartar carreras o problemas de vida útil y vigilar nuevos WER.
5. Si el buffer no llega a D3DX como se espera, usar la telemetría y las balizas de la fase 4 para seguirlo hasta el punto de transformación.
6. Investigar y validar la firma, ABI, hilo y propiedad del buffer del resolver interno general.
7. Sustituir o complementar el POC D3DX con el resolver general solo después de esa validación.

## 11. Prompt de contexto para retomar el proyecto

```text
Estoy construyendo un DLL x64 de modding para Darksiders II: Deathinitive
Edition. El DLL se carga como proxy dinput8.dll y debe hacer override virtual
de assets sueltos sin modificar los .upak.

Workspace:
C:\Users\vicen\Documents\Proyectos\Software\Darksiders2DLL

Juego:
C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition

Build soportado inicialmente:
Darksiders2.exe SHA-256
5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB

Extracciones:
C:\Users\vicen\Documents\Extractions\Darksiders

Herramientas locales:
- Darkstractor: C:\Users\vicen\Documents\Proyectos\Software\Darkstractor
- DarksideModManager: C:\Users\vicen\Documents\Proyectos\Software\DarksideModManager
- Anansi: C:\Users\vicen\Documents\Proyectos\Software\Anansi

Primer mod: first_test_mod
Fuente editada:
C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds

Virtual path canónico:
media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds

Destino runtime:
mods/first_test_mod/media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds

DDS: 4,224 bytes, 64x64, DXT5, SHA-256 editado
2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719

Está en media.upak, segmento media/ui/ui_icons_small., offset 0x33250800,
tamaño 0x52800, layout stream, miembro #72, offset descomprimido 0x4F800.
ReadFile es solo una baliza: el hook final debe operar después de la
descompresión/selección del miembro.

Estado actual al 11 de septiembre de 2026:
- Proyecto x64 implementado; Debug/Release usan /MT, MinHook está fijado y
  las pruebas de ambas configuraciones pasan. Smoke Release: 20/20.
- dinput8.dll Release SHA-256:
  8CE46A305F62BC862EEBE2B70D5A5D1ECF0EBE4ECA0F264DE3F1473383F3DBDA
- Proxy con seis exports y bootstrap worker programado desde DLL_PROCESS_ATTACH;
  DirectInput8Create vuelve a intentar programarlo si fuera necesario y las
  notificaciones de hilo permanecen activas para la CRT estática.
- Índice seguro de mods y POC exacto D3DX9/D3DX11 desplegados.
- Trampolines atómicos, enable de hooks en una sola cohorte, rollback
  indeterminate fail-closed, copia validada mediante ReadProcessMemory y
  eventos de primera entrada D3DX9/D3DX11.
- first_test_mod está instalado como archivo suelto; los .upak siguen intactos.
- Los logs confirman BUILD_SUPPORTED, MOD_INDEXED, ASSET_OVERRIDE_READY,
  TEXTURE_HOOK_ACTIVE y el ASSET_FALLBACK temprano con DDS ausente.
- Crash aislado PID 16272: c0000005 en Darksiders2.exe+0x9E553F; dump y WER
  conservados. La matriz posterior A/B/C (3 x 15 s por brazo) terminó 9/9
  estable, exit 0 y sin nuevos WER, por lo que no se atribuyó causalidad.
- Pendientes: ASSET_REQUEST/HIT, confirmación visual del reemplazo y fallback,
  reinicios largos y resolver general/ABI interna.
```

## 12. Créditos y procedencia

Los tres proyectos locales revisados usan licencia MIT. Si se copia o adapta código, conservar su aviso y registrar en `CREDITS.md` el archivo de origen y la adaptación concreta. También deben mantenerse los créditos transitivos de las especificaciones de formato que esos proyectos documentan.

Fuentes que deben quedar acreditadas cuando influyan en la implementación:

- Darkstractor y DarksideModManager (implementaciones locales de manifiesto/OBPK, catálogo y validación).
- Anansi, solo para futuras fases de modelos/OBP.
- Luigi Auriemma/QuickBMS/offzip y los scripts de Darksiders cuando se consulte su conocimiento de formato.
- LXIV-CXXVIII por DS2-RE y Darksiders-2-DLL-Loader si se usan firmas, direcciones o código.
- Tsuda Kageyu por MinHook.
- Microsoft por Windows SDK, DirectInput, Visual Studio/MSVC y vcpkg.
- x64dbg, Ghidra y sus respectivos autores/proyectos como herramientas de reversing.
