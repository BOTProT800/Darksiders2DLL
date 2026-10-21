# Darksiders2DLL

Cargador de mods (proxy de `dinput8.dll`) para Darksiders II Deathinitive
Edition. La documentación del proyecto está en español.

## Restricciones: solo las que fallaron en el juego

La regla completa, el registro de fallos (F-xx) y las confirmaciones (C-xx)
están en [RESTRICCIONES.md](RESTRICCIONES.md). Antes de añadir, mantener o
quitar cualquier comprobación que rechace, bloquee, reescale o desvíe contenido
de un mod, usa la skill `restricciones-probadas`.

- Ninguna comprobación rechaza contenido por una predicción (análisis estático,
  documentación de D3D9, «por seguridad», límites redondos, «sin probar»).
  Solo con un F-xx que cite la sesión del juego donde falló.
- Una predicción sin F-xx se registra como aviso en el log
  (`resize_warning="untested: …"`) y el contenido se entrega igual.
- Lo que la DLL aún no sabe hacer se dice así, «la DLL aún no lo implementa»,
  nunca «el juego no lo admite».
- No propongas límites nuevos «por si acaso» ni des por imposible algo que no
  se ha probado: propón la prueba en el juego.
- Cuando el usuario cuente un resultado del juego, anótalo en
  RESTRICCIONES.md el mismo día (F-xx o C-xx) y ajusta el código.

## Compilar y comprobar

```powershell
.\scripts\verify_release.ps1 -EvidenceName validation-NOMBRE
```

Compila las pruebas Debug, Release y de observación, dos Release que deben
coincidir byte a byte y un Debug, y ejecuta los smoke tests. No toca el juego.
La carpeta `build\` del checkout principal guarda los respaldos de la
instalación del juego: nunca la borres entera.
