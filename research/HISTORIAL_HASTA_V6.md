# Darksiders2DLL

Proxy x64 de `dinput8.dll` y primer override interno de texturas para
**Darksiders II: Deathinitive Edition**. El código falla de forma cerrada si
`Darksiders2.exe` no coincide con la huella soportada y nunca modifica los
archivos `.upak`.

El primer caso controlado sustituye únicamente este recurso mientras el juego
lee su payload BC3 descomprimido:

```text
media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds
```

La sustitución exige que los 4096 bytes originales tengan SHA-256
`9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51` y los
reemplaza por el payload cuyo SHA-256 es
`DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F`. El DDS
fuente editado mide 4224 bytes y tiene SHA-256
`2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719`.

Este hito no es todavía un resolver general de archivos sueltos. No cubre
modelos, audio, materiales ni otros recursos, y no promete resolver rutas
arbitrarias.

Estado verificado el 14 de septiembre de 2026: el proyecto x64 está
implementado y `first_test_mod` está desplegado. El override no escribe en los
`.upak` y su contenido instalado coincide actualmente con el manifiesto oficial
de Steam. La copia Release estable validada en el juego tiene SHA-256
`2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`; las
variantes Debug se despliegan solo de forma transitoria para diagnóstico y se
reemplazan de nuevo por ese artefacto al terminar cada prueba.

La salida Release local reproducible tiene SHA-256
`D27D35C760BF7597DC579FF459A65E29E4FF56A4670DB00D5EB4ADD6CB3B0620` y mide
`617,472` bytes. Dos compilaciones limpias con directorios intermedios
independientes resultaron idénticas byte a byte, ambas pasaron la prueba de
humo y la batería offline Release terminó en `PASS`. Se usa `/LTCG` completo,
`/Brepro` y `PDBALTPATH` para evitar la variación
legal de código observada con FastLTCG incremental. Esta salida aún no está
desplegada ni validada dinámicamente y no debe confundirse con la copia estable.
La configuración Release excluye el catálogo y el dry-run general; cualquier
nuevo binario debe repetir las pruebas del juego antes de sustituir el
artefacto validado.

Steam reemplazó `media.upak` como archivo completo a las `05:10:29Z` durante
la jornada de pruebas, por lo que cambió su metadata histórica. Su contenido
actual sí está verificado: el SHA-1
`D267F6DBE96E90A58AE8E4981CAD47150D6AEA52` coincide exactamente con el campo
de contenido del manifiesto oficial
`388411_5667116516349422569.manifest`. Su SHA-256 local actual es
`C262236AB15E563F5539C33F11851537A4F021A5A3402B8C61FDD8975F3AE993`.
El escaneo independiente de DarksideModManager leyó los `1,681` segmentos y
`75,627` archivos sin error; su comando `status` no registra parches instalados.
La comparación completa con el mismo manifiesto dio también coincidencia para
`anim_streams.upak` (`909E63E2E5C1BCB51F5C62C3184F753A7DA70F58`) y
`maps.upak` (`C7457FAD94858720532913252ADABC57B685F621`).

Una sesión de aproximadamente 83 segundos registró `ASSET_OVERRIDE_HIT` para
el payload BC3 de 4096 bytes. Las estadísticas finales fueron
`target_hash_matches=1`, `replacements_applied=1`,
`replacement_write_failures=0`, `dropped=0` y `copy_failures=0`; no se generó
un nuevo `Application Error`. El usuario confirmó además visualmente en el
árbol de habilidades que aparece el icono editado. El log de esa sesión está
en:

```text
C:\Users\vicen\AppData\Local\Darksiders2DLL\logs\Darksiders2DLL-20260912-012822-20632-000.log
```

Un control posterior con esta misma DLL apartó temporalmente el DDS, obtuvo
`ASSET_FALLBACK target DDS is absent from the mod index` en
`Darksiders2DLL-20260912-061100-15492-000.log` y restauró el archivo con su
hash exacto. Un segundo arranque positivo reprodujo `ASSET_OVERRIDE_HIT` y
los contadores `1/1/0` en
`Darksiders2DLL-20260912-061242-12308-000.log`, sin `Application Error`. Tanto
este reinicio como el control de fallback fueron breves.

La ruta de rechazo se comprobó además con un archivo deliberadamente inválido.
El PID `26060` registró `MOD_INDEX_ISSUE code=invalid_dds` y después
`ASSET_FALLBACK` en `Darksiders2DLL-20260912-063656-26060-000.log`; no instaló
el override. El DDS válido se restauró inmediatamente con su SHA-256 exacto y
no quedó archivo temporal.

Un tercer arranque positivo, PID `11164`, mantuvo el proceso respondiendo unos
134 segundos en `Darksiders2DLL-20260912-061608-11164-000.log` y terminó con
el mismo hit único y todos los contadores de error en cero, sin
`Application Error`. Incluso esta sesión sigue siendo corta: el fallback
visual y las pruebas prolongadas continúan pendientes.

