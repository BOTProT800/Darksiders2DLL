# Máscara de Death: identidad de archivo, 0.4.1 diagnostic

## Fallo observado

Sesión `Darksiders2DLL-20260915-025539-21556-000.log`, Release 0.4.0.
El usuario instaló este Release y exportó nuevamente `Base_Mask_Diff.dds`.
El catálogo aceptó el DDS: 1024 × 1024, BC1/DXT1, 11 mipmaps,
699 192 bytes completos y 699 064 bytes de payload; un candidato, cero rechazos.
El DDS modificado tiene SHA-256
`86845D195DEA2D7718471DA16DD40DE7414683E00D79D5F176E2D328C0D126E3`.

La identidad de la máscara en disco es `(0x31374000, 0x13800, 223)`.
La lectura número 223 del scope 24 pidió/devolvió 432 bytes, por lo que el
escritor rechazó el tamaño. Terminó con un candidato observado, un rechazo
de contrato y cero intentos de escritura. Exportar otra vez los mipmaps no
resuelve este fallo: el contrato del archivo ya es válido.

## Causa y evidencia independiente

La versión 0.4.0 usa el contador TLS `read_ordinal` como `member_ordinal` del
catálogo. Los dos iconos anteriores coincidían, pero no prueban esa equivalencia
para todos los archivos. Se inspeccionó el ejecutable real, de SHA-256
`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`,
con DUMPBIN en modo lectura, rango `0x1409FEE5C..0x1409FF1F4`.
La copia local del desensamblado está en
`build/validation-member-identity/resource-processing.disasm.txt`.

- RVA `0x9FEEF9`: pone a cero RDX y guarda el índice de archivo en `[rsp+0x30]`.
- RVA `0x9FEF34`: usa ese índice para obtener la cantidad de buffers del archivo;
  los bucles interiores recorren esos buffers.
- RVA `0x9FF01D`: un destino nulo salta la lectura y acumula los bytes omitidos.
- RVA `0x9FF051`: algunos buffers siguen una ruta virtual que no pasa por el
  callsite de lectura directa utilizado por el mod.
- RVA `0x9FF0C8`: RCX recibe `rsp+0x70`; la llamada en `0x9FF0CD` devuelve a
  `0x9FF0D2`.
- RVA `0x9FF140`: incrementa `[rsp+0x30]` una vez por archivo, también cuando se
  omite su carga. El contador de lecturas directas no tiene esta propiedad.

Por eso el contador de lecturas puede quedar desplazado respecto al catálogo.
La inspección prueba el defecto; todavía hace falta observar qué lectura exacta
recibe la máscara en una sesión con la DLL corregida.

## Corrección

`HookStreamRead`, declarado `noinline`, conserva la dirección de su ranura de
retorno mediante `_AddressOfReturnAddress`. En el callsite admitido, el CALL
introduce ocho bytes: el índice está en `ranura+0x38` y el stream debe ser
exactamente `ranura+0x78`. Se comprueban caller, aritmética, igualdad del stream,
copia de memoria completa de ocho bytes y límite de dos millones de archivos.
La lectura de memoria usa `ReadProcessMemory`; no desreferencia un puntero
externo directamente. Cero representa una identidad no recuperada.

El campo `read.member_ordinal` guarda el índice del archivo más uno. El contador
`read_ordinal` se conserva exclusivamente como telemetría. No hay fallback a ese
contador. El log muestra ambos campos por separado y contabiliza capturas
inválidas con `resource_identity_invalid_members`.

Las validaciones de caller exterior, paquete/tabla, destino, lectura completa,
tamaño, SHA-256 original y SHA-256 después de escribir siguen siendo obligatorias.
El contrato DDS, los límites, la configuración y la política de fallo del
escritor no se relajan. No se añaden hooks ni asignaciones dentro del detour.

