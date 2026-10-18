# Modelos: contrato experimental 0.5.1-shape-trial

## Malla nueva sin reglas de contenido: 0.9.0-unchecked-trial, 17 de octubre de 2026

Objetivo del usuario: sustituir una malla por otra totalmente nueva y que la DLL
no ponga ninguna prohibición, porque eso todavía no se ha probado en el juego.
`models=override_unchecked` (opcional, desactivado por defecto; `mode=observe`
sigue siendo la barrera global) elimina todas las reglas de contenido:
posiciones fuera de la caja, normales, UV, pesos, huesos, cajas y cualquier
correspondencia con el original.

**Único límite: los tamaños de lectura.** El motor lee la cabecera con el tamaño
original y, después, cada búfer de índices y cada array por vértice con los
recuentos de esa cabecera. `FitModelReplacement` encaja el reemplazo en esos
tamaños:

- La cabecera sale del reemplazo (cajas, paleta de huesos, nombres de material),
  pero conserva los recuentos, tramos, desplazamientos y la tabla de pares del
  original.
- Primero se colapsan todos los triángulos originales a `(a,a,a)`. Luego cada
  material escribe sus triángulos, reindexados, al principio de su tramo
  original, y sus vértices en las ranuras de ese tramo.
- Las ranuras sobrantes conservan registros originales sin referencias.

Se rechaza solo lo que no cabe físicamente: otro tamaño de cabecera, número de
mallas o materiales distinto, otro tipo de malla (con piel o estática), o una
malla o un material con más vértices o triángulos que el original. Para la
máscara de `death_head.2` (malla 3), el hueco es de 432 vértices y 732
triángulos. Una malla mayor necesita otros tamaños de lectura, que es el
«nivel 2», todavía sin investigar.

Para identificar cada lectura, el candidato añade un rango por cada array
(cabecera, índices, atributos, vértices, pesos y huesos; o posiciones y atributos
en las estáticas). En este modo se rechaza cualquier rango cambiado que sea
ambiguo: sin reglas de contenido, saltarse una lectura mezclaría recuentos o
paletas nuevos con arrays viejos.

**Corrección del parser.** El campo `relative` es un desplazamiento y crece con el
archivo. Se validaba como un recuento, con tope de 1.000.000, y eso rechazaba los
modelos de más de 1 MB (`count`), entre ellos `reaper_form_ghost.2` y
`serpent.2`. Ahora debe quedar dentro del archivo. Esto explica 11 de los 13
rechazos del muestreo de borrados del 7 de octubre.

**Pruebas:**
- Fixtures sintéticos: malla nueva con otros registros, otro sentido de giro y
  otra caja; mismo tamaño con UV, posición y peso libres; límites de vértices,
  triángulos y cabecera; candidato y detour real.
- Corpus, mallas «nuevas» derivadas de 300 modelos: borrado por zonas,
  vértices permutados, triángulos barajados por material, posiciones fuera de la
  caja, UV aleatorias y cajas ampliadas. Pasan 300 de 300, y en todos los
  triángulos visibles coinciden en orden con los del reemplazo, con las mismas
  posiciones, UV, pesos y huesos.
- Borrado (`override_shape`) con la misma semilla que el 7 de octubre: pasan 397
  de 400, con la geometría idéntica. Los tres rechazos restantes son reglas de
  contenido de ese contrato estricto: dos emparejamientos no encontrados y una
  base de tangentes del original.
- La edición actual de `death_head.2` también encaja en el modo nuevo: 49
  ranuras sobrantes y 76 triángulos colapsados.

**Juego, 17 de octubre (0.9.0, sesión `Darksiders2DLL-20261018-000727-23376-000.log`).**
El usuario sustituyó la hombrera `media/items/armor/ds1a01_shoulder.2` por una
malla nueva. El original mide 92.597 bytes y el reemplazo 29.885: quedan 618
ranuras de vértice sobrantes y 976 triángulos colapsados. El miembro 249 está en
el segmento de bloques independientes `0xABD000`, tabla `0x19800`. Es la primera
lectura identificada y escrita en el juego dentro de un segmento de bloques.
La sesión registró 27 lecturas de modelo, 27 coincidencias, 9 escrituras
verificadas y 0 fallos; la máscara de `death_head.2` también se escribió. El
usuario confirmó que la malla nueva carga sin fallar, **pero sin sus texturas**.

