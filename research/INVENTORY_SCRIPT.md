# Evidencia del parche de inventario

> **Retirado el 4 de octubre de 2026.** El mod de inventario se abandonó y su
> código (`scripts=`, hook de `CreateFile`, generador y pruebas) se eliminó de la
> DLL; está en el commit `cde33df`. Este documento se conserva solo como
> investigación del formato OBSP, que cita D2ScriptViewer.

Investigación local del 19 de septiembre de 2026. No se incorporaron scripts del
juego ni datos de partidas al repositorio.

En la instalación compatible, `media/scripts.obsp` empieza por `OBSP`, mide
18 334 463 bytes y tiene SHA-256
`B46DD3DA7F17A0ED016E30AF523BFBA86D358C9920DFFA866C69D4A0F4F67C7C`.
El registro `death/death` aparece en el offset de archivo `0x808769`. Sus
instrucciones contienen llamadas a `getInventory`, `getContainer` con los nombres
de categoría y `NumSlots`, seguidas por el marcador `0x23`, el operando int32 LE y
los bytes `0x29 0x32`. La lectura estática identifica estos operandos:

- `PrimaryWeapon`: 21, offset `0x80B242`.
- `SecondaryWeapon`: 21, offset `0x80B2C2`.
- `Shoulder`: 21, offset `0x80B33B`.
- `BodyArmor`: 22, offset `0x80B3B5`.
- `Gauntlet`: 22, offset `0x80B42E`.
- `Boot`: 22, offset `0x80B4A3`.
- `Talisman`: 21, offset `0x80B51C`.

Son offsets de esta copia completa del OBSP, no RVA del ejecutable. La validación
autentica todo el original, comprueba las instrucciones y solo permite cambiar
esos 28 bytes, sin modificar tamaño, tablas ni otros scripts.

El ejecutable compatible importa `CreateFileW` (IAT RVA `0x12B91E8`), `CreateFileA`
(`0x12B9380`), `ReadFile` y `ReadFileEx`. Estos datos se obtuvieron leyendo la
tabla PE local; no son firmas usadas por el hook. Se interceptan ambos exports
de apertura, se ejecuta primero la apertura original y se compara su `FILE_ID_INFO`
con el original autenticado. Un segundo handle, abierto con los argumentos de
lectura del juego, entrega el archivo del mod. Se verifica su identidad antes
de cerrar el handle original. Esto deja las operaciones de lectura, seek, EOF,
overlapped y mapping a Windows y evita parchear buffers que podrían seguir
pendientes de I/O. El último error de la apertura original se conserva.

Los pins usan `GENERIC_READ` y `FILE_SHARE_READ`, negando escritura y borrado.
Una prueba demostró que un pin de atributos no bloquea escritores; por ello se
descartó esa variante. Las llamadas inspeccionadas del ejecutable en RVA
`0xC48C1`, `0x78A2EF` (rama de lectura) y `0xD3A04E` usan acceso `GENERIC_READ`
y `FILE_SHARE_READ`. La última abre con flags `0x60000000`, es decir,
`OVERLAPPED | NO_BUFFERING`. Se admite esa combinación si ambos archivos están
en el mismo volumen, conservando su contrato de alineación. Se vuelve a calcular
el hash mediante el propio pin, cerrando cambios entre indexado y activación.
Los pins y trampolines
permanecen vivos mientras puede ejecutarse un hook. La activación solo se publica
tras instalar los dos hooks; los fallos dejan los detours sin permiso de redirigir.

La inicialización ocurre después de autenticar el ejecutable y leer el INI,
antes de construir los catálogos DDS/modelos. Sigue siendo posible que la carga
de scripts preceda a la activación; se exige evidencia `INVENTORY_SCRIPT_OPENS`
con `redirected>0` antes de atribuir resultados a esta función.

Las pruebas sintéticas del hook no ejecutan scripts ni sustituyen archivos del
juego. La compatibilidad de inventarios ampliados y partidas sigue pendiente de
prueba en el juego. No se afirma un intérprete OBSP completo ni soporte para
ediciones generales de scripts.