El punto interno usado es el RVA `0xABDC0`. Su ABI observada y confirmada es
`int32_t read(void* stream, void* destination, int32_t size)`: argumentos en
`RCX`, `RDX` y `R8D`, con el resultado en `EAX`. La ruta D3DX quedó desactivada
después de cinco minutos sin una sola entrada; no participa en el override
actual.

El siguiente escalón de investigación tiene un sondeo pasivo separado,
habilitable solo en Debug. Intercepta el método OBPK candidato en RVA
`0x9FEE5C`, correlaciona por hilo sus llamadas anidadas con la lectura de
`0xABDC0` y no decide reemplazos nuevos. El primer payload reprodujo `3/3` la
tupla `package_base=0x33250800`, `member_table_offset=0x3000` y
`read_ordinal=73` en los PIDs `19228`, `16968` y `21332`.

Una segunda ancla independiente,
`media/ui/ui_core/ui_icon_highlight.dds`, reprodujo también `3/3` la identidad
estática esperada en los PIDs `8692`, `20816` y `7064`:
`package_base=0x77800`, `member_table_offset=0x3800`, `read_ordinal=46`,
`requested=returned=4096` y SHA-256
`9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.

El prototipo Debug construye ya un catálogo inmutable desde `manifest.bin` y
los metadatos OBPK relevantes. En el PID `14792` tradujo la primera tupla a su
ruta canónica exacta como
`resolved_path=media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`;
la segunda ancla quedó
deliberadamente `unmapped` porque no existe en el índice de mods. Las pruebas
cubren además layouts `stream`, layouts por bloques no soportados, segmentos
vacíos, nombres inválidos y límites de recursos. Esa traducción alimenta ahora
el dry-run general y la opción de escritura separada, ambos solo en Debug.

El código integra ahora, detrás de `EnableGeneralResolverPrototype=true` y solo
en Debug x64, un dry-run que une cada identidad del catálogo con el asset DDS
ganador del `ModIndex`. Antes de publicar candidatos, abre el `media.upak`
instalado en modo lectura, infla de forma acotada una sola vez cada stream OBPK
solicitado y valida formato, dimensiones, mips, superficie y hashes del DDS
original y de su payload. Los layouts por bloques siguen excluidos de forma
cerrada hasta contar con evidencia runtime propia.

Para una lectura correlacionada exige el caller exterior `0x9FA980`, el retorno
interior `0x9FF0D2`, campos de identidad válidos, ambos objetos de stream no
nulos, destino escribible, retorno completo, tamaño exacto y el SHA-256 del
original recuperado del paquete. Publica
`GENERAL_DDS_VERIFIED_WOULD_OVERRIDE` si todo coincide, o
`GENERAL_DDS_CONTRACT_REJECTED` si un candidato falla el contrato; ambos
eventos declaran `buffer_writes=false`. Esta opción de observación conserva
el override exacto histórico de `first_test_mod`; la escritura general exige
la opción independiente descrita a continuación.

Los PIDs `18644` y `10616` validaron previamente el join por ruta y tamaño; el
segundo mantuvo dos candidatos durante más de nueve minutos con `2/2/0`. La
variante v5.1, PID `18148`, añadió la prueba criptográfica completa: recuperó
dos contratos de `media.upak` sin incidencias y produjo dos eventos
`GENERAL_DDS_VERIFIED_WOULD_OVERRIDE decision=payload`, con los hashes
`9726E7...A8FA` y `9F008D...EE51`. Terminó con
`general_candidate_hits=2`, `general_would_override=2`,
`general_contract_mismatches=0`, `general_source_hash_failures=0` y
`general_source_hash_mismatches=0`; el override exacto produjo además
`target_hash_matches=1`, `replacements_applied=1` y
`replacement_write_failures=0`. El usuario confirmó visualmente el icono
editado. La sesión permaneció respondiendo unos 4 min 44 s, no generó un nuevo
`Application Error` y terminó con los mismos contadores en cero.

Esta prueba reveló también una carrera de arranque reproducible: con las cuatro
firmas ejecutadas como código Debug sin optimizar, v5 tardó unos `2.35 s` en
activar los hooks y perdió la lectura temprana del icono. Se mantuvieron las
cuatro comprobaciones fail-closed y se optimizó únicamente el escáner; v5.1
activó la cohorte en unos `141 ms` y volvió a capturar y reemplazar el payload.
Después de la prueba se retiró el ancla pasiva y se restauró el proxy estable
`2E29A8...B629`; `first_test_mod` permaneció instalado y la metadata de los
`.upak` no cambió.

### Prototipo v6 de escritura general

`EnableGeneralResolverWritePrototype=true` habilita exclusivamente en Debug
la escritura general e incluye automáticamente identidad, catálogo, contratos
y zlib, incluso si las otras dos propiedades se pasan explícitamente como
`false`. Su valor predeterminado es `false`; activar solamente
`EnableGeneralResolverPrototype=true` sigue siendo observación.

Después de las mismas validaciones de identidad, contrato, destino, lectura
completa y SHA-256 original, selecciona los bytes inmutables del DDS completo
o del payload. `WriteProcessMemory` debe informar la copia completa; después
se vuelve a leer la memoria por fragmentos y se exige el SHA-256 del reemplazo.
Solo una copia completa y verificada produce `GENERAL_DDS_OVERRIDE_HIT` con
`replacement_written=true` y `replacement_verified=true`. Los fallos producen
`GENERAL_DDS_OVERRIDE_FAILED`: una copia parcial cuenta en
`general_write_failures`, y una copia completa cuyo hash posterior falla
cuenta en `general_write_verify_failures`. La detección posterior no revierte
bytes que Windows ya haya copiado.

Las pruebas offline Debug ejecutan el código real del detour dentro del
proceso de pruebas, sin instalar hooks. Cubren DDS completo/payload, rechazos
por identidad, destino, tamaños, lectura incompleta y hash, así como errores
de escritura, copia parcial y verificación posterior. La variante
`TestGeneralResolverWrite=false` prueba el mismo observador con buffers
intactos y los cuatro contadores de escritura en cero. La escritura general
v6 pasó además la prueba dinámica del 14 de septiembre y cuenta con una
captura del usuario que muestra el icono modificado.

Verificación v6 del 13 de septiembre de 2026: `PASS` offline en Debug write,
Debug observación y Release; siete DLL pasaron smoke (Debug predeterminado,
observación, write, identidad, write con las otras opciones en `false`, y dos
Release aislados). Los dos Release son idénticos byte a byte, conservan
`D27D35...B0620` y `617,472` bytes incluso solicitando todas las opciones del
prototipo. No contienen sus objetos, macros, zlib ni mensajes de diagnóstico;
conservan el evento histórico `GENERAL_RESOLVER_LIMITED` que describe solo
el override exacto. La inyección directa de macros con `NDEBUG` fue rechazada.

La DLL utilizada en la prueba temporal está en
`build/diagnostic-v6-general-write/dinput8.dll`, mide `2,362,880` bytes y tiene
SHA-256 `6DF8889D6EAC23D375852F12EFF8FE4CBBE4976372B220F289A1BCC09AC8A708`.
Su copia pasó smoke; incluye PDB y ejecutable de smoke. Tras la prueba se
restauró el proxy estable y esta DLL volvió a quedar solo como artefacto local.
La matriz y sus logs están en `build/validation-v6/`; se pueden regenerar con
`scripts/verify_general_resolver.ps1`, que no abre el juego ni modifica su
instalación. El proxy instalado sigue siendo `2E29A8...B629`, los `.upak`
conservan su metadata y solo permanece `mods/first_test_mod`.

La sesión v6, PID `8420`, activó los hooks `300 ms` después de
`SESSION_START` y produjo dos `GENERAL_DDS_OVERRIDE_HIT`: uno para el ancla
`ui_icon_highlight.dds` y otro para el icono de `first_test_mod`. Ambos
copiaron y verificaron `4096` bytes con `replacement_written=true`,
`replacement_verified=true` y `replacement_win32=0`. Los contadores finales
fueron `general_candidate_hits=2`, `general_would_override=2`,
`general_write_attempts=2`, `general_writes_completed=2`; todos los fallos de
contrato, hash, escritura y verificación quedaron en cero. El override exacto
registró cero coincidencias y copias porque el escritor general actuó primero.

El log abarca `10 min 52 s`, sin nuevos `Application Error` ni crash dumps
del juego. La captura aportada muestra la marca roja del icono editado en el
árbol de habilidades. Tras el cierre, sin intervención forzada, se retiró el
ancla y se restauró `2E29A8...B629`; los tres `.upak` conservaron tamaño y
fecha y solo quedó `first_test_mod`. El log original es
`Darksiders2DLL-20260914-125514-8420-000.log`; su copia, la captura y los
informes antes/después están en `build/dynamic-v6-20260914-125513/`.
La segunda textura conserva sus bytes originales: demuestra el camino de
copia y verificación para otra identidad, sin un segundo cambio visual.

## Compilar

Requisitos locales verificados: Visual Studio 2026 Community con MSVC v145,
Windows SDK y vcpkg. El manifiesto fija MinHook y zlib; ambas configuraciones
x64 usan la CRT estática `/MT`. zlib solo se enlaza en las pruebas offline y en
el extractor diagnóstico Debug habilitado expresamente.

En `DLL_PROCESS_ATTACH`, `DllMain` solo guarda el módulo y programa un worker
sin esperarlo. Las notificaciones de hilo permanecen activas porque se usa la
CRT estática `/MT`. El worker realiza después los logs, hashes, indexación e
inicialización de MinHook, ya fuera del *loader lock*. `DirectInput8Create`
vuelve a intentar programarlo si la primera creación del worker falla.

La activación interna está limitada por el hash del ejecutable soportado, el
RVA exacto y una firma única de código. El detour llama primero a la función
original y solo considera lecturas completas de 4224 o 4096 bytes. Copia el
rango de forma segura, exige el SHA-256 original exacto, comprueba que el
destino sea escribible y aplica el reemplazo en memoria. Un worker separado
registra la evidencia sin hacer I/O en el detour. Los hooks auxiliares de
`ReadFile` y `CloseHandle` aportan telemetría del segmento de `media.upak`.

En la instalación verificada existen a la vez las variables `PATH` y `Path`.
Esta invocación normaliza el entorno para evitar el error `MSB6001`:

```powershell
cmd.exe /d /s /c 'set PATH=& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" .\Darksiders2DLL.slnx /t:Build /p:Configuration=Release /p:Platform=x64 /m:1 /nr:false'
```

La salida de despliegue es `x64\Release\dinput8.dll`.

Ni el sondeo de identidad ni los prototipos general/write se incluyen en
Release. Para observar se usa `/p:EnableGeneralResolverPrototype=true`; para
escribir, `/p:EnableGeneralResolverWritePrototype=true`, siempre en Debug.
Ambas opciones incluyen sus dependencias sin otras propiedades. El sondeo
solo de identidad usa `/p:EnableResourceIdentityProbe=true`. Las macros del
prototipo también tienen una barrera de compilación que rechaza `NDEBUG` o
la ausencia de `DS2_DEBUG_BUILD`. Estas DLL son diagnósticas y no deben quedar
como proxy de uso normal.

## Pruebas sin abrir el juego

```powershell
.\x64\Release\offline_tests.exe `
  'C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds' `
  'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition\Darksiders2.exe' `
  'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition\mods'

$proxy = (Resolve-Path -LiteralPath .\x64\Release\dinput8.dll).Path
.\x64\Release\proxy_smoke.exe $proxy
```

