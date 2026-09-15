> Actualización 0.4.0 (2026-09-14): se retiró la dependencia del caso especial y
> el escritor exacto. Debug y Release usan el motor general validado por contrato
> y hash, con configuración INI, permiso de activación y bloqueo tras fallos.
> El paquete preliminar y su guía se describen en README y distribution/INSTALACION.md.
> Las restricciones Debug-only y los switches de prototipo descritos más abajo
> son históricos. La validación visual v6 no valida automáticamente este Release.

# Plan maestro actualizado: DLL de modding para Darksiders II – Deathinitive Edition (PC)

> Estado verificado localmente: 14 de septiembre de 2026. Este documento sustituye el borrador especulativo anterior. Las rutas, hashes y offsets marcados como confirmados se obtuvieron de los archivos presentes en este equipo. El proxy estable y `first_test_mod` están desplegados. El escritor general Debug v6 produjo dos copias verificadas y la captura del usuario muestra el icono editado; al terminar la prueba se retiró el ancla temporal y se restauró el proxy estable. Los `.upak` conservaron su metadata durante la sesión; su contenido se había verificado contra el manifiesto oficial de Steam.

## 1. Objetivo

Construir una DLL x64 que se instale como proxy `dinput8.dll` junto a `Darksiders2.exe` y actúe como framework de modding en tiempo de ejecución.

El resultado final deberá:

1. Reenviar correctamente DirectInput al `dinput8.dll` real de Windows.
2. Inicializar el framework fuera del *loader lock* de `DllMain`.
3. Descubrir mods de archivos sueltos bajo `mods/<id_mod>/`.
4. Interceptar de forma segura el flujo interno de lectura del motor y sustituir primero un recurso reconocido por tamaño y hash exactos; después, localizar un resolver general que conserve la identidad virtual de cada recurso.
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

La instalación se detecta correctamente desde Steam. Actualmente contiene el proxy local `dinput8.dll` y el DDS bajo `mods\first_test_mod\...`. `darkside status` no registra modificaciones permanentes; por sí solo ese estado no demuestra integridad, pero `darkside scan` leyó correctamente `1,681` segmentos y `75,627` archivos. Los `.upak` conservaron tamaño y fecha durante cada operación controlada de despliegue. Más tarde Steam reemplazó `media.upak` como archivo completo a las `05:10:29Z`, tras una validación que lo encontró ausente y no corrupto; por eso cambió su metadata histórica fuera de esas operaciones. La comparación SHA-1 completa contra el contenido del manifiesto oficial `388411_5667116516349422569.manifest` dio coincidencia para los tres paquetes: `anim_streams.upak=909E63E2E5C1BCB51F5C62C3184F753A7DA70F58`, `maps.upak=C7457FAD94858720532913252ADABC57B685F621` y `media.upak=D267F6DBE96E90A58AE8E4981CAD47150D6AEA52`. El SHA-256 local actual de `media.upak` es `C262236AB15E563F5539C33F11851537A4F021A5A3402B8C61FDD8975F3AE993`.

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
- Hook interno exacto en RVA `0xABDC0`, protegido por huella de build, firma única y validación criptográfica del payload antes de escribir el reemplazo.
- ABI comprobada: `int32_t read(void* stream, void* destination, int32_t size)`, con `stream` en `RCX`, destino en `RDX`, tamaño en `R8D` y resultado en `EAX` conforme a la ABI x64 de Microsoft.
- Captura segura del rango devuelto, SHA-256 portátil sin asignaciones en el detour, comprobación de memoria escribible y sustitución únicamente cuando coinciden los `4,096` bytes BC3 originales exactos.
- Propiedad estable del DDS editado mediante almacenamiento compartido fijado durante la vida del hook; cualquier tamaño, hash, memoria o build inesperados producen passthrough.
- Cohorte de hooks de diagnóstico para `ReadFile`, `CloseHandle` y la función interna, con colas/eventos acotados y logging fuera del camino crítico.
- Catálogo inmutable de identidades de paquete, construido offline desde `manifest.bin` y las tablas META nombradas de OBPK. Se compila únicamente en la variante diagnóstica Debug solicitada expresamente; no cambia la decisión de reemplazo del Release.
- Dry-run general, también exclusivo de Debug y opt-in, que recupera contratos DDS originales de `media.upak`, los une con la identidad de paquete y el asset ganador del `ModIndex`, y exige el SHA-256 runtime exacto antes de emitir `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE` o `GENERAL_DDS_CONTRACT_REJECTED`, siempre con `buffer_writes=false`. Está probado offline y dinámicamente con dos DDS de segmentos independientes; nunca copia bytes generales.
- Escritor general v6 separado mediante `EnableGeneralResolverWritePrototype=true`, desactivado por defecto y exclusivo de Debug. Valida el mismo contrato antes de escribir y el hash del reemplazo después; copias parciales y fallos de verificación están cubiertos offline. El PID `8420` produjo dos copias verificadas sin fallos y la captura del usuario muestra el icono modificado; el proxy estable volvió a instalarse al terminar.
- La antigua prueba de concepto D3DX9/D3DX11 se conserva como evidencia histórica, pero está desactivada: sus hooks estuvieron activos sin recibir la textura durante una observación de cinco minutos y ya no forman parte de la ruta final del primer override.
- Pruebas offline y de humo del proxy para Debug/Release; el proxy Release superó `20/20` ciclos consecutivos de la prueba de humo.

El binario Release estable validado tiene SHA-256 `2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629` y se conserva como artefacto de despliegue. La salida Release local reproducible posterior mide `617,472` bytes y tiene SHA-256 `D27D35C760BF7597DC579FF459A65E29E4FF56A4670DB00D5EB4ADD6CB3B0620`; dos builds limpios con directorios intermedios independientes fueron idénticos byte a byte, pasaron el smoke test y la batería offline Release terminó en `PASS`. La configuración usa `/LTCG` completo, `/Brepro` y `PDBALTPATH` para eliminar la variación legal observada con FastLTCG incremental. No está desplegada ni tiene validación dinámica, y debe repetir la batería del juego antes de sustituir el artefacto estable.

El bootstrap se programa desde `DLL_PROCESS_ATTACH` mediante un worker y se vuelve a intentar desde `DirectInput8Create` si la programación inicial falla. `DllMain` no abre logs, no calcula hashes, no recorre carpetas, no inicializa MinHook y no espera al worker: únicamente guarda el módulo y solicita la creación asíncrona. Las notificaciones de hilo permanecen activas porque ambas configuraciones enlazan la CRT estática `/MT`. El trabajo real comienza después de que el loader libere su bloqueo.

