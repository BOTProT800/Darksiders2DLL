# Texturas a su propio tamaño

Investigación estática del 18 de octubre de 2026 sobre `Darksiders2.exe`
(SHA-256 `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`).
Se usaron DUMPBIN, `.pdata`, RTTI, el `media.upak` instalado y los logs de
sesiones anteriores, siempre en modo lectura. Implementado en
0.11.0-texture-resize-trial (sección «Implementación») y **confirmado en el
juego el 19 de octubre** (sección «Juego»).

## Respuesta corta

Sí es viable sin tocar los `.upak`, con el mismo patrón que las mallas
([MESH_RESIZE_RESEARCH.md](MESH_RESIZE_RESEARCH.md)). El motor dimensiona cada
textura con el `extra` de 7 bytes del miembro en el bloque de metadatos del
paquete y nunca lee la cabecera DDS: la salta. Para entregar un DDS de otro
tamaño, formato o número de mips:

1. Al leerse el bloque de metadatos, se reescribe el `extra` del miembro con la
   misma longitud.
2. En la lectura de datos, se consume del paquete el payload original entero y
   se entrega el payload nuevo con un cursor propio.

A diferencia de las mallas, el primer búfer de una textura es un salto de 128
bytes (la cabecera) que el motor hace por su cuenta sobre el stream original.
Como original y reemplazo tienen la misma cabecera de 128 bytes, el salto no
necesita tratamiento.

Límites: texturas 2D en BGRA8, BC1, BC2 o BC3, lados de hasta 16384 (múltiplos
de 4 en BC), sin cabecera DX10 y sin recorte de mips activo en el motor (los
valores por defecto no recortan; véase «Recortes de mips»).

## Qué renderer usa el juego

`0x9E22B4` devuelve `Platform.Renderer`, cuyo valor por defecto es `"1"`. Solo el
argumento `-dx11` en la línea de comandos (`0xA22E8`) lo pone a 2 (y en Windows
XP `0x9E2238` lo devuelve a 1). Cada caché se elige con ese valor:

| Renderer | Texturas (`dds`) | Mallas (`dcm`) | Dispositivo |
|---|---|---|---|
| 2 (`-dx11`) | `gfc::dx11TextureCache`, vtable `0x136F0E8` (RTTI) | `gfc::dx11MeshCache`, `0x136EF88` | `0xD4775C` (D3D11) |
| otro | caché sin RTTI, vtable `0x15DD9E8` | caché sin RTTI, `0x15DD948` | `0x74A634` (D3D9, vtable `0x15DCC48`) |

Ambas cachés de texturas registran el nombre de tipo `"dds"` (`0x15DD9E0`); sus
constructores (`0x10878E0`, `0x75A2DC`) se eligen en `0x746320`.

**Evidencia de juego:** la sesión 0.6.0 del 19 de septiembre
(`Darksiders2DLL-20260919-183109-16444-000.log`) instaló el gancho de
`0xD4E62C` (subida 2D de D3D11) y leyó los payloads de las texturas
(`NATIVE_TEXTURE_SOURCE_VERIFIED`), pero nunca registró
`NATIVE_TEXTURE_UPLOAD_SEEN`, que se emite con cualquier llamada a esa función.
El juego del usuario usa la ruta D3D9. Esto explica que la subida nativa de la
0.6.0 no se activara nunca.

Nota lateral: el método `0x10867AC` que analizó la investigación de mallas está
en la vtable de `gfc::dx11MeshCache`. Con D3D9 el manejador de mallas es
`0x75908C`. Como el redimensionado de mallas funcionó en el juego (25 búferes
exactos), su lista de búferes coincide, pero `0x75908C` no se ha revisado.

## Fase 0: el manejador del tipo 6

La fase 0 (`0x9F9FC8` → `0x9FA148`) abre el substream del `extra` y llama a la
ranura `+0x38` de la caché: `0x75A63C` en D3D9 y `0x1087B98` en DX11. Ambas
leen lo mismo:

| Campo | Tamaño | Valores |
|---|---|---|
| formato del motor | u8 | 6 = BGRA8, 15 = BC1, 16 = BC2, 17 = BC3 |
| mips | u8 | niveles de la cadena |
| tipo | u8 | 1 = 2D, 2 = volumen, 3 = cubemap |
| ancho, alto | u16, u16 | del nivel superior |
| profundidad | u16 | solo si tipo = 2: son los `extra` de 9 bytes |

