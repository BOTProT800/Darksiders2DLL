> Actualización 0.4.0 (2026-09-14): se retiró la dependencia del caso especial y
> el escritor exacto. Debug y Release usan el motor general validado por contrato
> y hash, con configuración INI, permiso de activación y bloqueo tras fallos.
> El paquete preliminar y su guía se describen en README y distribution/INSTALACION.md.
> Las restricciones Debug-only y los switches de prototipo descritos más abajo
> son históricos. La validación visual v6 no valida automáticamente este Release.

# Investigación del resolver de assets

> Estado al 14 de septiembre de 2026: el Release mantiene el override exacto
> de `first_test_mod`. El escritor general Debug v6 ya copió y verificó dos
> payloads de identidades distintas; la captura del usuario muestra el icono
> modificado. Esto no demuestra soporte genérico de loose files.

## Alcance y build al que aplica

Todos los RVA, VA preferidos y offsets de archivo de este documento son
específicos de esta huella:

- Ejecutable: `Darksiders2.exe`
- Tamaño: `33,637,376` bytes
- Arquitectura: x64, PE32+
- Image base preferido: `0x140000000`
- SHA-256:
  `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`

En runtime se deben expresar los puntos de interés como
`base_real_de_Darksiders2.exe + RVA`, porque ASLR puede cambiar las VA. Una
huella diferente invalida estas direcciones. No debe instalarse ningún hook
interno solamente porque una dirección coincida.

## Evidencia dinámica confirmada

### Lectura interna en RVA `0x0ABDC0`

La ejecución pasiva confirmó que la rutina en
`base_real_de_Darksiders2.exe + 0x0ABDC0` tiene, para el uso observado, este
contrato:

```cpp
int32_t ReadStream(void* stream, void* destination, int32_t size);
```

