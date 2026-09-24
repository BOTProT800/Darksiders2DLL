# Inventario: 0.7.0-inventory-trial

La DLL admite `mods/<mod>/media/scripts.obsp` cuando `[loader]` contiene
`scripts=inventory`. El mod solo puede cambiar los siete enteros `NumSlots`
del script interno `death/death`. Los demás bytes deben ser idénticos al original.
No es soporte general para scripts nuevos, scripts extraídos ni código fuente.

## Preparar y usar un mod

Desde el repositorio, genera una copia modificada fuera de la instalación:

```powershell
.\scripts\prepare_inventory_mod.ps1 `
  -OutputDirectory .\build\inventory-mods\010_more_inventory -Slots 42
```

Para cantidades diferentes por categoría, usa `-CategorySlots`, por ejemplo
`-CategorySlots @{PrimaryWeapon=42; BodyArmor=60}`. Sin `-Slots`, las categorías
no indicadas conservan sus valores originales. El generador rechaza un destino
existente y nunca sobrescribe el archivo instalado.

Comprueba el mod con el ejecutable de pruebas de esta versión:

```powershell
.\build\validation-inventory-trial\tests-release\out\offline_tests.exe `
  --inventory-check `
  'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition' `
  .\build\inventory-mods
```

Con el juego cerrado, instala la DLL de esta versión junto a `Darksiders2.exe`
conservando la copia anterior. Copia la carpeta `010_more_inventory` dentro de
`mods`. La estructura final es:

```text
Darksiders2.exe
dinput8.dll
Darksiders2DLL.ini
mods/
  010_more_inventory/
    media/
      scripts.obsp
```

Añade esta clave a la sección `[loader]` que ya existe en tu INI:

```ini
scripts=inventory
```

No dupliques la sección ni la clave. Conserva las opciones de texturas/modelos
que ya uses. `scripts=off` es el valor predeterminado; `scripts=observe` registra
aperturas sin sustituirlas. `mode=observe` también impide reemplazar scripts,
aunque `scripts=inventory` esté seleccionado. Reinicia el juego tras cada cambio.

## Verificación en el juego

Los logs están en `%LOCALAPPDATA%\Darksiders2DLL\logs`. Busca:

- `INVENTORY_SCRIPT_CANDIDATE`: mod seleccionado y cantidades aceptadas.
- `INVENTORY_SCRIPT_HOOK_ACTIVE`: apertura interceptada; todavía no demuestra que
  el juego haya solicitado el archivo después de activar el hook.
- `INVENTORY_SCRIPT_OPENS`: `redirected` mayor que cero confirma que el juego
  recibió un handle de la copia del mod. El resumen se actualiza cada cinco segundos.
- `INVENTORY_SCRIPT_REJECTED` / `INVENTORY_SCRIPT_HOOK_FAILED`: explica el rechazo.

La carga de scripts es temprana. Si solo aparece `HOOK_ACTIVE`, sin aperturas
redirigidas, puede haberse instalado el hook demasiado tarde o haberse usado
otra ruta de apertura. Esta versión no parchea scripts que ya estaban cargados.

Las pruebas offline verifican el contenido y la entrega de archivos, pero no
demuestran la capacidad efectiva del inventario, su interfaz ni la persistencia
en partidas. Para la primera prueba usa una copia de tu partida y comprueba
recoger, equipar, vender, guardar y volver a cargar objetos por encima del límite
anterior. No se ha confirmado cómo cuentan los objetos equipados.

## Contrato y límites

- Ejecutable compatible: SHA-256 `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`.
- Original `media/scripts.obsp`: 18 334 463 bytes; SHA-256
  `B46DD3DA7F17A0ED016E30AF523BFBA86D358C9920DFFA866C69D4A0F4F67C7C`.
- Valores originales: 21 para `PrimaryWeapon`, `SecondaryWeapon`, `Shoulder`,
  `Talisman`; 22 para `BodyArmor`, `Gauntlet`, `Boot`.
- Se permiten aumentos hasta 255 y conservar valores originales. 255 es una
  restricción de esta implementación, no una capacidad validada en el juego.
- Entre mods de inventario gana el primero por orden léxico, como en los demás
  recursos. No se combinan archivos; un ganador inválido no activa al siguiente.
- El original y el mod quedan protegidos contra escritura y eliminación durante
  el proceso. Se usa identidad de archivo, no una coincidencia de nombre.
- La DLL abre primero el original y solo redirige lecturas de `OPEN_EXISTING`.
  Conserva el original si falla abrir o identificar el reemplazo. No redirige
  aperturas de escritura, de backup, de reparse points ni con
  borrado al cerrar. Las lecturas normales, asíncronas y los mapeos usan handles
  reales del sistema operativo. Sin buffering se exige que ambos archivos estén
  en el mismo volumen. Los lectores deben compartir lectura (`FILE_SHARE_READ`),
  como las rutas de lectura examinadas del ejecutable compatible; los pins
  impiden aperturas exclusivas mientras el mod está activo.
- Desactivar el mod devuelve los valores originales en el siguiente inicio.
  Reduce primero los objetos guardados al límite original: el comportamiento de
  una partida que excede ese límite no se ha validado.

Para compartir el cambio es preferible distribuir el generador y las cantidades;
el archivo completo contiene scripts del juego y no forma parte del paquete de la DLL.
