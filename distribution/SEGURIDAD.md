# Revisión de seguridad — Darksiders2DLL 0.4.0

Revisión de código y pruebas locales del 14 de septiembre de 2026. Se revisaron
el proxy, inicio, catálogos, lectura de mods, escritor y scripts de instalación.
No equivale a una auditoría independiente, fuzzing exhaustivo ni prueba de
ausencia de vulnerabilidades. El ejecutable, la DLL y los scripts de instalación
son código de confianza; los archivos de mods y configuración se consideran entradas no confiables.

## Hallazgos corregidos

**Activación incompleta del escritor (impacto alto sobre el proceso).** El
prototipo publicaba los candidatos antes de activar todos los hooks y no tenía
un permiso separado de escritura. Si la activación era parcial, un detour podía
encontrar candidatos. Ahora la propiedad del catálogo se publica primero, pero
el permiso solo se habilita al completar toda la instalación. La ruta de
recuperación lo deshabilita. Las pruebas cubren el permiso cerrado con candidatos
disponibles; la simulación no fuerza errores internos de MinHook en un juego real.

**Repetición de escrituras tras un fallo (impacto alto sobre estabilidad).** El
prototipo contaba errores pero podía volver a escribir. Ahora cualquier escritura
fallida, parcial o con hash posterior incorrecto bloquea nuevos intentos hasta
reiniciar. Un permiso atómico sin espera serializa los intentos: una lectura
concurrente pasa intacta. El hash original se comprueba de nuevo tras adquirir
el permiso. Las pruebas inyectan rechazo, escritura parcial, corrupción y fallo
de lectura de verificación, y comprueban que no se reanude el escritor.

**Superficie y consumo innecesarios.** El escritor exacto del antiguo icono se
retiró del proceso de arranque y del detour; el experimento D3DX ya no se compila
en la DLL. El cargador indexa solo DDS. Se limitan reemplazos y originales a
64 MiB por archivo y 256 MiB por conjunto, y cada log a 16 MiB. Se valida el INI
con un analizador estricto, sin rutas externas ni opciones para eludir hashes.

## Riesgos que siguen existiendo

- **Una DLL maliciosa controla el proceso y los permisos de su usuario.** Este
  mecanismo de carga también puede usarse para suplantar DLL. La validación de
  DDS no protege contra reemplazar el propio `dinput8.dll` o un script de
  instalación. Usa paquetes de procedencia comprobable y ejecuta el juego sin
  privilegios de administrador. El paquete no contiene firma Authenticode.
  Un SHA-256 sirve para contrastar bytes; un atacante que cambia el ZIP y sus
  hashes también puede falsificar esa comprobación. Publicar la huella por un
  canal independiente y firmar futuras versiones mejora la autenticidad.
  [Guía de Microsoft sobre seguridad de DLL](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security).
- **Los hooks alteran código y datos del juego.** ASLR, DEP, CFG y las comprobaciones
  de buffer reducen riesgos, pero no convierten C++/MinHook ni al juego en una
  zona aislada. MinHook usa memoria ejecutable para trampolines. Una carrera con
  el propio juego, otro inyector o un driver puede producir corrupción o un
  cierre inesperado. La DLL se fija en memoria durante la sesión; no se descarga
  con hooks activos. Reinicia para aplicar cambios.
- **Una escritura parcial ya puede haber alterado el buffer.** Se registra el
  fallo y se detienen las siguientes escrituras; no se promete restauración
  atómica del contenido ni recuperación del estado del juego. La preservación
  del retorno y de `GetLastError` se prueba por separado.
- **DDS y paquetes malformados aún pueden causar consumo de recursos o activar
  defectos desconocidos.** Se verifican rutas, tamaños, formatos, identidades y
  hashes, con presupuestos de recorrido/descompresión. Eso no demuestra que
  cualquier contenido aceptado sea inocuo para el decodificador del juego o la GPU.
  Un conjunto grande puede retrasar la activación y perder lecturas tempranas.
  Las sesiones de log anteriores se conservan; su total en disco no tiene rotación automática.
