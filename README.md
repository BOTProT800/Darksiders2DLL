# Darksiders2DLL 0.4.0

Cargador x64 de texturas DDS mediante un proxy de `dinput8.dll` para
**Darksiders II Deathinitive Edition**. Los mods se leen de archivos sueltos;
los paquetes del juego se abren únicamente para lectura.

La versión 0.4.0 elimina la dependencia de `first_test_mod`, del icono de prueba
y de sus hashes fijos. Debug y Release usan el catálogo general de DDS,
identidad de recursos y validación del contenido original. Cada candidato puede
pertenecer a cualquier mod; los conflictos se resuelven por orden léxico del
nombre normalizado de la carpeta.

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

## Historial

- [Resultados y huellas del Release 0.4.0](research/RELEASE_0_4_VALIDATION.md)
- [Estado y evidencias hasta Debug v6](research/HISTORIAL_HASTA_V6.md)
- [Investigación del resolver y pruebas dinámicas](research/RESOLVER_RESEARCH.md)
- [Plan de desarrollo](PLAN_MAESTRO.md)

Las restricciones de prototipos y los hashes de antiguos Release en esos
registros describen etapas anteriores. `asset_hook.cpp` se conserva como
referencia del experimento D3DX y ya no se compila en la DLL.