**Causa de las texturas: segmentos de bloques en el catálogo DDS.** Los tres DDS
nuevos (`DS1A01_Shoulder_Diff/Norm/Spec.dds`) están en ese mismo segmento. El
catálogo de identidades para texturas no activaba `allow_member_blocks`
(`PACKAGE_IDENTITY_ISSUE code=unsupported_layout`), y el de contratos DDS solo
agrupaba streams comunes, así que no hubo ningún candidato. En 0.9.1 ambos
catálogos aceptan bloques: cada miembro es su propio stream (u32 de tamaño y
zlib), con la misma comprobación de extensión y tamaño declarado que ya usan los
modelos. El catálogo offline sobre la instalación da ahora tres candidatos de
subida nativa, de 1024×1024 a 512×1024. Prueba sintética: dos DDS en bloques
producen los mismos contratos que en un stream, y un checksum dañado se rechaza.

**Juego, 0.9.1 (sesión `Darksiders2DLL-20261018-005428-23676-000.log`): sin
texturas.** Los tres DDS se identificaron y verificaron al leerse
(`NATIVE_TEXTURE_SOURCE_VERIFIED`), pero no apareció ningún
`NATIVE_TEXTURE_UPLOAD_SEEN`. Se revisaron los 54 logs: el gancho nativo estuvo
activo en 12 sesiones desde la 0.6.0 y **nunca** vio una subida por
`0xD4E62C` desde `0x1088457`. Lo que muestra el análisis estático:

- `0xD4E62C` tiene solo dos llamadores directos. `0xD7137C` genera una textura
  propia de ruido BGRA de 1 mip, por lo que excluirlo es correcto. El otro está
  en la tarea de recursos `0x1088148`.
- La tarea de recursos elige la función según `[rbx]`: 1 → 2D (`0xD4E62C`),
  2 → volumen (`0xD4F104`) y 3 → cubemap (`0xD4DD40`).
- El analizador DDS del motor (`0xA337AC`, comprobación de `DDS ` y tamaño `0x7C`)
  pertenece a un objeto cargador de imágenes con su propio stream y no a esa
  tarea. Las texturas DDS usan, por tanto, un camino de creación que el gancho
  nativo no cubre.

La subida nativa (0.6.0) queda sin confirmar y, de hecho, sin uso en el juego.

**0.9.2: conversión al contrato original.** Si la textura de reemplazo difiere
del original (dimensiones, mips, formato, o PNG), `ConvertDdsToContract`:

- decodifica el nivel superior (BC1/BC2/BC3/BGRA8/RGBA8/BGR8);
- lo reescala con un filtro triangular separable (bilineal al ampliar, promedio
  al reducir) y regenera la cadena de mips;
- lo codifica en el formato original (ajuste por eje principal en BC, con el
  modo transparente de BC1 si hay alfa) y usa la cabecera original byte a byte,
  que ahora conserva el catálogo de contratos.

El resultado mide exactamente lo mismo que el original y va por la copia del
mismo tamaño en el búfer de lectura. Esa vía funcionó en el juego con la
máscara (0.4.1) y, en este mismo segmento, con `ds1a01_shoulder.2`. La subida
nativa queda solo como último recurso para formatos que no se pueden convertir
(BC4/BC5) o mayores de 4096×4096. Las texturas mayores que el original se
reducen a su tamaño: el HD sigue dependiendo de la vía nativa, sin confirmar.

Texturas del usuario: las tres pasan de 512×1024 a 1024×1024 con la ruta
`converted_stream_copy`, y sus tamaños (699.192, 699.192 y 1.398.256 bytes)
coinciden con los originales. Comparadas con la fuente reescalada, la relación
señal-ruido es de 39,0 dB en la difusa, 46,7 dB en la normal y 34,1 dB en la
especular; el alfa no cambia. El catálogo completo con las tres conversiones
tarda 0,6 s. Pruebas: BGRA → BC1 con endpoints 565 exactos en todos los mips,
BC1 → BGRA, cabecera literal, límites, y una candidata convertida que llega al
escritor de streams.

