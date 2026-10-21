# Darksiders2DLL 0.12.0 — guía técnica

Cargador de texturas y modelos para **Darksiders II Deathinitive Edition**
(Windows x64). La guía para jugadores es `Darksiders2DLL-LEEME.txt`, dentro del
ZIP: instalar es copiar archivos. Esta página detalla el paquete, la
instalación avanzada, la configuración y el diagnóstico.

El paquete no contiene archivos del juego ni texturas: utiliza únicamente mods
que tengas derecho a usar y compartir.

## Compatibilidad

Solo se admite el ejecutable de Steam cuyo SHA-256 es:

`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`

Con un ejecutable distinto el proxy de DirectInput sigue funcionando, pero los
mods se desactivan (`BUILD_UNSUPPORTED` en el registro). Las pruebas se hicieron
en Windows 11. MinHook y zlib están enlazados estáticamente: no hay DLL
adicionales que instalar.

La DLL ocupa `dinput8.dll`, igual que "DLL Loader for Darksiders 2" de LOKI
(Custom FOV, DS2LM): no pueden instalarse los dos a la vez.

## Contenido del ZIP

La raíz del ZIP reproduce la carpeta del juego:

```text
dinput8.dll                  el cargador
Darksiders2DLL.ini           opciones, todas comentadas salvo enabled y mode
Darksiders2DLL-LEEME.txt     guía para jugadores (español e inglés)
mods/LEEME.txt               dónde van los mods (la DLL lo ignora)
Darksiders2DLL-info/
  CREDITS.md, THIRD_PARTY_NOTICES.md, LICENSE
  VALIDACION.json            evidencia de verify_release.ps1
  manifest.json              versión y hashes que lee Install.ps1
  SHA256SUMS.txt             SHA-256 de cada archivo, rutas desde la raíz del ZIP
```

`SHA256SUMS.txt` y `manifest.json` detectan cambios, pero no prueban quién creó
el paquete: compara el SHA-256 del ZIP con el publicado por el autor por un
canal de confianza. La DLL no está firmada.

## Instalar copiando

Con el juego cerrado, copia todo el contenido del ZIP en la carpeta de
`Darksiders2.exe` (Steam: Biblioteca → clic derecho → Administrar → Ver
archivos locales). Windows pide permiso de administrador para escribir en
`Program Files (x86)`; el juego se abre después de forma normal, sin
privilegios de administrador. Si Windows avisa de que ya existe `dinput8.dll`,
hay otro proxy instalado: guárdalo fuera de la carpeta del juego antes de
reemplazarlo. No copies la DLL a System32 ni uses `regsvr32`.

Para actualizar, copia el ZIP nuevo encima; omite `Darksiders2DLL.ini` si lo
personalizaste. Para desinstalar, borra `dinput8.dll`; el resto de archivos del
paquete, los mods y los registros no afectan al juego sin la DLL.

## Instalación avanzada con `Install.ps1`

`distribution/Install.ps1` no va en el ZIP. Instala desde la carpeta del ZIP
extraído **fuera** del juego, comprueba el ejecutable y los hashes de
`Darksiders2DLL-info/manifest.json`, bloquea el ejecutable durante la operación,
crea el INI si no existe y deja un registro para desinstalar:

```powershell
.\distribution\Install.ps1 -PackageDirectory 'CARPETA_DEL_ZIP_EXTRAIDO' `
  -GameDirectory 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition'
```

Sin `-PackageDirectory` usa la carpeta del propio script, así que también puede
copiarse dentro del ZIP extraído. `-Action Verify` comprueba el ejecutable y la
DLL instalada; `-Action Uninstall` retira solo la DLL de esa versión y el
registro, y restaura el backup si lo había. El script no se eleva, no cambia
las políticas de PowerShell y no inicia el juego.

Si ya existe `dinput8.dll`, la instalación se detiene y muestra su hash. Tras
identificar esa DLL, `-ReplaceSha256 'HASH_DE_64_CARACTERES'` autoriza
exactamente esa copia: se guarda como `dinput8.dll.ds2-backup` y se restaura al
desinstalar. Un backup existente no se sobrescribe. Para actualizar una
instalación gestionada, desinstala con su paquete anterior y después instala el
nuevo; el INI y los mods se conservan. Una DLL copiada a mano cuenta como
proxy existente.

## Mods

Cada subcarpeta inmediata de `mods` es un mod; los archivos sueltos en `mods`
se ignoran. El nombre es libre y debe ser un nombre de carpeta normal, sin
puntos finales, nombres reservados o rutas especiales. Las rutas dentro de cada
mod reproducen la ruta virtual original, incluyendo `media/`:

```text
mods/
  010_mi_mod/
    media/items/armor/DS1A01_Shoulder_Diff.dds
  020_otro_mod/
    media/characters/death/reaper_scythe_diff.png
    media/characters/death/death_head.2
