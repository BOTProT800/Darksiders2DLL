# Prueba de observación: 0.8.0-animation-trial

Esta DLL experimental **observa** las lecturas de animaciones `.anm`, pero no
las sustituye. Todas las versiones aceptan `animations=off|observe|override_keys`.
En esta, `override_keys` se comporta igual que `observe`, porque todavía no
existe un escritor de animaciones. Los DDS, los modelos y los scripts siguen
funcionando como indique su configuración.

## Qué comprueba

Si el motor lee los `.anm` por el mismo hook que ya reconoce DDS y modelos, y
en qué rangos. Para cada lectura de un clip candidato, el DLL calcula el
SHA-256 del buffer recibido y lo compara con el original y con el reemplazo.
Nunca escribe en ese buffer.

## Contrato `keys_in_place`

El reemplazo debe conservar byte a byte:

- la cabecera (FPS, duración, bucle y tamaño) y el nombre;
- las pistas, con sus flags, identificadores y pesos;
- el número de claves por curva, la longitud de cada segmento y los tiempos.

Solo pueden cambiar valores cuantizados: rotaciones, posiciones, escalas y el
canal escalar. Una rotación editada no puede superar magnitud 1. Compruébalo
antes con:

```powershell
.\check_animation.ps1 -Original "C:\original\D_Idle.anm" -Modified "C:\editado\D_Idle.anm"
```

`ANIM_CHECK_PASS contract=keys_in_place changed=1` confirma una edición válida.

## Sonda de esta prueba

`mods/anim_probe/media/characters/death/D_Idle.anm` suma 1 a un residuo de
rotación de una pista. El cambio máximo es de 0,0099°: es **invisible a
propósito**. La sesión solo busca identificar lecturas; no hay nada que mirar.

## Sesión

Desde el proyecto, con el juego cerrado:

```powershell
.\scripts\manage_animation_trial.ps1 -Action Plan
.\scripts\manage_animation_trial.ps1 -Action Observe
```

`Plan` no modifica nada. `Observe` guarda un respaldo en
`build/animation-trial/installation-backup`. Luego instala esta DLL, añade
`animations=observe` al INI actual y copia la sonda. El resto del INI no cambia.

Después:

1. Inicia el juego y carga una partida con Death.
2. Quédate quieto unos segundos para que se reproduzca su reposo.
3. Cierra el juego normalmente.
4. Ejecuta `.\scripts\manage_animation_trial.ps1 -Action Report`.

`Report` copia el log de la sesión y resume los eventos:

- `ANIMATION_VERIFIED_WOULD_OVERRIDE`: se leyó e identificó el rango editado.
- `ANIMATION_UNCHANGED_VERIFIED`: se reconoció un rango sin cambios, por
  ejemplo la cabecera.
- `ANIMATION_READ_UNMATCHED`: lectura del clip en un rango no previsto. Incluye
  su tamaño y SHA-256 para localizarla sin conjeturas.

## Volver atrás

Con el juego cerrado:

```powershell
.\scripts\manage_animation_trial.ps1 -Action Restore
```

Restaura la DLL y el INI anteriores (o su ausencia) y borra la sonda. Si algún
archivo cambió desde la instalación, se niega a tocarlo. No se modifican
paquetes `.upak`, esqueletos, modelos ni partidas.