**Juego, 0.9.2 (sesión `Darksiders2DLL-20261018-011604-23464-000.log`):
confirmado.** Hubo tres `GENERAL_DDS_CANDIDATE route=converted_stream_copy` y,
al cargar la hombrera, tres `GENERAL_DDS_OVERRIDE_HIT decision=payload` (miembros
796, 1202 y 1251). Contadores: 3 candidatas coincidentes, 3 intentos y 3
escrituras completadas, sin fallos de hash, escritura ni verificación; 9
escrituras de modelo y 0 fallos. El usuario confirmó que la malla nueva se ve
con sus texturas.

## Borrado de vértices: 0.8.1-deletion-trial, 7 de octubre de 2026

El exportador de Anansi (commit `9dd6a35`, addon 1.3.0, `native.prune_dcm`) ya
permite borrar vértices y caras de mallas con piel. Produce un `.2` **más
pequeño**: copia íntegros los registros que sobreviven y recalcula los recuentos,
los tramos de material y los offsets de las mallas posteriores. El motor no
puede recibirlo tal cual. Lee el miembro con los tamaños del original empaquetado:
la sesión del 16 de septiembre registró 21 lecturas, cuya suma más 8 bytes de
prefijo por malla da exactamente los 148.961 bytes. Entregar menos bytes
desajustaría ese stream único.

**Solución: reexpansión en la DLL.** `ExpandModelDeletion` reconstruye el borrado
con el layout original. La cabecera y los recuentos siguen siendo los originales.
Cada triángulo que sobrevive se busca, en orden, entre los originales, y sus
vértices vuelven a su ranura. El emparejamiento compara UV, pesos, huesos y los
campos +0/+16, porque posición, normal y tangente pueden haberse editado. Los
triángulos borrados quedan en su sitio como `(a,a,a)`, con área cero, y los
registros sin referencia conservan los bytes originales. El resultado dibuja los
mismos triángulos con los mismos datos que el archivo podado. Después se valida
con `bounded_shape`, que ahora admite ese colapso en el búfer de índices:
`a` debe ser uno de los índices del triángulo original, y la regla no se aplica a
`override_positions`. Se rechaza todo lo demás: mallas estáticas, materiales sin
nombre, cajas, huesos o materiales cambiados, triángulos reordenados o nuevos y
vértices sin triángulo. El candidato añade un rango por búfer de índices, que el
motor lee aparte, tras su prefijo de 8 bytes.

**Edición del usuario:** `death_head.2` sin la parte inferior de la máscara. La
malla 3 pasa de 432 a 383 vértices y de 732 a 656 triángulos; las otras tres
mallas son idénticas. El archivo podado mide 143.997 bytes, con SHA-256
`45F4A0F9B237D9FC3DE2C1460E2C6EC1F3135BCE3CD4BCD4A530FF04A9F93629`. La
reexpansión mide 148.961 bytes, con SHA-256
`E789666660451FC28EB2049FF9BF85EF9B20F2BE4C85082F317EA7585FED7110`, y solo cambia
el rango `(104825, 4392)`. Un script aparte relee ambos archivos con el parser de
Anansi: los triángulos visibles de la reexpansión coinciden uno a uno con los del
archivo podado en posiciones, UV, normales, pesos y huesos, y las submallas y las
cajas son las originales.

**Pruebas:** fixtures sintéticos (colapso en su sitio, borrado combinado con
movimiento, reordenación, UV, peso, cajas y tamaño no menor), candidato y detour
real con rangos de índices. En un muestreo de 400 borrados aleatorios por zonas,
hechos con `prune_dcm` sobre modelos del corpus extraído, la DLL aceptó 387 y su
geometría dibujada coincidió siempre. Rechazó 13 de forma segura: 11 por el límite
de recuento del parser (`count`), uno por un emparejamiento no encontrado
(`mosswarden_mesh_base.2`) y uno por una base de tangentes del original fuera de
tolerancia. Esos casos quedan pendientes y no afectan a esta edición.

