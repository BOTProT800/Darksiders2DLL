# Modelos: contrato experimental 0.5.1-shape-trial

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
