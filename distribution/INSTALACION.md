# Darksiders2DLL 0.4.0 — Windows x64

Release preliminar del cargador de DDS para **Darksiders II Deathinitive Edition**.
La DLL se instala al lado de `Darksiders2.exe`. El paquete no contiene archivos del
juego ni texturas: utiliza únicamente DDS que tengas derecho a usar y compartir.

## Compatibilidad

Solo se admite el ejecutable cuyo SHA-256 es:

`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`

La edición original de 2012 y otras compilaciones no están verificadas. Un
ejecutable distinto conserva el funcionamiento del proxy, pero desactiva los mods.
Se necesita Windows x64; las pruebas de este paquete se realizaron en Windows 11.
MinHook y zlib están enlazados estáticamente: no hay DLL adicionales que instalar.

El motor general anterior se probó visualmente en el juego con dos DDS. Este
Release añade configuración y protecciones y cuenta con pruebas automatizadas;
todavía necesita su propia prueba visual antes de considerarse una versión estable.

## Instalar

1. Cierra el juego normalmente y extrae el ZIP en una carpeta local **fuera** del
   directorio del juego. Conserva el paquete para poder desinstalarlo.
2. Comprueba el SHA-256 del ZIP contra el publicado por el autor por un canal de
   confianza. Los archivos `SHA256SUMS.txt` y `manifest.json` detectan cambios,
   pero no prueban quién creó el paquete. La DLL y los scripts no están firmados.
3. En PowerShell, desde la carpeta extraída, ejecuta (ajusta la ruta):

   ```powershell
   .\Install.ps1 -GameDirectory 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition'
   ```

   El script comprueba el juego y el paquete, bloquea el ejecutable durante la
   operación y crea el INI si no existe. Si Windows pide permisos para escribir
   en esa carpeta, usa una consola con los permisos necesarios **solo para instalar**.
   El script no se eleva, no cambia las políticas de PowerShell y no inicia el juego.
   Si una política bloquea el script, revísalo y sigue la política de tu equipo;
   también puedes seguir la instalación manual de abajo.
4. Crea las carpetas de tus mods según el ejemplo y abre el juego normalmente,
   sin ejecutarlo como administrador. Los cambios se leen al iniciar cada proceso.

Si ya existe `dinput8.dll`, la instalación se detiene y muestra su hash. Tras
identificar esa DLL, puedes autorizar exactamente esa copia con
`-ReplaceSha256 'HASH_DE_64_CARACTERES'`. Se guarda como
`dinput8.dll.ds2-backup` y se restaura al desinstalar. No se encadenan varios
proxies; sustituir el de otro mod desactiva sus funciones. Un backup existente
no se sobrescribe. Para actualizar una instalación gestionada, desinstala con
su paquete anterior y después instala el nuevo; el INI y los mods se conservan.

**Instalación manual:** guarda fuera de la carpeta del juego cualquier
`dinput8.dll` anterior; copia únicamente `dinput8.dll` y, si no existe,
`Darksiders2DLL.ini` junto al ejecutable. No copies la DLL a System32 y no uses
`regsvr32`. En esta modalidad debes retirar/restaurar el proxy manualmente;
el desinstalador requiere el registro creado por el instalador.

## Añadir mods y resolver conflictos

Cada subcarpeta inmediata de `mods` es un mod. El nombre es libre y debe ser un
nombre de carpeta normal, sin puntos finales, nombres reservados o rutas especiales.
Por ejemplo:

```text
Darksiders2.exe
dinput8.dll
Darksiders2DLL.ini
mods/
  010_mi_mod/
    media/ui/ui_core/ui_icon_highlight.dds
  020_otro_mod/
    media/ui/otra_carpeta/otro_recurso.dds
```

Las rutas dentro de cada mod reproducen la ruta virtual original, incluyendo
`media/`. No existe ningún requisito de tener `first_test_mod` ni un icono
concreto. Se ignoran los archivos que no terminan en `.dds`.