Bajo la ABI x64 de Microsoft recibe `stream` en `RCX`, `destination` en `RDX`
y `size` en `R8D`, y devuelve en `EAX` el número de bytes entregados. Para el
asset objetivo devolvió síncronamente `4096` bytes al buffer del llamador. El
hook se limita al ejecutable cuya huella se documenta arriba y exige una única
coincidencia de esta firma de prólogo:

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57
48 83 EC 20 8B 71 14 45 8B F0 48 8B FA 48 8B D9
```

La coincidencia del payload produjo esta pila en las ejecuciones pasiva y
activa; las direcciones del juego se expresan como RVA:

```text
0x7FFE84D3735D,
game+0x9FF0D2,
game+0x9FA980,
game+0x9F9E9A,
game+0x9F39E3,
game+0x9F34D3,
game+0x9F2DAE,
game+0xC3CBF,
game+0xFFE07,
0x7FFF151BCD87,
0x7FFF16BECAEC
```

El primer frame externo al juego varió entre sesiones, pero la cadena de RVA
del juego permaneció igual.

### Baliza de `ReadFile`

Las lecturas del intervalo conocido de `media.upak` se observaron como I/O
overlapped: `ReadFile` devolvió inicialmente `FALSE` con
`ERROR_IO_PENDING`. Esto es el comportamiento esperado de una operación
asíncrona y resultó útil como baliza para correlacionar el segmento del
paquete con la pila posterior. No es un lugar seguro para sustituir el DDS: en
ese nivel todavía se está leyendo el stream comprimido compartido.

### Huellas exactas del objetivo y reemplazo

- Payload BC3 original entregado por la lectura interna: `4096` bytes.
- SHA-256 del payload BC3 original:
  `9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51`.
- FNV-1a de 64 bits del payload BC3 original: `0x40A4729137EEC844`.
- SHA-256 del payload BC3 editado:
  `DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F`.
- SHA-256 del DDS fuente editado de `4224` bytes:
  `2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719`.

El reemplazo activo deja que la función original termine, exige retorno y
tamaño exactos, calcula el SHA-256 de los bytes devueltos, comprueba que el
destino sea escribible y solo entonces copia los `4096` bytes BC3 editados.
Toda lectura que no coincida exactamente continúa sin modificación.

### Sesiones reproducibles

La sesión pasiva fue el proceso PID `11892`, registrada en:

```text
C:\Users\vicen\AppData\Local\Darksiders2DLL\logs\Darksiders2DLL-20260912-011051-11892-000.log
```

En ella aparecen `RESOLVER_PROBE_ACTIVE`, `STREAM_READ_SAMPLE` con el
SHA-256 original y `STREAM_READ_TARGET_MATCH`. El registro no muestra eventos
perdidos ni fallos de copia de muestras.

La sesión con reemplazo fue el proceso PID `20632`, registrada en:

```text
C:\Users\vicen\AppData\Local\Darksiders2DLL\logs\Darksiders2DLL-20260912-012822-20632-000.log
```

En ella aparecen `INTERNAL_ASSET_OVERRIDE_ACTIVE`, la coincidencia exacta del
payload y el evento:

```text
ASSET_OVERRIDE_HIT first_test_mod media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds form=bc3_payload size=4096 source_sha256=9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51 replacement_bytes_sha256=DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F replacement_source_dds_sha256=2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719
```

Las estadísticas posteriores permanecieron en
`target_hash_matches=1`, `replacements_applied=1` y
`replacement_write_failures=0`; también conservaron `dropped_events=0` y
`sample_copy_failures=0`. La ejecución observada duró aproximadamente 83
segundos sin un evento `Application Error`. Este intervalo es una prueba corta,
no evidencia de estabilidad prolongada.

La captura facilitada por el usuario confirma visualmente que el icono editado
aparece en el árbol de habilidades del juego. Esta validación visual corresponde
al caso positivo exacto; no valida otros assets ni un camino visual de fallback.

Con el mismo DLL final se ejecutó después un control de archivo ausente. El DDS
se apartó de forma temporal, el proceso PID `15492` registró
`ASSET_FALLBACK target DDS is absent from the mod index` en
`Darksiders2DLL-20260912-061100-15492-000.log`, y el archivo se restauró de
inmediato con SHA-256 `2C0D7E...B719`; no quedó archivo de staging. La sesión
fue demasiado breve para aportar evidencia visual, pero confirma que el
bootstrap no instala el override cuando falta el asset.

Un segundo arranque positivo, PID `12308`, reprodujo la coincidencia y
`ASSET_OVERRIDE_HIT` en
`Darksiders2DLL-20260912-061242-12308-000.log`, otra vez con una aplicación,
cero fallos de escritura, cero drops y cero fallos de copia. No apareció un
`Application Error`. Esta repetición corta demuestra reproducibilidad tras un
reinicio, no estabilidad prolongada.

Un tercer arranque positivo, PID `11164`, volvió a producir un único
`ASSET_OVERRIDE_HIT` en
`Darksiders2DLL-20260912-061608-11164-000.log`. Permaneció respondiendo unos
134 segundos con `target_hash_matches=1`, `replacements_applied=1`,
`replacement_write_failures=0`, `dropped_events=0` y
`sample_copy_failures=0`, sin un nuevo `Application Error`. Es la ejecución
positiva más larga observada, pero sigue siendo una prueba corta.

El rechazo de contenido inválido se verificó también en el juego. Para el PID
`26060` se colocó temporalmente un archivo que no era DDS en la ruta objetivo;
`Darksiders2DLL-20260912-063656-26060-000.log` registró
`MOD_INDEX_ISSUE code=invalid_dds` y después `ASSET_FALLBACK`, sin activar el
hook interno. El DDS válido se restauró inmediatamente con su SHA-256 exacto y
no quedó archivo de staging. Esta es evidencia del fallback lógico; la vuelta
visual al icono original todavía requiere confirmación del usuario.

### Primera identidad OBPK reproducida

Se añadió una variante diagnóstica Debug, desactivada en Release, que engancha
pasivamente el método de vtable en RVA `0x9FEE5C`. Su ABI observada coincide con
el análisis estático: devuelve un entero en `EAX`, recibe cuatro argumentos en
registros y usa un quinto argumento de pila. El detour conserva los cinco
valores, llama al original y solo mantiene un contexto POD por hilo para
correlacionar llamadas anidadas con el lector RVA `0x0ABDC0`.

Los logs siguientes contienen la coincidencia exacta del SHA-256 original:

- PID `19228`: `Darksiders2DLL-20260912-185348-19228-000.log`.
- PID `16968`: `Darksiders2DLL-20260912-185651-16968-000.log`.
- PID `21332`: `Darksiders2DLL-20260912-185757-21332-000.log`.

En los tres procesos la muestra objetivo tuvo profundidad `1`, base de paquete
`0x33250800`, `member_table_offset=12288` (`0x3000`), ordinal de lectura `73`,
caller exterior `0x9FA980`, retorno de lectura `0x9FF0D2`, petición/retorno de
`4096` bytes y el SHA-256 `9F008D...EE51`. Los punteros de objeto, buffers y
streams sí cambiaron, por lo que quedan excluidos de la identidad. La suma
`0x33250800 + 0x3000` coincide con los cuatro bytes inmediatamente anteriores
al inicio zlib ya documentado en `0x33253804`, y el ordinal uno-basado `73`
coincide con el miembro extraído `#72`.

