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
contrato del contenedor. El primer override demostrado intercepta después de
la selección/descompresión, en la lectura interna RVA `0x0ABDC0`, y sustituye
únicamente los `4,096` bytes BC3 cuyo SHA-256 original es
`9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51` por el
payload editado con SHA-256
`DA59A81F1F96F18C09487458CAD089B5E8B9AD43541A77BE6FCFBE926CCD077F`.
El POC D3DX9/D3DX11 quedó desactivado después de cinco minutos sin entradas y
no participa en el override actual. El Release desplegado sigue limitado a
este objetivo exacto y tiene SHA-256
`2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`.

Un sondeo pasivo exclusivamente Debug, excluido del DLL Release, añadió una
ancla de identidad anterior a la lectura en RVA `0x9FEE5C`. En tres procesos
independientes, el SHA-256 original apareció con
`package_base=0x33250800`, `member_table_offset=0x3000`, ordinal de lectura
uno-basado `73`, caller exterior `0x9FA980` y retorno de lectura `0x9FF0D2`.
La base coincide con este segmento y el ordinal con el miembro `#72`. Esta
correlación se contrastó después con un segundo DDS; todavía no constituye una
decisión general de reemplazo.

## Segunda ancla de investigación

La segunda ancla no está instalada bajo `first_test_mod` y nunca se usó como
reemplazo editado. Se colocó temporalmente, sin cambios y en un mod diagnóstico
aislado, para comprobar de forma pasiva la identidad en otro segmento:

- Ruta virtual: `media/ui/ui_core/ui_icon_highlight.dds`
- Archivo extraído:
  `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\ui_icon_highlight.dds`
- Tamaño/formato: `4,224` bytes (`0x1080`), `64 x 64`, `DXT5`/BC3, campo
  `mipMapCount=0`
- SHA-256 del DDS:
  `7D7CA1D2B1A411DA28BA4071BB6D0A9AC54FFB5F2C0E608B28333A2D34EA3254`
- SHA-256 del payload BC3 de `4,096` bytes:
  `9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA`
- Paquete/segmento: `media.upak`, `media/ui/ui_core.`, offset `0x77800`,
  tamaño `0x811000`
- Layout OBPK: `stream`; tabla/payload `0x3800`; zlib absoluto `0x7B004`
- Tamaños del stream: comprimido `0x80D7FC`, descomprimido `0x1B498C1`
- Miembro `#45`, ordinal runtime `46`, offset descomprimido `0x267E78`,
  tamaño `0x1080`, tipo `6` (DDS)

El miembro descomprimido coincide byte a byte con el archivo extraído. Los
PIDs `8692`, `20816` y `7064` reprodujeron `3/3`
`package_base=0x77800`, `member_table_offset=0x3800`, `read_ordinal=46`,
caller exterior `0x9FA980`, retorno `0x9FF0D2`, petición/retorno de `4096`
bytes y el SHA-256 de payload indicado arriba.

## Tercera ancla offline de tamaño distinto

Esta ancla no está instalada como mod y aún no tiene evidencia runtime. Se
añadió para probar que el catálogo no depende del único contrato `4224/4096`:

- Ruta virtual: `media/ui/ui_core/icon_artifact.dds`
- Archivo extraído:
  `C:\Users\vicen\Documents\Extractions\Darksiders\media\ui\ui_core\icon_artifact.dds`
- Tamaño/formato: `1,152` bytes (`0x480`), `32 x 32`, `DXT5`/BC3, DDS
  tradicional, `mipMapCount=0` normalizado a un nivel
- SHA-256 del DDS:
  `7BCD13C620C6249B7FB80E60B5237F42548C7D5F4EE10B40A04CABBC94E3CE10`
- SHA-256 del payload BC3 de `1,024` bytes:
  `A1E8C36BBDDB387EAEF3F57BF8BEFA24FFDDF09C7C644FEEC3B42E4D1A13D2F7`
- Segmento `media/ui/ui_core.`: base `0x77800`, tabla/payload `0x3800`
- Miembro `#142`, ordinal uno-basado `143`, offset descomprimido `0xB727B8`,
  tamaño `0x480`, tipo `6` (DDS)

La descompresión directa del stream instalado produjo el total declarado
`0x1B498C1`; el miembro coincide byte a byte con la extracción. La prueba real
del catálogo C++ confirma `package_base=0x77800`,
`member_table_offset=0x3800`, `member_ordinal=143`. Esto valida el parser y el
contrato de tamaños offline; no demuestra todavía que esa lectura concreta
aparezca en el hook ni habilita un override.

## Traducción diagnóstica a ruta

Un catálogo inmutable exclusivo de Debug se construye offline desde
`manifest.bin` y META para las rutas presentes en el índice de mods. Sus
pruebas sintéticas y reales cubren `stream`, rechazo seguro de `blocks`,
solicitud vacía, las dos anclas dinámicas y una tercera ancla offline de tamaño
diferente. En el PID `14792` resolvió la primera
tupla a
`media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds`.
La segunda quedó `unmapped` porque `ui_icon_highlight.dds` no está en `mods`.
Este catálogo solo añade evidencia y logs: no decide reemplazos. El resolver
activo general sigue pendiente. El código Debug opt-in integra después un
dry-run que une la identidad con el DDS ganador del `ModIndex` y clasifica el
contrato como `GENERAL_DDS_WOULD_OVERRIDE` o
`GENERAL_DDS_CONTRACT_REJECTED`. Solo recibe lecturas con stream de alcance
de paquete válido, wrapper interior no nulo y el par estático de callers
`0x9FA980/0x9FF0D2`; ambos punteros de stream son objetos distintos por contrato
del callsite. Exige también destino escribible, retorno completo y tamaño
exacto del DDS o del payload. Los eventos declaran
`buffer_writes=false`: no se leen los bytes originales para autorizar un
reemplazo general ni se copian bytes del mod. La lógica tiene cobertura offline
y dinámica: el PID `18644` observó el primer candidato y el PID `10616` observó
las dos anclas con `general_candidate_hits=2`, `general_would_override=2`,
`general_contract_mismatches=0` durante más de nueve minutos. Ambos eventos
declararon `buffer_writes=false`; el único write siguió siendo el override
exacto ya conocido. La fase siguiente debe extraer y validar el DDS original
desde el paquete y conservar sus hashes de DDS/payload antes de considerar una
copia general.

## Ruta de despliegue esperada

`C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition\mods\first_test_mod\media\ui\ui_icons_small\ui_hudicon_passiveability_improved_agility.dds`

El override no escribe en los `.upak`. Durante las pruebas Steam reemplazó
`media.upak` como archivo completo, por lo que cambió su metadata histórica;
su contenido actual sí está verificado. Su SHA-1
`D267F6DBE96E90A58AE8E4981CAD47150D6AEA52` coincide con el manifiesto oficial
`388411_5667116516349422569.manifest` y su SHA-256 local es
`C262236AB15E563F5539C33F11851537A4F021A5A3402B8C61FDD8975F3AE993`.