La ejecución real del override interno, PID `20632`, quedó registrada en `%LOCALAPPDATA%\Darksiders2DLL\logs\Darksiders2DLL-20260912-012822-20632-000.log`. El log contiene `BUILD_SUPPORTED`, `MOD_INDEXED first_test_mod`, `INTERNAL_ASSET_OVERRIDE_ACTIVE`, `STREAM_READ_TARGET_MATCH` y `ASSET_OVERRIDE_HIT`. Los contadores finales relevantes fueron `target_hash_matches=1`, `replacements_applied=1` y `replacement_write_failures=0`, sin eventos descartados ni fallos de copia. La sesión permaneció estable aproximadamente 83 segundos y no produjo un nuevo `Application Error`. La captura aportada por el usuario confirma que el icono editado aparece en el árbol de habilidades. Un segundo arranque positivo, PID `12308`, reprodujo el hit y los mismos contadores sin `Application Error`. Entre ambos se ejecutó el control PID `15492` con el DDS temporalmente ausente: el DLL final emitió `ASSET_FALLBACK` y el archivo fue restaurado inmediatamente con su hash exacto. Un tercer positivo, PID `11164`, mantuvo el proceso respondiendo unos 134 segundos con el mismo hit único, todos los contadores de error en cero y sin `Application Error`.

El sondeo Debug de la siguiente capa confirmó dinámicamente el método OBPK en RVA `0x9FEE5C` y la lectura interna llamada en `0x9FF0CD` con retorno a `0x9FF0D2`. Los PIDs `19228`, `16968` y `21332` reprodujeron `3/3` la misma identidad para el payload objetivo: base de segmento `0x33250800`, desplazamiento `0x3000`, ordinal de lectura `73` y caller exterior `0x9FA980`. Los punteros variaron entre procesos y no se consideran identidad. El correlador es fijo, sin asignaciones, por hilo y fail-closed ante reentrada excesiva o cierres fuera de orden. Esta variante es exclusivamente diagnóstica y no forma parte de la DLL Release estable.

La regla se contrastó con un segundo DDS de otro segmento, `media/ui/ui_core/ui_icon_highlight.dds`. Los PIDs `8692`, `20816` y `7064` reprodujeron `3/3` la identidad `package_base=0x77800`, `member_table_offset=0x3800`, ordinal `46`, caller exterior `0x9FA980` y retorno `0x9FF0D2`; cada lectura pidió y devolvió `4096` bytes cuyo SHA-256 fue `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.

El prototipo Debug construyó además un catálogo inmutable desde el manifiesto y META para las rutas presentes en el índice de mods. En el PID `14792`, la identidad del primer objetivo resolvió exactamente a `media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`; la segunda ancla quedó `unmapped` de forma esperada porque `ui_icon_highlight.dds` no está en `mods`. Esto demuestra la correlación con dos segmentos y una traducción diagnóstica exacta para una ruta indexada, pero no un resolver general ni una nueva decisión de reemplazo. El Release permanece limitado al único objetivo exacto y conserva SHA-256 `2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`. Tampoco se ha repetido todavía una prueba visual nueva de fallback con la implementación interna ni una sesión de juego de cinco minutos.

El dry-run general está integrado detrás de `EnableGeneralResolverPrototype=true`, únicamente en Debug x64. Construye candidatos inmutables `identidad de paquete -> contrato DDS original -> asset ganador del ModIndex`. El contrato original se recupera del `media.upak` instalado mediante inflación zlib acotada y de solo lectura; valida formato, dimensiones, mips, superficie, tamaños y hashes de DDS/payload. En runtime solo evalúa lecturas correlacionadas por hilo entre el caller exterior `0x9FA980` y el retorno interior `0x9FF0D2`, con campos válidos y ambos objetos de stream no nulos. El objeto de paquete del alcance y el wrapper local son deliberadamente distintos; el callsite fijo en `0x9FF0C8` forma este último con `lea rcx,[rsp+70h]`. Exige además destino escribible, retorno completo, tamaño igual al DDS completo o a su payload y SHA-256 idéntico al original instalado. Registra `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE` o `GENERAL_DDS_CONTRACT_REJECTED`, siempre con `buffer_writes=false`; todavía no copia bytes generales del mod.

La ejecución PID `18644` produjo el primer `GENERAL_DDS_WOULD_OVERRIDE decision=payload` histórico para la identidad `0x33250800/0x3000/73`. Después se instaló temporalmente, en un mod diagnóstico aislado, una copia sin editar de `ui_icon_highlight.dds`. El PID `10616` construyó dos candidatos, rechazó cero y produjo dos eventos positivos: `0x77800/0x3800/46` y `0x33250800/0x3000/73`, ambos con `requested=returned=4096`, destino válido y `buffer_writes=false`. Durante más de nueve minutos conservó `general_candidate_hits=2`, `general_would_override=2`, `general_contract_mismatches=0`; el override exacto siguió `1/1/0`, sin drops, fallos de copia, overflow o reinicios de contexto. Una variante posterior añadió escaneo único y RVA exacto de los dos callsites completos antes de activar la cohorte.

La variante v5.1, PID `18148`, completó la fase de verificación del original. Extrajo dos contratos desde `media.upak` con cero incidencias y observó ambos payloads mediante `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE`: SHA-256 `9726E7...A8FA` para `0x77800/0x3800/46` y `9F008D...EE51` para `0x33250800/0x3000/73`. Los contadores quedaron `general_candidate_hits=2`, `general_would_override=2`, `general_contract_mismatches=0`, `general_source_hash_failures=0` y `general_source_hash_mismatches=0`; el override exacto volvió a registrar `1/1/0` y el usuario confirmó visualmente el icono editado. La sesión permaneció respondiendo unos 4 min 44 s y no produjo un nuevo `Application Error`. v5 había perdido esa lectura porque cuatro escaneos Debug sin optimizar tardaron unos `2.35 s`; manteniendo las cuatro firmas fail-closed, la optimización localizada del escáner redujo la activación a unos `141 ms`. Al terminar se retiró el ancla pasiva, se restauró el proxy estable `2E29A8...B629` y se comprobó que la metadata de los `.upak` no cambió. La evidencia habilita diseñar el write general, pero todavía no lo autoriza.

Durante la validación apareció un crash aislado en el PID `16272`: WER registró `0xc0000005` en `Darksiders2.exe+0x9E553F`. Se conservaron el dump `%LOCALAPPDATA%\CrashDumps\Darksiders2.exe.16272.dmp` y el `Report.wer` correspondiente bajo `C:\ProgramData\Microsoft\Windows\WER\ReportArchive`. El análisis posterior del `MINIDUMP_EXCEPTION_STREAM` confirmó una escritura en `0x0000000500000009`: la instrucción del juego era `lock xadd dword ptr [rcx+8], eax` y `RCX` contenía `0x0000000500000001`, compatible con un puntero inválido durante una liberación o actualización de conteo de referencias. El hilo de excepción conservado no contiene direcciones dentro de los rangos cargados de `dinput8.dll`, `d3dx11_43.dll` o `D3DX9_43.dll`, y el log de esa sesión registró únicamente la activación del POC D3DX, sin una entrada D3DX ni un reemplazo. Esto acota el fallo a estado inválido observado dentro del juego y a una variante histórica ya desactivada; no demuestra por sí solo ausencia de causalidad indirecta.

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
- SHA-256 de los `4,096` bytes BC3 originales, sin la cabecera DDS: `9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51`.
- FNV-1a 64 del payload BC3 original: `0x40A4729137EEC844`.
- SHA-256 de los `4,096` bytes BC3 editados: `DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F`.
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

La consecuencia técnica es importante: `ReadFile` ve un segmento OBPK comprimido compartido, no el DDS individual. Se usó como punto de rastreo, pero devolver un DDS suelto desde un hook de `ReadFile` corrompería el contrato esperado. El punto validado está después de descomprimir/seleccionar el miembro: la función interna en RVA `0xABDC0` devuelve de forma síncrona los `4,096` bytes BC3 al buffer del llamador, donde el reemplazo exacto puede aplicarse sin tocar el paquete.

### 3.3 Segunda ancla independiente

La ancla de contraste no forma parte de `first_test_mod` ni se usó como reemplazo editado. Se eligió para validar la identidad en otro segmento y más tarde se instaló temporalmente, sin cambios, bajo un mod diagnóstico aislado para el dry-run:

- Ruta virtual: `media/ui/ui_core/ui_icon_highlight.dds`.
- Archivo extraído: `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\ui_icon_highlight.dds`.
- Tamaño y formato: `4,224` bytes (`0x1080`), `64 × 64`, `DXT5/BC3`; el campo DDS `mipMapCount` vale `0`.
- SHA-256 del DDS: `7D7CA1D2B1A411DA28BA4071BB6D0A9AC54FFB5F2C0E608B28333A2D34EA3254`.
- SHA-256 del payload BC3 de `4,096` bytes: `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.
- Paquete/segmento: `media.upak`, `media/ui/ui_core.`, base `0x77800`, tamaño `0x811000`.
- Disposición: OBPK `stream`, tabla/payload en `0x3800`; el zlib comienza en `0x7B004`.
- Stream comprimido: `0x80D7FC`; stream descomprimido: `0x1B498C1`.
- Miembro: índice cero-basado `#45`, ordinal runtime uno-basado `46`, offset descomprimido `0x267E78`, tamaño `0x1080`, tipo OBPK `6`.

