# Manual de GIMP 7. Automatización: modo batch, Script-Fu y Python-Fu

## Cómo lo usa Nexus-Office
Nexus-Office lanza GIMP sin ventana (modo batch) con un perfil propio en `data/gimp`, así no toca la configuración
del usuario. GIMP 3 se programa en Python-Fu; GIMP 2.10 en Script-Fu. Si GIMP no está (Android, por ejemplo), la
misma lista de operaciones se hace con Pillow.

La herramienta `imagen_editar` recibe una lista ordenada de operaciones:
| Operación | Parámetros | Equivale en GIMP a |
|---|---|---|
| redimensionar | ancho, alto, porcentaje, modo (ajustar, exacto, rellenar) | Imagen > Escalar imagen (LoHalo) |
| recortar | x, y, ancho, alto | Herramienta Recortar |
| encuadrar | proporcion (4:5), ancho, alto, posicion | Recortar con relación de aspecto fija y escalar |
| lienzo | ancho, alto, color | Imagen > Tamaño del lienzo + capa de fondo |
| borde | grosor, color | Lienzo más grande con color |
| rotar | grados (horario) | Imagen > Transformar o herramienta Rotar |
| voltear | horizontal o vertical | Imagen > Transformar > Voltear |
| brillo_contraste | brillo, contraste (-100 a 100) | Colores > Brillo-Contraste |
| niveles_auto | — | Colores > Auto > Estirar contraste (mantener colores) |
| balance_blancos | — | Colores > Auto > Balance de blancos |
| saturacion | cantidad | Colores > Tono-Saturación |
| tono | grados | Colores > Tono-Saturación |
| gamma | valor | Colores > Niveles (gamma) |
| blanco_negro | — | Colores > Desaturar (luminancia) |
| sepia | — | Colores > Colorear |
| enfocar | radio, cantidad, umbral | Filtros > Realzar > Enfocar (máscara de desenfoque) |
| desenfocar | radio | Filtros > Desenfoques > Desenfoque gaussiano |
| reducir_ruido | fuerza 1-10 | Filtros > Realzar > Reducción de ruido |
| invertir | — | Colores > Invertir |
| posterizar | niveles | Colores > Posterizar |
| vineta | fuerza | Oscurecer los bordes con una selección elíptica difuminada |
| texto | texto, tamano, color, posicion, opacidad, fuente | Herramienta Texto |
| superponer | imagen, escala, posicion, opacidad | Abrir como capas + escalar + opacidad |
| ppp | valor | Imagen > Tamaño de impresión |
| aplanar | color | Imagen > Aplanar imagen |

Recetas ya preparadas: foto_carnet, redes_cuadrado, redes_vertical, historia, banner_web, producto_fondo_blanco,
mejora_rapida, web_ligera, impresion_a4. Posiciones: arriba-izquierda, arriba, arriba-derecha, izquierda, centro,
derecha, abajo-izquierda, abajo, abajo-derecha.

Ejemplo de llamada: imagen `adjuntos/tarta.jpg`, operaciones
`[{"op": "balance_blancos"}, {"op": "encuadrar", "proporcion": "4:5", "ancho": 1080},
{"op": "texto", "texto": "Pastelería Lola · 948 000 000", "posicion": "abajo"}]`, formato `jpg`.

## Modo batch desde la línea de órdenes
- GIMP 3: `gimp-console-3.0 -i -d --quit --batch-interpreter python-fu-eval -b "exec(open('script.py').read())"`
- GIMP 2.10: `gimp-console-2.10 -i -d -f --batch-interpreter plug-in-script-fu-eval -b '(load "script.scm")' -b '(gimp-quit 0)'`
- `-i` sin interfaz, `-d` sin cargar pinceles ni patrones, `-f` sin fuentes (no usarlo si hay texto). En Windows el
  programa está en `bin\gimp-console-3.0.exe` de la carpeta de GIMP; en macOS en `GIMP.app/Contents/MacOS/`.