Referencia de la intrínseca:
[_AddressOfReturnAddress, Microsoft](https://learn.microsoft.com/en-us/cpp/intrinsics/addressofreturnaddress?view=msvc-170).
Los offsets no proceden de una garantía genérica de C++: pertenecen únicamente
al ejecutable concreto inspeccionado y validado por hash.

## Pruebas y artefacto

La prueba MASM `tests/resource_read_frame.asm` reproduce el marco de llamada
observado y llama al detour real, tanto en Debug como en Release. Este archivo
solo se enlaza al ejecutable de pruebas. La suite ejercita saltos de archivos,
varias lecturas por archivo, contenido idéntico de otro archivo, índices
inválidos y marco incorrecto, además de los gates y fallos del escritor.
La prueba opcional del juego verifica las instrucciones indicadas y el contrato
original completo/payload de la máscara, leyendo únicamente los archivos.

La validación se ejecuta en `build/validation-member-identity`, separado de la
evidencia y del ZIP de 0.4.0. El nuevo binario se identifica como
`0.4.1-diagnostic`. El empaquetador 0.4.0 rechaza esta versión para no publicar
una DLL de diagnóstico con la etiqueta anterior.

La matriz terminó con `PASS` y se volvieron a ejecutar los tres perfiles tras
permitir optimizaciones en el detour real de las pruebas Release. Pasaron:

- Debug y Release del escritor, incluidos nueve gates y cuatro fallos inyectados.
- Release de observación, sin escrituras ni contadores de escritura.
- Comprobaciones de instrucciones del ejecutable y contrato real de la máscara.
- Dos DLL Release idénticas byte a byte y tres smoke tests de los seis exports
  y del reenvío a DirectInput de System32 (dos Release y Debug).
- Protecciones PE `0x4160`, ausencia del caso especial en la DLL y comparación
  de las fuentes con las huellas de la evidencia.
- DLL instalada y metadata de los tres `.upak` sin cambios.

Artefacto preparado: `build/diagnostic-member-identity/dinput8.dll`, 801 792 bytes,
SHA-256 `93942475C213D63DF7C2AFA480E9B59280ACD33A4BA02DF84C3B035F6FCAD54B`.
La evidencia completa está en `build/validation-member-identity/VALIDACION.json`.
El directorio del diagnóstico contiene una copia de esa evidencia y
`PRUEBA_PENDIENTE.json` con los hashes del DDS y del proxy instalado.

Se copió el proxy actual únicamente al respaldo local
`build/diagnostic-member-identity/installed-before/dinput8.dll`, con SHA-256
`172089776EA523EF4B7C2786A4BF6F4EFE1E8007ED1C2273F6BD0B091C2466D5`.

## Prueba en el juego: éxito confirmado

El usuario instaló la DLL de diagnóstico y confirmó: «Prueba exitosa».
La sesión `Darksiders2DLL-20260915-051305-29488-000.log` identifica
`version=0.4.1-diagnostic` y registra a las `2026-09-15T05:13:16.029Z`:

- `GENERAL_DDS_OVERRIDE_HIT`, ruta `media/characters/death/base_mask_diff.dds`.
- `read_ordinal=474`, **`member_ordinal=223`**, paquete `0x31374000`, tabla `0x13800`.
- `decision=payload`, petición y retorno completos de 699 064 bytes.
- SHA-256 original observado
  `93AA44179BA0185CC5E632E7A1EAEB2B45791136B6C4BA2FA32FF25AE8AAF694`.
- `replacement_written=true`, `replacement_verified=true`, error Win32 del
  reemplazo igual a cero.

Los contadores finales muestran un candidato, un intento y una escritura
completada; cero errores de identidad, contrato, hash, escritura o verificación.
La diferencia entre lectura 474 y miembro 223 confirma directamente la causa
del fallo anterior. Las dos sesiones inmediatamente previas aún usaban 0.4.0
y repetían el rechazo de 432 bytes; no pertenecen al binario corregido.

En la comprobación posterior el juego estaba cerrado, el DDS mantenía su hash,
y `dinput8.dll` coincidía con el diagnóstico validado `93942475...FCAD54B`.
La metadata de los tres `.upak` coincidía con la captura previa. No se volvió a
instalar ni a iniciar el juego desde el agente durante esta confirmación.
Se conserva la versión funcional instalada por el usuario y el respaldo local
de 0.4.0. No se restaura automáticamente un proxy anterior.

Evidencia de la sesión: `build/diagnostic-member-identity/game-validation.json`
y copia del log en ese directorio. `PRUEBA_PENDIENTE.json` y `VALIDACION.json`
se conservan como evidencia histórica anterior a la prueba, sin reescribir sus
resultados offline. Esta validación cubre la máscara y el ejecutable ensayados;
el ZIP público local 0.4.0 sigue conteniendo la versión anterior.
