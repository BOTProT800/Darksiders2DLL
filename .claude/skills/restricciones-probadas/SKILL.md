---
name: restricciones-probadas
description: Usar antes de añadir, mantener, endurecer o quitar cualquier comprobación de Darksiders2DLL que rechace, bloquee, reescale, convierta o desvíe contenido de un mod (texturas, .2, .anm, PNG, tamaños, formatos, límites de memoria, modos del INI); cuando un mod "no funciona" o se rechaza; cuando el usuario cuenta un resultado del juego (funcionó, se cerró, se ve mal, no cargó); y al documentar un límite en README o en las guías.
---

# Restricciones probadas en el juego

Regla del proyecto: una restricción de contenido solo existe si falló en el
juego y está registrada como F-xx en `RESTRICCIONES.md`. Todo lo demás es una
predicción: aviso en el log, nunca rechazo. Lee `RESTRICCIONES.md` antes de
seguir.

## 1. Clasifica la comprobación

Para cada rechazo, conversión o desvío que vayas a escribir o revisar:

| Categoría | Pregunta | Qué hacer |
|---|---|---|
| Fallo en el juego | ¿Hay un F-xx con la sesión donde falló? | Comprobación estrecha que cita el F-xx en el comentario y en el mensaje |
| Predicción | ¿Se basa en análisis estático, documentación, «por seguridad» o un límite redondo? | Aviso `resize_warning="untested: …"` (o equivalente en el log) y se entrega igual |
| Mecánica de la DLL | Sin ella, ¿la DLL entregaría bytes que el motor no pidió, escribiría fuera de un búfer o parchearía otra posición? | Se mantiene; el mensaje dice qué no puede hacer la DLL |
| Aún no implementado | ¿La DLL no sabe hacerlo todavía (otro tamaño de metadatos, cubemaps, otro formato)? | Mensaje «the DLL cannot yet…»; fila en «Aún no implementado» |
| Seguridad y plataforma | ¿Protege el proceso o la instalación sin mirar el contenido (exe, rutas, ganchos, INI)? | Se mantiene |

Si dudas entre «predicción» y «mecánica», pregúntate: ¿la DLL sabe entregar
exactamente lo que el motor va a leer? Si sí, es una predicción.

Señales de una predicción disfrazada:

- Topes fijos que no salen del formato ni del archivo (`256`, `4096`,
  `1'000'000`, `64 MiB`, `16384`).
- Valores de contenido: floats finitos, rangos, nombres, cajas, normales.
- Comentarios como «D3D9 documenta», «el motor no comprueba», «podría
  bloquear el juego», «no verificado».
- Modos por defecto que apagan algo ya confirmado en el juego (C-xx).

## 2. Cuando el usuario cuenta un resultado del juego

1. Reúne la evidencia: versión y SHA-256 de la DLL, log de la sesión
   (`Darksiders2DLL-*.log` en la carpeta del juego), archivos del mod con su
   SHA-256, y el síntoma con las palabras del usuario.
2. **Funcionó:** añade un C-xx en «Confirmaciones en el juego» y retira los
   avisos o límites que ese resultado desmiente.
3. **Falló:** busca la causa (log, `resize_warning`, volcado de WER). Añade un
   F-xx con fecha, sesión, síntoma, causa y la restricción que justifica
   («Ninguna» si fue un error de la DLL). Solo entonces convierte el aviso en
   rechazo, lo más estrecho posible, con una prueba del caso.
4. Actualiza la fecha de estado de `RESTRICCIONES.md` y las guías afectadas
   (`README.md`, `distribution/*.md`).

## 3. Al quitar una restricción

- Pásala a la tabla «Retirado» de `RESTRICCIONES.md` con lo que predecía y el
  aviso que la sustituye.
- Cambia las pruebas que esperaban el rechazo: ahora esperan el aviso y la
  entrega.
- Si el rechazo protegía la memoria de la DLL, deja el límite que sale del
  archivo (un recuento no puede superar los bytes del archivo) en vez de un
  número fijo.

## 4. Redacción

- Nunca «el juego no lo admite» o «not supported» sin un F-xx.
- Sí: «sin probar en el juego», «predicción: …», «la DLL aún no lo implementa».
- En los avisos del log, prefijo `untested:`.

## 5. Auditoría rápida

```bash
grep -n 'error = L"\|throw std::runtime_error\|AddIssue(' *.cpp
```

Cada resultado debe encajar en una categoría del paso 1. Compila y comprueba con
`scripts/verify_release.ps1` antes de dar el cambio por terminado.
