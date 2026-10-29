# GIMP en Nexus-Office: edición profesional de imágenes para los agentes

Los agentes de imagen y diseño editan las fotos y gráficos de la empresa con **GIMP**, sin ventana y sin que nadie
tenga que abrirlo. Trabajan sobre su **carpeta de trabajo** (`data/sandbox`), la misma que usan `file_list`,
`file_read` y `file_write`, y tienen en su biblioteca un **manual de GIMP** y un **grafo de conocimiento** sobre su
manejo para explicar a una persona cómo hacerlo ella misma.

## Qué GIMP se usa
Nexus-Office busca GIMP en este orden y usa el primero que arranca:

1. `media.gimp_path` en la configuración (o la variable `NEXUS_GIMP`).
2. Un GIMP copiado en `bundled/runtimes/gimp` (el mismo que preparan los instaladores de la edición Python).
3. El GIMP del sistema: PATH, `Archivos de programa\GIMP 3`, `/Applications/GIMP.app`, Flatpak `org.gimp.GIMP` o Snap.

Los instaladores de la edición C++ **no llevan GIMP** (pesa varios cientos de MB por sistema): sin él, todas las
operaciones las hace el motor de imagen propio de Nexus-Office (`src/media/raster.cpp`), sin internet. Si quieres GIMP,
instálalo en el equipo (o copia su carpeta en `bundled/runtimes/gimp`) y Nexus-Office lo usa solo.

| Sistema | Cómo se lanza GIMP |
|---|---|
| Windows x64 y ARM64 | `bin\gimp-console-3.x.exe` |
| macOS Apple Silicon e Intel | `GIMP.app/Contents/MacOS/gimp-console*` o `gimp` |
| Linux x86_64 y ARM64, Raspberry Pi 4/5 | `gimp-console` del sistema, Flatpak o Snap |
| Android | No existe GIMP para Android: motor propio |

GIMP 3 se programa en **Python-Fu**; si el equipo solo tiene GIMP 2.10, Nexus-Office le habla en **Script-Fu**.
Se ejecuta con un perfil propio en `data/gimp/perfil-<versión>`, así no cambia la configuración del GIMP del usuario.

**Sin GIMP** (Android, o un equipo donde no está o no se puede ejecutar) las mismas operaciones las hace el **motor propio**, con el
mismo resultado de medidas y formatos; cambia solo el detalle de algunos filtros (enfoque, reducción de ruido). Solo
el formato XCF y `gimp_script` necesitan GIMP. Si GIMP falla en una edición, se repite con el motor propio y la respuesta lo
dice (`nota`).

Ajustes (`data/config.yaml`):

```yaml
media:
  gimp: auto          # auto (GIMP si está, si no el motor propio) | gimp (solo GIMP) | pil (solo el motor propio)
  gimp_path: ""       # ruta a gimp-console si está en otro sitio
office:
  knowledge_packs: true   # instalar el manual de GIMP en los agentes de imagen
```

## Herramientas
| Herramienta | Riesgo | Qué hace |
|---|---|---|
| `imagen_editar` | bajo | Encadena operaciones o aplica una receta y guarda el resultado en `imagenes/` |
| `imagen_info` | bajo | Tamaño, formato, ppp y tamaño de impresión, transparencia, EXIF, colores dominantes |
| `gimp_estado` | bajo | Si hay GIMP, versión, origen e intérprete; operaciones, recetas y formatos |
| `gimp_script` | alto | Script propio en Python-Fu (GIMP 3) o Script-Fu (2.10) con `ENTRADA` y `SALIDA`. Lo aprueba un administrador |

Imágenes de entrada: una ruta de la carpeta de trabajo (`adjuntos/foto.jpg`), una imagen generada
(`/outputs/images/...`) o `ultima`. Las imágenes que se adjuntan en el chat se guardan solas en `adjuntos/` y el
agente recibe su ruta. El resultado nunca sobrescribe un fichero: si existe, se añade `_2`, `_3`... Las salidas en
JPEG, PNG, WebP o GIF se muestran en el chat.