**Juego:** se instaló la DLL Release
`B8E069C579AD520570EB6FE365A25B0DFA55C6F25F69096840BC0CFA3C995F2A` con
`models=override_shape`. El respaldo de la 0.8.0, el INI y el mod está en
`build/deletion-trial/installation-backup`. La sesión
`Darksiders2DLL-20261008-044815-5004-000.log` registró:

- `MODEL_CANDIDATE ... deletion_expanded=true mod_bytes=143997 removed_vertices=49 removed_triangles=76`.
- 21 lecturas del miembro 187 y 8 coincidencias (los cuatro búferes de índices
  y los cuatro de vértices, en los offsets previstos).
- Lectura 429: `MODEL_OVERRIDE_HIT requested=4392 offset=104825` con
  `replacement_written=true replacement_verified=true`, error 0.
- Un intento, una escritura, cero fallos y cero eventos perdidos.

La metadata de los `.upak` no cambió. El usuario confirmó que el juego carga el
modelo con los vértices borrados sin fallar. La evidencia está en
`build/deletion-trial` (`override.log` e `integration-state.json`).

**Verificación:** `scripts/verify_release.ps1 -EvidenceName validation-deletion-trial`,
con `-GameDirectory` y `-ModelModsDirectory build/deletion-trial/mods`, dio PASS
en sus 20 comprobaciones: matriz Debug/Release/observación con catálogo real,
dos Release idénticas, smoke tests de las tres DLL, endurecimiento PE e
instalación y `.upak` sin cambios. La Release reproducible tiene el mismo SHA-256
que la DLL probada en el juego.

**Alcance:** borrado en mallas con piel de este modelo, en una sesión. No se han
evaluado la estabilidad prolongada, otros modelos ni la adición de vértices. Añadir vértices
sigue necesitando cambiar el tamaño, que es la investigación pendiente del
«nivel 2».

## Integración con ANANSI, 16 de septiembre de 2026

El contrato `bounded_shape` añade al XYZ las normales en +20, duplicado normal.z
en +32 y tangente XYZW en +36 del registro skinned de 52 bytes. Valida vectores
finitos, longitudes unitarias, perpendicularidad, signo ±1 y duplicado normal.z,
tanto en la plantilla como en el resultado de cada bloque editado. +0 (número
de influencias), +16 (desconocido), UV, pesos, huesos, índices y metadata siguen
inmutables. Las cajas no pueden cambiar; las posiciones deben quedar dentro de
la AABB de los vértices originales. Las mallas estáticas solo admiten no-op.
Este subconjunto acepta la exportación concreta de la cabeza; no acepta todos
los resultados posibles del addon (ampliación de cajas, UV o pesos).

`models=observe` valida este contrato; `models=override_shape` escribe solamente
rangos cuyo contenido cambia. `override_positions` conserva el contrato anterior.
Los rangos idénticos producen `MODEL_UNCHANGED_VERIFIED`, nunca un falso HIT.
Cada bloque editado mantiene juntos posición/normal/tangente y no depende de
cambios de cabecera, que podrían leerse por separado. La contención en AABB de
modelo no prueba culling en espacio de hueso: sigue requiriendo prueba visual.

La edición del usuario estaba nombrada `death.2`, pero pertenece a
`media/characters/death/death_head.2`: 148.961 bytes, cuatro mallas y 71 vértices
movidos de la malla 3. Las cajas no cambiaron. La preparación conserva ambos
archivos bajo `build/shape-trial` y usa el nombre correcto para el candidato.

El catálogo del paquete instalado resolvió base 825704448, tabla 79872, miembro
187. El SHA-256 del original recuperado coincide con el extraído:
`70349AA4663AC54EDA6A03FB1BA506E0F887F9C2254A28B1DC3BDA2CA32482FD`.
La edición tiene SHA-256
`9D7DF5C03E4AB2C9DC33C70C69427B1284179842DDC5E1E2720E38AEE073FA20`.
El bloque modificado está en offset 117857 y mide 22464 bytes; también cambian
las huellas del archivo completo y del payload desde offset 3965. Las otras
tres mallas tienen hashes idénticos.

