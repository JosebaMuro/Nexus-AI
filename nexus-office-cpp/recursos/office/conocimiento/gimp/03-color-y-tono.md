# Manual de GIMP 3. Corrección de color y tono

Orden profesional para corregir una foto: 1) recortar y enderezar, 2) balance de blancos, 3) exposición y niveles,
4) curvas y contraste, 5) color y saturación, 6) retoque local, 7) reducir ruido, 8) redimensionar al tamaño final,
9) enfocar al final. Enfocar antes de reducir hace que el enfoque se pierda o salgan halos.

## Menú Colores
| Orden | Qué hace | Cuándo |
|---|---|---|
| Colores > Auto > Balance de blancos | Estira cada canal por separado y quita dominantes | Fotos amarillas (bombilla) o azules (sombra) |
| Colores > Auto > Estirar contraste | Estira el histograma; con «Mantener colores» no cambia el tono | Fotos grises o apagadas |
| Colores > Auto > Ecualizar | Reparte el histograma | Rara vez: suele quedar artificial |
| Colores > Niveles | Puntos negro, gris y blanco por canal; cuentagotas | Corrección básica más precisa |
| Colores > Curvas | Curva tonal por canal | Contraste fino (curva en S suave), virajes |
| Colores > Exposición | Exposición y nivel de negro en pasos de diafragma | Fotos sub o sobreexpuestas |
| Colores > Sombras-Luces | Aclara sombras y recupera luces | Contraluces, interiores con ventana |
| Colores > Temperatura de color | Pasa de una temperatura original a otra (K) | Corregir luz cálida o fría con precisión |
| Colores > Balance de color | Mueve sombras, medios tonos y luces hacia cian-rojo, magenta-verde y amarillo-azul | Ajuste de dominantes por zonas |
| Colores > Tono-Saturación | Tono, luminosidad y saturación, global o por color primario | Más viveza; cambiar el color de un objeto |
| Colores > Brillo-Contraste | Ajuste simple | Retoques rápidos |
| Colores > Desaturar > Desaturar (luminancia) | Blanco y negro | Rápido |
| Colores > Desaturar > Mezclador mono | Blanco y negro mezclando canales | Blanco y negro de calidad (como filtros de color) |
| Colores > Colorear | Monocromo de un color (sepia con tono 30-40) | Efecto antiguo |
| Colores > Invertir | Negativo | Escaneos de negativos |
| Colores > Posterizar | Reduce niveles de color | Efecto cartel |
| Colores > Color a alfa | Hace transparente un color | Logos sobre blanco |

## Leer el histograma
- Panel Histograma (Ventanas > Diálogos empotrables > Histograma). A la izquierda las sombras, a la derecha las luces.
- Un histograma que no llega a los extremos indica foto sin contraste: en Niveles se llevan los triángulos negro y
  blanco al inicio y final de los datos.
- Picos pegados al borde derecho: luces quemadas, sin detalle recuperable.
- En Niveles y Curvas el cuentagotas gris sobre algo que debe ser neutro (una pared blanca, una camisa gris) corrige
  el balance de blancos en un clic.

## Curvas
- Curva en S suave (bajar un punto en sombras y subir otro en luces) da contraste sin quemar.
- Subir solo el punto medio aclara medios tonos (como gamma mayor que 1).
- Por canal: subir el canal azul en sombras y bajar en luces da el viraje frío-cálido de cine.
- Se pueden guardar ajustes como predefinidos (botón + en el diálogo) para aplicar el mismo estilo a una sesión.

## Gestión de color
- Trabajar en sRGB para web y para la mayoría de imprentas digitales. Imagen > Gestión del color > Convertir a
  perfil de color si la imagen viene en Adobe RGB u otro perfil y va a web.
- Para imprenta offset: preguntar a la imprenta el perfil (p. ej. Coated FOGRA39 o PSO Coated v3). En GIMP 3 se activa
  Ver > Prueba de color con ese perfil para ver cómo saldrá; GIMP 3 puede exportar JPEG, TIFF y PSD en CMYK con el
  perfil de simulación. Si la imprenta pide PDF/X, se maqueta en Scribus con la imagen RGB y Scribus convierte.
- Colores muy saturados (verdes y azules eléctricos, naranjas fluorescentes) no existen en CMYK: avisar al cliente.
- Pantalla calibrada: sin calibrar, el color que ve el diseñador no es fiable.