## Python-Fu en GIMP 3 (para gimp_script)
```python
img = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(ENTRADA))
capa = img.merge_visible_layers(Gimp.MergeType.CLIP_TO_IMAGE)
img.scale(1200, 800)                                   # Imagen > Escalar imagen
capa.brightness_contrast(0.1, 0.15)                    # rangos -1 a 1
capa.hue_saturation(Gimp.HueRange.ALL, 0, 0, 20, 0)    # saturación +20
f = Gimp.DrawableFilter.new(capa, "gegl:unsharp-mask", "")   # cualquier filtro GEGL
cfg = f.get_config(); cfg.set_property("std-dev", 1.5); cfg.set_property("scale", 0.6)
f.update(); capa.merge_filter(f)                       # combina el filtro (no destructivo hasta aquí)
img.flatten()
Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, img, Gio.File.new_for_path(SALIDA), None)
img.delete()
```
- Filtros GEGL útiles: `gegl:gaussian-blur` (std-dev-x, std-dev-y), `gegl:unsharp-mask` (std-dev, scale, threshold),
  `gegl:noise-reduction` (iterations), `gegl:vignette` (radius, softness), `gegl:stretch-contrast` (keep-colors),
  `gegl:shadows-highlights` (shadows, highlights), `gegl:color-temperature` (original-temperature,
  intended-temperature), `gegl:exposure` (exposure, black-level), `gegl:dropshadow` (x, y, radius, opacity),
  `gegl:pixelize` (size-x, size-y) para pixelar matrículas o caras, `gegl:lens-distortion`.
- Capas: `Gimp.Layer.new(img, nombre, ancho, alto, Gimp.ImageType.RGBA_IMAGE, 100, Gimp.LayerMode.NORMAL)`,
  `img.insert_layer(capa, None, 0)`, `capa.set_opacity(60)`, `capa.set_offsets(x, y)`.
- Texto: `Gimp.text_font(img, None, x, y, "Hola", 0, True, 48, Gimp.Font.get_by_name("Sans-serif"))`.
- Selección: `img.select_rectangle(Gimp.ChannelOps.REPLACE, x, y, w, h)`, `Gimp.Selection.feather(img, 20)`,
  `Gimp.Selection.invert(img)`, `Gimp.Selection.none(img)`.
- Colores: `Gimp.context_set_foreground(Gegl.Color.new("#ff0000"))`, `capa.edit_fill(Gimp.FillType.FOREGROUND)`.
- Procedimientos del PDB por nombre: `Gimp.get_pdb().lookup_procedure("file-jpeg-export")`, `create_config()`,
  `set_property(...)`, `run(config)`. La lista completa: Ayuda > Navegador de procedimientos.

## Script-Fu en GIMP 2.10
```scheme
(let* ((img (car (gimp-file-load RUN-NONINTERACTIVE ENTRADA ENTRADA)))
       (capa (car (gimp-image-merge-visible-layers img CLIP-TO-IMAGE))))
  (gimp-image-scale img 1200 800)
  (plug-in-unsharp-mask RUN-NONINTERACTIVE img capa 1.5 0.6 0)
  (set! capa (car (gimp-image-flatten img)))
  (file-jpeg-save RUN-NONINTERACTIVE img capa SALIDA SALIDA 0.9 0 1 1 "" 2 1 0 2)
  (gimp-image-delete img))
```
- Cada llamada devuelve una lista: el valor está en `(car ...)`. Booleanos `TRUE`/`FALSE`.
- Procesar una carpeta en 2.10: `(file-glob "/ruta/*.jpg" 1)` devuelve los ficheros.

## Procesar muchas fotos
- Con Nexus-Office: pedir al agente que aplique la misma receta a cada foto de una carpeta (file_list e
  imagen_editar para cada una).
- En GIMP con ventana: el plugin BIMP (Batch Image Manipulation Plugin) hace lotes con asistente; no viene incluido.