El miembro descomprimido desde el paquete coincide byte a byte con el archivo extraído. La copia temporal fue idéntica al original y el dry-run declaró `buffer_writes=false`; esta ancla sirve solo como evidencia de correlación, no está en `first_test_mod` y no habilita un segundo override.

### 3.4 Tercera ancla offline de tamaño distinto

Para evitar que las pruebas generales quedaran limitadas al contrato `4,224/4,096`, se verificó un tercer DDS real sin instalarlo como mod ni observarlo todavía en runtime:

- Ruta virtual: `media/ui/ui_core/icon_artifact.dds`.
- Archivo extraído: `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\icon_artifact.dds`.
- Tamaño y formato: `1,152` bytes (`0x480`), `32 × 32`, `DXT5/BC3`, DDS tradicional; el campo crudo `mipMapCount` vale `0` y se normaliza a un nivel.
- SHA-256 del DDS: `7BCD13C620C6249B7FB80E60B5237F42548C7D5F4EE10B40A04CABBC94E3CE10`.
- SHA-256 del payload BC3 de `1,024` bytes: `A1E8C36BBDDB387EAEF3F57BF8BEFA24FFDDF09C7C644FEEC3B42E4D1A13D2F7`.
- Paquete/segmento: el mismo `media/ui/ui_core.` de base `0x77800`; tabla/payload `0x3800`, miembro cero-basado `#142`, ordinal uno-basado `143`, offset descomprimido `0xB727B8`, tamaño `0x480`, tipo `6`.

Una descompresión directa y acotada del stream instalado produjo `0x1B498C1` bytes, como declara el OBPK, y el miembro coincide byte a byte con la extracción. La prueba C++ del catálogo confirma además la identidad `0x77800/0x3800/143`. Esta es evidencia estática/offline para un tamaño diferente, no una validación dinámica ni autorización para escribir desde el resolvedor general.

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
            ├─ Identificación única de la función interna en RVA 0xABDC0
            └─ Override interno exacto del payload BC3
                 ├─ llamar primero a la función original
                 ├─ exigir retorno/tamaño de 4,096 bytes y SHA-256 original
                 ├─ copiar el payload editado desde almacenamiento estable
                 └─ hacer passthrough ante cualquier discrepancia