```

Gana el primer mod por orden léxico del nombre normalizado, sin distinguir
mayúsculas; los prefijos `010_`, `020_` ordenan la prioridad. Las rutas ambiguas
por alias o mayúsculas se rechazan. Para desactivar un mod, **muévelo fuera de
`mods`**: renombrarlo dentro solo cambia su prioridad.

- **Texturas DDS y PNG.** Con el valor por defecto (`textures=resize`) se
  entregan a su propio tamaño, formato y número de mips; si el motor no acepta
  el tamaño propio, se convierten al contrato del original. Detalles y formatos
  admitidos en [TEXTURAS_HD.md](TEXTURAS_HD.md).
- **Modelos `.2`.** Con el valor por defecto (`models=override_unchecked`) se
  carga cualquier malla, también más grande que la original, mientras conserve
  el número de mallas. Es experimental y no tiene respaldo: un `.2` roto puede
  romper el juego. [MODELOS_PRUEBA.md](MODELOS_PRUEBA.md).
- **Animaciones `.anm`.** No se sustituyen; `animations=observe` solo registra
  ([ANIMACIONES_PRUEBA.md](ANIMACIONES_PRUEBA.md)).
- Se ignoran los demás archivos.

Desde la 0.12.0 no hay presupuesto de memoria ni tope de archivos: si un archivo
no cabe en memoria, solo ese se descarta (`MOD_INDEX_ISSUE`)
([RESTRICCIONES.md](../RESTRICCIONES.md)). Las carpetas de mods admiten hasta 32
niveles y la extracción limita cada flujo comprimido/descomprimido a 1 GiB. Muchos archivos pueden
retrasar la preparación del catálogo: las lecturas que ocurran antes de activar
los hooks pasan intactas.

## Configurar

No hace falta un INI: sin él se usan los valores por defecto. La plantilla del
paquete los deja comentados, así que sobrescribirla al actualizar no cambia
nada. Edita `Darksiders2DLL.ini` con texto ASCII y reinicia el juego:

| Clave | Por defecto | Otros valores |
|---|---|---|
| `enabled` | `true` | `false`: solo el proxy de DirectInput |
| `mode` | `override` | `observe`: valida y registra sin modificar buffers |
| `textures` | `resize` | `native` (convertir al original), `exact` (solo DDS idénticos en contrato) |
| `models` | `override_unchecked` | `observe`, `off`; `override_shape` y `override_positions` son alias de `override_unchecked` |
| `animations` | `off` | `observe` |

`mode=override` sustituye únicamente lecturas cuya identidad y hash original
coinciden. Una clave desconocida o duplicada, un valor incorrecto, o un archivo
vacío, no legible, redirigido o mayor de 16 KiB desactivan el cargador
(`CONFIG_REJECTED`). No hay recarga en caliente ni opciones para omitir
validaciones.

## Comprobar y resolver problemas

Los registros se guardan en `%LOCALAPPDATA%\Darksiders2DLL\logs`, un archivo
por sesión de hasta 16 MiB; puedes borrar los antiguos con el juego cerrado.

- Arranque: `SESSION_START` (versión), `BUILD_SUPPORTED`, `LOADER_CONFIG`,
  `GENERAL_DDS_CATALOG_READY`, `MODEL_CATALOG_READY` e
  `INTERNAL_ASSET_OVERRIDE_ACTIVE`.
- Texturas: `GENERAL_DDS_OVERRIDE_HIT` con `replacement_verified=true`;
  `TEXTURE_RESIZE_METADATA_PATCHED` y `TEXTURE_RESIZE_COMPLETE` cuando se
  entregan a su tamaño; `GENERAL_DDS_CANDIDATE_REJECTED` y
  `GENERAL_DDS_CONTRACT_REJECTED` explican un rechazo.
- Modelos: `MODEL_OVERRIDE_HIT`, `MODEL_RESIZE_COMPLETE`; `MODEL_REJECTED`
  explica un rechazo.
- Desactivación: `CONFIG_REJECTED`, `LOADER_DISABLED`, `MOD_INDEX_FAILED`,
  `ASSET_FALLBACK` o `BUILD_UNSUPPORTED`. `MOD_INDEX_ISSUE` señala una ruta o un
  archivo de mod rechazado.

Un rechazo conserva el recurso original. Si aparece
`GENERAL_DDS_OVERRIDE_FAILED` o `MODEL_OVERRIDE_FAILED`, deja de usar ese mod y
cierra el juego normalmente: las siguientes escrituras quedan bloqueadas hasta
reiniciar, pero una escritura parcial ya realizada no puede considerarse
revertida. No cambies los `.upak`.

Consulta [SEGURIDAD.md](SEGURIDAD.md) para las protecciones y riesgos residuales
(revisión de la 0.4.0).