Con esos campos, `0x81D134` calcula el tamaño total: 128 + caras × Σ niveles
(6 caras en un cubemap). El tamaño de cada nivel lo da `0x7FC560`: en BC,
`max(1, (w+3)/4) × max(1, (h+3)/4) × bloque`, y si no, `w × h × bytes por píxel`;
las dimensiones de cada nivel son `max(1, d >> 1)`. La cabecera mide siempre 128
(`0x81D448(0)`): el `extra` no registra cabeceras DX10.

### D3D9 (`0x75A63C`)

1. Crea la textura **ya en la fase 0** con los campos del `extra`, mediante el
   dispositivo (`+0x348` 2D, `+0x350` volumen, `+0x358` cubemap). En 2D,
   `0x74D8F0` → `0x778A00` llama a `IDirect3DDevice9::CreateTexture`
   (`vtable+0xB8`) en `D3DPOOL_MANAGED` (global `0x1C0A840` = 1). El formato se
   traduce en `0x74FC50`: 6 → `A8R8G8B8`, 15/16/17 → `DXT1/3/5`.
2. Registra `{nulo, 128}` (la cabecera) y, por cara y nivel, bloquea el nivel
   (`LockRect`, `+0x98`) y registra `{pBits, tamaño}`. Si el nivel sigue en
   memoria al anterior, alarga ese búfer en vez de crear otro.
3. No comprueba que `CreateTexture` haya funcionado: con un puntero nulo se
   bloquea en `0x75A9B3`. De ahí los límites de formato y de lados múltiplos de 4.

En el juego los niveles quedan contiguos: la sesión 0.9.2 del 18 de octubre leyó
el payload de `ds1a01_shoulder_diff.dds` en una sola lectura (`requested=699064`).

La fase 2 (`0x75ACC0`, `0x75AC60` → `0x75AF2C`) solo desbloquea cada nivel
(`UnlockRect`); no compara tamaños.

### DX11 (`0x1087B98`)

Registra `{nulo, 128}`, `{nulo, mips descartados}` si los hay, y
`{reserva, payload}` (`0x9F91B0`). Guarda un descriptor de 0x20 bytes (tipo,
formato, ancho, alto, profundidad y mips) y la textura de GPU se crea después, en
la tarea `0x1088148`, con `Upload2D` (`0xD4E62C`). También aquí todo sale del
`extra`.

### Generador del extra

La ranura 6 de la caché D3D9 (`0x75A3E8`) es el código del empaquetador: analiza
la cabecera DDS (`0x81CDD4`), exige `128 + caras × Σ niveles == tamaño` y escribe
el `extra`. Reglas del análisis:

- DXT1/DXT3/DXT5 → 15/16/17. RGB de 32 bits con máscara alfa → 6, sin comprobar
  el orden de canales: el reemplazo BGRA8 debe usar las máscaras de `A8R8G8B8`.
- mips = `dwMipMapCount` solo con `DDSD_MIPMAPCOUNT`; si no, 1.
- `DDSCAPS2_CUBEMAP` con caras → 3, `DDSCAPS2_VOLUME` → 2, si no → 1.

Esto confirma el escaneo previo de `media.upak`: en 9008/9008 miembros
comprobables, `tamaño = 128 + caras × payload`.

## Recortes de mips

No hay presupuestos de memoria: los bytes que la fase 0 acumula
(`[contexto+0x48]`, `[+0x4C]`) solo alimentan estadísticas por tipo
(`0x9FA4FF`–`0x9FA528`). Sí hay dos ajustes que descartan los niveles superiores:

- `[Resources] MaxMipLevels` (palabra efectiva en `0x1EEF25C`, escrita en
  `0x9E4570`): si es distinto de 0 y menor que los mips, se descartan los
  sobrantes. Con `[Resources] MemoryDebug` se fuerza a 9 como mucho. Ambas
  cachés lo aplican.
- `[Renderer] TextureDetail` (entero en `0x1C0A844`, apuntado desde
  `0x1C3BD60`), solo en D3D9: con un valor distinto de 2, se descartan
  `2 − valor` niveles si quedan al menos 7.

En ambos casos no se descarta nada si el primer nivel conservado mide menos de 4
píxeles de lado. Los valores por defecto (0 y 2) no recortan, y ninguno aparece
en los archivos de configuración del usuario. Con recorte, el salto inicial del
motor incluiría niveles del reemplazo que el stream original no tiene; por eso
la DLL no aplica el parche cuando el recorte afectaría al reemplazo.

