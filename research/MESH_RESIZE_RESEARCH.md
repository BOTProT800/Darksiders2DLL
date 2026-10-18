# Nivel 2: mallas más grandes que el original

Investigación estática del 18 de octubre de 2026 sobre `Darksiders2.exe`
(SHA-256 `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`).
Se usaron DUMPBIN, `.pdata`, RTTI, el `media.upak` instalado y la extracción
local, siempre en modo lectura. Implementado en 0.10.0-resize-trial (sección
«Implementación») y **confirmado en el juego el 18 de octubre** (sección «Juego»).

## Respuesta corta

Sí es viable sin tocar los `.upak`. El tamaño de cada malla no lo decide el
archivo `.2`, sino un bloque de metadatos del paquete que el motor lee antes.
Para entregar una malla más grande hacen falta dos intervenciones en la función
de lectura que la DLL ya intercepta (`ReadStream`, `0xABDC0`):

1. Parchear en memoria los recuentos de esa malla en el bloque de metadatos, con
   la misma longitud.
2. Durante la lectura de datos, consumir del paquete el archivo original entero y
   entregar al motor el nuevo, con su propio cursor.

Límites que quedan: el mismo número de mallas y de pares (`extra` de igual
longitud) y como máximo 65.535 vértices por malla (índices de 16 bits).

## Cómo carga el motor un `.2`

### Metadatos del paquete

En los metadatos OBPK cada miembro tiene `u32 tamaño, u32 extra_size, extra`.
Para el tipo 2 (malla), el `extra` es una copia compacta de los recuentos de la
cabecera del `.2`:

```text
[tamaño_cabecera, mallas, pares, y por malla: índices, vértices (con signo), par (si i < pares)]
```

`death_head.2`: `3965, 4, 4, (96, 28, 0), (678, 128, 0), (3180, 854, 0), (2196, 432, 0)`.

**Validación:** en 4209 miembros tipo 2 de los 1138 segmentos, emparejados por
nombre y tamaño con la extracción, el `extra` coincide byte a byte con el derivado
de la cabecera. No hay ninguna diferencia.

El lector OBPK (`0x9FE8E8`) guarda `names_end` en `[+0x1C]` y `metadata_size` en
`[+0x20]`. El método `0x9FED44` lee **en una sola llamada** a `ReadStream` (retorno
`0x9FEDC7`) el bloque `[base + names_end, + metadata_size)` y lo envuelve en un
stream de memoria (`0x104F00`). Las entradas por miembro y la tabla final de
tamaños (o desplazamientos de bloque) están en ese bloque.

### Fase 0: objetos y lista de búferes (`0x9F9FC8` → `0x9FA148`)

Por cada archivo, el motor lee del stream de metadatos el tamaño y `extra_size`,
abre un substream de `extra_size` bytes (`0xF9894`) y llama al manejador del tipo
(`vtable+0x38`). Para mallas es `gfc::MeshResourceArray` (vtable `0x136EF70`,
RTTI), método 10 `0x10867AC`, que registra los búferes `{destino, tamaño}`:

| Parte | Búferes |
|---|---|
| Cabecera | `{reserva, tamaño_cabecera}` |
| Índices (`ic > 0`) | `{nulo, 8}` (prefijo que se **salta**), `{reserva, ic×2}` |
| Con piel (`vc ≥ 0`) | `vc×20`, `vc×52`, `vc×16`, `vc×4` |
| Estática, `par > 0` | `n×12`, `n×48`, `par×16` (`n = −vc`) |
| Estática, `par = −1` | `n×12`, `n×48`, `n×8` |
| Estática, `par = −2/−3/−4` | `n×4` / `n×8` / `n×12` |
| Estática, otro `par` | `n×12`, `n×48` |

**Validación:** con este modelo, la suma de búferes calculada desde la cabecera
coincide exactamente con el tamaño del archivo en los **7608 `.2`** extraídos
(1627 mallas con piel, 22.623 estáticas con cola y 8192 sin ella).

El tamaño del miembro de los metadatos solo se usa cuando no hay manejador (se
salta el archivo entero); no se compara con la suma de búferes.

### Fase 1: lectura de datos (`0x9FA8D8` → `0x9FEE5C`)

El bucle recorre los búferes de cada archivo:

- con destino nulo, acumula bytes por saltar;
- con destino real, primero salta lo acumulado (`0x100120`) y después lee con
  `ReadStream` desde `0x9FF0CD` (retorno `0x9FF0D2`).

Los archivos no cargados se saltan con el valor de la tabla final, que corresponde
al tamaño original.

El envoltorio de descompresión (vtable `0x16B2B28`) **no admite búsqueda**: su
`CanSeek` (`0x9B670`) es `xor al,al; ret`. Por eso `0x100120` salta leyendo con
`ReadStream`, en trozos de hasta 8 KB y retorno `0x100181`, hacia un búfer
global. Todo lo que la fase 1 consume del archivo pasa por `ReadStream`.

Marcos de pila de las dos rutas, con `R` = ranura de retorno del detour:

| Ruta | Comprobación | Índice de archivo | Envoltorio |
|---|---|---|---|
| Lectura directa | `[R] = 0x9FF0D2` | `[R+0x38]` | `R+0x78` (`RCX`) |
| Salto | `[R] = 0x100181`, `[R+0x30] = 0x9FF045` | `[R+0x68]` | `R+0xA8` (`RCX`) |

### Fase 2

`MeshResourceArray` método 9 (`0x1086174`) analiza la cabecera del búfer 0
(tablas de índices, vértices y pares) y crea los recursos. La cabecera entregada
debe coincidir con el `extra` parcheado; ambos se derivan del mismo `.2`.

## Diseño propuesto

**Catálogo** (al arrancar), cuando la malla nueva no cabe en el hueco original:

1. El `.2` nuevo cumple `suma de búferes del motor = tamaño del archivo`, con el
   mismo número de mallas y pares que el original.
2. El `extra` del paquete coincide con el derivado de la cabecera original
   (comprobación del esquema en cada archivo).
3. Se calculan el nuevo `extra`, su posición dentro del bloque de metadatos, y
   tamaño y SHA-256 del bloque original.

**Metadatos:** en `ReadStream` con retorno `0x9FEDC7`, si tamaño y SHA-256 del
bloque coinciden, se reescribe el `extra` del miembro con la misma longitud y se
verifica. La tabla final y el tamaño del miembro **no** se tocan: el stream
subyacente sigue conteniendo el original.

**Datos**, por hilo, para el miembro parcheado:

1. Los saltos que llegan antes de su primera lectura pertenecen a archivos
   anteriores y pasan intactos. La cabecera, que siempre es el primer búfer, es
   una lectura directa.
2. En la primera lectura directa se consume del envoltorio el original entero, en
   trozos y con SHA-256 incremental, y se compara con la huella original.
3. Desde ahí, cada lectura o salto del miembro se sirve del `.2` nuevo con un
   cursor propio, hasta entregar su tamaño. El siguiente archivo encuentra el
   stream alineado. En los segmentos de bloques, el envoltorio se reabre por
   miembro y el procedimiento es el mismo.

**Seguridad:** sin el parche de metadatos en ese proceso no se activa el empalme
y se lee el original. Hay que registrar el parche, el inicio con la huella del
original y el final con los bytes entregados.

## Implementación: 0.10.0-resize-trial

Solo con `models=override_unchecked`. Si la malla cabe en el hueco original se
usa el encaje, la vía ya probada. Si no cabe, se usa la ruta de redimensionado:

- **Catálogo** (`package_identity_catalog`): por miembro guarda el bloque de
  metadatos (`names_end`, `metadata_size`, SHA-256) y la posición y bytes de su
  `extra`.
- **Validación** (`model_validation`): `ReadModelEngineCounts`,
  `ModelEngineBuffers`, `ModelEngineSize` y `EncodeModelEngineExtra` replican
  `0x10867AC`. `PlanModelResize` exige:
  - que el `extra` del paquete repita la cabecera original;
  - el mismo número de mallas y de pares que el original;
  - que la suma de búferes del motor sea igual al tamaño del reemplazo;
  - registros de malla coherentes con los recuentos y desplazamientos;
  - índices de 16 bits dentro de cada malla.