```

El POC D3DX9/D3DX11 queda compilado como investigación histórica desactivada. Un prototipo Debug relaciona tuplas OBPK observadas con rutas virtuales presentes en el índice de mods mediante un catálogo inmutable; v6 ya utilizó ese contexto y los hashes originales para copiar y verificar dos payloads DDS. Falta ampliar los contratos soportados y repetir la matriz dinámica. El Release no incorpora ese comportamiento.

### 5.1 Reparto propuesto del código

- `proxy_dinput8.*`: carga segura de la DLL del sistema y reenvío de exports.
- `bootstrap.*`: inicialización única y estado global mínimo.
- `game_build.*`: hash/identificación de la versión soportada.
- `logging.*`: log rotativo o por sesión, sin consola obligatoria en Release.
- `mod_index.*`: descubrimiento, prioridad y snapshot inmutable.
- `virtual_path.*`: normalización y validación de rutas.
- `signature_scan.*`: búsqueda acotada en `.text`, validación del candidato y exigencia de una única coincidencia.
- `resolver_probe_filter.*`: clasificación y hashes portátiles del DDS/payload objetivo.
- `resolver_probe.*`: hooks de rastreo y override exacto en la función interna validada.
- `asset_hook.*`: POC histórico D3DX9/D3DX11, actualmente desactivado.
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

Como control offline opcional, el asset puede instalarse temporalmente mediante DarksideModManager y restaurarse inmediatamente usando su flujo de respaldo. No fue necesario contaminar la validación final: antes y después de la prueba del DLL, los paquetes siguieron sin modificaciones permanentes.

Criterio de salida: **cumplido**. El asset se reconoce en el árbol de habilidades y la captura del usuario confirma visualmente el icono editado durante la ejecución del override virtual.

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

**Estado: completada para el primer asset exacto; resolver general pendiente.** El rastreo de dos niveles (`ReadFile` sobre el segmento conocido y una función interna posterior) localizó el punto en el que el motor entrega el contenido BC3 ya seleccionado al buffer del llamador.

#### 4.1 Evidencia del rastreo

- `ReadFile` confirmó actividad asíncrona dentro de `media.upak` en el rango `0x33250800–0x332A3000`; `ERROR_IO_PENDING` fue el resultado esperado de esas lecturas overlapped.
- El hook interno observó una lectura síncrona de `4,096` bytes cuyo SHA-256 era exactamente `9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51`.
- Ese contenido coincide con el payload BC3 del miembro `#72`; no incluye los 128 bytes de la cabecera DDS.
- La pila observada incluyó llamadas del juego en `game+0x9FF0D2`, `game+0x9FA980`, `game+0x9F9E9A`, `game+0x9F39E3`, `game+0x9F34D3`, `game+0x9F2DAE`, `game+0xC3CBF` y `game+0xFFE07`.
- Un sondeo Debug posterior correlacionó el método RVA `0x9FEE5C` con esa lectura. Para el objetivo exacto produjo `3/3` la tupla estable `segment_base=0x33250800`, `member_table_offset=0x3000`, `read_ordinal=73`, caller exterior `0x9FA980` y retorno de lectura `0x9FF0D2`.
- La segunda ancla `media/ui/ui_core/ui_icon_highlight.dds` reprodujo también `3/3`, en los PIDs `8692`, `20816` y `7064`, la tupla `segment_base=0x77800`, `member_table_offset=0x3800`, `read_ordinal=46`, caller exterior `0x9FA980` y retorno `0x9FF0D2`. Las tres lecturas pidieron/devolvieron `4096` bytes con SHA-256 `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.
- Las balizas D3DX9/D3DX11 no recibieron esta textura durante cinco minutos; por ello el POC gráfico quedó desactivado y no se usa para el override final.

#### 4.2 Función interna validada

- RVA: `0xABDC0` respecto a la base de imagen `0x140000000`.
- ABI: `int32_t read(void* stream, void* destination, int32_t size)`.
- Registros de entrada: `RCX=stream`, `RDX=destination`, `R8D=size`; resultado en `EAX`.
- Firma única en el ejecutable soportado:

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57
41 56 41 57 48 83 EC 20 8B 71 14 45 8B F0 48 8B
FA 48 8B D9
```

La localización exige simultáneamente el SHA-256 soportado de `Darksiders2.exe`, el RVA esperado, una sección ejecutable y exactamente una coincidencia de la firma. Si cualquiera falla, el hook no se activa. El sondeo opcional de `0x9FEE5C` aplica las mismas compuertas y solo se compila al pedir expresamente `EnableResourceIdentityProbe=true` en Debug.

#### 4.3 Contrato demostrado y límites

El detour llama primero a la función original. Solo si esta devuelve exactamente el tamaño pedido, el tamaño es `4,096` o `4,224`, la copia segura puede leerse y su SHA-256 coincide con el original conocido, se considera el recurso objetivo. En el caso observado de `4,096` bytes, escribe únicamente el payload BC3 editado en el buffer ya proporcionado por el motor. No cambia el puntero, no altera el tamaño y conserva el comportamiento original para el resto de llamadas.

Esto valida la ABI y el contrato necesarios para `first_test_mod`. La correlación Debug ya demostró una identidad estable en dos segmentos y el catálogo prototipo tradujo la identidad del primer objetivo a su ruta indexada, pero la función no expone por sí sola una ruta y todavía no existe una forma general de sustituir cualquier solicitud.

Criterio de salida: **cumplido para el primer asset exacto**. El hook pasivo identificó el payload correcto y el mismo punto aplicó el reemplazo sin error durante una sesión estable. La generalización sigue siendo una fase posterior.

### Fase 5 — Implementar el override de archivos sueltos

**Estado: completada para el primer DDS exacto; adaptación DDS general probada en Debug.** El índice seguro se construye al inicio y el hook interno consume el snapshot estable. El Release identifica el primer recurso únicamente mediante tamaño y SHA-256 exactos. El escritor Debug v6 une identidad, ruta indexada y contrato original para decidir copias verificadas; no cubre solicitudes arbitrarias del motor.

