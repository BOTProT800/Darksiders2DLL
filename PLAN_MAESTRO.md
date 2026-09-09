# Plan maestro actualizado: DLL de modding para Darksiders II – Deathinitive Edition (PC)

> Estado verificado localmente: 8 de septiembre de 2026. Este documento sustituye el borrador especulativo anterior. Las rutas, hashes y offsets marcados como confirmados se obtuvieron de los archivos presentes en este equipo. No se ha modificado la instalación del juego durante esta revisión.

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

La instalación se detecta correctamente desde Steam. Actualmente no contiene un `dinput8.dll` local, no tiene carpeta `mods` y DarksideModManager informa que no hay modificaciones instaladas.

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

El workspace contiene todavía la plantilla básica de DLL de Visual Studio:

- C++20 y configuraciones Win32/x64.
- `Debug|x64` usa `/MT`.
- `Release|x64` no fija aún `/MT` y debe corregirse.
- `dllmain.cpp` no inicializa MinHook ni implementa el proxy.
- No hay `vcpkg.json` ni integración de MinHook verificable en el proyecto actual.
- El nombre de salida todavía no está configurado como `dinput8.dll`.

Por tanto, “MinHook ya instalado y configurado” deja de ser una suposición del plan y pasa a ser trabajo explícito de la fase de preparación.

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
       └─ Bootstrap seguro
            ├─ Huella/compatibilidad del ejecutable
            ├─ Logger
            ├─ Índice inmutable de mods
            ├─ Escáner de firmas
            └─ Hook del resolver
                 ├─ normalizar clave virtual
                 ├─ buscar override por prioridad
                 ├─ servir bytes con vida útil correcta
                 └─ fallback inmediato a la función original
```

### 5.1 Reparto propuesto del código

- `proxy_dinput8.*`: carga segura de la DLL del sistema y reenvío de exports.
- `bootstrap.*`: inicialización única y estado global mínimo.
- `game_build.*`: hash/identificación de la versión soportada.
- `logging.*`: log rotativo o por sesión, sin consola obligatoria en Release.
- `mod_index.*`: descubrimiento, prioridad y snapshot inmutable.
- `virtual_path.*`: normalización y validación de rutas.
- `signature_scan.*`: búsqueda acotada en `.text` y validación del candidato.
- `asset_hook.*`: typedef exacto, trampoline y lógica de fallback.
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

1. Registrar en el log la huella SHA-256 de `Darksiders2.exe` y rechazar hooks desconocidos por defecto.
2. Configurar salida x64 con nombre `dinput8.dll`.
3. Fijar `/MT` en Debug y Release x64; retirar Win32 del camino soportado aunque pueda seguir en la solución.
4. Añadir un manifiesto de vcpkg y MinHook estático compatible con el CRT elegido, o documentar otra integración reproducible.
5. Añadir `CREDITS.md` y conservar avisos/licencias si se adapta código MIT de los proyectos locales.
6. Compilar primero un proxy sin hooks.

Criterio de salida: el juego arranca, conserva teclado/control, crea un log identificable y sale limpiamente con el proxy presente.

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

Como control A/B opcional, antes del runtime hook se puede instalar temporalmente mediante DarksideModManager, comprobar que el icono correcto cambia y restaurar inmediatamente. Esa prueba debe hacerse con el juego cerrado y usando el flujo de respaldo del manager. Antes de probar la DLL, `darkside status` debe volver a indicar que no hay modificaciones instaladas.

Criterio de salida: el asset se reconoce visualmente en la situación de juego elegida, o se documenta con precisión cómo provocar su aparición.

### Fase 3 — Proxy `dinput8` y bootstrap seguro

1. Reenviar los seis exports del `dinput8.dll` de Windows:
   - `DirectInput8Create`
   - `DllCanUnloadNow`
   - `DllGetClassObject`
   - `DllRegisterServer`
   - `DllUnregisterServer`
   - `GetdfDIJoystick`
2. Resolver la DLL real mediante una ruta absoluta obtenida con `GetSystemDirectoryW`; nunca llamar a `LoadLibrary("dinput8.dll")` desde la carpeta del juego, porque recargaría el proxy.
3. Mantener `DllMain` mínimo: guardar el `HMODULE` y llamar a `DisableThreadLibraryCalls`. No inicializar MinHook, recorrer carpetas, abrir logs complejos ni esperar hilos bajo el *loader lock*.
4. Ejecutar la inicialización una sola vez fuera de `DllMain`, de forma perezosa desde el primer `DirectInput8Create` o desde un punto equivalente ya fuera del *loader lock*.
5. En terminación normal del proceso, permitir que Windows recupere recursos; cualquier desinstalación explícita de hooks deberá ocurrir fuera de `DllMain`.

Criterio de salida: todas las exportaciones resuelven, DirectInput sigue funcionando y 20 ciclos de arranque/salida no producen crash ni cuelgue.

### Fase 4 — Localizar el punto correcto de hook

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

Estas APIs son ayudas de instrumentación y, como mucho, un POC exclusivo de texturas. No son el hook general definitivo.

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
ASSET_REQUEST media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds
ASSET_OVERRIDE_HIT first_test_mod ... size=4224 sha256=2C0D...
ASSET_FALLBACK <ruta> <motivo>
```