La primera ejecución emitía muestras de otros payloads del mismo tamaño y se
usó para confirmar el alcance. Después se filtró la cola al hash objetivo; las
dos repeticiones siguientes conservaron cero drops, cero fallos de copia, cero
overflows y cero reinicios de contexto. Este resultado prueba una identidad
estable para el primer miembro.

### Segunda ancla y contraste entre segmentos

Se eligió `media/ui/ui_core/ui_icon_highlight.dds` como segunda ancla. Al
principio no se instaló como mod; más tarde se colocó temporalmente una copia
sin editar en un mod diagnóstico aislado para comprobar el dry-run. Sus hechos
estáticos son:

- Archivo extraído:
  `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\ui_icon_highlight.dds`.
- Tamaño y formato: `4224` bytes (`0x1080`), `64 x 64`, `DXT5`/BC3; campo
  `mipMapCount` igual a `0`.
- SHA-256 del DDS:
  `7D7CA1D2B1A411DA28BA4071BB6D0A9AC54FFB5F2C0E608B28333A2D34EA3254`.
- SHA-256 del payload BC3 de `4096` bytes:
  `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.
- Paquete/segmento: `media.upak`, `media/ui/ui_core.`, base `0x77800`, tamaño
  `0x811000`.
- Layout OBPK `stream`, tabla/payload en `0x3800` e inicio zlib absoluto en
  `0x7B004`.
- Tamaño comprimido `0x80D7FC`; tamaño descomprimido `0x1B498C1`.
- Miembro cero-basado `#45`, ordinal runtime uno-basado `46`, offset en el
  stream descomprimido `0x267E78`, tamaño `0x1080` y tipo OBPK `6`.

El miembro descomprimido desde el paquete coincidió byte a byte con el DDS
extraído. La comprobación dinámica se reprodujo en:

- PID `8692`: `Darksiders2DLL-20260912-235322-8692-000.log`.
- PID `20816`: `Darksiders2DLL-20260912-235417-20816-000.log`.
- PID `7064`: `Darksiders2DLL-20260912-235445-7064-000.log`.

Las tres sesiones emitieron la misma identidad:
`package_base=0x77800`, `member_table_offset=14336` (`0x3800`),
`read_ordinal=46`, caller exterior `0x9FA980`, retorno `0x9FF0D2`, petición y
retorno de `4096` bytes, y SHA-256
`9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`.
El primer objetivo permaneció simultáneamente estable en su tupla
`0x33250800/0x3000/73`. No hubo drops, fallos de copia, overflows ni reinicios
de contexto. Por tanto, la correlación base/tabla/ordinal ya está contrastada
con dos segmentos independientes.

### Tercera ancla offline con otro contrato de tamaño

Se añadió `media/ui/ui_core/icon_artifact.dds` como control estático, todavía
sin instalarlo como mod ni atribuirle una muestra runtime. Comparte el segmento
`media/ui/ui_core.` (`package_base=0x77800`, tabla/payload `0x3800`), pero ocupa
`1152` bytes completos y `1024` bytes de payload BC3, frente a `4224/4096` de
las dos anclas dinámicas. Es un DDS tradicional `32 x 32`, DXT5/BC3, con
ordinal uno-basado `143`, offset descomprimido `0xB727B8` y tipo `6`.