## Fase 1: lectura

El bucle `0x9FEE5C` acumula los búferes nulos en `[lector+0x30]` y los salta
(`0x100120`) justo antes del primer búfer real de cada archivo. El salto de 128
bytes de una textura se suma así a lo que dejaran pendiente los archivos
anteriores. No hace falta separarlo: el stream queda al principio del payload
original, que es lo que la DLL consume después.

## Implementación: 0.11.0-texture-resize-trial

Solo con `textures=resize` en `[loader]` (y `mode=override`). En cualquier otro
modo no cambia nada.

- **Plan** (`texture_resize.cpp`, `PlanTextureResize`), para cada DDS o PNG de
  contrato distinto al original. Exige:
  - que el `extra` del paquete repita el contrato del original (comprobación del
    esquema en cada archivo);
  - BGRA8, BC1, BC2 o BC3 en 2D, lados de hasta 16384 (múltiplos de 4 en BC),
    hasta 255 mips y cabecera de 128 bytes;
  - que el archivo mida 128 + Σ niveles, con los tamaños de `0x7FC560`.

  Guarda el `extra` nuevo, su posición en el bloque, tamaño y SHA-256 del bloque,
  y los tamaños de las primeras lecturas posibles (sumas de niveles desde el
  superior) de ambas versiones. La conversión al contrato original de la 0.9.2 se
  sigue preparando como respaldo.
- **Metadatos** (`resolver_probe.cpp`): tras la lectura que vuelve a
  `0x9FEDC7`, el bloque se hashea **una sola vez antes de cualquier parche**, de
  modo que mallas y texturas del mismo segmento se parchean juntas. Si
  `MaxMipLevels` o `TextureDetail` recortarían el reemplazo (o no se pueden leer),
  se conserva el `extra` original y se registra el rechazo; si no, se reescribe y
  se verifica.
- **Datos**: los saltos previos a la primera lectura del miembro pasan intactos.
  La primera lectura directa debe pedir una suma de niveles del reemplazo; si
  pide una de la versión original, el motor usó otro `extra` y el miembro sigue
  por la ruta de copia (la conversión 0.9.2). Si ambas coinciden, decide que todos
  los parches se aplicaran sin fallos. Entonces se consume el payload original
  (tamaño − 128) con SHA-256 y se sirve el payload nuevo con un cursor por hilo,
  verificando lo escrito, hasta entregar el reemplazo entero.
- **Log:** `TEXTURE_RESIZE_METADATA_PATCHED`, `TEXTURE_RESIZE_METADATA_DECLINED`,
  `TEXTURE_RESIZE_BEGIN` (`original_verified`), `TEXTURE_RESIZE_COMPLETE`
  (`delivered`, `operations`) y `TEXTURE_RESIZE_INCOMPLETE`,
  `TEXTURE_RESIZE_FAILED` o `TEXTURE_RESIZE_METADATA_FAILED`. El candidato
  muestra `resize=…` o `resize_error=…`, y `RESOLVER_PROBE_STATS` lleva los
  contadores `texture_resize_*`. Cualquier fallo de escritura activa el bloqueo
  compartido del escritor.

**Pruebas:**

- `texture_resize`: codificación del `extra` (formatos del motor y rechazos),
  tamaños de nivel, la réplica del recorte de mips en 10 casos, el plan y 8
  rechazos, y el modo del INI.
- `texture_resize_catalog`: los catálogos reales sobre un `media.upak` sintético
  con `extra` como los del empaquetador, un reemplazo mayor en otro formato (BC1
  8×8 → BC3 16×8) y otro menor (→ BC1 4×4); conversión de respaldo, modo
  desactivado y esquema que no coincide.
- `texture_resize_runtime`, por el detour real con el marco MASM de la 0.10.0:
  - un bloque con dos texturas parcheado con un solo hash;
  - sin parche, o con una lectura del tamaño original, pasa la ruta de copia;
  - el salto de cabecera sumado a bytes de un miembro anterior;
  - el payload en una lectura y por niveles;
  - los originales consumidos exactamente y el siguiente miembro alineado,
    también tras un abandono a mitad;
  - el rechazo por recorte de mips y con ajustes ilegibles;
  - el bloqueo por fallo.

  En la build de observación se comprueba que no se toca nada.

