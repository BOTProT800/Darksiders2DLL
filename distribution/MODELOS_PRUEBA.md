# Prueba de forma: 0.5.1-shape-trial

Esta DLL experimental acepta la edición de `death_head.2` exportada por ANANSI
sin eliminar sus normales ni tangentes recalculadas. El paquete local está en
`build/shape-trial/package`. Pasar las pruebas fuera del juego no demuestra la
carga ni el resultado visual dentro del motor. Para esta cabeza concreta, la
sesión del 16 de septiembre de 2026 ya confirmó escritura verificada, cero
fallos del escritor y cambio visible mediante captura y confirmación del usuario.
Véase `research/MODEL_TRIAL.md` en el proyecto para la evidencia de esta prueba.

## Contrato compartido con el exportador

- Igual tamaño, cantidad y orden de vértices/triángulos.
- Posiciones finitas dentro de la AABB calculada con los vértices originales,
  con tolerancia `max(1, extensión del eje) * 0.00001`.
- En mallas skinned verificadas: normales y tangentes XYZ unitarias,
  perpendiculares entre sí, signo de tangente ±1 y copia coherente de normal Z.
  Tolerancia de la base: 0,0011; se comprueba la plantilla y la edición.
- Cabeceras, **cajas de límites**, índices, UV, colores, pesos, huesos, materiales
  y campos desconocidos idénticos byte a byte. Las mallas estáticas se conservan
  sin edición en este contrato.

El addon también puede exportar cambios de UV, pesos o cajas: esta prueba los
rechaza. Ampliar cajas requiere resolver el espacio de `BBFollowNode` y comprobar
su lectura coherente en el motor; no se presume que las cajas estén en espacio
de modelo. La contención usada aquí no constituye una validación general de
culling o animación.

`models=override_positions` conserva el contrato anterior, exclusivamente XYZ.
El contrato nuevo se usa con `models=observe` y `models=override_shape`.
Los modelos siguen desactivados por defecto (`models=off`).

## Revisión previa

Desde el paquete:

```powershell
.\offline_tests.exe --model-check "C:\original\death_head.2" "C:\editado\death_head.2"
.\offline_tests.exe --model-catalog "C:\ruta\del\juego" "C:\ruta\de\mods"
```

`MODEL_CHECK_PASS contract=bounded_shape changed=1` confirma una edición válida.
`changed=0` indica una vuelta sin cambios. `MODEL_CATALOG_PASS` confirma la ruta
y el contrato contra los bytes recuperados del paquete instalado. El catálogo
imprime las huellas del original y del reemplazo de cada rango.

La copia investigada debe ir en:

```text
mods/first_test_mod/media/characters/death/death_head.2
```

Es una cabeza de 148.961 bytes y cuatro mallas. `death.2` es otro recurso, de
381.861 bytes. Conserva la copia equivocadamente nombrada fuera del árbol de
mods y usa el nombre de la fuente. Ningún nombre de mod tiene trato especial.

## Observación y reemplazo

Con el juego cerrado, conserva un respaldo externo de la DLL, el INI si existe
y los archivos del mod que vas a cambiar. Instala la DLL comprobada y configura:

```ini
[loader]
enabled=true
mode=override
models=observe
```

Los DDS mantienen su funcionamiento. Carga una partida en la que aparezca la
cabeza. En `%LOCALAPPDATA%\Darksiders2DLL\logs` deben aparecer:

1. `SESSION_START version=0.5.1-shape-trial`.
2. `MODEL_CANDIDATE` para `media/characters/death/death_head.2`.
3. `MODEL_VERIFIED_WOULD_OVERRIDE changed=true`, con tamaño y SHA-256 originales
   coincidentes. `MODEL_UNCHANGED_VERIFIED` solo identifica una parte intacta.

Las lecturas de cabeceras, índices, UV y pesos pueden producir
`MODEL_READ_UNMATCHED`: esos campos no se editan ni se reemplazan. Si no se
verifica el rango que contiene la edición, hay que adaptar la interceptación
antes de habilitar escritura. No se buscan patrones dentro de buffers
arbitrarios ni se adivinan offsets.

Después de confirmar la lectura modificada, cierra normalmente el juego y cambia
únicamente `models=override_shape`. Reinicia, carga la misma partida y exige
`MODEL_OVERRIDE_HIT changed=true replacement_written=true replacement_verified=true`,
cero fallos y cambio visible. Revisa orientación, animación, iluminación y
desapariciones al mover la cámara. Un log de escritura por sí solo no acredita
el resultado visual.

La lectura puede ser el archivo completo, el payload o un bloque nativo de
vértices. Cada bloque skinned contiene juntas posición, normal y tangente; los
demás campos permanecen intactos. Cada rango exige identidad, tamaño y huella
original inequívocos, segunda verificación antes de escribir y huella posterior.

`MODEL_OVERRIDE_FAILED` bloquea nuevas escrituras, también DDS, hasta reiniciar.
Una copia parcial fallida no se revierte automáticamente. `mode=observe` es la
barrera global para toda escritura. Para volver a la instalación anterior,
restaura DLL, INI (o su ausencia) y nombres del mod desde el respaldo con el juego
cerrado. No se modifican paquetes `.upak`, esqueletos, animaciones ni colisión.
