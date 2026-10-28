# Manual de GIMP 2. Capas, máscaras y selecciones

## Capas
- Panel Capas (Ctrl+L). Cada capa tiene visibilidad (ojo), bloqueo, modo de fusión y opacidad.
- Capa nueva: Mayús+Ctrl+N. Duplicar capa: Mayús+Ctrl+D. Combinar hacia abajo: Capa > Combinar hacia abajo.
  Aplanar imagen (quita transparencia y combina todo sobre el color de fondo): Imagen > Aplanar imagen.
- Grupos de capas para ordenar (botón de carpeta en el panel Capas).
- Tamaño de capa: Capa > Tamaño de borde de capa; Capa > Capa a tamaño de imagen. Una capa más pequeña que el lienzo
  deja zonas transparentes alrededor (líneas discontinuas amarillas).
- Selección flotante (aparece al pegar): hay que anclarla (Ctrl+H) o convertirla en capa nueva (Mayús+Ctrl+N).
- Pegar como imagen nueva: Mayús+Ctrl+V. Pegar en sitio: Ctrl+Alt+V.
- Herramienta Mover (M): por defecto mueve la capa bajo el cursor; con Mayús mueve la capa activa. Las flechas mueven
  un píxel, Mayús+flecha 25 píxeles.
- Alinear (Q): alinea y distribuye capas respecto a la imagen, la selección o la primera capa elegida.

## Modos de fusión más útiles
| Modo | Para qué |
|---|---|
| Normal | Superponer |
| Multiplicar | Oscurece: sombras, firmas escaneadas sobre fondo blanco (el blanco desaparece) |
| Pantalla | Aclara: luces, destellos |
| Superponer / Luz suave | Contraste, esquivar y quemar en una capa gris al 50 % |
| Color | Colorear manteniendo la luminosidad (cambiar el color de una prenda) |
| Extraer granulado / Combinar granulado | Separación de frecuencias en retoque de piel |

## Máscaras de capa (edición no destructiva)
- Capa > Máscara > Añadir máscara de capa: blanco (todo visible), negro (todo oculto), desde la selección o desde una
  copia en grises.
- En la máscara se pinta: negro oculta, blanco muestra, gris deja semitransparente. Así se borra sin borrar: se puede
  corregir siempre. Es la forma profesional de recortar a una persona o fundir dos fotos.
- Alt+clic en la miniatura de la máscara la muestra sola; Ctrl+clic la desactiva temporalmente.
- Capa > Máscara > Aplicar máscara la hace definitiva. Pintar con degradado negro-blanco en la máscara funde dos
  imágenes (cielo, montajes, banners).

## Herramientas de selección
| Herramienta | Tecla | Uso |
|---|---|---|
| Rectangular | R | Rectángulos; opción «Fijo: relación de aspecto» para encuadres 1:1, 4:5 o 16:9 |
| Elíptica | E | Círculos (Mayús mantiene la proporción) |
| Libre (lazo) | F | Clic a clic para polígonos, arrastrar para forma libre; Intro cierra |
| Difusa (varita mágica) | U | Zonas contiguas de color parecido; ajustar Umbral |
| Por color | Mayús+O | Todo el color parecido de la imagen, aunque no se toque |
| Tijeras inteligentes | I | Sigue bordes con contraste |
| Primer plano | — | Extrae una persona u objeto: se marca por encima y se pinta el interior |
| Rutas | B | Contornos precisos con curvas Bézier; Mayús+V convierte la ruta en selección |

- Modos: Mayús añade a la selección, Ctrl resta, Mayús+Ctrl interseca.
- Seleccionar todo Ctrl+A, nada Mayús+Ctrl+A, invertir Ctrl+I.
- Seleccionar > Difuminar suaviza el borde (2-5 px en fotos, más para viñetas); Encoger, Agrandar y Borde.
- Máscara rápida (Mayús+Q): la selección se ve en rojo y se retoca pintando con pinceles; Mayús+Q de nuevo vuelve.
- Seleccionar > Guardar en canal guarda una selección para usarla otra vez (panel Canales).
- Seleccionar > Desde el canal alfa (Capa > Transparencia > Alfa a selección) selecciona lo no transparente.

## Quitar un fondo
1. Si el fondo es liso (blanco o verde): Colores > Color a alfa, o Seleccionar por color y Suprimir tras Capa >
   Transparencia > Añadir canal alfa.
2. Si es complejo: herramienta Selección de primer plano o Rutas, después Capa > Máscara > Añadir máscara desde la
   selección y repasar la máscara con un pincel suave (pelo).
3. Exportar en PNG o WebP para conservar la transparencia. En una tienda online lo habitual es fondo blanco puro
   (#ffffff): añadir una capa blanca debajo y aplanar.
