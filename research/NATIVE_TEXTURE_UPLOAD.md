# Carga nativa de texturas 2D — 0.6.0

Build de referencia: `Darksiders2.exe`, SHA-256
`5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB`.
Análisis de DUMPBIN de la instalación local, sin modificar el ejecutable.
Los rangos y evidencia de compilación están en `build/texture-hd-trial` y
`build/validation-texture-trial` (archivos locales excluidos de Git).

## ABI y flujo observados estáticamente

RVA `0xD4E62C` recibe, según sus lecturas de registros y pila:

```cpp
bool Upload2D(void* textureObject, void* device,
              uint16_t width, uint16_t height, uint32_t engineFormat,
              uint16_t mipCount, const void* pixels, uint32_t usageFlags);
```

El prólogo guarda el RSP de entrada en RAX y establece RBP = RAX - 0x47.
Por tanto `[rbp+0x6F]`, `+0x77`, `+0x7F`, `+0x87` son los argumentos
5, 6, 7 y 8. Itera los niveles a partir del puntero de píxeles, calcula tamaño y
pitch con `0x7514C8` y construye un array `D3D11_SUBRESOURCE_DATA` de 16 bytes.
En `0xD4E7C7` llama a `0xD4E424` con un `D3D11_TEXTURE2D_DESC` y esos datos.

El helper `0xD4E424` libera los recursos previos (`vtable+8`), llama a
`ID3D11Device::CreateTexture2D` (`vtable+0x28`, en `0xD4E45B`), y usa el mismo
descriptor para crear las vistas mediante `0xD4ED98`. El helper exterior guarda
formato, ancho, alto, profundidad, mipmaps y bytes en `textureObject+0x20..0x2F`
solo al tener éxito. Así se actualizan el recurso de GPU y los metadatos del
objeto; interceptar solo CreateTexture2D dejaría esos metadatos antiguos.

El llamador de recursos `0x108842B..0x1088457` pasa el puntero RSI y los campos de
un descriptor del recurso. Se admite exclusivamente su retorno `0x1088457`.
Otro llamador `0xD7137C` crea datos propios y queda excluido. La instalación
verifica una firma única del prólogo, su RVA y una firma única del callsite.
El bootstrap ya exige el hash del ejecutable compatible.

La conversión de formatos en `0x751260` prueba los valores internos admitidos:
6 → DXGI 87 (BGRA8), 15 → 71 (BC1), 16 → 74 (BC2), 17 → 77 (BC3).
El flag del objeto `+0x18` controla la conversión sRGB del motor y se conserva.
No se infieren formatos adicionales a partir de su número DDS/DXGI.

## Autorización del reemplazo y vida de los datos

La identidad procede del miembro OBPK explícito de la corrección 0.4.1.
Después de una lectura completa se comprueban tamaño y SHA-256 del original
recuperado del paquete, y se registra el puntero a su payload. El registro
contiene a lo sumo un buffer pendiente por candidato. Una lectura posterior
que solape ese buffer lo invalida; un upload lo consume una sola vez.

En la creación nativa se exigen ese mismo puntero, el ancho/alto/mips/formato
originales, flags de uso estático igual a cero y nuevamente el SHA-256 de todos
los bytes originales. No hay fallback por tamaño, basename o hash global entre
miembros distintos. Descriptores o punteros transformados no se sustituyen.
Las lecturas de memoria usan ReadProcessMemory con bloques acotados y SHA-256
sin asignación. El detour no abre archivos ni decodifica PNG.

El snapshot conserva los bytes de reemplazo durante la vida del proceso. La
función original recibe su puntero y los parámetros nuevos, construye los mips,
la textura y vistas y actualiza su objeto. Si devuelve false se invoca otra vez
con los argumentos originales. Los bytes originales y los `.upak` no se alteran.
La ruta antigua de copia rechaza explícitamente todo candidato nativo.

Las escrituras nativas se habilitan solo después de activar la cohorte de
identidad/lectura. `mode=observe` nunca cambia los argumentos. Los fallos del
hook mantienen passthrough. La telemetría se publica mediante atómicos y se
formatea/escribe en el hilo de logs.

## Verificación y límite de evidencia

Las pruebas cubren 512 → 2048 con distinta cadena mip, 512² → 1024×256 con
cambio BC1 → BGRA8, creación real de textura y vista en Direct3D 11 WARP,
conservación del buffer original, fallback ante fallo, identidad equivocada,
lectura corta, modificación posterior de datos, contratos incorrectos,
invalidación por solapamiento, observe y consumo del registro.
PNG se comprueba con transparencia, orden BGRA, tamaño impar, presupuesto,
datos inválidos y conflicto DDS/PNG. Las tres imágenes reales del usuario y
sus contratos originales también se comprueban mediante los comandos de catálogo.

El mock y WARP **no ejecutan el helper del juego**. El análisis de ABI es
estático. Falta confirmar en una sesión del juego que los buffers de esos
recursos llegan sin transformación al helper y que la sustitución se ve bien.
No se afirma compatibilidad visual de materiales ni estabilidad prolongada.

Referencias primarias de las estructuras utilizadas:
[CreateTexture2D](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d)
y [WIC desde memoria](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-bitmapsources-howto-loadfromresource).
