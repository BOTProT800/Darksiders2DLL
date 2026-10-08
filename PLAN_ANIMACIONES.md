# Plan: reemplazo de animaciones `.anm`

Propuesta del 24 de septiembre de 2026.

**Estado:** la fase C (validador y catálogo) está hecha. De la fase D ya
existen la observación en el hook y el paquete de prueba (0.8.0-animation-trial);
falta la sesión en el juego. La evidencia está en
[research/ANIMATION_TRIAL.md](research/ANIMATION_TRIAL.md). Las fases A, B, E y F
siguen pendientes. El DLL **no escribe animaciones**: con
`animations=observe|override_keys` solo valida candidatos y observa lecturas.
Build de referencia: `Darksiders2.exe` con SHA-256
`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`.

## Objetivo

Que un mod de archivos sueltos sustituya una animación `.anm` de `media.upak`
con el mismo método que funcionó con `death_head.2`. El exportador y el cargador
comparten un contrato. Primero se observa, después se escribe con verificación
y al final se confirma visualmente. Cualquier duda deja el original intacto.

## Evidencia de partida

**Formato.** ANM v1, decodificado por Anansi
(`Anansi/services/darksiders/animation.py`):

| Parte | Contenido |
|---|---|
| Cabecera, 16 bytes | `ANM`, versión u8 = 1, endianness u8 = 0, FPS u8, frames u16, tamaño u32 (= total − 16), flags u32 (bit 0 = bucle; `0x2` sin semántica conocida) |
| Nombre | Cadena terminada en cero |
| Pista | Flags u8 (bits 0–5); nombre inline o hash u64 alineado a 8; peso i16 |
| Rotación | Cuaternión comprimido: segmentos de 4 bytes (base de 6 bits y paso por componente, longitud de 6 bits y componente omitida) y residuos int8 |
| Posición | Segmentos `u16 longitud, i16×3 base` y residuos int8 × 0,032 (× 0,001 con el flag 16) |
| Escala / escalar | f16 × 3 / u8 ÷ 255 |
| Tiempos | Deltas u8 acumulados por curva |

**Corpus.** 6.865 archivos y 134.862.492 bytes, todos decodificados por Anansi
sin bytes sobrantes. Hay seis variantes de flags de pista y 94 clips con
identificadores repetidos. El oráculo que emula las rutinas del ejecutable
(`scripts/qa_darksiders_animation_native.py`) coincide con errores máximos de
0,000122 en posición y 2,2e-7 en cuaternión. Anansi importa a Blender, pero
**no exporta `.anm`** (`docs/darksiders-animation-quality-plan.md`).

**Empaquetado.** Consulta de solo lectura del 24 de septiembre con el parser
OBPK de DarksideModManager sobre la instalación local:

- Los 6.865 `.anm` están en 292 segmentos de `media.upak`, **todos de stream
  único**. No hay variantes de bloques.
- 225 de esos segmentos también contienen modelos `.2`.
- El mayor `.anm` mide 2.241.062 bytes, lejos del límite de 64 MiB por archivo.
- `media/characters/death.` (base 825704448) contiene 1.057 `.anm`. Es el mismo
  segmento donde el hook identificó y sustituyó `death_head.2` (miembro 187).
- `anim_streams.upak` no tiene tabla de contenidos y queda fuera de este plan.

**Implicación.** Es plausible, pero **no está probado**, que las lecturas de
`.anm` pasen por el hook de lectura actual con la identidad
`(package_base, member_table_offset, member_ordinal)`. La fase D lo comprueba
antes de que exista cualquier escritura.

## Decisión principal: dos niveles de contrato

### Nivel 1: `keys_in_place` (mismo tamaño)

Reutiliza la copia verificada sobre el buffer que ya usan los modelos. Solo
cambian valores; la estructura queda idéntica byte a byte.

**Inmutable:** cabecera completa (FPS, duración, flags, tamaño), nombre, orden
y número de pistas, flags, nombres y hashes de pista, pesos, número de claves
por curva, longitud de cada segmento, tiempos y relleno de alineación.

**Mutable:** bases y selectores de paso de los segmentos de rotación, componente
omitida, residuos, bases de posición, valores f16 de escala y bytes escalares.

Para quien anima esto significa:

- **Sí:** cambiar la rotación, la posición o la escala de huesos en las claves
  que ya existen. Ejemplos: corregir un brazo o girar más la cabeza.
- **No:** añadir o quitar claves, cambiar tiempos, duración, FPS o bucle, ni
  añadir o quitar huesos.
- Una pista constante (una sola clave) sigue siendo constante.
- Un movimiento que no quepa en el rango de cuantización de un segmento hace que
  el exportador **rechace** el archivo en vez de aproximarlo en silencio. En
  posición, ese rango es de ±4,06 unidades alrededor de la base del segmento
  (±0,127 con el flag fino).

### Nivel 2: tamaño y estructura libres