Gana el primer mod por orden léxico del nombre normalizado, sin distinguir
mayúsculas. Los prefijos `010_`, `020_` facilitan ordenar la prioridad. Las rutas
ambiguas por alias o diferencias de mayúsculas se rechazan. Los logs identifican
el mod y la ruta de cada candidato aceptado. Para desactivar un mod, **muévelo
fuera de `mods`**; renombrarlo dentro de esa carpeta solo cambia su prioridad.

El DDS debe conservar formato, dimensiones, mipmaps, tipo de superficie y tamaño
del recurso original. Se aceptan únicamente recursos identificables en segmentos
OBPK con nombres y flujo comprimido único de `media.upak`. Por ahora se rechazan
DDS DX10, layouts por bloques y lecturas parciales; tampoco se sustituyen modelos,
sonido ni scripts. Un formato aceptado por el analizador no demuestra compatibilidad
visual con todos los recursos: prueba los mods de forma gradual.

Límites del cargador: 64 MiB por DDS, 256 MiB para el conjunto de reemplazos y otros
256 MiB para recuperar originales; 16 384 archivos, 100 000 entradas y profundidad
32. La extracción limita cada flujo comprimido/descomprimido a 1 GiB. Estos límites
no representan un máximo total de memoria del proceso.

## Configurar

Edita `Darksiders2DLL.ini` con texto ASCII y reinicia el juego:

```ini
[loader]
enabled=true
mode=override
```

- `enabled=false`: desactiva el cargador; mantiene el proxy de DirectInput.
- `mode=observe`: valida y registra candidatos sin modificar buffers del juego.
- `mode=override`: sustituye únicamente lecturas cuya identidad, contrato y hash
  original coinciden, y verifica el hash después de escribir.

Si falta el INI se usan `enabled=true` y `mode=override`. Una clave desconocida,
duplicada, valor incorrecto, archivo vacío, no legible, redirigido o mayor de 16 KiB
desactiva el cargador. No hay recarga en caliente ni opciones para omitir validaciones.

## Comprobar y resolver problemas

```powershell
.\Install.ps1 -GameDirectory 'RUTA_DEL_JUEGO' -Action Verify
```

La comprobación valida el ejecutable y la DLL, sin exigir que tu INI conserve
sus valores originales. Los registros se guardan en
`%LOCALAPPDATA%\Darksiders2DLL\logs`. Cada archivo tiene un límite de 16 MiB;
las sesiones anteriores se conservan y puedes borrarlas manualmente con el juego cerrado.

Busca `BUILD_SUPPORTED`, `LOADER_CONFIG`, `GENERAL_DDS_CATALOG_READY` e
`INTERNAL_ASSET_OVERRIDE_ACTIVE`. Cada sustitución correcta genera
`GENERAL_DDS_OVERRIDE_HIT` con `replacement_written=true` y
`replacement_verified=true`. En observación verás
`GENERAL_DDS_VERIFIED_WOULD_OVERRIDE`.

`CONFIG_REJECTED`, `MOD_INDEX_FAILED`, `ASSET_FALLBACK` o `BUILD_UNSUPPORTED`
explican una desactivación. Un rechazo de contrato conserva el recurso original.
Si aparece `GENERAL_DDS_OVERRIDE_FAILED`, deja de usar ese mod y cierra el juego
normalmente: las siguientes escrituras quedan bloqueadas hasta reiniciar, pero
una escritura parcial ya realizada no puede considerarse revertida.

No cambies los `.upak`. Si un mod no aparece, comprueba su ruta, prioridad,
compatibilidad y eventos del registro. Muchos DDS pueden retrasar la preparación
del catálogo: las lecturas que ocurran antes de activar los hooks pasan intactas.

## Desinstalar

Con el juego cerrado, desde el mismo paquete:

```powershell
.\Install.ps1 -GameDirectory 'RUTA_DEL_JUEGO' -Action Uninstall
```

Retira exclusivamente la DLL de esta versión y el registro de instalación; restaura
el backup si lo había. Conserva mods, INI, logs y archivos del juego. Si los hashes
no coinciden, se detiene para revisión manual. Nunca borres un backup sin identificarlo.

Consulta `SEGURIDAD.md` para las protecciones, riesgos residuales y alcance de la revisión.
