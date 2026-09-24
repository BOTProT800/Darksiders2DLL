# Darksiders2DLL

**Código actual: 0.7.0-inventory-trial.** Admite mods de `media/scripts.obsp`
limitados a los siete valores `NumSlots`, con validación completa del original,
identidad de archivo y apertura de una copia del mod. Se activa con
`scripts=inventory`; por defecto está apagado. Incluye generador y comprobador
offline. [Uso y límites](distribution/INVENTARIO.md).
La capacidad y las partidas aún requieren validación en el juego.

**Texturas: 0.6.0-texture-trial.** Añade reemplazos DDS de resolución
variable y PNG (incluidos rectangulares) mediante la creación nativa de texturas
2D. Los reemplazos grandes nunca se copian al buffer del original. Conserva el
cargador de modelos 0.5.1. Esta ruta nueva necesita validación visual en el juego;
las pruebas offline no equivalen a esa validación. [Uso y límites](distribution/TEXTURAS_HD.md)
y [contrato del hook](research/NATIVE_TEXTURE_UPLOAD.md).

**Versión anterior: 0.5.1-shape-trial.** Preparación experimental de cambios de
posición, normales y tangentes en modelos nativos `.2`, con límites, UV y pesos
inmutables, validador previo y modos de observación y
reemplazo. La edición de `death_head.2` del usuario ya se sustituyó y verificó
en el juego, con confirmación visual (16 de septiembre de 2026). Los modelos
siguen desactivados por defecto en el paquete. [Guía de la prueba](distribution/MODELOS_PRUEBA.md) y
[contrato técnico](research/MODEL_TRIAL.md).
La instalación de prueba usa `models=override_shape`; conserva el respaldo de
**0.4.1-diagnostic** y también verificó el reemplazo de la máscara DDS.
Las instrucciones del ZIP 0.4.0 que siguen describen el paquete anterior.

**Fallo confirmado en 0.4.0:** algunas texturas válidas, incluida la máscara de
Death, no se sustituyen porque el contador de lecturas se usaba como índice de
archivo. La corrección `0.4.1-diagnostic` usa el índice real del juego: la máscara
ya se sustituyó y verificó en una sesión con confirmación visual del usuario.
[Diagnóstico y estado](research/MASK_MEMBER_IDENTITY_FIX.md).
El ZIP 0.4.0 conserva esa limitación.

Cargador x64 de texturas DDS mediante un proxy de `dinput8.dll` para
**Darksiders II Deathinitive Edition**. Los mods se leen de archivos sueltos;
los paquetes del juego se abren únicamente para lectura.

La versión 0.4.0 elimina la dependencia de `first_test_mod`, del icono de prueba
y de sus hashes fijos. Debug y Release usan el catálogo general de DDS,
identidad de recursos y validación del contenido original. Cada candidato puede
pertenecer a cualquier mod; los conflictos se resuelven por orden léxico del
nombre normalizado de la carpeta.

## Compatibilidad

**Importante:** este cargador usa un proxy de `dinput8.dll` para cargarse junto a
`Darksiders2.exe`, igual que "DLL Loader for Darksiders 2" (LOKI). Ambos compiten
por el mismo archivo `dinput8.dll`: **no se pueden instalar los dos a la vez**.
Si ya tienes el DLL Loader de LOKI (o mods que dependen de él, como Custom FOV o
DS2LM), quítalo antes de instalar este.

Este proyecto es una implementación independiente del mismo patrón de proxy
DirectInput, con foco en la carga de texturas DDS. No es un fork ni contiene
código del DLL Loader de LOKI. El detalle completo de referencias e influencias
está en [CREDITS.md](CREDITS.md).

## Instalar y configurar

El paquete se genera en `build/dist/Darksiders2DLL-0.4.0-win64.zip` e incluye la
DLL, un INI, instalador/desinstalador, huellas, evidencia y avisos de terceros.
No incluye DDS, archivos del juego ni dependencias dinámicas adicionales.

- [Guía de instalación, mods, configuración y desinstalación](distribution/INSTALACION.md)
- [Revisión de seguridad y riesgos residuales](distribution/SEGURIDAD.md)
- [Créditos](CREDITS.md) y [licencias de terceros](THIRD_PARTY_NOTICES.md)

El ejecutable compatible tiene SHA-256
`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`.
Otros ejecutables desactivan los mods. En el INI, `enabled=false` desactiva el
cargador y `mode=observe` permite diagnosticar sin escribir buffers.

## Alcance del Release

Este es un **Release preliminar**. El motor general v6 anterior se comprobó en
el juego con dos DDS y confirmación visual. Las nuevas protecciones y el INI
se verifican mediante pruebas automatizadas; este binario todavía necesita su
propia sesión visual antes de sustituir definitivamente la copia estable.
Preparar el paquete no lo despliega ni inicia el juego.