La descompresión directa del stream instalado confirmó el total declarado
`0x1B498C1`, coincidencia byte a byte con la extracción y hashes
`7BCD13C620C6249B7FB80E60B5237F42548C7D5F4EE10B40A04CABBC94E3CE10`
para el DDS y
`A1E8C36BBDDB387EAEF3F57BF8BEFA24FFDDF09C7C644FEEC3B42E4D1A13D2F7`
para su payload. La prueba real del catálogo C++ resolvió
`0x77800/0x3800/143` al path exacto. Esto elimina una dependencia accidental
del parser respecto al tamaño anterior, pero no sustituye una prueba dinámica.

### Catálogo inmutable de identidad, solo Debug

El prototipo `package_identity_catalog.*` analiza offline el manifiesto y las
tablas META nombradas de los segmentos relevantes para construir una relación
inmutable entre `(package_base, member_table_offset, member_ordinal)` y ruta
virtual canónica. Solo solicita rutas ya presentes en el snapshot de mods y no
descomprime recursos ni escribe en los paquetes. La batería sintética y real
cubre OBPK `stream`, detección/rechazo seguro de `blocks`, solicitud vacía, las
dos anclas dinámicas y una tercera ancla offline de tamaño diferente.

En el PID `14792`, registrado en
`Darksiders2DLL-20260913-001154-14792-000.log`, el arranque publicó:

```text
PACKAGE_IDENTITY_MAPPED package_base=0x33250800 member_table_offset=0x3000 member_ordinal=73 original_size=4224 path=media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds
PACKAGE_IDENTITY_CATALOG_READY requested=1 mapped=1 issues=0 dropped_issues=0
```

La muestra runtime del primer objetivo añadió exactamente
`resolved_path=media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`.
La segunda ancla añadió `resolved_path=unmapped`, resultado esperado porque
`ui_icon_highlight.dds` no forma parte de `mods`. El catálogo y estos campos de
log son diagnóstico Debug: no toman decisiones de reemplazo y no forman parte
del Release estable.

### Dry-run general integrado, solo Debug

Detrás de `EnableGeneralResolverPrototype=true`, la variante Debug x64 une las
entradas del catálogo, un contrato DDS recuperado del paquete instalado y el
asset ganador del `ModIndex`, y publica un snapshot inmutable de candidatos.
`package_dds_contract.*` abre `media.upak` en modo lectura, agrupa solicitudes
por stream OBPK, infla cada uno una sola vez en fragmentos acotados y conserva
solo los intervalos pedidos. Exige EOF zlib válido, tamaño descomprimido exacto,
DDS válido y hashes SHA-256 del archivo completo y de su payload. El join
rechaza tipos que no sean DDS, contratos ausentes, assets o metadata
incoherentes, cabeceras DX10 y diferencias de formato, dimensiones, mips,
superficie o tamaño. Los layouts `blocks` permanecen fuera de esta ruta.

La clasificación runtime solo se alcanza tras correlacionar una lectura con el
contexto OBPK por hilo: exige el caller exterior `0x9FA980`, el retorno interior
`0x9FF0D2`, campos válidos y que tanto el objeto de paquete capturado como el
wrapper de lectura sean no nulos. No exige igualdad entre esos punteros: los
logs históricos muestran que son distintos y el callsite fijo `0x9FF0C8`
construye el wrapper con `lea rcx,[rsp+70h]`. Después exige destino escribible,
conteos coherentes, retorno completo y tamaño igual al DDS entero o a su
payload. Para ese candidato copia el rango observado mediante
`ReadProcessMemory` en fragmentos fijos, calcula SHA-256 sin asignaciones y lo
compara con el contrato instalado. Una coincidencia produce
`GENERAL_DDS_VERIFIED_WOULD_OVERRIDE`; un candidato mapeado que no satisface el
contrato produce `GENERAL_DDS_CONTRACT_REJECTED`. Ambos logs incluyen
`buffer_writes=false` y la ruta general nunca copia bytes del asset suelto.

La construcción, extracción y clasificación tienen cobertura offline. La
evidencia dinámica se acumuló en dos etapas:

- PID `18644`, log `Darksiders2DLL-20260913-224351-18644-000.log`: un candidato,
  `GENERAL_DDS_WOULD_OVERRIDE decision=payload` para
  `0x33250800/0x3000/73`, seguido del override exacto conocido.