1. Construir al inicio un snapshot inmutable de `mods\`; no consultar el disco en cada llamada del resolver.
2. Normalizar la identidad observada en runtime a la clave canónica `media/...`.
3. Buscar el primer override habilitado según un orden determinista.
4. Validar el archivo antes de publicarlo. Para DDS: magic, tamaño mínimo, dimensiones, formato, mipmaps y tamaño esperado.
5. Mantener los bytes en almacenamiento con vida útil suficiente. Nunca devolver un puntero a un `std::vector` temporal o a memoria liberada al salir del hook.
6. Respetar el contrato del motor. Para el punto demostrado, llamar al original y sustituir en el buffer de destino únicamente los bytes que este acaba de devolver; no cambiar punteros, tamaño ni valor de retorno.
7. Añadir guardia de reentrada y sincronización apropiada si el resolver se usa desde varios hilos.
8. Ante cualquier error, registrar el motivo y llamar a la función original sin alterar argumentos.

Eventos mínimos de log:

```text
BUILD_SUPPORTED
MOD_INDEXED first_test_mod
INTERNAL_ASSET_OVERRIDE_ACTIVE rva=0xABDC0
STREAM_READ_TARGET_MATCH form=bc3_payload size=4096 sha256=9F008D...
ASSET_OVERRIDE_HIT first_test_mod ... size=4096 replacement_bytes_sha256=DA59A8...
ASSET_FALLBACK <ruta> <motivo>
RESOURCE_IDENTITY_SAMPLE package_base=0x33250800 member_table_offset=12288 read_ordinal=73
```

Criterio de salida: **cumplido para el archivo presente**. Tres arranques positivos independientes produjeron una coincidencia y un reemplazo por proceso (`1/1/0`), sin drops ni fallos de copia, y el cambio fue visible en la primera sesión. El fallback temprano quedó validado también con la DLL interna final; solo falta comprobar visualmente que muestra el original.

### Fase 6 — Prueba end-to-end de `first_test_mod`

**Estado al 13 de septiembre de 2026: reemplazo positivo, fallback lógico y rechazo de DDS inválido completados; fallback visual y pruebas largas pendientes.** El proxy Release y el asset están desplegados sin modificar `.upak`. La ejecución PID `20632` produjo `STREAM_READ_TARGET_MATCH` y `ASSET_OVERRIDE_HIT`, con `target_hash_matches=1`, `replacements_applied=1`, `replacement_write_failures=0`, sin drops ni fallos de copia. Permaneció estable unos 83 segundos, no generó `Application Error`, y la captura del usuario confirma visualmente el icono editado. Los arranques PID `12308` y PID `11164` reprodujeron el hit con los mismos contadores; el último permaneció respondiendo unos 134 segundos. El control PID `15492`, ya con la DLL final, emitió `ASSET_FALLBACK` al apartar temporalmente el DDS; se restauró enseguida con su SHA-256 exacto. El PID `26060` rechazó un archivo deliberadamente inválido con `MOD_INDEX_ISSUE code=invalid_dds` y `ASSET_FALLBACK`; el DDS válido se restauró con su hash exacto y sin staging residual. La matriz corta A/B/C histórica terminó `9/9` estable, aunque todavía usaba el POC D3DX.

1. Confirmar que DarksideModManager no tiene parches instalados.
2. Verificar nuevamente el SHA-256 del archivo editado.
3. Copiarlo —no moverlo— a la ruta absoluta del apartado 3. Como la instalación está bajo `Program Files (x86)`, el despliegue puede requerir elevación; la herramienta de build no debe ocultar ese fallo ni escribir parcialmente.
4. Instalar el proxy compilado como `<juego>\dinput8.dll` y conservar fuera del juego los símbolos PDB.
5. Arrancar el juego y provocar la aparición del icono de agilidad mejorada. **Confirmado.**
6. Confirmar las dos señales de éxito. **Confirmadas:**
   - El log muestra la ruta canónica y `first_test_mod` como origen.
   - El icono cambia visualmente.
7. Renombrar temporalmente el DDS o desactivar el mod. **Fallback lógico confirmado con la DLL final:** el bootstrap emitió `ASSET_FALLBACK` y el DDS se restauró con su hash exacto. Falta confirmar visualmente que vuelve el original.
8. Repetir cargas de menú/partida y reinicios de mayor duración para detectar vida útil incorrecta, cachés o carreras.

No considerar éxito una modificación hecha simultáneamente dentro de `media.upak`: eso impediría saber si funcionó la DLL o el parche offline.

### Fase 7 — Generalizar y endurecer

**Estado: correlación, contratos originales, dry-run y primer escritor general Debug validados con dos anclas; ampliación y repetición dinámica pendientes.** `resource_identity.*` implementa un contexto POD, fijo y por hilo con pruebas de anidamiento, overflow, orden incorrecto, secuencias incompletas y límite de lecturas. La variante Debug añadió de forma transaccional el cuarto hook en RVA `0x9FEE5C`; tres reinicios conservaron la tupla del primer asset y otros tres la del segundo DDS situado en otro segmento.

`package_identity_catalog.*` construye un snapshot inmutable desde `manifest.bin` y las tablas META nombradas de los OBPK relevantes. Las pruebas sintéticas y reales cubren layout `stream`, rechazo seguro de `blocks` y solicitud vacía. En el PID `14792`, el catálogo resolvió la identidad del primer objetivo a su ruta canónica exacta; informó la segunda ancla como `unmapped` porque esa ruta no estaba presente en `mods`, que es el comportamiento acotado esperado. Todo esto permanece bajo opciones explícitas de Debug y no se ha convertido en una decisión de override. El Release estable no define el sondeo ni el catálogo general y conserva el reemplazo único por hash.

`package_dds_contract.*` agrupa las rutas DDS solicitadas por stream, abre `media.upak` sin permiso de escritura, infla zlib por fragmentos acotados y conserva únicamente los intervalos requeridos. Exige EOF zlib válido y tamaño declarado exacto, valida cada DDS y calcula SHA-256 completo y de payload; los layouts `blocks` permanecen excluidos. `general_dds_candidate.*` une ese contrato independiente con el catálogo y el asset ganador del `ModIndex`, rechazando diferencias de formato, dimensiones, mips, cabecera, superficie o tamaño. Su evaluador puro clasifica `full_dds`, `payload`, `short_read`, `size_mismatch`, `source_hash_mismatch`, `invalid` o `unmapped`. El hook solo encola un evento para candidatos mapeados y lecturas correlacionadas por `0x9FA980/0x9FF0D2`; copia de forma segura el rango en fragmentos fijos y exige el SHA-256 original antes de publicar `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE`. El PID `18148` confirmó dos candidatos de segmentos independientes con `2/2/0`, cero fallos o discrepancias de hash y `buffer_writes=false`. Esta fase demuestra el contrato de contenido, pero todavía no copia el reemplazo general.

Verificación offline v6, 13 de septiembre de 2026: Debug write y observación
ejecutan el detour real en pruebas sin instalar hooks. Pasaron DDS completo y
payload, ocho rechazos, cuatro fallos simulados de escritura/verificación y
preservación del retorno/`GetLastError`; observación dejó buffers intactos y
los cuatro contadores write en cero. Release offline también pasó. Las cuatro
variantes Debug, write con propiedades general/identity en `false` y dos
Release aislados pasaron smoke (`7/7`). Se corrigió la propagación de
dependencias cuando MSBuild recibe esas propiedades contradictorias y se
aisló la referencia del proyecto smoke al compilarlo por separado.

Los Release, uno predeterminado y otro con todas las opciones de prototipo
solicitadas, son idénticos byte a byte (`617,472` bytes,
`D27D35C760BF7597DC579FF459A65E29E4FF56A4670DB00D5EB4ADD6CB3B0620`).
La auditoría de objetos, órdenes de compilación/enlace y mensajes no encontró
prototipos ni zlib. Se conserva únicamente la etiqueta histórica
`GENERAL_RESOLVER_LIMITED` del override exacto. La barrera del encabezado
también rechazó macros forzadas con `NDEBUG`.

Artefacto empleado en la prueba dinámica v6:
`build/diagnostic-v6-general-write/dinput8.dll`, SHA-256
`6DF8889D6EAC23D375852F12EFF8FE4CBBE4976372B220F289A1BCC09AC8A708`,
`2,362,880` bytes, acompañado por PDB y smoke. Evidencia:
`build/validation-v6/artifacts.json`, logs `tests-*.run.log`,
`release-guard.log`, `release-byte-comparison.log` y `final-audit.log`.
Durante la verificación offline no se abrió el juego. Tras la prueba dinámica
posterior sigue instalado el proxy estable `2E29A8...B629`, solo
`first_test_mod` y la metadata de los `.upak` permanece intacta.

El 14 de septiembre de 2026, PID `8420`, v6 activó los hooks a `300 ms` de
`SESSION_START` y produjo dos `GENERAL_DDS_OVERRIDE_HIT` con
`replacement_written=true`, `replacement_verified=true`, tamaño `4096` y
error de escritura `0`. Quedaron `general_candidate_hits=2`,
`general_would_override=2`, `general_write_attempts=2`,
`general_writes_completed=2`, con todos los fallos de contrato, hash,
escritura/verificación en cero. El override exacto registró `0/0/0` porque
el general sustituyó antes el buffer objetivo. La captura aportada por el
usuario muestra el icono con la marca roja en el árbol de habilidades.

El intervalo de log abarca `652.093 s` (`10 min 52 s`), sin nuevos
`Application Error` ni crash dumps del juego. Tras el cierre sin intervención
forzada se retiró el ancla y se restauró el proxy estable a las
`13:06:12Z`. Se comprobó otra vez el hash estable, la metadata de los tres
`.upak` y que solo queda `first_test_mod`. Evidencia en
`build/dynamic-v6-20260914-125513/`: `session.log`,
`visual-confirmation.jpg`, `runtime-evidence.json` y `after.json`.
El ancla pasiva conserva su contenido original; solo el primer DDS demuestra
un cambio visual. No se infiere cobertura de otras texturas, formatos o builds.

Después del primer DDS:

- Definir `mods.json` o formato equivalente para `enabled` y prioridad explícita.
- Resolver conflictos con una regla documentada y visible en logs.
- Añadir caché negativo/positivo sin impedir que un nuevo escaneo explícito reconstruya el snapshot.
- Probar nombres Unicode, mayúsculas, `/`/`\` y rutas maliciosas.
- Añadir límites de tamaño y presupuesto de memoria.
- Separar configuración, logs, assets y plugins.
- Incorporar una API de plugins versionada solo después de estabilizar el loader de assets.
- Mantener como regresión las dos anclas DDS ya verificadas y probar después un asset no DDS cuyo contrato de carga sea conocido.
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
- Catálogo de identidad de paquetes con fixtures sintéticos `stream`/`blocks`, solicitud vacía y comprobaciones reales de las dos anclas sin escribir en la instalación.
- Join de candidatos DDS y evaluador de dry-run general con identidades mapeadas/no mapeadas, destino inválido, lecturas cortas, tamaños completo/payload y mismatches; ninguna prueba de esta capa escribe en el buffer del juego.

Los fixtures deben ser sintéticos o copias mínimas aisladas. No ejecutar pruebas que escriban en la instalación real.

### Integración del proxy

- Comprobar con `dumpbin /exports` los seis exports.
- Verificar que la DLL del sistema se cargó desde `System32` y no recursivamente desde el juego.
- Probar con MinHook desactivado, patrón ausente y mod ausente.
- Probar arranque/salida repetidos.
- Verificar que la huella del DLL compilado coincide con la desplegada: `2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`.
- Comprobar que cada operación controlada de despliegue no cambia tamaño ni fecha de los `.upak`; si Steam reemplaza un paquete fuera de esa operación, verificar su contenido completo contra el manifiesto oficial en vez de inferir integridad por metadata.

### En el juego

- Confirmado en matriz corta: vanilla sin proxy, tres ejecuciones de 15 segundos.
- Confirmado en matriz corta: proxy con DDS ausente, fallback temprano y sin inicializar MinHook, tres ejecuciones de 15 segundos.
- Confirmado históricamente en matriz corta: proxy con DDS presente y hooks D3DX9/D3DX11 activos, tres ejecuciones de 15 segundos. Ese POC está ahora desactivado.
- Confirmado con la ruta interna final: hook pasivo, coincidencia exacta del payload y logging sin drops ni fallos de copia.
- Confirmado con `first_test_mod` habilitado: `ASSET_OVERRIDE_HIT` y cambio visual en la pantalla objetivo.
- Pendiente: mod deshabilitado/archivo ausente con comprobación visual del fallback.
- Confirmado lógicamente con PID `26060`: un asset inválido se omite del índice,
  emite `MOD_INDEX_ISSUE code=invalid_dds` y cae a `ASSET_FALLBACK` sin instalar
  el override. La comprobación visual del original sigue cubierta por el mismo
  pendiente de fallback visual.
- Varias cargas prolongadas y cambios de zona/menú.

La matriz corta histórica completa terminó `9/9` con exit code `0` y sin nuevos WER. Las tres pruebas positivas de la ruta interna reprodujeron el hit; la más larga duró unos 134 segundos, permaneció respondiendo y no produjo `Application Error`. El crash aislado del PID `16272` permanece documentado y no se considera explicado por esas ausencias de reproducción.

## 8. Riesgos y mitigaciones

- **Hook demasiado bajo:** `ReadFile` entrega el stream comprimido compartido. Usarlo solo para rastrear; hookear selección/entrega del miembro.
- **Firma o ABI incorrectas:** para la build soportada ya se validaron el RVA `0xABDC0`, la ABI y una firma única. Mantener la triple compuerta huella/RVA/firma y fallar cerrado ante cualquier diferencia.
- **Puntero con vida útil insuficiente:** modelar propiedad del buffer y mantener almacenamiento estable.
- **Trabajo bajo loader lock:** `DllMain` solo guarda el módulo y programa el worker sin esperarlo; logs, hashes, índice, MinHook y teardown se ejecutan fuera de él. `DirectInput8Create` permite reintentar la programación. Las notificaciones de hilo permanecen activas por compatibilidad con la CRT estática.
- **Activación parcial de hooks:** publicar los trampolines de forma atómica, encolar la cohorte de `ReadFile`, `CloseHandle` y lectura interna y aplicarla una sola vez; si la instalación no queda íntegra, descartar el reemplazo y fallar cerrado.
- **Buffer fuente inválido o cambiante:** validar el rango, copiar de forma segura los bytes devueltos y exigir el SHA-256 exacto antes de escribir; una copia fallida, rango no escribible o hash diferente hace passthrough. La ejecución positiva registró cero fallos de copia y cero fallos de escritura.
- **Crash aislado sin causalidad demostrada:** el dump identifica una escritura de `Darksiders2.exe+0x9E553F` sobre `0x0000000500000009`, mediante un puntero de conteo de referencias inválido; la pila capturada no contiene direcciones del proxy ni de D3DX. Conservar dump y WER, comparar configuraciones A/B/C y exigir pruebas largas antes de cerrar el riesgo. La matriz corta posterior y las ejecuciones posteriores del resolvedor interno fueron estables, pero no prueban por sí solas que el incidente histórico sea ajeno al POC.
- **Actualización del juego:** huella obligatoria y fail-closed sin instalar hooks desconocidos.
- **Patrón falso positivo:** sección ejecutable acotada, RVA esperado, firma completa y coincidencia única.
- **Ruta runtime sin prefijo `media/`:** registrar el valor crudo y transformarlo solo mediante una regla demostrada; la convención en disco seguirá siendo canónica `media/...`.
- **DDS exportado con cabecera diferente:** el preflight ya confirma compatibilidad; normalizar una copia solo si aparece un fallo reproducible.
- **Prueba contaminada por parche offline:** exigir `darkside status` limpio antes de cada prueba del DLL.
- **Conflicto entre mods:** orden explícito, nunca dependiente del filesystem.
- **Traversal o carga de código no deseada:** containment estricto y carpetas separadas para assets/plugins.
- **OBPK sin nombres:** fuera del MVP; requerirá mapear índices/hashes demostrados.

## 9. Criterios de finalización del primer hito

El primer hito termina únicamente cuando se cumpla todo lo siguiente. Al 13 de septiembre de 2026, los puntos marcados como confirmados ya tienen evidencia local:

- Confirmado: `dinput8.dll` x64 carga, conserva los seis exports y reenvía DirectInput al DLL de `System32`.
- Confirmado: el build del juego coincide con la huella soportada.
- Confirmado: MinHook y toda operación pesada se inicializan en el worker, fuera de `DllMain`.
- Confirmado: el rastreo pasivo identificó primero el payload BC3 y el hook activo conserva esa identificación exacta mediante tamaño y SHA-256.
- Confirmado: `first_test_mod` se indexa desde la ruta canónica correcta y queda listo para override.
- Confirmado: el log registra un hit de exactamente `4,096` bytes, verifica el SHA-256 original `9F008D...EE51` y aplica el payload editado `DA59A8...077F`.
- Confirmado: el icono cambia visualmente sin modificar `media.upak`.
- Confirmado: al apartar el DDS, el DLL final registra `ASSET_FALLBACK`, no instala el hook interno y el archivo se restaura con su hash exacto.
- Confirmado: un DDS inválido se rechaza durante la indexación, produce `ASSET_FALLBACK` y el archivo válido puede restaurarse sin residuos.
- Confirmado para investigación Debug: una segunda ancla DDS de otro segmento reprodujo `3/3` su base, offset de tabla y ordinal, y el catálogo inmutable resolvió la primera identidad a la ruta canónica del mod.
- Confirmado offline para investigación Debug: el join `catálogo -> ModIndex` y el evaluador general clasifican candidatos DDS de forma acotada y mantienen `buffer_writes=false`.
- Pendiente: comprobar visualmente que ese fallback vuelve al icono original.
- Pendiente: completar una sesión de juego de cinco minutos, cargas repetidas y reinicios largos; el incidente aislado del PID `16272` debe quedar explicado o suficientemente acotado con evidencia.
- Confirmado en Debug: el dry-run v5.1 observó `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE` para dos identidades de segmentos independientes, con SHA-256 runtime exacto, `buffer_writes=false`, `2/2/0` y cero fallos o discrepancias de hash.
- Confirmado en Debug: los DDS originales se extraen de forma acotada de cada layout `stream` solicitado, se validan y conservan hashes completos y de payload; `blocks` continúa fail-closed.
- Confirmado: el repositorio contiene instrucciones reproducibles, pruebas, licencias y créditos del código o conocimiento adaptado hasta este hito.

## 10. Orden inmediato recomendado

1. Retirar o renombrar de forma controlada el DDS del mod y comprobar visualmente que vuelve el icono original con el DLL final; restaurarlo después sin tocar los `.upak`.
2. Repetir cargas de menú/partida y reinicios completos de mayor duración para buscar carreras o problemas de vida útil y vigilar nuevos WER. V6 ya registra una sesión de más de diez minutos; eso no sustituye las repeticiones ni demuestra toda la navegación.
3. Mantener como regresión el dry-run Debug criptográficamente validado con más de un recurso y `buffer_writes=false`.
4. Conservar la sesión v6 como primera regresión positiva del escritor general: dos copias completas y verificadas, captura del icono editado y restauración posterior del proxy estable. Repetirla y ampliar la matriz negativa antes de trasladar este comportamiento a Release.
5. Mantener `blocks` fuera hasta obtener evidencia dinámica específica de su contrato de entrega y ampliar después la matriz negativa antes de admitir otros formatos.
6. Mantener el POC D3DX desactivado salvo como instrumento diagnóstico explícito.

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
ReadFile fue solo una baliza. El hook final opera después de la
descompresión/selección del miembro, en la función interna RVA 0xABDC0.

Estado actual al 13 de septiembre de 2026:
- Proyecto x64 implementado; Debug/Release usan /MT, MinHook está fijado y
  las pruebas de ambas configuraciones pasan. Smoke Release: 20/20.
- dinput8.dll Release estable validado y desplegado, SHA-256:
  2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629
- La salida Release local reproducible es D27D35C760BF7597DC579FF459A65E29E4FF56A4670DB00D5EB4ADD6CB3B0620
  (617472 bytes); dos builds limpios fueron idénticos, pasaron smoke y la
  batería offline Release, pero no
  está desplegada ni validada dinámicamente.
- Proxy con seis exports y bootstrap worker programado desde DLL_PROCESS_ATTACH;
  DirectInput8Create vuelve a intentar programarlo si fuera necesario y las
  notificaciones de hilo permanecen activas para la CRT estática.
- Índice seguro de mods y override interno exacto desplegados.
- Hook en RVA 0xABDC0; ABI comprobada:
  int32_t read(void* stream, void* destination, int32_t size).
- La localización exige build soportada, RVA exacto y firma única:
  48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57
  48 83 EC 20 8B 71 14 45 8B F0 48 8B FA 48 8B D9.
- Payload BC3 original de 4096 bytes:
  SHA-256 9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51,
  FNV-1a 64 0x40A4729137EEC844.
- Payload BC3 editado SHA-256:
  DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F.
- DDS fuente de 4224 bytes SHA-256:
  2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719.
- first_test_mod está instalado como archivo suelto; los .upak siguen intactos.
- Ejecución positiva PID 20632, log
  Darksiders2DLL-20260912-012822-20632-000.log: ASSET_OVERRIDE_HIT,
  target_hash_matches=1, replacements_applied=1,
  replacement_write_failures=0, sin drops ni fallos de copia.
- La sesión duró unos 83 s sin Application Error y la captura del usuario
  confirma visualmente el icono editado. El override no escribió los .upak;
  Steam reemplazó después media.upak como archivo completo, por lo que su
  metadata cambió, y el contenido actual coincide con el manifiesto oficial.
- Un segundo positivo, PID 12308 y log
  Darksiders2DLL-20260912-061242-12308-000.log, reprodujo el hit con 1/1/0.
- Un tercer positivo, PID 11164 y log
  Darksiders2DLL-20260912-061608-11164-000.log, reprodujo el mismo hit y
  permaneció respondiendo unos 134 s; sigue siendo una prueba corta.
- El control de archivo ausente, PID 15492 y log
  Darksiders2DLL-20260912-061100-15492-000.log, emitió ASSET_FALLBACK; el DDS
  se restauró inmediatamente con su SHA-256 exacto y sin staging residual.
- El control de DDS inválido, PID 26060 y log
  Darksiders2DLL-20260912-063656-26060-000.log, emitió MOD_INDEX_ISSUE
  code=invalid_dds y ASSET_FALLBACK; el archivo válido volvió a su ruta con su
  SHA-256 exacto y sin staging residual.
- El sondeo de identidad Debug, firma/RVA `0x9FEE5C`, reprodujo en los PIDs
  19228, 16968 y 21332 la tupla `0x33250800 + 0x3000`, ordinal 73, caller
  exterior `0x9FA980` y lectura `0x9FF0D2` para el SHA-256 original.
- La segunda ancla media/ui/ui_core/ui_icon_highlight.dds reprodujo 3/3 en los
  PIDs 8692, 20816 y 7064 la tupla `0x77800 + 0x3800`, ordinal 46, con
  petición/retorno de 4096 bytes y SHA-256
  9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA.
- El catálogo inmutable Debug construido desde manifest/META pasó pruebas
  sintéticas y reales de stream, blocks y solicitud vacía. En el PID 14792
  resolvió la primera identidad a la ruta exacta del mod; la segunda quedó
  unmapped porque no estaba en mods. Esto todavía no decide reemplazos.
- El dry-run general Debug une identidades del catálogo con el DDS ganador del
  ModIndex y emite WOULD_OVERRIDE o CONTRACT_REJECTED para contratos mapeados.
  Los PIDs 18644 y 10616 lo validaron en runtime; el segundo observó dos
  segmentos con 2/2/0 durante más de nueve minutos. Siempre declara
  buffer_writes=false y aún no autoriza ni copia bytes para un reemplazo general.
- La sesión v5.1 (PID 18148) añadió SHA-256 original a esas dos decisiones,
  conservó buffer_writes=false y reprodujo visualmente el override exacto.
- V6 ya implementa el write general como opt-in Debug independiente, con
  dependencias implícitas, copia completa obligatoria y hash posterior.
  Debug write/observación y Release offline pasaron; 7 variantes pasaron smoke.
  Dos Release, incluido uno con todas las opciones activadas por propiedades,
  son idénticos byte a byte y conservan D27D35...B0620 sin prototipos ni zlib.
  El artefacto build/diagnostic-v6-general-write/dinput8.dll tiene SHA-256
  6DF8889D6EAC23D375852F12EFF8FE4CBBE4976372B220F289A1BCC09AC8A708.
  Se probó con autorización el 14 de septiembre, PID 8420, en
  Darksiders2DLL-20260914-125514-8420-000.log: dos HIT con
  replacement_written=true, replacement_verified=true y cero fallos. El
  override exacto quedó en cero porque el general escribió primero.
  La captura del usuario muestra el icono editado; el log abarca 10 min 52 s,
  sin nuevo Application Error ni crash dump. El juego ya está cerrado, el
  ancla fue retirada y 2E29A8...B629 fue restaurado. Los .upak conservaron
  metadata y solo quedó first_test_mod. Evidencia y captura en
  build/dynamic-v6-20260914-125513/.
- El POC D3DX9/D3DX11 queda como código histórico desactivado: sus detours no
  recibieron la textura objetivo durante una observación de cinco minutos.
- Crash aislado PID 16272: c0000005 en Darksiders2.exe+0x9E553F; dump y WER
  conservados. El dump confirma una escritura a 0x0000000500000009 desde una
  operación de conteo de referencias del juego, con RCX inválido, y ninguna
  dirección del proxy o D3DX en la pila capturada. La matriz posterior A/B/C
  (3 x 15 s por brazo) terminó 9/9 estable, exit 0 y sin nuevos WER. El riesgo
  queda acotado a una variante histórica desactivada, no causalmente cerrado.
- Pendientes: fallback visual nuevo con el DLL final, más navegación,
  reinicios largos y ampliación del escritor general. La ABI, dos tuplas OBPK
  y dos copias verificadas en Debug ya están demostradas; el Release
  sigue limitado al objetivo exacto y conserva SHA-256
  2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629.
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