- **El instalador es código con los permisos de quien lo ejecuta.** Comprueba
  huellas, bloquea el ejecutable, fija los directorios mediante handles que
  impiden renombrarlos, rechaza reparse points y usa nombres de destino fijos.
  Conserva un proxy previo solo con su hash expresamente indicado; no descarga
  ni ejecuta DLL para comprobarlas. No debe ejecutarse elevado desde una carpeta
  compartida con usuarios no confiables. Las comprobaciones de archivos y su
  sustitución no forman una transacción global frente a escritores locales
  adversarios o una caída de alimentación. Sus hashes y registro local no son
  una frontera de seguridad frente a alguien que puede modificarlos.
- **Inicio bajo el loader lock.** `DllMain` solo guarda el módulo y programa un
  worker; no espera por él. El escaneo, hashes, archivos y MinHook se ejecutan
  después. Se conserva este inicio temprano por las lecturas iniciales del juego.
  Microsoft advierte que crear un hilo desde `DllMain`, aunque no se espere,
  requiere cautela. No se ha demostrado compatibilidad con todos los inyectores.
  [Buenas prácticas de Microsoft](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices).

## Protecciones verificables en el código

La DLL real se abre por su ruta absoluta de System32 con
`LOAD_LIBRARY_SEARCH_SYSTEM32`. No se busca un segundo `dinput8.dll` en `mods`,
el directorio actual o `PATH`. Los seis exports reenvían a Windows. No hay
descarga automática, servidor de red ni ejecución de DLL/scripts de mods.

El cargador exige el SHA-256 del ejecutable compatible y cuatro firmas únicas
en memoria. Cada escritura exige una identidad de miembro/callsite admitida,
contrato DDS compatible, rango válido, lectura completa y SHA-256 original;
después vuelve a comprobar los bytes escritos. Los desvíos usan snapshots
inmutables y una cola acotada; no asignan buffers dinámicos ni hacen acceso a
archivos o logging directo. Se conservan el retorno y `GetLastError` originales.

Las rutas virtuales rechazan traversal, ADS, nombres reservados y alias ambiguos.
La carga comprueba la ubicación final mediante handles y descarta enlaces.
El archivo se lee sin permitir escritores simultáneos; se conserva un snapshot.
Estas medidas restringen entradas, pero no aíslan un proceso que ya está bajo
control de otro programa del mismo usuario.

Los registros escapan controles y saltos de línea para evitar líneas falsificadas.
Incluyen nombres/rutas locales y direcciones de diagnóstico: revísalos antes de
publicarlos. La DLL Release activa `/GS`, `/sdl`, ASLR, DEP y CFG; el encabezado
PE tiene `DllCharacteristics=0x4160`. No se afirma que todas las dependencias
estáticas se hayan compilado individualmente con CFG.

Dependencias fijadas mediante vcpkg: MinHook 1.3.4 y zlib 1.3.2#2. Se consultaron
sus publicaciones oficiales; no se realizó un inventario completo de CVE ni se
declaran libres de vulnerabilidades.
[MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/releases/tag/v1.3.4),
[zlib](https://www.zlib.net/).

## Validación y límites de las conclusiones

Las comprobaciones del paquete están en `VALIDACION.json`: builds independientes,
pruebas de contratos/candidatos/configuración/rutas, escritor real ejecutado en
el proceso de pruebas, modos de escritura y observación, exports y reproducibilidad
de la DLL. La huella de cada fuente ensayada vincula la evidencia con el código.
`tests/install_tests.ps1` prueba instalar, verificar, restaurar un proxy, preservar
datos del usuario y rechazar entradas peligrosas en carpetas temporales del proyecto.

La validación visual anterior correspondía a Debug v6. Este Release no se instala
automáticamente en el juego ni se considera validado visualmente por haber pasado
las pruebas offline. Antes de publicarlo como estable, repite una sesión controlada
con DDS de distintos mods y conserva un camino de restauración.