- **Gancho** (`resolver_probe`), solo en builds con escritura y con el escritor
  activo:
  - Tras la lectura que vuelve a `0x9FEDC7`, si tamaño y SHA-256 coinciden con el
    bloque, reescribe el `extra` y lo verifica leyéndolo de nuevo.
  - Antes de cada lectura, clasifica la operación como directa (`0x9FF0D2`) o
    salto (`0x100181`, con `[R+0x30] = 0x9FF045`) y comprueba en ambas el
    envoltorio y el índice de archivo del marco.
  - La primera lectura directa del miembro debe pedir la cabecera del reemplazo.
    Entonces se consume el original (SHA-256 incremental), y esa lectura y el
    resto se sirven del reemplazo con un cursor por hilo, verificando lo escrito.
  - Si se pide el tamaño de cabecera original, no se toca nada. Si ambas
    cabeceras miden lo mismo, la decisión depende de que el parche se haya
    aplicado sin fallos.
- **Log:** `MODEL_RESIZE_METADATA_PATCHED`, `MODEL_RESIZE_BEGIN`
  (`original_verified`), `MODEL_RESIZE_COMPLETE` (`delivered`, `operations`) y
  `MODEL_RESIZE_INCOMPLETE`, `MODEL_RESIZE_FAILED` o
  `MODEL_RESIZE_METADATA_FAILED`, más contadores en `RESOLVER_PROBE_STATS`.
  Cualquier fallo de escritura activa el bloqueo compartido del escritor.

**Pruebas:** recuentos, búferes y codificación del fixture; las seis variantes
estáticas; plan, rechazos y candidato. La prueba de ejecución pasa por el detour
real con un marco MASM (`InvokeEngineFrame`) que reproduce las rutas directa y de
salto con el mismo envoltorio, y cubre:

- el parche del bloque y la lectura sin parche;
- la cabecera de tamaño original;
- la entrega completa, consumiendo exactamente el original;
- el siguiente miembro alineado, también tras un abandono a mitad;
- el bloqueo por fallo.

En la build de observación se comprueba que no se toca nada.

**Datos reales:** `build/resize-trial/grow_mesh.py` duplica la máscara de
`death_head.2` con una copia ampliada un 15 %: la malla 3 pasa de 432 a 864
vértices y de 732 a 1464 triángulos, y el archivo de 148.961 a 193.097 bytes. Lo
lee el parser independiente del addon. Contra el `media.upak` instalado, el
catálogo crea el candidato (bloque de 21.450 bytes, `extra` en 2872, 25 búferes),
y un lector OBPK aparte confirma esa posición y esos recuentos.

## Juego: 18 de octubre de 2026

Se instaló la DLL Release verificada
`06BA1796AF0EC3399280844C10318DECE72116A8E7C696552A76469210016F58`
(`validation-resize-trial`, 26 comprobaciones). El usuario usó el archivo de
prueba con la máscara duplicada (193.097 bytes, SHA-256 `EA4431D0…`). La sesión
`Darksiders2DLL-20261018-184344-13908-000.log` (copia en
`build/resize-trial/resize.log`) registró:

- `MODEL_CANDIDATE ... resize=true buffers=25 metadata_bytes=21450`.
- `MODEL_RESIZE_METADATA_PATCHED`: el motor leyó el bloque de metadatos después
  de instalarse los ganchos, y el parche se aplicó y se verificó.
- `MODEL_RESIZE_BEGIN first_request=3965 original_verified=true`: la primera
  lectura fue la cabecera, y el original consumido coincidió con su huella.
- `MODEL_RESIZE_COMPLETE delivered=193097 operations=25`: se entregaron los 25
  búferes previstos, sin un byte de más ni de menos.
- Estadísticas: un parche, un miembro redimensionado, cero fallos y cero eventos
  perdidos. La metadata de los `.upak` no cambió.

El usuario confirmó que funciona. Queda resuelto el primer riesgo, el momento de
la lectura de los metadatos. Los demás siguen abiertos para mallas mucho mayores.

## Riesgos que solo resuelve una prueba en el juego

- **Momento de la lectura:** si algún paquete leyera sus metadatos antes de que
  la DLL instale los ganchos, el parche no se aplicaría. El diseño cae entonces
  al original, pero hay que verlo en el log.
- **Presupuestos de memoria:** el motor acumula los bytes por tipo
  (`[rsi+0x48]`, `[rsi+0x4C]`, `[+0x118]`, `[+0x16C]`). Se desconoce si hay
  límites de streaming.
- **Shaders:** el máximo de huesos por malla no está determinado. Una paleta
  mayor que la original podría superar el límite del shader.
- **Culling:** una malla más grande en el espacio necesita cajas actualizadas en
  la cabecera.