Se admiten DDS con el mismo contrato del original en segmentos OBPK con nombres
y flujo único de `media.upak`. Se rechazan layouts por bloques, DDS DX10,
lecturas parciales, rutas inseguras y diferencias de contrato/hash. No se
cubren modelos, audio, materiales ni scripts. La guía explica los límites de
memoria y el posible retraso con catálogos grandes.

La escritura solo se habilita cuando todos los hooks están activos. Una
escritura fallida, parcial o no verificada bloquea nuevos intentos hasta
reiniciar. No hay restauración garantizada del buffer tras una escritura parcial.
La DLL tiene ASLR, DEP y CFG; sigue siendo código nativo con los permisos del juego.

## Construcción y comprobaciones

Visual Studio 2026, herramienta MSVC v145, Windows SDK y C++20; arquitectura x64,
CRT estático `/MT`, advertencias `/W4 /WX`. MinHook 1.3.4 y zlib 1.3.2#2 se
resuelven con el baseline fijado en `vcpkg-configuration.json`. Configura la
integración de vcpkg para MSBuild en tu equipo y restaura el manifiesto.

Desde PowerShell con ejecución permitida para estos scripts revisados:

```powershell
.\scripts\verify_release.ps1
.\tests\install_tests.ps1 `
  -GameExecutable 'RUTA_DEL_JUEGO\Darksiders2.exe' `
  -DllPath .\build\validation-release-0.4\release-a\out\dinput8.dll
.\scripts\package_release.ps1 `
  -DllPath .\build\validation-release-0.4\release-a\out\dinput8.dll `
  -ValidationPath .\build\validation-release-0.4\VALIDACION.json `
  -InstallerValidationPath 'CARPETA_DE_ENSAYO\installer-validation.json'
```

`verify_release.ps1` acepta `-MSBuild` para otra ubicación de Visual Studio y,
opcionalmente, `-GameDirectory` y `-AssetSource` para comprobaciones adicionales
de lectura sobre la instalación y el DDS de regresión. No despliega ni lanza el
juego. Produce builds independientes y evidencia en `build/validation-release-0.4`.

La matriz ejecuta pruebas Debug y Release del escritor real dentro del proceso
de pruebas, pruebas de observación sin escrituras, dos builds Release que deben
coincidir byte a byte, un build Debug y smoke tests de los seis exports de cada
DLL. Comprueba protecciones PE y ausencia del antiguo caso especial en Release.
Los switches `EnableResourceIdentityProbe`, `EnableGeneralResolverPrototype` y
`EnableGeneralResolverWritePrototype` fueron retirados: activarlos genera error.
La configuración de observación/escritura ahora es el INI de ambas compilaciones.

El instalador se prueba sobre una copia local del ejecutable, sin tocar el juego:

```powershell
.\tests\install_tests.ps1 `
  -GameExecutable 'RUTA_DEL_JUEGO\Darksiders2.exe' `
  -DllPath .\build\validation-release-0.4\release-a\out\dinput8.dll
```

La salida indica la carpeta de ensayo que contiene `installer-validation.json`;
úsala al empaquetar. El empaquetador también exige una prueba del instalador
con esa DLL y comprueba que el script no haya cambiado.

El empaquetador verifica la huella de la DLL y las fuentes contra la evidencia.
El ZIP tiene una lista cerrada de archivos; PDB, ejecutables, DDS y fixtures no
se distribuyen. El ZIP y sus hashes no llevan firma digital y no prueban por sí
solos la identidad de quien los publica.

## Créditos

La DLL incorpora **MinHook**, de Tsuda Kageyu y colaboradores (incluido HDE,
de Vyacheslav Patkov), y **zlib**, de Jean-loup Gailly y Mark Adler, mediante
enlace estático. Los lectores de formatos se apoyan en las referencias de
**Darkstractor, Darkside Mod Manager y Anansi**, de BOTProT800, y en la
investigación previa de la comunidad que esos proyectos reconocen.

Las aportaciones y fuentes están detalladas en [CREDITS.md](CREDITS.md), y los
avisos de licencia en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
El empaquetador incluye ambos archivos en el ZIP.

## Historial

- [Resultados y huellas del Release 0.4.0](research/RELEASE_0_4_VALIDATION.md)
- [Estado y evidencias hasta Debug v6](research/HISTORIAL_HASTA_V6.md)
- [Investigación del resolver y pruebas dinámicas](research/RESOLVER_RESEARCH.md)
- [Plan de desarrollo](PLAN_MAESTRO.md)

Las restricciones de prototipos y los hashes de antiguos Release en esos
registros describen etapas anteriores. `asset_hook.cpp` se conserva como
referencia del experimento D3DX y ya no se compila en la DLL.