Permitiría reanimar de verdad. Un `.anm` más grande no cabe en el buffer que el
motor reserva con el tamaño de META, y en un stream único no se puede cambiar el
tamaño sin desincronizar la lectura. Hace falta intervenir en el consumidor del
tipo 8, como hizo 0.6.0 con `Upload2D` para texturas. Es investigación (fase F)
y no se empieza hasta cerrar el nivel 1.

## Fases

Cada fase tiene un criterio de salida. Si no se cumple, no se pasa a la siguiente.

### Fase A: especificación y fixtures

1. `research/ANIMATION_FORMAT.md`: mapa campo por campo que separe estructura y
   valores, con referencia al parser de Anansi.
2. Ampliar `qa_darksiders_animation_corpus.py` para contar bytes estructurales
   y bytes de valor en los 6.865 archivos.
3. Fixtures sintéticos, generados por script, que cubran las seis variantes de
   flags de pista y el flag fino de posición. Anansi exporta sus valores
   decodificados como vectores dorados en JSON. No entra ningún asset comercial
   en git.

**Salida:** el mapa explica el 100 % de los bytes del corpus y los fixtures
reproducen los valores dorados.

### Fase B: exportador conservador (repositorio Anansi)

1. Escritor que parte de los bytes del `.anm` original como plantilla, igual que
   el exportador `.2`. Evalúa la acción de Blender en los tiempos originales y
   recodifica los valores dentro de los segmentos existentes.
2. Invertir exactamente la conversión del importador: la convención pasiva de
   los cuaterniones y los dos modos de orientación. Hay que añadir una prueba de
   regresión para el error de convención que ya se detectó al importar.
3. Una ida y vuelta sin cambios debe dar **bytes idénticos en los 6.865 archivos**.
4. En una edición, se decodifica de nuevo y se compara con la acción. Se informa
   del error máximo y se rechaza lo que supere la cuantización o no quepa.
5. Contrastar el archivo exportado con el oráculo nativo (RVA `0x7EA584`,
   `0x7E9BCC`, `0x7E9EA4` y `0x7EA3D0`).
6. Operador de Blender «Exportar `.anm` (misma estructura)» que nunca
   sobrescriba la fuente.

**Salida:** ida y vuelta byte idéntica en todo el corpus, edición sintética
aprobada por el oráculo y aceptada por el validador C++ de la fase C.

### Fase C: validador y catálogo en el DLL, sin hooks

**Hecha el 24 de septiembre.** Además de lo previsto se añadió
`--anim-corpus`, que dio paridad total con Anansi en los 6.865 clips. Las
curvas editadas se limitan a una magnitud ≤ 1; ningún original pasa de 0,7501.

- `animation_validation.{h,cpp}`: port del parser y contrato `keys_in_place`.
  Parsea el original y el reemplazo, exige la misma estructura y decodifica los
  valores nuevos: finitos, f16 sin NaN ni infinito y cuaterniones con suma de
  cuadrados ≤ 1,0001, igual que Anansi.
- `animation_candidate.{h,cpp}`: une el `ModIndex` con las entradas tipo 8 del
  catálogo y recupera el original con `ReadMediaPackageMembers`, como
  [model_candidate.cpp](model_candidate.cpp). Prepara rangos para el archivo
  completo, la cabecera (0, 16) y el cuerpo (16, resto). Los rangos definitivos
  dependen de la fase D.
- `mod_index`: opción `include_animations` para `.anm`, desactivada por defecto.
- `loader_config`: `animations=off|observe|override_keys`, con `off` por defecto.
- `offline_tests`: comandos `--anim-check original editado` y
  `--anim-catalog juego mods`, más el script `scripts/check_animation.ps1`.
- `tests/animation_tests.{h,cpp}` rechaza:
  - cambios en claves, tiempos, longitud de segmento, flags, nombre, hash, peso,
    cabecera o tamaño;
  - f16 NaN o infinito y cuaterniones inválidos;
  - truncamientos, bytes sobrantes, identidades ambiguas y activación sin opt-in.
- Las pruebas de DDS y modelos deben seguir pasando en la matriz de
  `scripts/verify_release.ps1`.

**Salida:** `ANIM_CHECK_PASS contract=keys_in_place changed=1` con la edición
real y `ANIM_CATALOG_PASS` contra el paquete instalado.

### Fase D: observación en el juego, sin escrituras

**Código y paquete listos el 24 de septiembre; falta la sesión.** La rama de
observación no comparte todavía el escritor de modelos. Esa unificación pasa a
la fase E, para que este build no tenga ninguna ruta de escritura de animaciones.

- `ObserveAnimationRead` en [resolver_probe.cpp](resolver_probe.cpp), junto a
  `ObserveModelRead`. Reutiliza la identidad, el hash y la cola de eventos.
- Conviene extraer el núcleo común de `ObserveModelRead`: buscar el rango por
  tamaño y hash, volver a comprobarlo bajo `g_writer_busy`, copiar, verificar y
  bloquear tras un fallo. Así modelos y animaciones comparten un solo escritor.
  Las pruebas de modelos deben pasar sin cambiarlas.
- Logs `ANIMATION_CANDIDATE`, `ANIMATION_VERIFIED_WOULD_OVERRIDE`,
  `ANIMATION_UNCHANGED_VERIFIED` y `ANIMATION_READ_UNMATCHED`, con tamaño y SHA-256.
