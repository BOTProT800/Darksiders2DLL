# Animaciones: fase C del plan (validador y catálogo, sin hooks)

Trabajo del 24 de septiembre de 2026, fase C de
[PLAN_ANIMACIONES.md](../PLAN_ANIMACIONES.md). El código sigue identificándose
como `0.7.0-inventory-trial`. Esta fase no intercepta lecturas ni escribe bytes
de animación, así que no se asigna versión nueva hasta la fase D.

## Qué se implementó

- `animation_validation.{h,cpp}`: port en C++ del lector ANM v1 de Anansi
  (`services/darksiders/animation.py`) y contrato `keys_in_place`.
- `animation_candidate.{h,cpp}`: une el índice de mods con los miembros tipo 8
  del catálogo de `media.upak` y recupera cada original con
  `ReadMediaPackageMembers`. Solo acepta segmentos de stream único. Prepara tres
  rangos provisionales: clip completo `(0, tamaño)`, cabecera `(0, 16)` y cuerpo
  `(16, tamaño − 16)`.
- `mod_index`: opción `include_animations` para `.anm`, desactivada por defecto.
- INI: `animations=off|observe|override_keys`, con `off` por defecto. En esta
  versión, `observe` y `override_keys` **solo construyen y registran el
  catálogo**. Eventos: `ANIMATION_CANDIDATE`, `ANIMATION_REJECTED`,
  `ANIMATION_CATALOG_FAILED`, `ANIMATION_CATALOG_READY` y `ANIMATION_MODE`.
  Ningún hook consume los candidatos.
- `offline_tests`: `--anim-check ORIGINAL EDITADO`, `--anim-catalog JUEGO MODS` y
  `--anim-corpus CARPETA [INFORME.tsv]`. Se añaden `scripts/check_animation.ps1`
  y los parámetros `-AnimationModsDirectory` y `-AnimationCorpusDirectory` de
  `scripts/verify_release.ps1`.

## Contrato `keys_in_place`

El validador analiza el original y el reemplazo, y marca los bits que una
edición puede cambiar. Cualquier otra diferencia se rechaza, incluido el relleno
de alineación.

| Zona | Bits mutables |
|---|---|
| Segmento de rotación, bytes 0–2 (base de 6 bits y selector de paso) | todos |
| Segmento de rotación, byte 3 | bits 6–7 (componente omitida); la longitud del segmento no cambia |
| Residuos de rotación y de posición (int8) | todos |
| Segmento de posición, bytes 2–7 (base i16 × 3) | todos; la longitud u16 no cambia |
| Escala (f16 × 3) y canal escalar (u8) | todos |

Comprobaciones de valores:

- Cualquier clip: suma de cuadrados de los componentes almacenados ≤ 1,0001,
  el mismo límite del decodificador de Anansi, con la misma aritmética float32.
- Curvas de rotación editadas: ≤ 1,0. El comportamiento del motor por encima de
  1 no está verificado.
- Escalas: sin infinitos ni NaN.

## Evidencia

**Pruebas sintéticas** (`tests/animation_tests.cpp`, `tests/offline_tests.cpp`).
El generador del fixture registra por su cuenta los bits mutables. La prueba
invierte cada bit del clip: todo bit estructural se rechaza y todo bit de valor
se acepta, salvo que salga del rango del decodificador. También se prueban:

- truncamientos con el campo de tamaño corregido, que solo se aceptan en límites
  de pista;
- cabecera, flags, tiempos repetidos y cuaterniones fuera de rango;
- el límite de la esfera unidad (1,0000229 rechazado en una edición, 0,9964
  aceptado);
- escalas no finitas, rangos y ambigüedad de candidatos;
- la unión con un paquete OBPK tipo 8, un tipo equivocado, checksum dañado y
  layout de bloques;
- la configuración y la activación explícita.

**Corpus.** `--anim-corpus` sobre la extracción local:

- 6.865 archivos y 134.862.492 bytes, todos aceptados.
- 679.399 pistas y 19.999.238 claves.
- Coincidencia archivo por archivo (pistas, claves y bytes) con el registro de
  Anansi `temp/anm-corpus-final.json`: cero diferencias.

La magnitud máxima es 0,750109, así que ningún original supera 1. Es coherente
con la codificación de los tres componentes menores (se omite el mayor). El
exportador de la fase B debería mantener esa convención.

**Catálogo instalado** (solo lectura, metadata de los `.upak` sin cambios):

- `media/characters/death/d_idle.anm` resuelve a base 825704448, tabla 79872,
  miembro 610. Es el mismo segmento que `death_head.2` (miembro 187).
- El original recuperado del paquete tiene SHA-256
  `8EE83F6892C04FC519FF675E11FFA5672A5F080F72B22F13801000465DB830CF`, igual que
  el archivo extraído.
- Una sonda de valor suma 1 a un residuo de la pista 9 (hash
  `0x71A41E8C1965AB4F`), con un cambio máximo de 0,0099°. Produce
  `ANIM_CATALOG_PASS` con `changed_tracks=1`: la cabecera queda idéntica y el
  cuerpo y el clip completo cambian.
- La sonda y su generador están en `build/animation-catalog/`, fuera de Git.

