# Restricciones: solo las que fallaron en el juego

Estado al 20 de octubre de 2026 (código 0.12.0-tested-limits-trial).

## La regla

1. **Ninguna comprobación puede rechazar, bloquear, reescalar o desviar el
   contenido de un mod por una predicción.** Una predicción es todo lo que no se
   ha visto fallar en el juego: análisis estático del ejecutable, documentación
   de D3D9, un «por seguridad», un límite redondo o un «nunca se ha probado».
2. Una restricción de contenido solo existe si tiene una entrada **F-xx** en el
   [registro de fallos](#registro-de-fallos-en-el-juego), con la sesión del juego
   que la demuestra. El código y su mensaje citan esa entrada.
3. Mientras no haya fallo, la predicción se registra como **aviso** en el log
   (`resize_warning="untested: …"`) y el contenido se entrega igual. Si el juego
   falla con ese mod, el aviso dice qué hipótesis probar primero.
4. Lo que la DLL todavía no sabe hacer se describe como «**la DLL aún no lo
   implementa**», nunca como «el juego no lo admite». Son tareas pendientes, no
   límites del juego.
5. Un resultado del juego, bueno o malo, se anota aquí el mismo día: un fallo
   como F-xx y una confirmación como C-xx. Una restricción que el juego desmiente
   se elimina.

Las comprobaciones de seguridad y de la mecánica de la DLL no limitan el
contenido: protegen el proceso o evitan que la DLL entregue bytes equivocados.
Están [clasificadas abajo](#lo-que-se-mantiene-y-por-qué) y también necesitan
una categoría antes de añadirse.

**Por qué existe esta regla.** Varias veces un límite sin probar bloqueó un mod
que sí funcionaba:

- Las texturas de los segmentos por bloques (`media/items/...`) se rechazaban
  «por seguridad». En el juego, la hombrera nueva cargó sin sus texturas (F-03).
- Las texturas de otra resolución se reescalaban al tamaño original porque la
  entrega a su tamaño «no estaba confirmada». En cuanto se probó, funcionó a la
  primera (C-07).
- Un tope de 1.000.000 en un desplazamiento del `.2` rechazaba todos los modelos
  de más de 1 MB, entre ellos `reaper_form_ghost.2` y `serpent.2` (11 de 13
  rechazos del muestreo del 7 de octubre; [MODEL_TRIAL.md](research/MODEL_TRIAL.md)).

## Registro de fallos en el juego

**A día de hoy ninguna restricción de contenido está respaldada por un fallo en
el juego.** Todos los fallos vistos hasta ahora fueron errores o vías inactivas
de la propia DLL, y ninguno justifica rechazar contenido.

| Id | Versión y sesión | Qué se vio | Causa | Restricción que justifica |
|---|---|---|---|---|
| F-01 | 0.4.0, `Darksiders2DLL-20260915-025539-21556-000.log` | La máscara de Death no se sustituyó aunque su DDS era válido | La DLL usaba el contador de lecturas como índice de archivo | Ninguna: error de la DLL, corregido en 0.4.1 (C-01). [Detalle](research/MASK_MEMBER_IDENTITY_FIX.md) |
| F-02 | 0.6.0 a 0.9.1, 12 sesiones desde `Darksiders2DLL-20260919-183109-16444-000.log` | La subida nativa D3D11 (`0xD4E62C`) nunca se ejecutó | El juego usa D3D9 salvo con `-dx11` | Ninguna: la vía nativa no sirve con D3D9. [Detalle](research/TEXTURE_RESIZE_RESEARCH.md) |
| F-03 | 0.9.0, `Darksiders2DLL-20261018-000727-23376-000.log` | La hombrera nueva cargó, pero sin sus texturas | El catálogo DDS rechazaba los segmentos por bloques, un límite sin probar | Ninguna: se quitó el límite en 0.9.1 (C-05). [Detalle](research/MODEL_TRIAL.md) |
| F-04 | Validación del POC D3DX, septiembre de 2026, PID 16272 | `0xc0000005` en `Darksiders2.exe+0x9E553F` | No demostrada; variante retirada, no se reprodujo en 9 ejecuciones | Ninguna. [Detalle](PLAN_MAESTRO.md) |

## Confirmaciones en el juego

Lo que el juego ya aceptó. Ninguna regla puede volver a prohibirlo sin un F-xx.

| Id | Versión | Confirmado |
|---|---|---|
| C-01 | 0.4.1 | DDS con el contrato original (máscara de Death), copia en el búfer de lectura |
| C-02 | 0.5.1 | `death_head.2` con posiciones, normales y tangentes editadas |
| C-03 | 0.8.1 | `.2` con vértices y caras borrados (Anansi), reexpandido al tamaño original |
| C-04 | 0.9.0 | Malla nueva sin reglas de contenido (`ds1a01_shoulder.2`), en un segmento por bloques |
| C-05 | 0.9.2 | DDS de 512×1024 convertidos al contrato original de 1024² |
| C-06 | 0.10.0 | `.2` mayor que el original (máscara de 432 a 864 vértices), a su tamaño |
| C-07 | 0.11.0 | DDS de otro tamaño, formato y mips (512×1024 BC1 de 11 niveles y BC3 de 1 nivel) a su tamaño |

## Retirado en 0.12.0

Restricciones sin fallo en el juego que se quitaron. Cada una indica qué
predecía y qué buscar en el log si un mod falla.

**Texturas**

| Antes | Ahora | Predicción sin probar |
|---|---|---|
| `textures=native` por defecto: en D3D9, toda textura distinta se reescalaba al original | `textures=resize` por defecto; `native` sigue disponible | — |
| Lados BC no múltiplos de 4: rechazo | Se entrega con `resize_warning="untested: D3D9 documents BC sides as multiples of 4"` | `CreateTexture` falla y el motor se cierra (`0x75A9B3`) |
| Lados de más de 16384: rechazo | Hasta 65535, el máximo del campo u16 del `extra`; por encima de 16384, aviso | El dispositivo no crea la textura |
| DDS con cabecera DX10: rechazo | Se reescribe con la cabecera clásica de su formato | — |
| RGBA8 y BGR8: reescalados al original | Se entregan a su tamaño como BGRA8 (sin pérdida) | — |
| `max_dimension` de 32768 en la validación DDS | 65535 | — |

**Mallas**

| Antes | Ahora | Predicción sin probar |
|---|---|---|
| `models=off` por defecto | `models=override_unchecked` por defecto | — |
| `override_positions` y `override_shape` con reglas de contenido (posiciones dentro de la caja, UV, pesos y huesos inmutables, bases ortonormales…) | Alias de `override_unchecked`; las reglas quedan solo en las comprobaciones offline | — |
| Topes de 64 MiB por `.2`, 256 mallas, 1.000.000 por recuento y por vértices, 256 huesos, 4096 materiales y 4096 bytes por nombre | Solo los límites del archivo: cada recuento cabe en el archivo y las lecturas del motor son de 31 bits | — |
| Cajas y matrices de huesos finitas, cajas no invertidas, nombres no vacíos | Sin regla | — |
| Redimensionado: cabecera incoherente con los recuentos, índices fuera de la malla o prefijo de índices distinto: rechazo | Se entrega con `resize_warning="untested: …"` (el primer problema encontrado) | La fase 2 del motor (`0x777D7C`) lee mal la cabecera o la GPU lee fuera del búfer |

**Memoria y tamaño de la carpeta de mods**

| Antes | Ahora |
|---|---|
| 64 MiB por archivo y 256 MiB en total (índice de mods, originales de texturas y de mallas). Un PNG de 4096² con mips ya los superaba, y pasarse del total desactivaba **todo** el cargador | Sin presupuesto. Si un archivo no cabe en memoria, solo ese archivo se descarta (`MOD_INDEX_ISSUE`) |
| 16.384 archivos de mods, 100.000 entradas y 16.384 miembros por lectura del paquete | Sin tope |

## Lo que se mantiene, y por qué

### Seguridad y plataforma

No dependen del contenido del mod.

- SHA-256 del ejecutable soportado: los ganchos usan direcciones fijas de esa
  compilación. Otro ejecutable desactiva los mods.
- Ganchos completos o ninguno; tras un fallo de escritura o verificación, el
  escritor se bloquea hasta reiniciar.
- Rutas dentro de `mods/`, sin puntos de reanálisis ni salidas de la carpeta;
  profundidad máxima de 32 carpetas.
- INI estricto: una clave desconocida o repetida desactiva el cargador.
- Los `.upak` solo se abren para lectura.
- PNG de más de 4 GiB decodificados (límite `DWORD` de WIC).

### Mecánica de la DLL

Sin estas comprobaciones la DLL entregaría bytes que el motor no pidió. Su
mensaje dice qué no puede hacer la DLL.

- Identidad del miembro y SHA-256 del original leído del stream.
- El `extra` del paquete debe repetir el contrato del original (texturas) o los
  recuentos de su cabecera (mallas): demuestra que la DLL entiende ese miembro.
- El archivo mide exactamente lo que leerá el motor: 128 + Σ niveles en
  texturas y Σ búferes en mallas.
- Campos del `extra`: lados u16, mips u8 y lecturas de 31 bits.
- Lecturas ambiguas en `override_unchecked` (dos rangos iguales del original).
- Encaje en los tamaños originales: la estructura que la DLL necesita para
  recolocar arrays (tipo de malla, recuentos, desplazamientos, índices dentro de
  la malla). Si no encaja, se intenta el redimensionado.

### Aún no implementado en la DLL

No son límites del juego. Hoy esos mods se rechazan o se reescalan.

| Caso | Hoy | Qué haría falta |
|---|---|---|
| Otro número de mallas o de pares en un `.2` | Rechazo (`the DLL cannot yet change the length of the package metadata entry`) | Reescribir el bloque de metadatos con otra longitud |
| Recuento de pares positivo en un `.2` | Rechazo | Reproducir la tabla de 4 bytes del generador `0x758A60` |
| Cubemaps, volúmenes y arrays | Sin candidato | `extra` de tipo 3/2 y lectura por caras |
| Formatos sin código de motor conocido (BC4, BC5, BC7…) | BC4/BC5 sin candidato; los demás no pasan la validación DDS | Descubrir sus códigos o decodificarlos a BGRA8 |
| Recorte de mips activo (`MaxMipLevels`, `TextureDetail`) | Conversión al original (`TEXTURE_RESIZE_METADATA_DECLINED`) | Saltar en el reemplazo los niveles que descarta el motor |
| Conversión al original de más de 4096 por lado | Sin conversión (solo se usa como respaldo del redimensionado) | — |
| Sustitución de animaciones `.anm` | Solo observación | Escritor de animaciones |

## Cómo añadir una restricción

1. **Reprodúcela en el juego.** Anota versión y SHA-256 de la DLL, el log de la
   sesión, los archivos del mod (ruta y SHA-256) y el síntoma: cierre, error
   visual o el recurso no carga. Guarda el volcado si hubo cierre.
2. **Encuentra la causa** con el log, el volcado o el análisis del ejecutable.
   Si es un error de la DLL, se corrige la DLL y se registra como F-xx con
   «Ninguna» en la última columna.
3. **Registra el F-xx** en la tabla de arriba antes de tocar el código.
4. **Implementa la comprobación más estrecha** que evite ese fallo y nada más.
   El comentario del código y el mensaje del rechazo citan el F-xx, por ejemplo
   `"BC sides not multiple of 4 crash CreateTexture (RESTRICCIONES.md F-NN)"`.
   Añade una prueba con el caso del fallo.
5. Actualiza las guías que mencionen el límite con el enlace al F-xx.

Para comprobar una predicción en el juego, instala la prueba con los scripts de
`scripts/` (respaldan la instalación) y busca `resize_warning` en el log. Si el
juego la acepta, regístrala como C-xx y retira el aviso; si falla, regístrala
como F-xx y conviértela en restricción.

## Auditoría

Cada rechazo de contenido debe estar en una de las categorías de arriba:

```bash
grep -n 'error = L"\|throw std::runtime_error\|AddIssue(' *.cpp
```

Un rechazo sin categoría, o un texto como «not supported» o «el juego no lo
admite» sin un F-xx, se convierte en aviso o se elimina.
