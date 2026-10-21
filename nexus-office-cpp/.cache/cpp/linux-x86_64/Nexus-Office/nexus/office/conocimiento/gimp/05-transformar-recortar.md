# Manual de GIMP 5. Recortar, enderezar, escalar y transformar

## Recortar
- Herramienta Recortar (Mayús+C): arrastrar, ajustar los tiradores e Intro. Opciones: «Fijo: relación de aspecto»
  (1:1, 4:5, 16:9, 3:2...), guías de composición (regla de los tercios, proporción áurea, diagonales) y «Eliminar
  píxeles recortados» desactivado para poder reencuadrar después.
- Imagen > Recortar al contenido elimina bordes vacíos o de color uniforme. Imagen > Recortar a la selección.
- Composición: el sujeto en un tercio, la mirada con aire hacia donde mira, el horizonte en un tercio y recto.

## Enderezar
- Herramienta Medir (Mayús+M): arrastrar a lo largo del horizonte o de una pared vertical y pulsar «Enderezar» en las
  opciones de la herramienta. GIMP gira lo justo y se recorta el borde después.
- Herramienta Rotar (Mayús+R) para giros libres; Imagen > Transformar > Rotar 90° para orientación.
- Fotos de móvil que salen giradas: GIMP pregunta al abrir si aplicar la orientación EXIF; responder que sí.

## Escalar y tamaño
- Imagen > Escalar imagen cambia los píxeles. Mantener la cadena cerrada para no deformar. Interpolación: LoHalo para
  reducir (más nítido), NoHalo o Cúbica para ampliar. Ampliar más del 150-200 % siempre pierde calidad.
- Imagen > Tamaño de impresión cambia solo los ppp (resolución) sin tocar los píxeles: sirve para decirle a la
  imprenta a qué tamaño va.
- Imagen > Tamaño del lienzo amplía o reduce el área sin escalar (añadir márgenes o bordes). Después, Capa > Capa a
  tamaño de imagen y rellenar el fondo.
- Capa > Escalar capa escala solo una capa (un logo dentro de un montaje).

## Píxeles, centímetros y ppp
- Píxeles = centímetros / 2,54 × ppp. Ejemplos a 300 ppp: 1 cm = 118 px; A4 (21 × 29,7 cm) = 2480 × 3508 px;
  A5 = 1748 × 2480 px; tarjeta de visita 85 × 55 mm = 1004 × 650 px (más sangrado).
- Calidad de impresión: 300 ppp a tamaño final para folletos y fotos; 150 ppp suele bastar en carteles y lonas que se
  ven de lejos. 72 o 96 ppp no significan nada para la web: allí solo cuentan los píxeles.
- Sangrado: los diseños que llegan al borde del papel llevan 3 mm extra por cada lado (A4 con sangrado:
  216 × 303 mm = 2551 × 3579 px a 300 ppp) y los textos importantes a 3-5 mm del corte (margen de seguridad).

## Transformaciones
| Herramienta | Tecla | Uso |
|---|---|---|
| Transformación unificada | Mayús+T | Mover, escalar, rotar, inclinar y perspectiva a la vez |
| Escalar | Mayús+S | Escalar capa o selección |
| Perspectiva | Mayús+P | Corregir líneas convergentes de edificios; poner una imagen sobre una pantalla o cartel |
| Voltear | Mayús+F | Espejo horizontal (Ctrl para vertical). Ojo con textos y logotipos que quedarían al revés |
| Jaula | Mayús+G | Deformar una parte dentro de un polígono |
| Deformación (warp) | W | Licuar, a pincel |
| Transformación de mango | Mayús+L | Deformar con puntos de anclaje |
| Transformación 3D | Mayús+W | Girar la capa en el espacio (maquetas en perspectiva) |

- Corregir la perspectiva de una fachada: guías arrastradas desde las reglas, herramienta Perspectiva en modo
  «Corrección (hacia atrás)» y llevar las esquinas a las guías.
- Filtros > Distorsiones > Distorsión de lente corrige barril o cojín de objetivos angulares.