Verificación actual: `scripts/verify_release.ps1 -EvidenceName validation-shape-trial`
con `-GameDirectory` y `-ModelModsDirectory build/shape-trial/mods`.
Paquete: `scripts/prepare_model_trial.ps1`, salida `build/shape-trial/package`.
El criterio final sigue siendo lectura identificada, escritura verificada y
confirmación visual del modelo en el juego; el catálogo no acredita ese último paso.

La matriz Debug/Release/observación y los smoke tests de las tres DLL pasaron.
Los dos Release son idénticos: SHA-256
`74BE4EE543193A971914B2F5DC4E0F86FB914BC9628D2C0DE41008C4F98A75F7`.
Se instaló esta DLL con `models=observe`, conservando DDS en modo override.
Se corrigió el nombre de la cabeza y se comprobó de nuevo el catálogo instalado.
La DLL previa, la copia mal nombrada y la ausencia original de INI están
registradas en `build/shape-trial/installation-backup/state.json`.
Restauración con el juego cerrado:
`build/shape-trial/Manage-Trial.ps1 -Action Restore`.
La metadata de `.upak` no cambió durante instalación; ningún paquete se escribió.
La sesión `Darksiders2DLL-20260916-183715-7132-000.log` confirmó en el juego la
lectura del miembro 187: 21 lecturas, cuatro bloques de vértices reconocidos,
cero escrituras de modelo y cero fallos. En la lectura 431 se obtuvo
`MODEL_VERIFIED_WOULD_OVERRIDE changed=true offset=117857 requested=22464 returned=22464`
con SHA-256 `1DAFD1E289FC09E0D87884B63882E5E501479F38A321E3E487D6C53F62669522`.
Las otras tres mallas produjeron `MODEL_UNCHANGED_VERIFIED`. Las 17 lecturas
restantes son cabecera, índices, atributos, pesos e índices de huesos, que
permanecen intactos. No hace falta ampliar los rangos para esta edición.
El DDS de la máscara produjo escritura y verificación correctas en esa sesión.
El usuario confirmó aspecto normal, coherente con observación. Con el juego
cerrado se activó `models=override_shape`, manteniendo el respaldo restaurable.
La segunda sesión `Darksiders2DLL-20260916-185250-5724-000.log` produjo a las
18:53:02.181Z `MODEL_OVERRIDE_HIT` para ese mismo rango, con `changed=true`,
`replacement_written=true`, `replacement_verified=true` y error de escritura 0.
Estadísticas a las 18:56:37.304Z: 21 lecturas, cuatro coincidencias, un intento,
una escritura, cero fallos de modelo y cero eventos perdidos. También se
sustituyó y verificó el DDS de la máscara, sin fallos de escritura/verificación.

El usuario confirmó «Exitoooooo» y aportó una captura frontal de Death con la
edición visible. Evidencia local: `build/shape-trial/override.log`,
`visual-confirmation.jpg` e `integration-state.json`. Se verificó que la metadata
de los tres `.upak` coincide con el estado previo. La instalación conserva
0.5.1 y `models=override_shape`, con respaldo restaurable de 0.4.1.

**Criterio de primera integración cumplido para esta copia:** recurso correcto,
lectura y escritura verificadas, edición visible confirmada. La captura y esta
sesión corta no acreditan cobertura general de animaciones, culling, otros
modelos, cambios de cajas/UV/pesos ni estabilidad prolongada.

## Antecedente: 0.5.0-model-trial

Preparación del 15 de septiembre de 2026. La validación visual de 0.4.1 corresponde
a la máscara DDS; **no valida este nuevo binario ni reemplazos de geometría**.

## Contrato

`models=off|observe|override_positions`, desactivado por defecto. El índice solo
incluye `.2` cuando se activa este modo. `mode=observe` sigue siendo una barrera
global. Los `.o3d`, DLL de terceros, OBJ y FBX quedan fuera del índice de producción.