- PID `10616`, log `Darksiders2DLL-20260913-224648-10616-000.log`: dos
  candidatos y cero rechazos; eventos positivos para `0x77800/0x3800/46` y
  `0x33250800/0x3000/73`, ambos `4096/4096`, destino válido y
  `buffer_writes=false`. Durante más de nueve minutos mantuvo
  `general_candidate_hits=2`, `general_would_override=2`,
  `general_contract_mismatches=0`, con el override exacto `1/1/0`, cero drops,
  cero fallos de copia, cero overflows y cero reinicios de contexto.

- PID `18148`, log `Darksiders2DLL-20260913-233812-18148-000.log`: dos
  contratos extraídos de `media.upak`, cero incidencias y dos eventos
  `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE decision=payload`. El primer hash fue
  `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA` y
  el segundo `9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51`.
  Los contadores quedaron `2/2/0`, con
  `general_source_hash_failures=0` y
  `general_source_hash_mismatches=0`; el override exacto siguió siendo el
  único write y registró `1/1/0`. El usuario confirmó visualmente el icono
  editado; la sesión permaneció respondiendo unos 4 min 44 s y no generó un
  nuevo `Application Error`.

La variante actual exige además que las secuencias completas de los
callsites que comienzan en `0x9FA964` y `0x9FF0BF` sean únicas y aparezcan en
sus RVA exactos antes de activar la cohorte. La primera v5 ejecutó los cuatro
escaneos con `/Od`, tardó unos `2.35 s` en activar los hooks y perdió la lectura
temprana del icono; el usuario observó el original y el log confirmó cero hits,
no un rechazo de contrato. v5.1 mantuvo las cuatro verificaciones fail-closed,
optimizó únicamente `signature_scan.cpp`, activó la cohorte en unos `141 ms` y
capturó ambas anclas. El único write habilitado sigue siendo el override estable
del primer payload exacto por tamaño y SHA-256. Al terminar se eliminó el mod
de ancla pasiva y se restauró la DLL estable `2E29A8...B629`, sin cambios en la
metadata de los `.upak`.

### Resultado negativo de la baliza D3DX

Los hooks de D3DX9 y D3DX11 no recibieron ninguna entrada durante una ejecución
de cinco minutos. Por esa evidencia negativa se desactivó el POC D3DX y se aisló
el override en la lectura interna confirmada.

## Hechos estáticos confirmados

### Literal del asset objetivo

El ejecutable contiene el basename ASCII:

`ui_hudicon_passiveability_improved_agility.dds`

Su localización y referencia observada son:

- Offset crudo del literal: `0x141BA40`
- RVA del literal: `0x141C840`
- VA preferida del literal: `0x14141C840`
- Xref de código: RVA `0x18D396`, VA preferida `0x14018D396`
- Función que contiene el xref según unwind info:
  RVA `[0x18ABD0, 0x18D479)`

Secuencia relevante alrededor del xref:

```asm
14018D387  lea  rcx,[rbp+170h]
14018D38E  call  1400C19E4
14018D393  mov  r8b,r13b
14018D396  lea  rdx,[14141C840]
14018D39D  lea  rcx,[rbp+180h]
14018D3A4  call  1400C183C
14018D3AA  lea  rcx,[rdi+20h]
14018D3AE  mov  rdx,rax
14018D3B1  call  1400C1A4C
```

Esto demuestra que el nombre forma parte de un objeto o descriptor construido
por el juego. No demuestra que RVA `0x18D396`, ni las tres funciones llamadas,
sean el resolver de archivos. La ubicación parece útil como breakpoint de
correlación y para inspeccionar el descriptor resultante, pero podría ejecutarse
solo durante inicialización de datos de gameplay.

### Parser OBPK

El literal de magic `OBPK` aparece en:

- Offset crudo: `0x1672C78`
- RVA: `0x1673A78`
- VA preferida: `0x141673A78`

Existe un xref en RVA `0x9FEA15` (VA preferida `0x1409FEA15`) dentro de la
función delimitada por unwind info como RVA `[0x9FE8E8, 0x9FEC14)`. La función:

- recibe en `RCX` un objeto de salida;
- recibe en `RDX` un objeto de stream o smart pointer;
- conserva `R8` en `this + 0x28`;
- recibe un booleano en `R9`;
- usa el método virtual `+0x40` del stream para posicionarse;
- llama a la rutina en RVA `0x0ABDC0` para leer;
- lee cuatro bytes, comprueba el magic `OBPK` y continúa con versión y offsets.

Por ese comportamiento se clasifica como constructor/parser OBPK candidato.
Los call sites estáticos observados son:

- RVA `0x9FB74C`
- RVA `0x9FBD36`
- RVA `0x9FBFB3`

La vtable asociada comienza en RVA `0x1673A80` (offset crudo `0x1672C80`, VA
preferida `0x141673A80`). Sus primeras entradas observadas son:

| Slot | RVA de función | Interpretación provisional |
|---:|---:|---|
| 0 | `0x9FEC14` | Destructor candidato |
| 1 | `0x9FE8E0` | Método corto todavía no clasificado |
| 2 | `0x9FECE8` | Posiciona el stream y abre/procesa una región |
| 3 | `0x9FED44` | Lee un bloque descrito por campos del objeto |
| 4 | `0x9FEE5C` | Método grande de procesamiento de recursos/miembros |
| 5 | `0x9FF654` | No clasificado |
| 6 | `0x0BDBD0` | No clasificado |
| 7 | `0x9FF6FC` | Rebobina el stream y abre/procesa desde cero |
| 8 | `0x9FE584` | No clasificado |
| 9 | `0x9FF750` | No clasificado |

Detalles que sustentan las interpretaciones, sin elevarlas a firmas:

- RVA `0x9FECE8`: si `[this+0x14] != -1`, posiciona el stream en
  `[this+0x28] + offset` y llama a RVA `0x9FC234` con el stream.
- RVA `0x9FED44`: posiciona en `[this+0x28] + [this+0x1C]`, lee
  `[this+0x20]` bytes y llama a RVA `0x104F00` para construir otro objeto.
- RVA `0x9FEE5C`: posiciona en `[this+0x28] + [this+0x24]`, itera estructuras
  y crea streams; es el candidato más prometedor para seguir selección y
  entrega de miembros.
- RVA `0x9FF6FC`: posiciona el stream subyacente en cero y llama a RVA
  `0x9FC234`.

Los campos, tipos, nombres y contratos de esos métodos de parser/vtable siguen
siendo inferencias de flujo de datos. La evidencia dinámica confirmó el
contrato de la lectura genérica en RVA `0x0ABDC0` para el caso observado y
confirmó `0x9FEE5C` como ancla de hook diagnóstica para correlación. No
demuestra los demás métodos de la tabla ni autoriza a usar `0x9FEE5C` como
punto de reemplazo general.

## Ancla de datos para `first_test_mod`

La ruta virtual es:

`media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`

Su localización comprobada en `media.upak` es:

- Segmento: `media/ui/ui_icons_small.`
- Intervalo del segmento: `[0x33250800, 0x332A3000)`
- Tamaño del segmento: `0x52800`
- Layout: OBPK `stream`
- Inicio del zlib: `0x33253804` (`segmento + 0x3004`)
- Tamaño comprimido: `0x4F7FC` (`325,628`)
- Tamaño descomprimido: `0xAC500` (`705,792`)
- Miembro: `#72`
- Intervalo del miembro en el stream descomprimido:
  `[0x4F800, 0x50880)`
- Tamaño del miembro: `0x1080` (`4,224`)
- Tipo OBPK: `6`, DDS
- Código interno de formato: `0x11`
- SHA-256 del DDS original:
  `E2173F05C575677756071AD7DEC88E4CF49BBD0A734F902159E93C79F191B22D`
- SHA-256 del DDS editado:
  `2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719`

`ReadFile` observa el stream comprimido compartido, no un DDS independiente.
Sustituir el resultado de esa lectura por `0x1080` bytes rompería el tamaño, el
zlib y el resto de miembros del segmento. Los breakpoints de archivo son solo
balizas para recuperar la pila y llegar al consumidor posterior.