Criterio de salida: con el archivo presente solo las solicitudes exactas generan `ASSET_OVERRIDE_HIT`; al retirarlo se obtiene fallback limpio y no cambia el comportamiento original.

### Fase 6 — Prueba end-to-end de `first_test_mod`

1. Confirmar que DarksideModManager no tiene parches instalados.
2. Verificar nuevamente el SHA-256 del archivo editado.
3. Copiarlo —no moverlo— a la ruta absoluta del apartado 3. Como la instalación está bajo `Program Files (x86)`, el despliegue puede requerir elevación; la herramienta de build no debe ocultar ese fallo ni escribir parcialmente.
4. Instalar el proxy compilado como `<juego>\dinput8.dll` y conservar fuera del juego los símbolos PDB.
5. Arrancar el juego y provocar la aparición del icono de agilidad mejorada.
6. Confirmar las dos señales de éxito:
   - El log muestra la ruta canónica y `first_test_mod` como origen.
   - El icono cambia visualmente.
7. Renombrar temporalmente el DDS o desactivar el mod y confirmar fallback al original.
8. Repetir cargas de menú/partida y varios reinicios para detectar vida útil incorrecta, cachés o carreras.

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

- Vanilla sin proxy.
- Proxy, hooks desactivados.
- Hook pasivo con logging.
- `first_test_mod` habilitado.
- Mod deshabilitado/archivo ausente.
- Asset inválido: debe rechazarse y caer al original.
- Varias cargas y cambios de zona/menú.

## 8. Riesgos y mitigaciones

- **Hook demasiado bajo:** `ReadFile` entrega el stream comprimido compartido. Usarlo solo para rastrear; hookear selección/entrega del miembro.
- **Firma o ABI incorrectas:** validar registros, stack y varios call sites antes de escribir bytes.
- **Puntero con vida útil insuficiente:** modelar propiedad del buffer y mantener almacenamiento estable.
- **Trabajo bajo loader lock:** `DllMain` mínimo; inicialización y teardown fuera de él.
- **Actualización del juego:** huella obligatoria y fail-closed sin instalar hooks desconocidos.
- **Patrón falso positivo:** sección acotada, contexto y coincidencia única.
- **Ruta runtime sin prefijo `media/`:** registrar el valor crudo y transformarlo solo mediante una regla demostrada; la convención en disco seguirá siendo canónica `media/...`.
- **DDS exportado con cabecera diferente:** el preflight ya confirma compatibilidad; normalizar una copia solo si aparece un fallo reproducible.
- **Prueba contaminada por parche offline:** exigir `darkside status` limpio antes de cada prueba del DLL.
- **Conflicto entre mods:** orden explícito, nunca dependiente del filesystem.
- **Traversal o carga de código no deseada:** containment estricto y carpetas separadas para assets/plugins.
- **OBPK sin nombres:** fuera del MVP; requerirá mapear índices/hashes demostrados.

## 9. Criterios de finalización del primer hito

El primer hito termina únicamente cuando se cumpla todo lo siguiente:

- `dinput8.dll` x64 carga y reenvía DirectInput sin regresiones.
- El build del juego coincide con la huella soportada.
- MinHook se inicializa fuera de `DllMain`.
- El hook pasivo identifica de forma estable el asset elegido.
- `first_test_mod` se indexa desde la ruta canónica correcta.
- El log registra un hit de exactamente `4,224` bytes y el SHA-256 editado.
- El icono cambia visualmente sin modificar `media.upak`.
- Al deshabilitar el mod vuelve el original.
- No hay crashes tras cargas repetidas y reinicios.
- El repositorio contiene instrucciones reproducibles, pruebas, licencias y créditos de todo código o conocimiento adaptado.

## 10. Orden inmediato recomendado

1. Corregir la configuración x64 (`/MT` Release, nombre `dinput8`, vcpkg/MinHook reproducible).
2. Implementar y probar solo el proxy con los seis exports.
3. Añadir fingerprint y logging.
4. Implementar el índice seguro de `mods\` y preparar `first_test_mod` sin instalarlo todavía.
5. Hacer instrumentación pasiva usando los offsets conocidos y las balizas D3DX.
6. Confirmar firma, ABI, hilo y propiedad del buffer.
7. Activar el override únicamente para el DDS de prueba.
8. Ejecutar la matriz end-to-end y, después, generalizar.

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

Estado actual: [DESCRIBIR FASE, HALLAZGOS, RVA/FIRMA Y ÚLTIMO LOG]
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