El catálogo se une por ruta canónica a un miembro tipo 2 de `media.upak`.
`model_validation.cpp` interpreta metadata y rangos de vértices a partir del
original recuperado. Exige igual tamaño y bytes idénticos fuera de las posiciones
XYZ. Las posiciones deben ser finitas y quedar en el AABB local calculado del
original, con tolerancia numérica. No se modifican normales, índices, UV, pesos,
referencias, matrices, límites de cabecera ni cantidades.

La identidad dinámica usa `(package_base, member_table_offset, member_ordinal)`.
`read_ordinal` es solo telemetría, como en la corrección de la máscara. Cada rango
de lectura exige tamaño y SHA-256 originales únicos; no se buscan patrones dentro
de buffers arbitrarios. Se preparan archivo completo, datos desde `data_offset`
y cada bloque de vértices. El destino se vuelve a comprobar bajo la exclusión del
escritor y se verifica después de copiar. El bloqueo tras fallos es compartido con
DDS. Los detours consumen snapshots inmutables y publican eventos POD, sin leer
mods ni paquetes desde el hook.

## Bloques independientes y armas reales

El segmento `media/items/weapons` comienza en `0x68A02800`, mide 407 707 648 bytes
y su payload/tabla de runtime está en `0x1B800`. **Es una variante de bloques
independientes**, no un único stream. La tabla final contiene offsets crecientes;
los tamaños originales salen de META y se contrastan con el prefijo del bloque.
La nueva opción de catálogo mantiene esta variante desactivada por defecto;
`BuildModelCandidates` la activa explícitamente. El lector original reutiliza la
descompresión acotada, verifica tamaño, EOF zlib y checksum y descarta todos los
resultados si falla cualquier bloque solicitado.

Pruebas locales: copias de `axea01.2` (miembro 778, 106 694 bytes) y
`hammera01.2` (miembro 838, 84 834 bytes), con una coordenada de un vértice movida
hacia el centro. Ambos modelos tienen un mesh skinned: 1 058 y 841 vértices.
Los originales se recuperan del paquete instalado y coinciden con los extraídos.
Las copias son fixtures privados bajo `build/model-trial/fixtures`; no son mods
distribuibles ni forman parte del código fuente.

Rangos preparados para el hacha: `(0,106694)`, `(410,106284)`, `(30518,55016)`.
Para el martillo: `(0,84834)`, `(380,84454)`, `(24282,43732)`.

El desensamblado de `0x9FEE5C` muestra que tanto el stream común como el reinicio
por miembro convergen en el bucle que lee con retorno `0x9FF0D2`; su índice de
archivo sigue en `[rsp+0x30]`. Esto respalda reutilizar la identidad corregida,
pero falta observar qué rangos solicita un modelo real y confirmar su apariencia.

## Verificación reproducible

`scripts/verify_release.ps1 -EvidenceName validation-model-trial` construye
pruebas Debug, Release y observación, dos DLL Release reproducibles, DLL Debug y
smoke tests de exports. Con `-GameDirectory`, `-AssetSource` y
`-ModelModsDirectory` comprueba además la instalación y candidatos reales sin
lanzar el juego. Evidencia en `build/validation-model-trial/VALIDACION.json`.

Las pruebas nuevas cubren layouts estáticos/skinned, modificación fuera de
posiciones, cambios de longitud, NaN/infinito, límites locales, originales
truncados, rangos ambiguos, opt-in del índice/configuración, streams comunes y
bloques independientes, checksum, tamaños/extent incorrectos y presupuestos.
El harness llama al detour real mediante un frame MASM, con miembro 223 y contador
de lectura independiente, y simula escrituras fallidas/parciales y fallos de
verificación. Las pruebas DDS anteriores se ejecutan también.

Pendiente: modelo nativo editado por el usuario, sesión de observación, sesión de
escritura verificada y confirmación visual. La instalación del usuario permanece
en 0.4.1 mientras se prepara este artefacto. No se han modificado `.upak`.
