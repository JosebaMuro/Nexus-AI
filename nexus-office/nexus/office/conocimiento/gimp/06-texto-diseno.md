# Manual de GIMP 6. Texto, diseño gráfico y piezas para la empresa

## Texto
- Herramienta Texto (T): clic en el lienzo y escribir, o arrastrar un cuadro para texto con ajuste de línea. Cada
  texto es una capa editable mientras no se transforme o se pinte sobre ella.
- Opciones: fuente, tamaño (px, pt o mm), color, justificación, interlineado e interletraje; con el texto
  seleccionado aparece un editor flotante para cambiar letras sueltas.
- GIMP 3 añade estilo de contorno al texto (relleno, trazo o ambos) en las opciones de la herramienta.
- Texto legible sobre fotos: sombra arrojada (Filtros > Luz y sombra > Sombra arrojada), una franja semitransparente
  detrás del texto o contorno fino. Contraste mínimo recomendable entre texto y fondo 4,5:1.
- Texto a lo largo de una ruta: crear la ruta (B), clic derecho en la capa de texto > Texto a lo largo de la ruta; el
  resultado es una ruta que se rellena o se traza (Editar > Trazar ruta).
- Fuentes: GIMP usa las del sistema. Instalar fuentes nuevas en el sistema y en GIMP Editar > Preferencias >
  Carpetas > Tipografías, o Fuentes > Recargar. Revisar la licencia de la fuente para uso comercial (Google Fonts y
  las SIL OFL se pueden usar en trabajos comerciales).

## Guías, rejillas y alineación
- Arrastrar desde las reglas crea guías; Imagen > Guías > Nueva guía (en porcentaje) pone guías exactas (márgenes,
  centro, sangrado). Ver > Ajustar a las guías.
- Imagen > Configurar la rejilla y Ver > Mostrar rejilla para maquetar.
- Herramienta Alinear (Q) para centrar un logo o repartir elementos.

## Rutas y formas
- Herramienta Rutas (B): clic para nodos rectos, arrastrar para curvas; Ctrl cierra la ruta. Editar > Trazar ruta
  dibuja su contorno con un grosor; Seleccionar > Desde la ruta y Editar > Rellenar crean formas.
- GIMP no es un programa vectorial: los logotipos de marca se diseñan mejor en Inkscape (SVG) y luego se importan en
  GIMP al tamaño necesario. GIMP abre SVG y pregunta la resolución.

## Marca de agua y logotipo
1. Archivo > Abrir como capas (Ctrl+Alt+O) el logo en PNG con transparencia.
2. Capa > Escalar capa a un 15-25 % del ancho de la imagen; colocarlo con la herramienta Alinear en una esquina con
   margen.
3. Opacidad de la capa 40-70 % para marca de agua; 100 % para firma de marca.
4. Para muchas fotos, `imagen_editar` con la operación `superponer` o `texto` lo hace igual en lote.

## Piezas habituales (medidas en píxeles)
Las redes cambian sus medidas: estas son las habituales, conviene comprobarlas antes de una campaña.
| Pieza | Medida |
|---|---|
| Publicación cuadrada (Instagram, Facebook, LinkedIn) | 1080 × 1080 |
| Publicación vertical Instagram | 1080 × 1350 (4:5) |
| Historia, reel, TikTok | 1080 × 1920 (9:16); dejar libres unos 250 px arriba y abajo |
| Imagen de enlace (Facebook, LinkedIn, WhatsApp) | 1200 × 630 |
| Publicación horizontal X (Twitter) | 1600 × 900 |
| Miniatura de YouTube | 1280 × 720 |
| Portada de Facebook | 851 × 315 (mejor diseñar a 1702 × 630) |
| Cabecera web | 1920 × 600 aprox. |
| Foto de perfil | 400 × 400 o más, el contenido importante en el círculo central |
| Ficha de producto | 2000 × 2000 sobre blanco, producto ocupando el 85-90 % |
| Firma de correo | logo de 300-600 px de ancho, PNG |

## Impresión
| Pieza | Tamaño final | Píxeles a 300 ppp con 3 mm de sangrado |
|---|---|---|
| Tarjeta de visita | 85 × 55 mm | 1075 × 720 |
| Flyer A6 | 105 × 148 mm | 1311 × 1819 |
| Flyer A5 | 148 × 210 mm | 1819 × 2551 |
| Cartel A4 | 210 × 297 mm | 2551 × 3579 |
| Cartel A3 | 297 × 420 mm | 3579 × 5031 |

- Textos a 3-5 mm del borde de corte; nada importante en el sangrado.
- Negro de textos pequeños: negro puro; un fondo negro grande en offset, «negro enriquecido» (lo pide la imprenta).
- Entregar PDF o TIFF a 300 ppp, o lo que pida la imprenta. Para maquetas de varias páginas usar Scribus.

## Diseño que funciona
- Jerarquía: un mensaje principal grande, uno secundario y la llamada a la acción (teléfono, web, «Reserva ya»).
- Dos fuentes como máximo y una paleta de 2-3 colores de la marca (con su código hexadecimal guardado).
- Espacio en blanco: no llenar todo.
- Fotos propias o con licencia: las imágenes de internet tienen autor (Ley de Propiedad Intelectual). Bancos libres
  con licencia comercial o generar la imagen con `generate_image`.
- Personas reconocibles: hace falta su consentimiento para usar su imagen en publicidad (LO 1/1982 y RGPD).
