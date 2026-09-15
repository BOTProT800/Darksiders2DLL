# Release 0.4.0 — resultado local del 14 de septiembre de 2026

Paquete: `build/dist/Darksiders2DLL-0.4.0-win64.zip`.

- SHA-256 del ZIP: `00B2E5652E08D05B81697E3DBF3B369520732215D82BBAB985AFCAA4F8DCE1D5`.
- SHA-256 de `dinput8.dll`: `172089776EA523EF4B7C2786A4BF6F4EFE1E8007ED1C2273F6BD0B091C2466D5`.
- DLL x64 de 801 280 bytes; dos builds Release independientes idénticos.
- `DllCharacteristics=0x4160`: high entropy, ASLR, DEP y CFG. Sin firma Authenticode.

## Comprobaciones aprobadas

`scripts/verify_release.ps1` completó las pruebas Debug y Release del escritor
general, observación con cero escrituras, tres smoke tests de los seis exports
(Debug y ambos Release), protecciones PE y ausencia del antiguo caso especial
en el binario. Evidencia: `build/validation-release-0.4/VALIDACION.json`, logs de
build y ejecución en ese mismo directorio.

El escritor se ensayó con DDS completo y payload, ocho rechazos por identidad,
destino, tamaño o contenido, cuatro fallos de escritura/verificación y los
permisos de activación, fallo permanente y concurrencia. Se comprobó la
preservación del retorno y `GetLastError`. También pasaron configuración,
rutas, límites, contratos, candidatos arbitrarios y límite del log.

Los tres antiguos macros de prototipo produjeron error de compilación al
inyectarlos con `NDEBUG`; la propiedad MSBuild del write prototype también fue
rechazada antes de compilar. Logs `guard-*.log` y `obsolete-switch.log`.

El instalador pasó 16 casos con el Release final. Se repitieron después de
extraer el ZIP final y comprobar todos los hashes de `SHA256SUMS.txt`.
Evidencia de esa última ejecución:
`build/installer-tests-837117c89b4d4bcb92090a9ac63a4a30/installer-validation.json`.
Incluye instalación nueva, verificación, backup/restauración, rechazos de
modificaciones, ejecutable ocupado, junction y ejecutable incompatible;
también ejecución repetida en una misma sesión y creación/preservación del INI.

El ZIP contiene exactamente diez archivos: DLL, INI, instalador, dos guías,
créditos, avisos de terceros, manifiesto, sumas y validación. No contiene DDS,
ejecutables, archivos del juego ni símbolos de depuración. El registro incluido
enlaza los hashes de las fuentes y del instalador con la DLL ensayada.

`git diff --check` terminó sin errores sobre los cambios de trabajo. No se
modificó el índice de Git ni se descartaron cambios previos.

## Instalación real y límite de la validación

El proxy real conserva SHA-256
`2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`.
`anim_streams.upak`, `maps.upak` y `media.upak` conservaron tamaño y fecha de
última escritura entre las comprobaciones. Esta fase no ejecutó una nueva
lectura/hash integral de los 7 GiB de `media.upak` ni desplegó el Release.

**Pendiente antes de declararlo estable:** una sesión visual de este binario en
el juego, con autorización para el despliegue temporal y el lanzamiento. La
prueba visual anterior de Debug v6 constituye evidencia del motor de partida,
no de este artefacto nuevo. Mientras tanto se distribuye como Release preliminar.

## Confirmación posterior del usuario

El usuario confirmó: «Se veia bien, funcionaba bien».
El registro más reciente disponible corresponde a la sesión
`Darksiders2DLL-20260915-003127-19352-000.log`, entre 00:31:27Z y 00:41:19Z:
declara `version=0.3.0 mode=first_internal_asset_override`, una sustitución
aplicada y cero fallos de escritura. La DLL instalada sigue teniendo la huella
estable `2E29A827180A6EE394DE667D47AF41A838B32A68C3EE59FB030216BFF3CAB629`.
Los tamaños y fechas de los tres `.upak` siguen coincidiendo con la comprobación
anterior; el juego estaba cerrado al consultar el estado.

La confirmación visual se conserva, pero esta sesión no acredita el nuevo
Release 0.4.0. Su validación visual permanece pendiente y el ZIP no se modifica.