La prueba offline valida también las dos anclas reales del catálogo, extrae y
verifica tres contratos DDS reales desde el paquete instalado, mantiene fuera
un OBPK real por bloques y cubre un segmento vacío, zlib truncado, límites y
fixtures corruptos. La prueba de humo comprueba los seis exports por nombre y
ordinal, la carga absoluta desde `System32` y una llamada real a
`DirectInput8Create`.

## Desplegar `first_test_mod` por primera vez

Con el juego cerrado, ejecutar PowerShell como administrador:

```powershell
.\scripts\deploy_first_test_mod.ps1 `
  -AssetSource 'C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds'
```

El script vuelve a comprobar el SHA-256 del juego y del DDS, ejecuta el smoke
test, exige una prueba aislada sin mods previos, rechaza reparse points y se
niega a pisar destinos existentes. Publica de forma atómica solamente estos
dos archivos y verifica que tamaño y fecha de los `.upak` no cambien:

```text
<juego>\dinput8.dll
<juego>\mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds
```

Los logs de cada sesión se crean en:

```text
%LOCALAPPDATA%\Darksiders2DLL\logs
```

Para actualizar únicamente un proxy ya desplegado, con el juego cerrado, se
debe proporcionar el hash exacto de la DLL instalada:

```powershell
.\scripts\update_deployed_proxy.ps1 `
  -ExpectedCurrentProxySha256 '<SHA-256_DE_LA_DLL_INSTALADA>'
```

El actualizador prueba el reemplazo, comprueba juego, asset y reparse points,
publica la DLL de forma atómica y revierte desde su respaldo si la verificación
final falla.

El override exacto y su resultado visual ya están validados y se reprodujeron
tras un reinicio. El fallback lógico también está validado con la DLL final.
También se validó el rechazo seguro de un DDS inválido. Quedan fuera de esta
prueba el fallback visual con el DDS retirado y los reinicios de larga duración.
La identidad ya se reprodujo con dos DDS y el catálogo tradujo la primera ruta;
el escritor general v6 ya tiene prueba dinámica con dos identidades y
evidencia visual del icono objetivo. Sigue siendo exclusivo de Debug y acotado
al contrato DDS documentado.

Consulta [PLAN_MAESTRO.md](PLAN_MAESTRO.md),
[research/ASSET_EVIDENCE.md](research/ASSET_EVIDENCE.md) y
[research/RESOLVER_RESEARCH.md](research/RESOLVER_RESEARCH.md) para los datos
del build, paquete y trabajo de ingeniería inversa pendiente.