**Datos reales:** con dobles sintéticos de las texturas del usuario (512×1024 BC1
con 11 mips para `diff` y `norm`, 512×1024 BC3 de 1 nivel para `spec`) y una
2048×2048 BC1, `offline_tests --texture-catalog-resize` crea el plan de las
cuatro contra el `media.upak` instalado:

| Miembro | Original | Reemplazo | `extra` en el bloque |
|---|---|---|---|
| 796 `ds1a01_shoulder_norm.dds` | 1024² BC1, 11 mips | 512×1024 BC1, 11 mips (349.672 bytes) | 17752 |
| 1202 `ds1a01_shoulder_spec.dds` | 1024² BC3, 11 mips | 512×1024 BC3, 1 mip (524.416 bytes) | 23842 |
| 1251 `ds1a01_shoulder_diff.dds` | 1024² BC1, 11 mips | 512×1024 BC1, 11 mips o 2048² BC1, 12 mips | 24577 |

Segmento `0xABD000` (por bloques), bloque de metadatos de 34.044 bytes en
`names_end` 69.111. Un lector OBPK aparte en Python confirma esas posiciones,
los `extra` originales (`0f0b0100040004`, `110b0100040004`) y que cada miembro
mide 128 + payload.

**Build:** `verify_release.ps1 -TextureModsDirectory …` (`validation-texture-resize-trial`,
26 comprobaciones: pruebas Debug, Release y de observación, planes de texturas y
del redimensionado de mallas sobre la instalación, dos Release idénticas, Debug,
smoke de los exports y protecciones PE). DLL Release
`645EA9B48BDD432A90BE6AAD2831FD2FED1AFB0B3898C084E913132E541E52A6`. La
instalación no cambió durante las pruebas.

## Juego: 19 de octubre de 2026

`scripts/install_texture_resize_trial.ps1` instaló la DLL Release verificada
`645EA9B4…52A6`, añadió `textures=resize` al INI (conservando
`models=override_unchecked` y `animations=observe`) y copió a
`mods/first_test_mod/media/items/armor` tres DDS sintéticos con el contrato de
las texturas del usuario: `diff` y `norm` en 512×1024 BC1 con 11 mips (349.672
bytes; tablero rojo y blanco y normal plana) y `spec` en 512×1024 BC3 con 1 nivel
(524.416 bytes). El respaldo está en `build/texture-resize-trial/installation-backup`.

La sesión `Darksiders2DLL-20261020-034822-9404-000.log` (hora UTC; copia en
`build/texture-resize-trial/texture-resize.log`) registró:

- `LOADER_CONFIG mode=override textures=resize` y los tres candidatos con
  `resize=512x1024/…`, junto al de la máscara de `death_head.2`.
- `TEXTURE_RESIZE_METADATA_PATCHED` para los tres miembros, cuatro veces cada
  uno: el segmento por bloques `0xABD000` relee su bloque de metadatos y cada
  lectura se parcheó. En el mismo proceso, `MODEL_RESIZE_METADATA_PATCHED` y
  `MODEL_RESIZE_COMPLETE delivered=193097 operations=25` para la máscara.
- `TEXTURE_RESIZE_BEGIN ... original_verified=true` con
  `first_request=349544` (BC1) y `524288` (BC3): el payload entero en una sola
  lectura, con los niveles contiguos en la memoria de D3D9.
- `TEXTURE_RESIZE_COMPLETE delivered=349672` (×2) y `delivered=524416`, con
  `operations=1` en cada uno.
- Estadísticas: 12 parches de metadatos, 3 texturas entregadas, 0 rechazos,
  0 fallos y 0 eventos perdidos. `general_write_attempts=0`: la conversión 0.9.2
  no llegó a usarse.

El usuario confirmó que funciona. Quedan resueltos el momento de la lectura en
segmentos por bloques y la contigüidad de la memoria de D3D9.

## Riesgos que siguen abiertos

- **Creación de la textura:** un `CreateTexture` fallido bloquearía el juego.
  Los límites del plan evitan los casos conocidos; texturas muy grandes podrían
  agotar memoria.
- **Materiales:** el muestreo usa coordenadas normalizadas, así que un tamaño
  distinto no debería afectar. No se ha comprobado si algún shader asume el
  tamaño original.
- **DX11 (`-dx11`):** la ruta es la misma a nivel de stream, pero no se ha
  probado.
