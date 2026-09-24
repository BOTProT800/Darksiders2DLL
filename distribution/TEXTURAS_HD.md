# Texturas HD y PNG — 0.6.0-texture-trial

La DLL admite DDS de dimensiones distintas al original y PNG rectangulares.
Por ejemplo, una textura original de 512 × 512 puede tener un reemplazo de
2048 × 2048 o de 1024 × 256. Se conserva el nombre/ruta del recurso de destino.
El motor crea la textura, sus mipmaps y sus vistas con los parámetros nuevos.
Los buffers del paquete original no se agrandan ni se sobrescriben con esos datos.

La ruta nativa es experimental y específica del ejecutable compatible
`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`.
Se debe comprobar en el juego el evento de sustitución y el resultado visual.

## Archivos y configuración

Un DDS mantiene la ruta habitual, por ejemplo:
`mods/mi_mod/media/characters/death/reaper_scythe_diff.dds`.

Para un PNG utiliza `reaper_scythe_diff.dds.png` o `reaper_scythe_diff.png` en
esa misma carpeta; ambas formas apuntan a `reaper_scythe_diff.dds` del juego.
No dejes simultáneamente DDS y PNG con el mismo destino dentro de un mod:
se rechazan todos esos alias para evitar una elección dependiente del orden.
Entre mods sigue ganando el primer nombre de carpeta en orden léxico.

En `[loader]`, `textures=native` es el valor predeterminado y permite PNG y
contratos de tamaño distinto. `textures=exact` recupera el comportamiento anterior:
solo DDS con el contrato original. `mode=observe` registra las coincidencias sin
sustituir texturas. `models` conserva su configuración independiente.

## Formatos y límites

- En la ruta nativa: DDS tradicionales BGRA8, BC1/DXT1, BC2/DXT3 y BC3/DXT5.
  El original también debe ser uno de esos formatos.
- PNG se decodifica mediante Windows Imaging Component a BGRA8, con alfa sin
  premultiplicar y una cadena completa de mipmaps. No se cambia el tamaño ni se
  aplica una transformación de perfil de color. No se necesitan DLL adicionales.
- Los mipmaps PNG usan promedio de canales. No se reconstruyen normales ni se
  adapta automáticamente el material entre juegos. Para controlar filtrado,
  compresión, canales o mipmaps especializados, prepara un DDS.
- Máximo 16384 por dimensión; el dispositivo puede imponer un límite menor.
  Sigue existiendo un presupuesto de 64 MiB por asset y 256 MiB total. Para PNG
  se cuenta el archivo comprimido además de todos los píxeles/mipmaps decodificados.
- No se admiten DX10, cubemaps, arrays, volúmenes, superficies dinámicas, ni
  paquetes por bloques en esta ruta. Si el juego omite mipmaps por calidad baja,
  transforma los datos o usa otra ruta de carga, el contrato puede no coincidir y
  se conserva el recurso original.

PNG sin compresión de GPU consume más memoria que DDS BC1/BC3. Una textura
2048 × 2048 usa aproximadamente 16 veces los datos de una 512 × 512 con formato
y mipmaps equivalentes.

## Lifehunt Scythe → guadaña de forma de segador

Los tres PNG del usuario, todos 1024 × 256, se preparan así:

- `WP_A_0810.png` → `reaper_scythe_diff.dds.png`.
- `WP_A_0810_n.png` → `reaper_scythe_norm.dds.png`.
- `WP_A_0810_s.png` → `reaper_scythe_spec.dds.png`.

Cada PNG decodificado ocupa 1398236 bytes, tiene 11 mipmaps y conserva sus canales.
El mapa emisivo original continúa siendo el del juego. Esta asignación permite
probar los mapas, pero **no importa el modelo OBJ ni adapta sus UV/materiales**.
El modelo de guadaña encontrado en el mod coincide con el original del juego;
por eso la apariencia no equivale a una conversión completa de Lifehunt Scythe.

## Comprobación

`offline_tests.exe --texture-check CARPETA_O_ARCHIVO` comprueba PNG/DDS.
`offline_tests.exe --texture-catalog CARPETA_JUEGO CARPETA_MODS` verifica las
identidades y contratos reales leyendo los paquetes sin modificarlos.

En el log de una sesión busca `NATIVE_TEXTURE_HOOK_ACTIVE`,
`NATIVE_TEXTURE_SOURCE_VERIFIED`, `NATIVE_TEXTURE_VERIFIED_UPLOAD` y
`NATIVE_TEXTURE_OVERRIDE_HIT`. Este último indica que el helper nativo devolvió
éxito al crear el reemplazo; todavía hace falta confirmar su apariencia.
`NATIVE_TEXTURE_FALLBACK` significa que se intentó crear el reemplazo, falló y
se llamó de nuevo al helper con los parámetros originales.
`NATIVE_TEXTURE_CONTRACT_REJECTED` conserva el original sin intentar reemplazarlo.

## Instalación y restauración de esta prueba

`scripts/install_lifehunt_texture_trial.ps1` comprueba la compilación validada,
el ejecutable y la DLL previa, requiere el juego cerrado y respalda los archivos
antes de sustituirlos. Su acción predeterminada `Check` solo muestra los cambios.
La instalación retira el DDS duplicado del mod y guarda el original en el respaldo.
No modifica el INI, otros mapas/modelos ni los paquetes del juego.

Para restaurar esta instalación, desde la carpeta del proyecto y con el juego cerrado:

```powershell
.\scripts\install_lifehunt_texture_trial.ps1 -Action Restore `
  -GameDirectory 'C:\Program Files (x86)\Steam\steamapps\common\Darksiders II Deathinitive Edition' `
  -BackupDirectory "$PWD\build\texture-hd-trial\installed-before"
```

La restauración comprueba los hashes y se detiene si los destinos cambiaron
después de instalar. Conserva la carpeta de respaldo. La escritura en la carpeta
de Steam puede requerir permisos adicionales.
