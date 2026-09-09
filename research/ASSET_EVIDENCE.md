# Evidencia congelada de `first_test_mod`

Esta nota registra los datos comprobados para el primer override. No contiene
bytes comerciales del juego ni una copia de la textura.

## Identidad del juego

- Ejecutable: `Darksiders2.exe`
- Tamaño: `33,637,376` bytes
- SHA-256: `5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`
- `media/manifest.bin`: versión `13` (`0x0D`), `449,222` bytes
- SHA-256 de `manifest.bin`: `C30209E55D05EA7AF1D7E4901F0E998A50F00658C04884C0F37A7C1846F90824`

Toda dirección virtual documentada en la investigación solo corresponde a
esta huella. Una huella distinta debe desactivar los hooks y dejar activo
únicamente el proxy de DirectInput.

## Asset objetivo

- Mod: `first_test_mod`
- Ruta virtual canónica:
  `media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`
- Archivo de trabajo:
  `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds`
- SHA-256 editado:
  `2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719`
- SHA-256 original:
  `E2173F05C575677756071AD7DEC88E4CF49BBD0A734F902159E93C79F191B22D`
- Tamaño: `4,224` bytes (`0x1080`)
- DDS: `64 x 64`, `DXT5`/BC3, textura 2D, un nivel

El preflight estricto de DarksideModManager aceptó el DDS editado. La cabecera
editada expresa explícitamente profundidad y mip count iguales a uno; si el
juego la rechaza, se debe ensayar una copia de staging con la cabecera original
antes de cambiar el hook.

## Localización dentro del paquete

- Paquete: `media.upak`
- Segmento del manifiesto: `media/ui/ui_icons_small.`
- Offset del segmento: `0x33250800`
- Tamaño del segmento: `0x52800` (`337,920` bytes)
- Intervalo: `[0x33250800, 0x332A3000)`
- Layout OBPK: `stream`
- Inicio del zlib: `segmento + 0x3004 = 0x33253804`
- Tamaño comprimido: `325,628` (`0x4F7FC`)
- Tamaño descomprimido: `705,792` (`0xAC500`)
- Miembro: `#72`
- Offset en el stream descomprimido: `0x4F800`
- Tamaño del miembro: `0x1080`
- Tipo: `6` (DDS)
- Código de formato interno: `0x11`

El segmento comparte un stream zlib entre muchos miembros. Un hook de
`ReadFile` no puede sustituir este miembro por un DDS suelto sin romper el
contrato del contenedor. Para el primer POC gráfico se puede interceptar la
creación de textura y reconocer el contenido original; el loader general debe
interceptar después de la selección/descompresión del miembro.

## Ruta de despliegue esperada

`C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition\mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds`

Los `.upak` deben permanecer intactos durante la prueba de la DLL.