**Matriz** `scripts/verify_release.ps1 -EvidenceName validation-animation-catalog`,
con `-GameDirectory`, `-ModelModsDirectory build/shape-trial/mods`,
`-AnimationModsDirectory build/animation-catalog/mods` y
`-AnimationCorpusDirectory` sobre la extracción. `PASS` en sus 26 comprobaciones:

- pruebas Debug, Release y observación, cada una con los catálogos de modelos y
  animaciones y el corpus;
- dos Release idénticos byte a byte, con SHA-256
  `36E4EF586C604815F3FD0B4F10821E916E46F74A3CCA7D504197EF04D6AF5B44`;
- build Debug y smoke tests de las tres DLL;
- protecciones PE (`0x4160`);
- DLL instalada, metadata de los `.upak` y `scripts.obsp` sin cambios.

Evidencia en `build/validation-animation-catalog/VALIDACION.json`. Esta DLL no
se ha instalado ni probado en el juego.

## Límites

- Nada de esto demuestra que el motor lea los `.anm` por el hook de lectura, ni
  en qué rangos. Eso lo responde la fase D.
- La sonda es invisible a propósito: sirve para la cadena offline, no como
  prueba visual.
- Los tres rangos son provisionales hasta observar las lecturas reales.
- Todavía no hay exportador en Anansi. Una edición solo puede prepararse a mano
  o con herramientas de laboratorio como la sonda.

# Fase D: observación en el juego (0.8.0-animation-trial)

## Implementación

- `ObserveAnimationRead` en `resolver_probe.cpp`, llamado desde el mismo detour
  de lectura (RVA `0xABDC0`) que modelos y DDS. Usa la identidad corregida
  `(package_base, member_table_offset, member_ordinal)`.
- Para cada lectura completa de un miembro candidato, de hasta el tamaño del
  clip, calcula el SHA-256 del buffer de destino. Lo hace mediante
  `ReadProcessMemory` acotado, sin escribir. Después lo compara con los tres
  rangos del candidato.
- Las lecturas no previstas también publican su tamaño y hash, así que se
  pueden localizar en el original sin conjeturas.
- Esta rama **no tiene código de escritura**. No toca `g_writer_busy` ni el
  bloqueo tras fallos. La unificación con el escritor de modelos queda para la
  fase E.
- Eventos `ANIMATION_VERIFIED_WOULD_OVERRIDE`, `ANIMATION_UNCHANGED_VERIFIED` y
  `ANIMATION_READ_UNMATCHED`, más `animation_reads` y `animation_matches` en
  `RESOLVER_PROBE_STATS`.
- El bootstrap instala el hook también cuando solo hay candidatos de animación.

## Pruebas del detour

`TestAnimationRuntime` llama al detour real mediante el marco MASM y mantiene
armados el escritor compartido y el de modelos. Comprueba:

- que los tres rangos se reconocen, con su hash;
- que una lectura no prevista de 100 bytes se registra con su hash;
- siete puertas de rechazo: paquete, miembro, llamador, lectura corta, tamaño
  excesivo, destino ilegible y tamaño cero;
- que el buffer nunca cambia y que no hay ninguna llamada de escritura.

## Paquete y sesión

- `scripts/prepare_animation_trial.ps1` genera `build/animation-trial/package`
  a partir de la evidencia verificada.
- `scripts/manage_animation_trial.ps1` tiene cuatro acciones:
  - `Plan`: solo lectura; ejecuta además el catálogo contra el juego.
  - `Observe`: guarda un respaldo, instala la DLL, añade `animations=observe`
    al INI existente y copia la sonda.
  - `Report`: solo lectura; copia y resume el log de la sesión.
  - `Restore`: devuelve la DLL, el INI y la carpeta de mods a su estado previo.
- Guía: `distribution/ANIMACIONES_PRUEBA.md`.

## Verificación

`scripts/verify_release.ps1 -EvidenceName validation-animation-trial`, con los
mismos parámetros que en la fase C: `PASS` en sus 26 comprobaciones.

- Las tres suites de pruebas incluyen `animation_runtime`.
- Dos Release idénticos byte a byte, con SHA-256
  `6C6E3613194F95928A143F0C45FBA6837AF02F8E4827DC86FB4E811AB565974A`.
- Protecciones PE `0x4160`.
- Instalación y `.upak` sin cambios.

`prepare_animation_trial.ps1` empaquetó esa DLL. `manage_animation_trial.ps1
-Action Plan` pasó el catálogo contra el juego (miembro 610) y mostró el cambio
previsto: DLL 0.6.0 (`DCA4CD22…`) → 0.8.0, INI actual más `animations=observe`,
y la sonda nueva. No modificó la instalación.

**Pendiente:** sesión en el juego con `Observe`, `Report` y análisis de las
lecturas. Hasta entonces, nada acredita que el motor lea `.anm` por este hook.

## Instalación de observación (autorizada por el usuario el 24 de septiembre)

`manage_animation_trial.ps1 -Action Observe` con el juego cerrado. Estado
verificado:

- DLL `6C6E3613…974A`.
- INI `1DB1F6EA…021E`: configuración previa más `animations=observe`.
- Sonda `18669CCE…DCE6` en `mods/anim_probe`.

El respaldo está en `build/animation-trial/installation-backup`: DLL 0.6.0 de
875.520 bytes, INI de 62 bytes y `state.json`. Se restaura con
`-Action Restore`.