Durante las pruebas Steam reemplazó `media.upak` como archivo completo a las
`05:10:29Z`, lo que explica el cambio de metadata observado. La copia actual
tiene SHA-1 `D267F6DBE96E90A58AE8E4981CAD47150D6AEA52`, idéntico al hash de
contenido del manifiesto oficial `388411_5667116516349422569.manifest`, y
SHA-256 `C262236AB15E563F5539C33F11851537A4F021A5A3402B8C61FDD8975F3AE993`.
DarksideModManager pudo escanear sus `1,681` segmentos y `75,627` archivos sin
errores estructurales. Así se distingue una renovación completa hecha por
Steam de una escritura parcial atribuible al override.

La misma comparación SHA-1 de archivo completo dio coincidencia para los otros
dos paquetes del depot: `anim_streams.upak` =
`909E63E2E5C1BCB51F5C62C3184F753A7DA70F58` y `maps.upak` =
`C7457FAD94858720532913252ADABC57B685F621`. Los tres `.upak` instalados son,
por tanto, byte a byte los contenidos declarados por el manifiesto de Steam.

## Límite de lo demostrado

La evidencia cubre el override puntual Release y el primer escritor DDS
general Debug en dos identidades concretas. Sus límites son:

- El override Release todavía decide por tamaño y SHA-256 exactos y conserva
  SHA-256 `2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`.
  No incorpora una decisión general de reemplazo.
- El sondeo Debug ya correlaciona dos payloads de segmentos distintos con
  tuplas OBPK estables. El modo de observación conserva la lectura; la opción
  write v6 utiliza esas identidades y los contratos DDS para copiar y verificar
  los candidatos indexados.
- El dry-run general Debug ya recupera los contratos originales desde
  `media.upak`, los une con candidatos del `ModIndex` y exige el SHA-256 runtime
  exacto antes de publicar `GENERAL_DDS_VERIFIED_WOULD_OVERRIDE`; cualquier
  rechazo mapeado usa `GENERAL_DDS_CONTRACT_REJECTED`. Existe evidencia dinámica
  positiva para dos candidatos de segmentos independientes, siempre con
  `buffer_writes=false`, pero todavía no autoriza una copia general.
- V6 verificó dos copias de payload, pero solo el primer DDS/BC3 contiene un
  cambio visual; la segunda textura es un ancla pasiva con bytes idénticos a
  los originales. No hay evidencia todavía para reemplazar modelos, audio,
  materiales, animaciones, Scaleform ni para otras revisiones del ejecutable.
- La captura valida el caso positivo y el log valida el fallback lógico con el
  archivo ausente. No se documenta como validado ningún fallback visual.
- La sesión visual v6 registró `10 min 52 s` sin nuevo `Application Error`,
  superando v5.1 (`4 min 44 s`) y las positivas históricas de 83 y 134 segundos.
  Sigue faltando ampliar la navegación y repetir ejecuciones largas.
- Las lecturas overlapped de `media.upak` continúan siendo balizas. El método
  OBPK `0x9FEE5C` y el catálogo aportan contexto reproducible para dos anclas,
  pero permanecen diagnósticos y no deciden sustituciones por sí mismos.
- La ausencia de entradas D3DX durante cinco minutos justifica dejar ese POC
  desactivado, pero no prueba que ninguna otra textura del juego use D3DX.

## Escritor general v6: comprobado offline y en una sesión dinámica

El 13 de septiembre de 2026 se completó la opción
`EnableGeneralResolverWritePrototype=true` y la matriz offline. El escritor
solo recibe una evaluación `full_dds` o `payload` aceptada por el SHA-256
original. Usa los bytes inmutables del candidato, exige que Windows copie el
tamaño íntegro y compara el hash posterior con el hash del reemplazo elegido.
Una copia parcial cuenta como fallo de escritura; una copia completa cuyo
hash no coincide o no se puede leer cuenta como fallo de verificación. La
detección de ese fallo no revierte bytes ya copiados.

El ejecutable de pruebas incluye el código real del detour en una unidad
separada, con inyección de fallos limitada al propio proceso de pruebas. Se
comprobaron DDS completo/payload, ocho rechazos antes de escribir, cuatro
fallos de copia/verificación y conservación del retorno/`GetLastError`.
La compilación de observación dejó buffers intactos y los cuatro contadores
de escritura en cero. Las pruebas offline Release también pasaron.