### Operaciones
`redimensionar` (ancho, alto, porcentaje, modo ajustar/exacto/rellenar), `recortar`, `encuadrar` (proporción como
4:5 y medida final), `lienzo`, `borde`, `rotar` (grados, sentido horario), `voltear`, `brillo_contraste`,
`niveles_auto`, `balance_blancos`, `saturacion`, `tono`, `gamma`, `blanco_negro`, `sepia`, `enfocar` (máscara de
desenfoque), `desenfocar`, `reducir_ruido`, `invertir`, `posterizar`, `vineta`, `texto` (posición, color, opacidad,
fuente), `superponer` (un logo de la carpeta de trabajo, escala y posición), `ppp` y `aplanar`.

Formatos de salida: JPEG (calidad), PNG, WebP, TIFF (LZW), BMP y XCF (proyecto de GIMP, solo con GIMP).

### Recetas
| Receta | Resultado |
|---|---|
| `foto_carnet` | DNI y pasaporte: 26 × 32 mm a 300 ppp (307 × 378 px), fondo blanco |
| `redes_cuadrado` | 1080 × 1080 |
| `redes_vertical` | 1080 × 1350 (4:5) |
| `historia` | 1080 × 1920 (9:16) |
| `banner_web` | 1920 × 600 |
| `producto_fondo_blanco` | 2000 × 2000 sobre blanco, colores corregidos y enfocado |
| `mejora_rapida` | Balance de blancos, niveles, saturación y enfoque |
| `web_ligera` | 1600 px de lado mayor en WebP |
| `impresion_a4` | 2480 × 3508 px a 300 ppp en TIFF |

Ejemplo de lo que pide un agente:

```json
{"imagen": "adjuntos/tarta.jpg", "receta": "redes_vertical",
 "operaciones": [{"op": "texto", "texto": "Pastelería Lola · 948 000 000", "posicion": "abajo"},
                 {"op": "superponer", "imagen": "imagenes/logo.png", "escala": 15, "posicion": "arriba-derecha"}]}
```

## Agentes
- **Diseñador gráfico** (`disenador-grafico`), nuevo: retoque, diseño y preimpresión con todas las herramientas de
  GIMP, incluida `gimp_script`. Está en las oficinas `agencia-marketing`, `imprenta-artes-graficas` y
  `estudio-fotografia`.
- **Fotógrafo**, **Estratega de marketing**, **Impresor**, **Comercial** y el **Artista** de los agentes por defecto
  usan `imagen_editar`, `imagen_info` y `gimp_estado`, y delegan lo complejo en el diseñador.

## Manual (RAG) y grafo de conocimiento
`nexus/office/conocimiento/gimp/` es un **paquete de conocimiento**:

- Nueve capítulos en Markdown: interfaz y formatos; capas, máscaras y selecciones; color y tono; retoque;
  recortar, enderezar y escalar; texto, diseño y medidas de redes e imprenta; automatización (modo batch,
  Python-Fu, Script-Fu y las operaciones de Nexus-Office); atajos de teclado; recetas para pymes (foto de carnet,
  producto, redes, carta del restaurante, inmobiliaria, antes y después, escaneos, logos) con los avisos legales.
- `grafo.yaml`: 119 relaciones del manejo de GIMP (herramientas y teclas, menús, formatos, medidas, procedimientos).

Al arrancar, cada agente activo de la lista `agentes` de `paquete.yaml` recibe el manual en **su** biblioteca RAG
(una página por apartado, para citar bien) y las relaciones en **su** subgrafo, ancladas con «conoce → Manual de
GIMP». No va al grafo general: el resto de agentes no se llena de documentación de diseño. Es idempotente; al subir
`version` en `paquete.yaml` se sustituye. Si el agente ya estaba instalado de una versión anterior, se le añaden las
herramientas del paquete sin tocar el resto de su configuración. Al aplicar un perfil desde la app se instala en ese
momento. Se pueden añadir paquetes para otros programas con la misma estructura.

## Licencia
GIMP es software libre (GNU GPL v3); Nexus-Office solo lo ejecuta como programa aparte. Su código fuente está en
https://download.gimp.org/gimp/.

## Pruebas
En la 2.1 las 73 ediciones de imagen (con el motor propio y con GIMP) se compararon píxel a píxel con la edición
Python. Desde que la edición Python quedó congelada, esas comparaciones se retiraron.