- Sesión con `animations=observe`. Los DDS siguen como estén configurados.

Preguntas que debe responder la sesión:

1. ¿Se lee el miembro por este hook?
2. ¿En qué rangos?
3. ¿Se lee una vez por sesión o en cada carga de zona?
4. ¿La lectura ocurre después de activar los hooks?

**Salida:** `ANIMATION_VERIFIED_WOULD_OVERRIDE changed=true` en el rango que
contiene la edición, sin fallos.

- Si solo aparece `ANIMATION_READ_UNMATCHED`, se adaptan los rangos antes de
  habilitar la escritura.
- Si el miembro nunca se lee por este hook, el nivel 1 no es viable por esta vía
  y se pasa directamente a la investigación de la fase F.

### Fase E: escritura verificada y confirmación visual

- Con el juego cerrado, respaldo restaurable de la DLL, el INI y el mod, con el
  patrón de `Manage-Trial.ps1`. La preparación va en
  `scripts/prepare_animation_trial.ps1`.
- `animations=override_keys` debe producir `ANIMATION_OVERRIDE_HIT changed=true
  replacement_written=true replacement_verified=true`, sin fallos.
- El usuario confirma el cambio con una captura o un vídeo. También se revisan:
  - transiciones y mezclas con otros clips;
  - que no haya deriva de la raíz;
  - estabilidad en una sesión más larga;
  - que al retirar el mod vuelva el original.
- Documentación: `distribution/ANIMACIONES_PRUEBA.md`,
  `research/ANIMATION_TRIAL.md`, README y CREDITS (parser adaptado de Anansi,
  licencia MIT).

**Salida:** recurso correcto, lectura y escritura verificadas y cambio visible
confirmado. Igual que con la cabeza, eso acredita **ese clip**, no la
compatibilidad general.

### Fase F: nivel 2, investigación

Empieza solo después de E.

1. Seguir el tipo 8 desde el bucle de miembros (`0x9FEE5C`, retorno `0x9FF0D2`)
   hasta la fábrica de ANM. Las RVA del oráculo sirven de punto de entrada:
   `0x7C6A68` prepara cada pista con un puntero a sus datos.
2. **Propiedad del buffer.** Averiguar quién lo reserva, si los objetos de runtime
   guardan punteros a él (`0x7C6A68` indica que sí), quién lo libera, con qué
   asignador y si se conserva el tamaño de META.
3. Diseño candidato, como en 0.6.0: registrar el buffer tras la lectura
   verificada y, en el consumidor, sustituir el puntero y el tamaño por los del
   reemplazo, que vive durante todo el proceso. Solo si se demuestra que el
   motor nunca libera ni reasigna ese buffer. Si no, hay que reservar con el
   asignador del juego. Si no hay prueba, se mantiene fail-closed.
4. Investigar la semántica antes de permitir cambios de duración o FPS: flag de
   cabecera `0x2`, canal escalar, eventos y tiempos de golpe, y movimiento de
   raíz.

**Salida:** un documento de ABI y propiedad del buffer con evidencia estática y
dinámica. No se escribe código del nivel 2 sin él.

## Primera prueba propuesta

`media/characters/death/D_Idle.anm` (9.580 bytes), del segmento ya validado con
la cabeza. Es probablemente el reposo de Death, pero la fase D debe confirmar
que se lee. Edición: girar unos grados un hueso visible que no sea la raíz, por
ejemplo la cabeza o un brazo. No se toca la raíz, para no alterar el
desplazamiento.

## Riesgos

- **El hook podría no ver estas lecturas.** La fase D lo detecta sin escribir nada.
- **Movimiento de raíz.** Si el motor extrae desplazamiento de la animación,
  editar la raíz puede afectar al gameplay. Queda fuera de la primera prueba.
- **Clips compartidos.** Un `.anm` puede usarse en varios personajes o
  situaciones (por ejemplo `absolom_common/Death_Interactive.anm`). La edición
  se aplica en todos los usos de esa ruta.
- **Pérdida por cuantización.** Recodificar añade hasta un paso de error. Se mide
  en la fase B y se informa.
- **Bloqueo compartido.** Un fallo de escritura bloquea también DDS y modelos
  hasta reiniciar, como hoy.
- **Assets comerciales.** Ningún `.anm` real entra en git. Las copias de trabajo
  quedan en `build/`.

## Fuera de alcance

- `anim_streams.upak`.
- Añadir clips nuevos o cambiar qué clip usa cada acción.
- Esqueletos `.o3d`, rig o pesos del `.2`.
- Eventos, física y tela.
- Otros ejecutables.

## Dependencias y orden

```text
A ──► B (Anansi) ──┐
  └─► C (DLL) ─────┴─► D (juego, observe) ──► E (juego, override) ──► F
```

B y C avanzan en paralelo y convergen en el mismo archivo de prueba. El usuario
interviene en la edición en Blender y en las sesiones D y E: lanza el juego,
autoriza la instalación temporal y confirma el resultado visual.