Siete variantes pasaron smoke: las cuatro Debug solicitadas, write con
general/identity explícitamente en `false`, Release predeterminado y Release
con las tres opciones de prototipo solicitadas. Los dos últimos son idénticos
byte a byte, miden `617,472` bytes y conservan SHA-256
`D27D35C760BF7597DC579FF459A65E29E4FF56A4670DB00D5EB4ADD6CB3B0620`.
Se verificó la ausencia de objetos/macros/dependencias zlib y mensajes propios
del prototipo. El evento histórico `GENERAL_RESOLVER_LIMITED`, ya presente en
el Release anterior, describe únicamente el caso exacto. Una compilación con
macros forzadas y `NDEBUG` fue rechazada por la barrera del encabezado.

La DLL diagnóstica mide `2,362,880` bytes y está aislada en
`build/diagnostic-v6-general-write/dinput8.dll` con SHA-256
`6DF8889D6EAC23D375852F12EFF8FE4CBBE4976372B220F289A1BCC09AC8A708`.
Su copia pasó smoke. Los logs y hashes están en `build/validation-v6/`.
No se abrió el juego ni se desplegó v6 durante la verificación offline. El proxy
estable `2E29A8...B629`, el único mod `first_test_mod` y la metadata de los
`.upak` permanecen como estaban.

La prueba autorizada del 14 de septiembre instaló temporalmente el ancla
mediante `manage_general_dry_run_anchor.ps1` y desplegó v6 con
`update_deployed_proxy.ps1`. El PID `8420` activó los hooks a `300 ms` de
`SESSION_START`. Produjo dos `GENERAL_DDS_OVERRIDE_HIT decision=payload`
para las identidades `0x77800/0x3800/46` y `0x33250800/0x3000/73`, con
los hashes originales `9726E7...A8FA` y `9F008D...EE51`. Cada evento informa
`replacement_written=true`, `replacement_verified=true`,
`replacement_size=4096` y `replacement_win32=0`; conserva `win32=997`
como error de la lectura original.

Estadísticas finales: `general_candidate_hits=2`, `general_would_override=2`,
`general_write_attempts=2`, `general_writes_completed=2`; todos los fallos de
contrato, hash, escritura y verificación en cero, sin drops ni fallos de copia.
El override exacto quedó en `target_hash_matches=0` y
`replacements_applied=0`, coherente con que el escritor general cambió antes
el buffer objetivo. El ancla pasiva copió sus mismos bytes originales; el
primer DDS usó el payload editado `DA59A8...077F`.

La captura aportada por el usuario muestra el icono con la marca roja en el
árbol de habilidades. El log va de `12:55:14.922Z` a `13:06:07.015Z`,
`652.093 s` (`10 min 52 s`); no apareció un nuevo `Application Error` ni
crash dump del juego. Después del cierre sin intervención forzada, el ancla
fue retirada y el proxy estable quedó restaurado a las `13:06:12Z`.
La comprobación posterior confirmó el hash estable, solo `first_test_mod`
y la misma metadata de los tres `.upak`.

Log original: `Darksiders2DLL-20260914-125514-8420-000.log`. Evidencia local
en `build/dynamic-v6-20260914-125513/`: `session.log`,
`visual-confirmation.jpg`, `runtime-evidence.json`, `before.json`,
`after.json` y los registros de instalación/restauración.

## Próximos pasos de investigación

La siguiente etapa es repetir y ampliar la matriz dinámica del escritor Debug
v6 conservando el contrato demostrado: identidad correlacionada, metadata
idéntica, retorno completo, destino escribible y SHA-256 original, seguido
del hash posterior del reemplazo.
Identidad ausente, metadata inválida, zlib incompleto, límites excedidos y
layouts no soportados deben conservar passthrough. Los layouts `blocks` deben
seguir fuera hasta contar con evidencia dinámica específica de su callsite.
También faltan la confirmación visual del fallback y más navegación/reinicios
prolongados; la primera sesión v6 ya superó diez minutos. Mientras tanto, el
Release permanece limitado al payload y build documentados aquí.
